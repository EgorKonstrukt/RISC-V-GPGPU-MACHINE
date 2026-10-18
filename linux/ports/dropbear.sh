#!/usr/bin/env bash
# Builds Dropbear (MIT and others, LICENSE) as one program, dropbearmulti, in place of upstream's
# OpenSSH, whose server keeps running in children it forks: the server dropbear (in the foreground,
# a connection's child re-executes the program: dropbear.patch), the client dbclient, also ssh,
# scp, dropbearkey and dropbearconvert, as names linked to it.
#   wsl -- bash /mnt/c/.../linux/nommu/build.sh ports/dropbear.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
source "$HERE/fetch.sh"
OUT=${PORTS_OUT:-$REPO/build/images/linux/root}	# (PORTS_OUT: a private image's overlay)
V=2025.88
fetch dropbear https://matt.ucc.asn.au/dropbear/releases/dropbear-$V.tar.bz2 \
    783f50ea27b17c16da89578fafdb6decfa44bb8f6590e5698a4e4d3672dc53d4 "$HERE/dropbear.patch"
fetch monocypher https://monocypher.org/download/monocypher-4.0.2.tar.gz \
    38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c
D=$WORK/src/dropbear
# X25519 and Ed25519 by Monocypher (dropbear-curve25519.c): TweetNaCl's took 12 s of guest time a multiplication
MC=$WORK/src/monocypher/src
for f in "$HERE/dropbear-curve25519.c:curve25519.c" "$MC/monocypher.c:" "$MC/monocypher.h:" \
         "$MC/optional/monocypher-ed25519.c:" "$MC/optional/monocypher-ed25519.h:"; do
    s=${f%%:*}; n=${f#*:}; n=${n:-$(basename "$s")}
    cmp -s "$s" "$D/src/$n" || cp "$s" "$D/src/$n"
done
cat > "$D/localoptions.h.new" <<'EOF'
#define DEFAULT_PATH "/usr/bin:/bin:/usr/sbin:/sbin"
#define DROPBEAR_PATH_SSH_PROGRAM "/usr/bin/dbclient"
#define DROPBEAR_SNTRUP761 0
#define DROPBEAR_MLKEM768 0
EOF
# (make does not see localoptions.h change: a new one builds everything again)
cmp -s "$D/localoptions.h.new" "$D/localoptions.h" || { mv "$D/localoptions.h.new" "$D/localoptions.h"; make -s -C "$D" clean > /dev/null 2>&1 || true; }
# no utmp, lastlog or zlib on the machine; ac_cv_func_fork=no makes DROPBEAR_VFORK, its no-MMU paths, and
# without getrandom() the seed is /dev/urandom's: the kernel's pool is never ready here, getrandom() waits forever
CONF="--host=riscv32-linux-musl CC=rv32-cc AR=riscv32-linux-ar RANLIB=riscv32-linux-ranlib CFLAGS=-O2
    ac_cv_func_fork=no ac_cv_func_getrandom=no --enable-static --enable-bundled-libtom --disable-harden --disable-zlib
    --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx --disable-pututline --disable-pututxline"
if [ "$(cat "$D/.conf" 2>/dev/null)" != "$CONF" ]; then
    (cd "$D" && { [ ! -f Makefile ] || make -s clean; } && ./configure -q $CONF) && echo "$CONF" > "$D/.conf"
fi
! grep -q '^#define HAVE_FORK\|^#define HAVE_GETRANDOM' "$D/config.h" || { echo "configure found fork or getrandom"; exit 1; }
make -s -C "$D" -j"$(nproc)" MULTI=1 PROGRAMS="dropbear dbclient dropbearkey dropbearconvert scp" > /dev/null
mkdir -p "$OUT/usr/bin" "$OUT/usr/sbin" "$OUT/usr/share/dropbear"
riscv32-linux-strip -o "$OUT/usr/bin/dropbearmulti" "$D/dropbearmulti"
# the names, hard links in the ROM (tools/make_linux_image.py); dbmulti chooses by the name it is run as
for n in usr/sbin/dropbear usr/bin/dbclient usr/bin/ssh usr/bin/scp usr/bin/dropbearkey usr/bin/dropbearconvert; do
    echo /usr/bin/dropbearmulti > "$OUT/$n.romlink"
done
cp "$D/LICENSE" "$OUT/usr/share/dropbear/LICENSE"
cp "$WORK/src/monocypher/LICENCE.md" "$OUT/usr/share/dropbear/LICENCE-monocypher.md"
ls -l "$OUT/usr/bin/dropbearmulti"
