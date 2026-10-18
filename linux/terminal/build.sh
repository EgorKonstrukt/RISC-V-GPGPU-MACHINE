#!/usr/bin/env bash
# Builds the terminal programs on ncurses (static, one copy here) into build/images/linux/root at
# upstream's paths, in place of upstream's dynamic ELF programs, which do not run on the machine
# without an MMU (docs/system/terminal.md): nano, htop, sl, dialog, mg, file, mc (on glib).
#   wsl -- bash /mnt/c/.../linux/nommu/build.sh terminal/build.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
T=$WORK/src/terminal
ST=$T/sysroot	# the libraries' headers and archives: ncurses, pcre, glib
OUT=${TERMINAL_OUT:-$REPO/build/images/linux/root}	# (TERMINAL_OUT: a private image's overlay)
mkdir -p "$T/dl" "$ST"
HOST="--host=riscv32-linux-musl --build=x86_64-pc-linux-gnu"
TOOLS="CC=rv32-cc AR=riscv32-linux-ar RANLIB=riscv32-linux-ranlib STRIP=riscv32-linux-strip"
export PKG_CONFIG_LIBDIR=$ST/usr/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=$ST
CURSES_CFLAGS="-I$ST/usr/include/ncursesw -I$ST/usr/include"
CURSES_LIBS="-L$ST/usr/lib -lncursesw"

# get NAME URL SHA256: the release unpacked into $T/NAME once
get() {
    local f=$T/dl/$(basename "$2")
    [ -d "$T/$1" ] && return
    echo "== $1"
    [ -s "$f" ] || curl -sSfL -o "$f" "$2"
    echo "$3  $f" | sha256sum -c --quiet || { rm -f "$f"; echo "$1: wrong sum"; exit 1; }
    mkdir -p "$T/$1.new" && tar -xf "$f" -C "$T/$1.new" --strip-components=1 && mv "$T/$1.new" "$T/$1"
}
# conf NAME ARGS...: configure once (objects stay between runs)
conf() {
    local n=$1; shift
    if [ ! -f "$T/$n/.configured" ]; then
        # (select and pselect: the forced nommu_fork.h declares them through sys/types.h, so
        # autoconf's test, which declares its own, fails to compile)
        (cd "$T/$n" && ./configure $HOST --prefix=/usr --disable-shared $TOOLS \
            ac_cv_func_select=yes ac_cv_func_pselect=yes "$@" > configure.out 2>&1) || { tail -30 "$T/$n/config.log"; exit 1; }
        # (gnulib takes its warning flags from the preprocessor's output, which the forced
        # nommu_fork.h fills with musl's declarations)
        find "$T/$n" -name Makefile -exec sed -i 's/^GL_CFLAG_GNULIB_WARNINGS = .*/GL_CFLAG_GNULIB_WARNINGS =/' {} +
        touch "$T/$n/.configured"
    fi
}
put() { mkdir -p "$(dirname "$OUT/$2")" && riscv32-linux-strip -o "$OUT/$2" "$1"; }
lic() { mkdir -p "$OUT/usr/share/$1" && cp "$2" "$OUT/usr/share/$1/${3:-$(basename "$2")}"; }
relink() { for f; do [ ! "$ST/usr/lib/libncursesw.a" -nt "$f" ] || rm -f "$f"; done; }	# programs older than ncurses

