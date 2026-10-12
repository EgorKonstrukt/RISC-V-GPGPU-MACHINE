"""An image folder that runs programs\\bin\\lazy.bin in place of the kernel (programs\\lazy\\lazy.c).

    python tools\\make_lazy_image.py OUT_ROOT

Writes OUT_ROOT\\build\\images\\linux\\{linux_payload.bin, rootfs.bin, dts.bin}: start rvc_cpu
with --image OUT_ROOT\\build\\images\\linux, or rvc_harness with OUT_ROOT as the working
directory and --image linux-net. The ROM is sixteen pages of a pattern, which the program copies from.
"""
import os, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.join(sys.argv[1], 'build', 'images', 'linux')
os.makedirs(out, exist_ok=True)
program = open(os.path.join(root, 'programs', 'bin', 'lazy.bin'), 'rb').read()
tree = open(os.path.join(root, 'build', 'images', 'linux', 'dts.bin'), 'rb').read()
at, tree_at = 0x400000, 0x2200000
open(os.path.join(out, 'linux_payload.bin'), 'wb').write(bytes(at) + program + bytes(tree_at - at - len(program)) + tree)
# (a pattern after the name: the program copies pages of it)
open(os.path.join(out, 'rootfs.bin'), 'wb').write(b'-rom1fs-' + bytes((i * 197 + (i >> 8) * 31 + 7) & 255 for i in range(8, 65536)))
open(os.path.join(out, 'dts.bin'), 'wb').write(tree)
print('lazy image in', out, '(%d bytes of program)' % len(program))
