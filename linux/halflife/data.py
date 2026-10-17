"""Uplink's data into an image's overlay, where the engine is (linux/nommu/image.py calls it): the
installer fetched and unpacked (data.sh), pak0 made smaller (repack.py), the files the game reads.
Valve's data goes only into build/: never into linux/prebuilt or the repository.
   python linux/halflife/data.py OVERLAY"""
import os, shutil, struct, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
B = os.path.join(ROOT, 'build', 'halflife')
FILES = ('decals.wad', 'gfx.wad', 'cached.wad', 'liblist.gam', 'default.cfg', 'skill.cfg', 'settings.scr', 'Titles.txt')


def wsl(path):
    return '/mnt/' + path[0].lower() + path[2:].replace('\\', '/')


G = os.path.join(sys.argv[1], 'usr', 'share', 'halflife', 'valve')
subprocess.run(['wsl', '--', 'bash', wsl(os.path.join(ROOT, 'linux', 'halflife', 'data.sh'))], check=True)
V = os.path.join(B, 'uplink', 'valve')
pak = os.path.join(V, 'pak0.PAK')
subprocess.run([sys.executable, os.path.join(ROOT, 'linux', 'halflife', 'repack.py'), pak, G,
                os.path.join(B, 'repack-cache'), wsl(os.path.join(B, 'adpcm_encode'))], check=True)
for n in FILES:
    shutil.copy(os.path.join(V, n), G)
shutil.copy(os.path.join(B, 'uplink', 'delta.lst'), G)
# the menu's pictures pak0 has itself come from pak0
with open(pak, 'rb') as f:
    _, at, size = struct.unpack('<4sii', f.read(12))
    f.seek(at)
    have = {os.path.splitext(f.read(64).split(b'\0')[0].decode().lower())[0] for _ in range(size // 64)}
shell = os.path.join(G, 'extras.pk3dir', 'gfx', 'shell')
for n in os.listdir(shell) if os.path.isdir(shell) else []:
    if 'gfx/shell/' + os.path.splitext(n.lower())[0] in have:
        os.remove(os.path.join(shell, n))
print('Uplink in', os.path.relpath(G, ROOT))