get ncurses https://ftp.gnu.org/gnu/ncurses/ncurses-6.5.tar.gz 136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6
get nano https://www.nano-editor.org/dist/v8/nano-8.2.tar.xz d5ad07dd862facae03051c54c6535e54c7ed7407318783fcad1ad2d7076fffeb
get htop https://github.com/htop-dev/htop/releases/download/3.3.0/htop-3.3.0.tar.xz a69acf9b42ff592c4861010fce7d8006805f0d6ef0e8ee647a6ee6e59b743d5c
get sl https://github.com/mtoyoda/sl/archive/refs/tags/5.02.tar.gz 1e5996757f879c81f202a18ad8e982195cf51c41727d3fea4af01fdcbbb5563a
get dialog https://invisible-island.net/archives/dialog/dialog-1.3-20240619.tgz 5d8c4318963db3fd383525340276e0e05ee3dea9a6686c20779f5433b199547d
get mg https://github.com/troglobit/mg/releases/download/v3.7/mg-3.7.tar.gz 05101360d2194392da0950e8b6f18d067d8af0fd2f572461ba4d4e7b4ccbb4c1
get file https://astron.com/pub/file/file-5.41.tar.gz 13e532c7b364f7d57e23dfeea3147103150cb90593a57af86c10e4f6e411603f
get pcre https://sourceforge.net/projects/pcre/files/pcre/8.45/pcre-8.45.tar.bz2 4dae6fdcd2bb0bb6c37b5f97c33c2be954da743985369cddac3546e3218bffb8
get glib https://download.gnome.org/sources/glib/2.70/glib-2.70.5.tar.xz f70bf76ebcc84e0705722f038be8e2f9a58d17e1a700810c635fcc18b8974b7e
get mc https://ftp.osuosl.org/pub/midnightcommander/mc-4.8.27.tar.xz 31be59225ffa9920816e9a8b3be0ab225a16d19e4faf46890f25bdffa02a4ff4

# ncurses, wide characters; the terminal descriptions are the ROM's (/usr/share/terminfo).
# (cf_cv_*: run tests; without poll a key read with a timeout never comes)
conf ncurses --with-default-terminfo-dir=/usr/share/terminfo --with-terminfo-dirs=/usr/share/terminfo \
    --enable-widec --without-cxx --without-cxx-binding --without-ada --without-tests --without-progs \
    --without-manpages --disable-db-install --without-debug --disable-stripping --with-build-cc=gcc \
    --enable-pc-files --with-pkg-config-libdir=/usr/lib/pkgconfig --disable-home-terminfo \
    cf_cv_working_poll=yes cf_cv_func_nanosleep=yes
