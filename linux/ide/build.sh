#!/usr/bin/env bash
# Builds nxide, the place to write C in (docs/ide.md), into build/images/linux/root. After
# linux/nanox/build.sh; what it compiles with in the guest is linux/tcc/build.sh's.
#   wsl -- bash /mnt/c/.../linux/ide/build.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
OUT=$REPO/build/images/linux/root/usr
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
mkdir -p "$OUT/bin" "$OUT/share" "$WORK/src/ide"
rv32-cc -O2 -Wall -Wno-unused-function -I"$MW/src/include" -I"$HERE/../apps" -I"$HERE/../userland" "$HERE/nxide.c" \
    "$MW/src/lib/libnano-X.a" -o "$WORK/src/ide/nxide"
riscv32-linux-strip -o "$OUT/bin/nxide" "$WORK/src/ide/nxide"
riscv32-linux-nm -n "$WORK/src/ide/nxide" > "$WORK/src/nxide.nm"
# the Start menu, and whose a C file is (docs/open.md: before the editor's line for text)
printf '%s\n' "Programming/IDE=nxide" > "$OUT/share/nxapps.28-ide"
printf '%s\n' "source   c,h   -   nxide %s" > "$OUT/share/nxopen.15-ide"
ls -l "$OUT/bin/nxide"
