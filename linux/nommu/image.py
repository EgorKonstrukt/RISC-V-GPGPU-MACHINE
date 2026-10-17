"""build/images/NAME (default nommu): the shared image with the flat root (root.sh) as its overlay, the
no-MMU kernel (kernel.sh), and RAM in the device tree from the kernel's address on. Without a build
in build/nommu, linux/prebuilt/nommu: its kernel and the files that differ from linux/prebuilt/root.
   python linux/nommu/image.py [NAME [ROOT [KERNEL]]] [--save-prebuilt]"""
import filecmp, os, shutil, struct, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PREBUILT = os.path.join(ROOT, 'linux', 'prebuilt', 'nommu')
save = '--save-prebuilt' in sys.argv
argv = [a for a in sys.argv[1:] if a != '--save-prebuilt']
built = os.path.join(ROOT, 'build', 'nommu')
own = os.path.exists(os.path.join(built, 'Image')) and os.path.isdir(os.path.join(built, 'flatroot'))
name = argv[0] if len(argv) > 0 else 'nommu'
flat = argv[1] if len(argv) > 1 else os.path.join(built, 'flatroot') if own else os.path.join(PREBUILT, 'root')
kernel = argv[2] if len(argv) > 2 else os.path.join(built, 'Image') if own else os.path.join(PREBUILT, 'Image')
if save and os.path.abspath(flat).startswith(PREBUILT):
    raise SystemExit('--save-prebuilt saves a build (build/nommu): there is none')
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
print(name, 'made from', os.path.relpath(flat, ROOT), 'and', os.path.relpath(kernel, ROOT),
      '; memory node 0x80400000 + 0x05c00000')

if save:
    # what the overlay needs over linux/prebuilt/root: save the MMU build there first
    base = os.path.join(ROOT, 'linux', 'prebuilt', 'root')
    shutil.rmtree(PREBUILT, ignore_errors=True)
    os.makedirs(PREBUILT)
    shutil.copyfile(kernel, os.path.join(PREBUILT, 'Image'))
    kept = 0
    for folder, _, names in os.walk(flat):
        for n in names:
            p = os.path.join(folder, n)
            rel = os.path.relpath(p, flat)
            q = os.path.join(base, rel)
            if os.path.exists(q) and filecmp.cmp(p, q, shallow=False):
                continue
            os.makedirs(os.path.dirname(os.path.join(PREBUILT, 'root', rel)), exist_ok=True)
            shutil.copy2(p, os.path.join(PREBUILT, 'root', rel))
            kept += 1
    print('linux/prebuilt/nommu now holds this kernel and the', kept, 'files that differ from linux/prebuilt/root')
