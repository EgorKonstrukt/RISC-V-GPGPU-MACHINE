#!/usr/bin/env bash
# Builds liblockfile 1.17 (LGPL, dotlockfile GPL) and lockfile-progs 0.1.19 (GPL): dotlockfile, and
# lockfile-create with its other names (lockfile-check, -remove, -touch, mail-lock, mail-unlock,
# mail-touchlock: one program that acts by the name it is run as), in place of upstream's.
#   wsl -- bash /mnt/c/.../linux/nommu/build.sh ports/lockfile.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
source "$HERE/fetch.sh"
OUT=${PORTS_OUT:-$REPO/build/images/linux/root}	# (PORTS_OUT: a private image's overlay)
fetch liblockfile https://github.com/miquels/liblockfile/archive/refs/tags/v1.17.tar.gz \
    7d34e4b054f878be492dd782c34bfe2107689414aa82a361202756bbe3ec6ad8
fetch lockfile-progs http://deb.debian.org/debian/pool/main/l/lockfile-progs/lockfile-progs_0.1.19.tar.gz \
    2c5704b01c8f474f82921780e4592a927b2bf6a6d7616354a6c5d7cd5664857e
L=$WORK/src/liblockfile
P=$WORK/src/lockfile-progs
# the machine has root only: no mail group, dotlockfile is not setgid
CONF="--host=riscv32-linux-musl --prefix=/usr --bindir=/usr/bin CC=rv32-cc CFLAGS=-O2"
if [ "$(cat "$L/.conf" 2>/dev/null)" != "$CONF" ]; then
    (cd "$L" && ./configure -q $CONF) && echo "$CONF" > "$L/.conf"
fi
make -s -C "$L" AR=riscv32-linux-ar liblockfile.a dotlockfile
make -s -C "$P" CC=rv32-cc CFLAGS="-O2 -Wall -I$L" LDFLAGS="-L$L" lockfile-create
mkdir -p "$OUT/usr/bin" "$OUT/usr/share/liblockfile" "$OUT/usr/share/lockfile-progs"
riscv32-linux-strip -o "$OUT/usr/bin/dotlockfile" "$L/dotlockfile"
riscv32-linux-strip -o "$OUT/usr/bin/lockfile-create" "$P/lockfile-create"
for n in lockfile-check lockfile-remove lockfile-touch mail-lock mail-unlock mail-touchlock; do
    echo /usr/bin/lockfile-create > "$OUT/usr/bin/$n.romlink"	# a hard link in the ROM (tools/make_linux_image.py)
done
cp "$L/COPYRIGHT" "$OUT/usr/share/liblockfile/COPYRIGHT"
cp "$P/COPYING" "$OUT/usr/share/lockfile-progs/COPYING"
ls -l "$OUT/usr/bin/dotlockfile" "$OUT/usr/bin/lockfile-create"
