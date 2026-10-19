#!/usr/bin/env bash
# Builds PCRE 8.45 (BSD, LICENCE): pcregrep and pcretest in place of upstream's dynamic ones, with
# UTF-8 and Unicode properties, no C++, no JIT (it writes code the machine cannot fetch from data).
#   wsl -- bash /mnt/c/.../linux/nommu/build.sh ports/pcre.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
source "$HERE/fetch.sh"
OUT=${PORTS_OUT:-$REPO/build/images/linux/root}	# (PORTS_OUT: a private image's overlay)
fetch pcre https://sourceforge.net/projects/pcre/files/pcre/8.45/pcre-8.45.tar.bz2/download \
    4dae6fdcd2bb0bb6c37b5f97c33c2be954da743985369cddac3546e3218bffb8
P=$WORK/src/pcre
CONF="--host=riscv32-linux-musl CC=rv32-cc AR=riscv32-linux-ar RANLIB=riscv32-linux-ranlib CFLAGS=-O2
    --disable-shared --enable-static --disable-cpp --enable-utf --enable-unicode-properties
    --disable-pcregrep-libz --disable-pcregrep-libbz2 --disable-pcretest-libreadline --disable-pcretest-libedit"
if [ "$(cat "$P/.conf" 2>/dev/null)" != "$CONF" ]; then
    (cd "$P" && ./configure -q $CONF) && echo "$CONF" > "$P/.conf"
fi
make -s -C "$P" -j"$(nproc)" pcregrep pcretest > /dev/null
mkdir -p "$OUT/usr/bin" "$OUT/usr/share/pcre"
riscv32-linux-strip -o "$OUT/usr/bin/pcregrep" "$P/pcregrep"
riscv32-linux-strip -o "$OUT/usr/bin/pcretest" "$P/pcretest"
cp "$P/LICENCE" "$OUT/usr/share/pcre/LICENCE"
ls -l "$OUT/usr/bin/pcregrep" "$OUT/usr/bin/pcretest"
