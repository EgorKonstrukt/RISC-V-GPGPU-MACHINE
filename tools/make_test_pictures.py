"""Makes the image's two test pictures.

    python tools/make_test_pictures.py            the pictures, into linux/apps/pictures

The pictures are cut from the desktop's own picture (linux/prebuilt's wallpaper): a PNG of
8-bit colours and a baseline JPEG with its colours at half size, the common kinds. The JPEG is
in restart intervals, as the image's own pictures are (docs/media.md). The decoder that reads
them is libmedia's, checked against Pillow by linux/libmedia/test/check_pictures.py.
"""
import os, sys
from PIL import Image

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, 'linux', 'apps', 'pictures')
SHARE = os.path.join(HERE, 'linux', 'prebuilt', 'root', 'usr', 'share')


def make():
    os.makedirs(OUT, exist_ok=True)
    source = next(os.path.join(SHARE, n) for n in ('wallpaper-fox.jpg', 'wallpaper-fox.ppm') if os.path.exists(os.path.join(SHARE, n)))
    picture = Image.open(source).convert('RGB')
    w, h = picture.size
    side = min(w, h)
    picture = picture.crop(((w - side * 4 // 3) // 2 if side * 4 // 3 <= w else 0, 0, (w + side * 4 // 3) // 2 if side * 4 // 3 <= w else w, side))
    picture = picture.resize((640, 480), Image.LANCZOS)
    picture.save(os.path.join(OUT, 'picture-fox.png'), optimize=True)
    picture.save(os.path.join(OUT, 'picture-fox.jpg'), quality=85, subsampling='4:2:0', progressive=False, restart_marker_blocks=3)
    for name in ('picture-fox.png', 'picture-fox.jpg'):
        print(name, os.path.getsize(os.path.join(OUT, name)), 'bytes')


if __name__ == '__main__':
    if '--check' in sys.argv:
        sys.exit('the decoder is libmedia\'s now: python linux/libmedia/test/check_pictures.py MEDIA_TEST --wsl')
    make()
