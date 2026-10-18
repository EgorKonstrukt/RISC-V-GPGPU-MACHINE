#!/usr/bin/env bash
# Builds socat 1.8.0.3 (GPL-2.0) with filan and procan, in place of upstream's dynamic ones. Without
# what forks: its children return from fork() and run on, which a vfork cannot do (socat.patch makes
# the fork option an error; EXEC, SYSTEM and SHELL are left out). No OpenSSL, readline or libwrap.
#   wsl -- bash /mnt/c/.../linux/nommu/build.sh ports/socat.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
source "$HERE/fetch.sh"
OUT=${PORTS_OUT:-$REPO/build/images/linux/root}	# (PORTS_OUT: a private image's overlay)
fetch socat http://www.dest-unreach.org/socat/download/socat-1.8.0.3.tar.gz \
    a9f9eb6cfb9aa6b1b4b8fe260edbac3f2c743f294db1e362b932eb3feca37ba4 "$HERE/socat.patch"
S=$WORK/src/socat
CONF="--host=riscv32-linux-musl CC=rv32-cc AR=riscv32-linux-ar RANLIB=riscv32-linux-ranlib CFLAGS=-O2
    --disable-exec --disable-system --disable-shell --disable-openssl --disable-readline --disable-libwrap
    --disable-fips --disable-ip6 --disable-sctp --disable-dccp --disable-vsock"
if [ "$(cat "$S/.conf" 2>/dev/null)" != "$CONF" ]; then
    (cd "$S" && ./configure -q $CONF > /dev/null) && echo "$CONF" > "$S/.conf"
fi
make -s -C "$S" -j"$(nproc)" socat filan procan > /dev/null
mkdir -p "$OUT/usr/bin" "$OUT/usr/share/socat"
for p in socat filan procan; do riscv32-linux-strip -o "$OUT/usr/bin/$p" "$S/$p"; done
cp "$S/COPYING" "$OUT/usr/share/socat/COPYING"
ls -l "$OUT"/usr/bin/{socat,filan,procan}
