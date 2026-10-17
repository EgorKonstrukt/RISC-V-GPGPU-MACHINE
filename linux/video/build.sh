#!/usr/bin/env bash
# Builds nxvideo, the video player on the worker cores (docs/apps/video.md), for the image. Its
# decoders are libmedia's (docs/system/media.md, "Films"), whose media_test-host gives the sums a
# guest's must equal.
#   wsl -- bash /mnt/c/.../linux/video/build.sh   (after linux/nanox/build.sh and linux/libmedia/build.sh)
# In the guest: nano-X -p & nxvideo
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
OUT=$REPO/build/images/linux/root
BUILD=$WORK/src/video
MEDIA=$WORK/src/libmedia/libmedia.a
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
[ -f "$MEDIA" ] || { echo "run linux/libmedia/build.sh first"; exit 1; }
mkdir -p "$BUILD" "$OUT/usr/bin" "$OUT/usr/share" "$OUT/root/Videos"

INC="-I$REPO/linux/libmedia -I$REPO/linux/apps -idirafter $REPO/programs/linux/include -I$MW/src/include -I$REPO/linux/userland -I$REPO/programs/mc"
rv32-cc -O2 -fno-pie -Wall -Wno-unused-function $INC -c "$HERE/nxvideo.c" -o "$BUILD/nxvideo.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/linux/gles.c" -o "$BUILD/gles.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/mc/mcw.c" -o "$BUILD/mcw.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/mc/mcwx.c" -o "$BUILD/mcwx.o"
rv32-cc -O2 -no-pie "$BUILD/nxvideo.o" "$MEDIA" "$BUILD/gles.o" "$BUILD/mcw.o" "$BUILD/mcwx.o" "$MW/src/lib/libnano-X.a" -lm -o "$BUILD/nxvideo"
riscv32-linux-nm -n "$BUILD/nxvideo" > "$WORK/src/nxvideo.nm"
riscv32-linux-strip -o "$OUT/usr/bin/nxvideo" "$BUILD/nxvideo"

# (the sample in /root/Videos is tools/make_video.py's, run on the host)
echo "Other/Video player=nxvideo" > "$OUT/usr/share/nxapps.86-video"
# what `open` starts for a film (linux/apps/nxopen.conf has the columns; a GIF is a picture's, by its first bytes)
printf '%s\n' 'video mpg,mpeg,vqa,avi RIFF\x..\x..\x..\x..AVI\x20,FORM\x..\x..\x..\x..WVQA,\x00\x00\x01\xba,\x00\x00\x01\xb3 nxvideo %s' > "$OUT/usr/share/nxopen.86-video"
ls -l "$OUT/usr/bin/nxvideo"
