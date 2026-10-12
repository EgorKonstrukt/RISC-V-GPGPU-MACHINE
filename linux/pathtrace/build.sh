#!/usr/bin/env bash
# Builds nxpath, the path tracer whose tiles are traced by the worker cores and whose panel is
# Dear ImGui (docs/raytrace.md), for the image.
#   wsl -- bash /mnt/c/.../linux/pathtrace/build.sh        (after linux/imgui/build.sh)
# In the guest: nano-X -p & nxpath
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
IMGUI=$WORK/src/imgui
LIB=$WORK/src/imgui-build        # Dear ImGui, its backend, the OpenGL library with floats: as linux/imgui built them
BUILD=$WORK/src/pathtrace-build
OUT=$REPO/build/images/linux/root
[ -f "$LIB/imgui.o" ] && [ -f "$LIB/gles.o" ] || { echo "run linux/imgui/build.sh first"; exit 1; }
mkdir -p "$BUILD" "$OUT/usr/bin" "$OUT/usr/share"
# the models (docs/raytrace.md, "Meshes"): the Utah teapot as Stanford's course has it, and the
# Stanford rabbit at its third size; fetched once, and made triangles and a tree of boxes
DATA=$WORK/src/pathtrace-data
mkdir -p "$DATA"
[ -f "$DATA/teapot.obj" ] || curl -sSfL -o "$DATA/teapot.obj" https://graphics.stanford.edu/courses/cs148-10-summer/as3/code/as3/teapot.obj
if [ ! -f "$DATA/bunny.ply" ]; then
    curl -sSfL -o "$DATA/bunny.tar.gz" http://graphics.stanford.edu/pub/3Dscanrep/bunny.tar.gz
    tar -xzf "$DATA/bunny.tar.gz" -C "$DATA" bunny/reconstruction/bun_zipper_res3.ply
    mv "$DATA/bunny/reconstruction/bun_zipper_res3.ply" "$DATA/bunny.ply"
fi
if [ ! -f "$BUILD/meshes.c" ] || [ "$HERE/make_meshes.py" -nt "$BUILD/meshes.c" ]; then
    python3 "$HERE/make_meshes.py" "$DATA/teapot.obj" "$DATA/bunny.ply" "$BUILD/meshes.c"
fi
INC="-I$IMGUI -I$REPO/linux/imgui -I$HERE -I$MW/src/include -I$REPO/programs/mc -idirafter $REPO/programs/linux/include"
# single-precision constants (the machine has no doubles), and its own square root; nothing
# that changes what a float comes to (the sums are the test)
TUNE="-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops"
rv32-cc $TUNE -fno-pie -Wall -fsingle-precision-constant -fno-math-errno $INC -c "$HERE/tracer.c" -o "$BUILD/tracer.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/mc/mcw.c" -o "$BUILD/mcw.o"
rv32-cc -O2 -fno-pie -w $INC -c "$BUILD/meshes.c" -o "$BUILD/meshes.o"
rv32-c++ -O2 -DNDEBUG -fno-pie -fno-math-errno -fno-strict-aliasing -w -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS \
    $INC -c "$HERE/nxpath.cpp" -o "$BUILD/nxpath.o"
# (Dear ImGui's objects are to be optimised at the link, as linux/imgui/build.sh links them)
rv32-c++ $TUNE --param=max-inline-insns-auto=60 --param=inline-unit-growth=80 -flto=auto -w "$BUILD/nxpath.o" "$BUILD/tracer.o" "$BUILD/meshes.o" "$BUILD/mcw.o" "$LIB/imgui.o" "$LIB/imgui_draw.o" "$LIB/imgui_tables.o" "$LIB/imgui_widgets.o" \
    "$LIB/imgui_impl_shaderemu.o" "$LIB/cxxrt.o" "$LIB/gles.o" "$MW/src/lib/libnano-X.a" -o "$BUILD/nxpath"
riscv32-linux-strip -o "$OUT/usr/bin/nxpath" "$BUILD/nxpath"
echo "Other/Path tracer (every core)=nxpath" > "$OUT/usr/share/nxapps.83-pathtrace"
ls -l "$OUT/usr/bin/nxpath"
