#!/usr/bin/env bash
# Builds nxplay, the audio player (docs/play.md), and puts it and its test files in the image.
#   wsl -- bash /mnt/c/.../linux/play/build.sh        (after linux/nanox/build.sh and linux/libmedia/build.sh)
# In the guest: nxplay /usr/share/play-test.mp3
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
OUT=$REPO/build/images/linux/root
BUILD=$WORK/src/play
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
LIB=$WORK/src/libmedia
[ -f "$LIB/libmedia.a" ] || { echo "run linux/libmedia/build.sh first"; exit 1; }
mkdir -p "$BUILD" "$OUT/usr/bin" "$OUT/usr/share"

# The decoders are libmedia's (docs/media.md); what is left here is the player.
INC="-I$REPO/linux/libmedia -I$REPO/linux/apps -I$MW/src/include -I$REPO/linux/userland -I$REPO/programs/mc"
# (-O3 with loops unrolled: the ring's copies and the window a little fewer instructions)
DECODE="-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops"
rv32-cc $DECODE -fno-pie -Wall -Wno-unused-function $INC -c "$HERE/nxplay.c" -o "$BUILD/nxplay.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/mc/mcw.c" -o "$BUILD/mcw.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/mc/mcwx.c" -o "$BUILD/mcwx.o"
rv32-cc -O2 -no-pie -Wl,--gc-sections "$BUILD/nxplay.o" "$LIB/libmedia.a" "$BUILD/mcw.o" "$BUILD/mcwx.o" \
    "$MW/src/lib/libnano-X.a" -lm -o "$BUILD/nxplay"
riscv32-linux-nm -n "$BUILD/nxplay" > "$WORK/src/nxplay.nm"
riscv32-linux-strip -o "$OUT/usr/bin/nxplay" "$BUILD/nxplay"
# its test files (make_test.py made them); /usr/share, since the image builder makes no folders
cp "$HERE"/test/play-test.* "$OUT/usr/share/"
echo "Other/Music player=nxplay" > "$OUT/usr/share/nxapps.84-play"
# what `open` starts for a sound file (linux/apps/nxopen.conf has the columns)
printf '%s\n' 'sound wav,mp3,flac,mod,ogg ID3,fLaC,OggS,\xff\xfb nxplay %s' > "$OUT/usr/share/nxopen.84-play"
ls -l "$OUT/usr/bin/nxplay"
