"""build/images/NAME (default nommu): the shared image with the flat root (root.sh) as its overlay, the
no-MMU kernel (kernel.sh), and RAM in the device tree from the kernel's address on.
   python linux/nommu/image.py [NAME [ROOT [KERNEL]]]"""
import os, shutil, struct, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
name = sys.argv[1] if len(sys.argv) > 1 else 'nommu'
flat = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'build', 'nommu', 'flatroot')
kernel = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, 'build', 'nommu', 'Image')
D = os.path.join(ROOT, 'build', 'images', name)
if os.path.isdir(os.path.join(D, 'overlay')):
    shutil.rmtree(os.path.join(D, 'overlay'))
os.makedirs(D, exist_ok=True)
shutil.copytree(flat, os.path.join(D, 'overlay'), symlinks=True)
shutil.copy(kernel, os.path.join(D, 'Image'))
r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'private_image.py'), name], cwd=ROOT, capture_output=True, text=True)
print(r.stdout[-600:], r.stderr[-600:])
# (the kernel lies at 0x80400000: without an MMU, memory below it is no RAM the kernel may hand out)
old = struct.pack('>IIII', 0, 0x80000000, 0, 0x06000000)
new = struct.pack('>IIII', 0, 0x80400000, 0, 0x05c00000)
for f in ('dts.bin', 'linux_payload.bin'):
    p = os.path.join(D, f)
    b = bytearray(open(p, 'rb').read())
    assert b.count(old) == 1, (f, b.count(old))
    i = b.index(old); b[i:i + 16] = new
    open(p, 'wb').write(b)
print(name, 'made; memory node 0x80400000 + 0x05c00000')
