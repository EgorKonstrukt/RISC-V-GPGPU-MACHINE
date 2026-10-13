#!/usr/bin/env bash
# Builds nxvideo, the video player on the worker cores (docs/video.md), and its sample, for
# the image.   wsl -- bash /mnt/c/.../linux/video/build.sh        (after linux/nanox/build.sh)
# In the guest: nano-X -p & nxvideo
# Also builds the same decoder for this machine (nxvideo-host --decode FILE), whose sums a
# guest's must equal.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
OUT=$REPO/build/images/linux/root
BUILD=$WORK/src/video
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
mkdir -p "$BUILD/fetch" "$OUT/usr/bin" "$OUT/usr/share" "$OUT/root/Videos"

# pl_mpeg is one header (Dominic Szablewski, MIT), fetched at one commit and not kept here
[ -f "$BUILD/fetch/pl_mpeg.h" ] || curl -sSfL \
    https://raw.githubusercontent.com/phoboslab/pl_mpeg/c871f2be022ece7ef4f64230b4fb8e1fb9eb6023/pl_mpeg.h -o "$BUILD/fetch/pl_mpeg.h"
echo "3a8cb30c83c2a1147719c30fe0c8b93da2987aa43140077c575b39aaa75fc2c9  $BUILD/fetch/pl_mpeg.h" | sha256sum -c --quiet -
# One change: it moves a stream's bytes down as it reads them, and a group's bytes here are
# part of the whole stream, which other cores are reading. It does so only for a buffer that asks.
sed 's/^\t\tplm_buffer_discard_read_bytes(self->buffer);$/\t\tif (self->buffer->discard_read_bytes) plm_buffer_discard_read_bytes(self->buffer);/' \
    "$BUILD/fetch/pl_mpeg.h" > "$BUILD/pl_mpeg.h"
[ "$(grep -c 'if (self->buffer->discard_read_bytes) plm_buffer_discard_read_bytes' "$BUILD/pl_mpeg.h")" = 1 ] || { echo "pl_mpeg.h: the line to change is not there"; exit 1; }
# and the sound's window four blocks a turn (the same additions in the same order: the same sums)
python3 "$HERE/plm_window.py" "$BUILD/pl_mpeg.h"
# and bits and codes from a word of the stream, the transform only where a block has
# coefficients (docs/video.md): a picture 1,364 thousand instructions to 966, the same sums
python3 "$HERE/plm_fast.py" "$BUILD/pl_mpeg.h"

# single-precision constants: the sound's filter is floats, and the machine has no doubles
FLOAT="-fsingle-precision-constant -fno-math-errno"
INC="-I$BUILD -I$HERE -I$REPO/linux/apps -idirafter $REPO/programs/linux/include -I$MW/src/include -I$REPO/linux/userland -I$REPO/programs/mc"
rv32-cc -O2 -fno-pie -Wall -Wno-unused-function $INC -c "$HERE/nxvideo.c" -o "$BUILD/nxvideo.o"
# (the decoder at -O3 with its loops unrolled: 1,388 thousand instructions a picture where -O2 took 1,792, the same sums)
rv32-cc -O3 -fno-tree-vectorize -fsched-pressure -funroll-loops -fno-pie -fno-common -fdata-sections -w $FLOAT $INC -c "$HERE/mpeg.c" -o "$BUILD/mpeg.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/linux/gles.c" -o "$BUILD/gles.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/mc/mcw.c" -o "$BUILD/mcw.o"
rv32-cc -O2 -no-pie "$BUILD/nxvideo.o" "$BUILD/mpeg.o" "$BUILD/gles.o" "$BUILD/mcw.o" "$MW/src/lib/libnano-X.a" -lm -o "$BUILD/nxvideo"
riscv32-linux-nm -n "$BUILD/nxvideo" > "$WORK/src/nxvideo.nm"
riscv32-linux-strip -o "$OUT/usr/bin/nxvideo" "$BUILD/nxvideo"
gcc -O2 -w -DMPEG_HOST -I"$BUILD" -I"$HERE" "$HERE/nxvideo.c" "$HERE/mpeg.c" -lm -o "$BUILD/nxvideo-host"

# (the sample in /root/Videos is tools/make_video.py's, run on the host)
echo "Other/Video player=nxvideo" > "$OUT/usr/share/nxapps.86-video"
# what `open` starts for a video file (linux/apps/nxopen.conf has the columns)
printf '%s\n' 'video mpg,mpeg - nxvideo %s' > "$OUT/usr/share/nxopen.86-video"
ls -l "$OUT/usr/bin/nxvideo"