make -s -C "$T/ncurses" -j"$(nproc)" > /dev/null 2>&1 || { make -C "$T/ncurses" 2>&1 | grep -m5 -i error; exit 1; }
[ ! "$T/ncurses/lib/libncursesw.a" -nt "$ST/usr/lib/libncursesw.a" ] || make -s -C "$T/ncurses" install DESTDIR="$ST" > /dev/null
for l in ncurses tinfo curses; do ln -sf libncursesw.a "$ST/usr/lib/lib$l.a"; done
for d in ncurses ncursesw; do [ -e "$ST/usr/include/$d" ] || ln -sfn . "$ST/usr/include/$d"; done	# (the headers are in include itself)
sed -i "s|^prefix=.*|prefix=/usr|" "$ST"/usr/lib/pkgconfig/*.pc
[ -f "$ST/usr/lib/pkgconfig/ncurses.pc" ] || ln -sf ncursesw.pc "$ST/usr/lib/pkgconfig/ncurses.pc"
lic ncurses "$T/ncurses/COPYING"

# sl: one file
relink "$T/sl/sl"
[ -f "$T/sl/sl" ] || rv32-cc -O2 -w $CURSES_CFLAGS "$T/sl/sl.c" $CURSES_LIBS -o "$T/sl/sl"
put "$T/sl/sl" usr/bin/sl; lic sl "$T/sl/LICENSE"

# nano (gnulib's sys/types.h wants its config.h before it, and nommu_fork.h comes first)
conf nano --disable-nls --disable-libmagic --enable-utf8 CPPFLAGS="$CURSES_CFLAGS -D_GL_CONFIG_H_INCLUDED=1" LDFLAGS="-L$ST/usr/lib"
relink "$T/nano/src/nano"
make -s -C "$T/nano" -j"$(nproc)" > /dev/null
put "$T/nano/src/nano" usr/bin/nano; lic nano "$T/nano/COPYING"

# htop: /proc only (no sensors, capabilities, delay accounting or hwloc)
conf htop --enable-unicode --disable-sensors --disable-capabilities --disable-delayacct --disable-hwloc \
    --disable-affinity --disable-unwind CPPFLAGS="$CURSES_CFLAGS" LDFLAGS="-L$ST/usr/lib"
relink "$T/htop/htop"
make -s -C "$T/htop" -j"$(nproc)" > /dev/null
put "$T/htop/htop" usr/bin/htop; lic htop "$T/htop/COPYING"

# dialog
conf dialog --with-ncursesw --disable-nls --disable-rpath-hack CPPFLAGS="$CURSES_CFLAGS" LDFLAGS="-L$ST/usr/lib"
relink "$T/dialog/dialog"
make -s -C "$T/dialog" -j"$(nproc)" dialog > /dev/null
put "$T/dialog/dialog" usr/bin/dialog; lic dialog "$T/dialog/COPYING"

# mg (Troglobit's portable mg)
conf mg CPPFLAGS="$CURSES_CFLAGS" LDFLAGS="-L$ST/usr/lib"
relink "$T/mg/src/mg"
make -s -C "$T/mg" -j"$(nproc)" > /dev/null
put "$T/mg/src/mg" usr/bin/mg; lic mg "$T/mg/UNLICENSE" LICENSE

# file 5.41, the version of the ROM's /usr/share/misc/magic.mgc
conf file --enable-static --disable-zlib --disable-bzlib --disable-xzlib --disable-zstdlib --disable-lzlib --disable-libseccomp
make -s -C "$T/file/src" -j"$(nproc)" magic.h > /dev/null
make -s -C "$T/file/src" -j"$(nproc)" file > /dev/null
put "$T/file/src/file" usr/bin/file; lic file "$T/file/COPYING"

# pcre 8.45 for glib's regular expressions
conf pcre --enable-static --disable-cpp --enable-utf --enable-unicode-properties --disable-pcregrep-libz --disable-pcregrep-libbz2
make -s -C "$T/pcre" -j"$(nproc)" libpcre.la > /dev/null
[ ! "$T/pcre/.libs/libpcre.a" -nt "$ST/usr/lib/libpcre.a" ] || make -s -C "$T/pcre" install-libLTLIBRARIES install-includeHEADERS \
    install-nodist_includeHEADERS install-pkgconfigDATA DESTDIR="$ST" > /dev/null
sed -i "s|^prefix=.*|prefix=/usr|" "$ST"/usr/lib/pkgconfig/libpcre*.pc
rm -f "$ST"/usr/lib/*.la	# (libtool takes them for libraries that were moved)

# glib 2.70 with meson (from PyPI, in a folder of the build's own), only libglib-2.0: no gobject or
# gio, so no libffi or zlib. Its atomic operations are its locked ones (RV32IMAF has no atomic instructions).
MV=$T/meson-venv
[ -x "$MV/bin/meson" ] || { python3 -m venv "$MV" && "$MV/bin/pip" install -q meson==1.2.3 ninja==1.11.1.1; }
export PATH="$PATH:$MV/bin"
G=$T/glib
if ! grep -q "^# (ShaderEmu)" "$G/meson.build"; then
    sed -i -e "s/^libffi_dep = dependency('libffi'.*/libffi_dep = dependency('', required : false)/" \
        -e "s/libz_dep = subproject('zlib').get_variable('zlib_dep')/libz_dep = dependency('', required : false)/" \
        -e "s/^subdir('gobject')$//; s/^subdir('gio')$//; s/^subdir('fuzzing')$//" -e "1i # (ShaderEmu)" "$G/meson.build"
fi
cat > "$T/cross.txt" <<EOF
[binaries]
c = 'rv32-cc'
cpp = 'rv32-c++'
ar = 'riscv32-linux-ar'
strip = 'riscv32-linux-strip'
pkg-config = 'pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'riscv32'
cpu = 'riscv32'
endian = 'little'
[properties]
needs_exe_wrapper = true
growing_stack = false
have_c99_vsnprintf = true
have_c99_snprintf = true
have_unix98_printf = true
va_val_copy = true
EOF
if [ ! -f "$G/build/build.ninja" ]; then
    "$MV/bin/meson" setup "$G/build" "$G" --cross-file "$T/cross.txt" --prefix=/usr --buildtype=release \
        -Ddefault_library=static -Dlibmount=disabled -Dselinux=disabled -Dxattr=false -Dtests=false \
        -Dinstalled_tests=false -Dnls=disabled -Dman=false -Dgtk_doc=false -Dlibelf=disabled -Dsysprof=disabled \
        -Dwrap_mode=nofallback > "$T/glib-setup.out" 2>&1 || { tail -30 "$T/glib-setup.out"; exit 1; }
