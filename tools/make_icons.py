"""Makes the desktop's icons and the window captions' colours for the Linux image:

  icons32.ppm, icons16.ppm   sheets of icons, 16 cells a row; a pixel of the key colour
                             (magenta) is not drawn (GR_IMAGE_CELL, docs/system/nanox.md)
  icons.idx                  "name cell" lines: which cell is whose (linux/apps/icons.txt)
  caption-on.ppm, caption-off.ppm
                             256 x 1: a caption's colours from its left end to its right

all in build/images/linux/root/usr/share. The pictures are Chicago95's (GPL-3.0+/MIT,
https://github.com/grassmunk/Chicago95), fetched once into build/fetch at the commit below,
and the pictures in linux/apps/icons (its README says whose) for what that set lacks.

    python tools/make_icons.py
"""
import os, subprocess, sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# (ICONS_OUT: another folder, for an image of one's own: tools/private_image.py)
OUT = os.environ.get('ICONS_OUT') or os.path.join(ROOT, 'build', 'images', 'linux', 'root', 'usr', 'share')
LIST = os.path.join(ROOT, 'linux', 'apps', 'icons.txt')
OURS = os.path.join(ROOT, 'linux', 'apps', 'icons')
FETCH = os.path.join(ROOT, 'build', 'fetch', 'Chicago95')
REPO, COMMIT = 'https://github.com/grassmunk/Chicago95', '5da19b8b1e2a886ebf6628403023d5fac3acc3ee'
FOLDERS = ['apps', 'places', 'mimes', 'devices', 'categories']
KEY = (255, 0, 255)
ACROSS = 16


def fetch():
    """The theme's 16 and 32 pixel icons only: a sparse checkout of one commit."""
    theme = os.path.join(FETCH, 'Icons', 'Chicago95')
    if os.path.isdir(os.path.join(theme, 'apps', '32')):
        return theme
    os.makedirs(os.path.dirname(FETCH), exist_ok=True)
    git = lambda *a, **k: subprocess.run(['git'] + list(a), check=True, **k)
    git('clone', '-q', '--filter=blob:none', '--sparse', '--no-checkout', REPO, FETCH)
    git('sparse-checkout', 'set', *['Icons/Chicago95/%s/%d' % (f, s) for f in FOLDERS for s in (16, 32)], cwd=FETCH)
    git('checkout', '-q', COMMIT, cwd=FETCH)
    return theme


def find(theme, source, size):
    if '/' in source:
        folder, name = source.split('/')
        path = os.path.join(theme, folder, str(size), name + '.png')
    else:
        path = os.path.join(OURS, source + '.png')   # one picture, brought down to the size
    return path if os.path.exists(path) else None


def cell(path, size):
    """The picture as RGB with the key colour where it is clear."""
    picture = Image.open(path).convert('RGBA')
    if picture.size != (size, size):
        picture = picture.resize((size, size), Image.LANCZOS)
    out = Image.new('RGB', (size, size), KEY)
    pixels, to = picture.load(), out.load()
    for y in range(size):
        for x in range(size):
            r, g, b, a = pixels[x, y]
            if a >= 128:
                to[x, y] = (254, 0, 254) if (r, g, b) == KEY else (r, g, b)
    return out


def gradient(left, right):
    strip = Image.new('RGB', (256, 1))
    strip.putdata([tuple((left[k] * (255 - x) + right[k] * x + 127) // 255 for k in range(3)) for x in range(256)])
    return strip


def main():
    theme = fetch()
    rows = []
    for line in open(LIST):
        words = line.split('#')[0].split()
        if not words:
            continue
        for source in words[1:]:
            if find(theme, source, 32) and find(theme, source, 16):
                rows.append((words[0].split(','), source))
                break
        else:
            raise SystemExit('icons.txt: none of %s exists for %s' % (' '.join(words[1:]), words[0]))
    os.makedirs(OUT, exist_ok=True)
    for size in (32, 16):
        sheet = Image.new('RGB', (ACROSS * size, ((len(rows) + ACROSS - 1) // ACROSS) * size), KEY)
        for n, (_, source) in enumerate(rows):
            sheet.paste(cell(find(theme, source, size), size), (n % ACROSS * size, n // ACROSS * size))
        sheet.save(os.path.join(OUT, 'icons%d.ppm' % size))
    with open(os.path.join(OUT, 'icons.idx'), 'w', newline='\n') as index:
        for n, (names, _) in enumerate(rows):
            for name in names:
                index.write('%s %d\n' % (name, n))
    # a window's caption: the navy of old to a light blue, and greys for a window without the keyboard
    gradient((10, 36, 106), (166, 202, 240)).save(os.path.join(OUT, 'caption-on.ppm'))
    gradient((128, 128, 128), (192, 192, 192)).save(os.path.join(OUT, 'caption-off.ppm'))
    print('%d icons: %s' % (len(rows), ' '.join('%s<-%s' % (names[0], source) for names, source in rows)))


if __name__ == '__main__':
    main()
