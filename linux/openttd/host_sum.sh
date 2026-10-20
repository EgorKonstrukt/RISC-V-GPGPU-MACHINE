#!/usr/bin/env bash
# The sum test's reference (docs/games/openttd.md, "Is it the same game"): the tree build.sh left,
# built for this computer without a window, run with test.cfg for TICKS ticks of seed SEED.
#   wsl -- bash /mnt/c/.../linux/openttd/host_sum.sh [SEED,TICKS ...]   (default 1,500; after build.sh)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HOME/nm/work}
SRC=$WORK/src/openttd
NATIVE=$WORK/src/openttd-native
[ -f "$SRC/src/video/shaderemu_v.cpp" ] || { echo "run linux/openttd/build.sh first"; exit 1; }
cmake -S "$SRC" -B "$NATIVE" -DCMAKE_BUILD_TYPE=Release -DOPTION_DEDICATED=ON > "$NATIVE.log" 2>&1
cmake --build "$NATIVE" -j"$(nproc)" --target openttd >> "$NATIVE.log" 2>&1 || { tail -20 "$NATIVE.log"; exit 1; }
python3 -m zipfile -e "$WORK/src/opengfx-8.0-all.zip" "$NATIVE/baseset"
for run in ${*:-1,500}; do
    cp "$HERE/test.cfg" /tmp/openttd-sum.cfg
    (cd "$NATIVE" && OPENTTD_SUM=1 ./openttd -v null:ticks=${run#*,} -s null -m null -g -G ${run%,*} -c /tmp/openttd-sum.cfg 2>/dev/null |
        grep '^openttd: ')
done