fi
"$MV/bin/ninja" -C "$G/build" glib/libglib-2.0.a > /dev/null
if [ "$G/build/glib/libglib-2.0.a" -nt "$ST/usr/lib/libglib-2.0.a" ]; then
    rm -rf "$ST/usr/include/glib-2.0"; mkdir -p "$ST/usr/include/glib-2.0/glib" "$ST/usr/lib/glib-2.0/include"
    cp "$G/build/glib/libglib-2.0.a" "$ST/usr/lib/"
    (cd "$G/glib" && find . -name '*.h' ! -path './tests/*' -exec cp --parents {} "$ST/usr/include/glib-2.0/glib/" \;)
    cp "$G/glib/glib.h" "$G/glib/glib-unix.h" "$ST/usr/include/glib-2.0/"
    cp "$G/build/glib/glibconfig.h" "$ST/usr/lib/glib-2.0/include/"
    cat > "$ST/usr/lib/pkgconfig/glib-2.0.pc" <<EOF
prefix=/usr
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: GLib
Description: C Utility Library
Version: 2.70.5
Libs: -L\${libdir} -lglib-2.0 -lpcre
Cflags: -I\${includedir}/glib-2.0 -I\${libdir}/glib-2.0/include
EOF
fi
lic glib "$G/COPYING"

# mc 4.8.27, the version of the ROM's /etc/mc, /usr/share/mc and /usr/libexec/mc
# A fork's child here may only exec or exit, in the function that forked: my_systemv forks itself
# (mc-vfork.patch), and copies and moves in the background (a child that goes on copying) are left out.
grep -q my_fork_wait "$T/mc/lib/utilunix.c" || patch -s -d "$T/mc" -p1 < "$HERE/mc-vfork.patch"
GLIB_CFLAGS=$(pkg-config --cflags glib-2.0); GLIB_LIBS=$(pkg-config --libs glib-2.0)
conf mc --sysconfdir=/etc --libexecdir=/usr/libexec --with-screen=ncurses --without-x --without-gpm-mouse --disable-background \
    --disable-nls --disable-vfs-sftp --disable-vfs-undelfs --disable-doxygen-doc --with-search-engine=glib \
    --with-ncurses-includes="$ST/usr/include" --with-ncurses-libs="$ST/usr/lib" \
    GLIB_CFLAGS="$GLIB_CFLAGS" GLIB_LIBS="$GLIB_LIBS" CPPFLAGS="$CURSES_CFLAGS" LDFLAGS="-L$ST/usr/lib"
relink "$T/mc/src/mc"
make -s -C "$T/mc" -j"$(nproc)" > /dev/null
put "$T/mc/src/mc" usr/bin/mc; put "$T/mc/src/consaver/cons.saver" usr/libexec/mc/cons.saver; lic mc "$T/mc/COPYING"

# zsh 5.8.1 (its functions are the ROM's) only when asked: it runs on in the child of every fork
# for an external command, a pipeline or $(...), so on vfork the first such command ends it
if [ "${TERMINAL_ZSH:-0}" = 1 ]; then
    get zsh https://www.zsh.org/pub/old/zsh-5.8.1.tar.xz b6973520bace600b4779200269b1e5d79e5f505ac4952058c11ad5bbf0dd9919
    conf zsh --disable-dynamic --disable-gdbm --disable-pcre --disable-cap --with-tcsetpgrp --enable-etcdir=/etc \
        --with-term-lib=ncursesw CPPFLAGS="$CURSES_CFLAGS" LDFLAGS="-L$ST/usr/lib"
    relink "$T/zsh/Src/zsh"
    make -s -C "$T/zsh" -j"$(nproc)" > /dev/null
    put "$T/zsh/Src/zsh" bin/zsh; lic zsh "$T/zsh/LICENCE"
fi

ls -l "$OUT"/usr/bin/{sl,nano,htop,dialog,mg,file,mc}
