#!/usr/bin/env bash
# Builds MicroPython's Unix port (MIT) as the interpreter micropython into build/images/linux/root
# (docs/system/scripts.md), in place of upstream's dynamic ELF program, which does not run on the
# machine without an MMU. Its path leaves out the ROM's /usr/lib/micropython; asyncio is frozen in.
#   wsl -- bash /mnt/c/.../linux/micropython/build.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
M=$WORK/src/micropython
OUT=${MICROPYTHON_OUT:-$REPO/build/images/linux/root}	# (MICROPYTHON_OUT: a private image's overlay)
COMMIT=0fd6c573ea815774668bbb16b8e197c8822368b2	# v1.29.0
if [ ! -d "$M/.git" ]; then
    echo "== MicroPython $COMMIT"
    git clone -q https://github.com/micropython/micropython.git "$M"
fi
[ "$(git -C "$M" rev-parse HEAD)" = $COMMIT ] || { git -C "$M" checkout -q -f "$COMMIT" && git -C "$M" clean -q -fdx; }
# (the manifest's tools want micropython-lib's tree, at the commit MicroPython's names)
[ -f "$M/lib/micropython-lib/README.md" ] || git -C "$M" submodule update -q --init lib/micropython-lib
# mpy-cross for this computer first: made from the port's make it would get the guest's compiler
make -s -C "$M/mpy-cross" -j"$(nproc)" > /dev/null
# Two changes to its source, each made once: sys.settrace for the IDE's debugger (linux/ide/pydbg.py)
# makes no frame object for a call while no trace is set, or every call pays for one (40 times
# slower; 2.4% with this); random's seed does not wait for the kernel's pool, which never fills here.
python3 - "$M" <<'PY'
import sys
FRAME = '    mp_obj_frame_t *frame = MP_OBJ_TO_PTR(mp_obj_new_frame(code_state));'
SEED = '    int fd = open("/dev/%s", O_RDONLY);'   # (musl's build reads the device, not getrandom)
for f, old, new in (('py/profile.c', FRAME, '    if (!prof_trace_cb) {\n        return MP_OBJ_NULL;\n    }\n' + FRAME),
                    ('ports/unix/unix_mphal.c', SEED % 'random', SEED % 'urandom')):
    p = sys.argv[1] + '/' + f; s = open(p).read()
    if new not in s:
        assert s.count(old) == 1, (f, old)
        open(p, 'w').write(s.replace(old, new))
PY
EXTRA="-DMICROPY_EMIT_RV32=0 -DMICROPY_EMIT_INLINE_RV32=0 -DMICROPY_PY_SYS_SETTRACE=1"
# Not the ROM's /usr/lib/micropython: that library of 2017 wants ffi and comes before the built-in
# modules it names (import time, os, re, json, asyncio failed).
EXTRA="$EXTRA -DMICROPY_PY_SYS_PATH_DEFAULT=\\\".frozen:~/.micropython/lib\\\""
# (make does not see flags change: a build made with others is made anew)
S=$M/ports/unix/build-shaderemu/.flags
[ "$(cat "$S" 2>/dev/null)" = "$EXTRA" ] || rm -rf "$M/ports/unix/build-shaderemu"
# Without what wants threads, libffi, TLS, a submodule or native code: the native emitters write
# compressed instructions, which the machine does not have. asyncio is frozen in.
make -s -C "$M/ports/unix" -j"$(nproc)" VARIANT=standard BUILD=build-shaderemu \
    CC=rv32-cc STRIP=riscv32-linux-strip SIZE=riscv32-linux-size CWARN="-Wall -Wno-unused-parameter" \
    COPT="-O2 -DNDEBUG -fdata-sections -ffunction-sections" \
    CFLAGS_EXTRA="$EXTRA" \
    MICROPY_PY_BTREE=0 MICROPY_PY_THREAD=0 MICROPY_PY_FFI=0 MICROPY_PY_SSL=0 MICROPY_SSL_MBEDTLS=0 \
    MICROPY_VFS_FAT=0 MICROPY_VFS_LFS1=0 MICROPY_VFS_LFS2=0 \
    FROZEN_MANIFEST="$HERE/manifest.py" GIT_SUBMODULES= 2>&1 | grep -v "^GEN\|^MPY\|^CC\|^LINK\|^QSTR\|^mkdir" || true
B=$M/ports/unix/build-shaderemu/micropython
[ -f "$B" ] || { echo "micropython was not built"; exit 1; }
echo "$EXTRA" > "$S"
mkdir -p "$OUT/usr/bin" "$OUT/usr/share/micropython"
riscv32-linux-strip -o "$OUT/usr/bin/micropython" "$B"
cp "$M/LICENSE" "$OUT/usr/share/micropython/LICENSE"
ls -l "$OUT/usr/bin/micropython"
