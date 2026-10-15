"""Makes the desktop's pictures for the Linux image, as JPEG files named wallpaper-NAME.jpg under
build/images/linux/root/usr/share (tools/make_linux_image.py then puts them in the image). The
desktop decodes them on the worker cores when it starts (docs/media.md): each is written in
restart intervals, some 512 of them, which every core decodes by itself, and beside each are its
twins cut to the common screens, wallpaper-NAME@800x600.jpg (the machine's own screen),
@1280x720 and @1920x1080, decoded as they are.

    python tools/make_wallpaper.py [FOLDER ...]
    python tools/make_wallpaper.py --from-ppm     (the PPM pictures of an older build as JPEG)
    python tools/make_wallpaper.py --twins        (the twins missing, from the pictures there)

- Every line of linux/apps/wallpapers.txt is a photograph on Unsplash: a name and the photo's
  id (the end of its address). They are fetched here and not kept in this repository; the
  Unsplash licence allows using, changing and passing them on.
- Every picture in a FOLDER given on the command line is added too, named after its file.

A picture 1920 or more across is brought down to fit 1920x1080, one 1280 or more to fit
1280x720, and smaller ones are left as they are. The desktop crops and scales what it shows
to the screen (linux/apps/ui.h). Needs Pillow and, for the photographs, curl and the network.
"""
import io, os, re, subprocess, sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'images', 'linux', 'root', 'usr', 'share')
FETCH = os.path.join(ROOT, 'build', 'fetch')
LIST = os.path.join(ROOT, 'linux', 'apps', 'wallpapers.txt')
AGENT = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36'


def fetch(photo):
    """The photograph with that Unsplash id, 2400 across at most, from build/fetch if it is there."""
    cached = os.path.join(FETCH, 'unsplash-%s.jpg' % photo)
    if not os.path.exists(cached):
        os.makedirs(FETCH, exist_ok=True)
        # the site answers a browser's request and refuses a bare one (and Python's): curl
        address = 'https://unsplash.com/photos/%s/download?force=true&w=2400' % photo
        data = subprocess.run(['curl', '-sL', '-m', '90', '-A', AGENT, '-H', 'Accept: image/avif,image/webp,image/*,*/*', address],
                              capture_output=True, check=True).stdout
        Image.open(io.BytesIO(data)).verify()   # a picture, not an error page
        open(cached, 'wb').write(data)
    return Image.open(cached)


SCREENS = ((800, 600), (1280, 720), (1920, 1080))   # twins for these (the machine's, the world's, the largest)
QUALITY = 88


def save_jpeg(picture, path):
    """A baseline JPEG in restart intervals of about a 512th of the picture (colours at half size)."""
    mcus = -(-picture.size[0] // 16) * -(-picture.size[1] // 16)
    picture.save(path, 'JPEG', quality=QUALITY, subsampling=2, restart_marker_blocks=max(1, -(-mcus // 512)))


def covered(picture, w, h):
    """The picture scaled to cover w x h and its middle kept, as the desktop shows it on such a screen."""
    pw, ph = picture.size
    if pw * h > ph * w:
        cw, ch = ph * w // h, ph
    else:
        cw, ch = pw, pw * h // w
    left, top = (pw - cw) // 2, (ph - ch) // 2
    return picture.crop((left, top, left + cw, top + ch)).resize((w, h), Image.LANCZOS)


def twin(name, picture, sw, sh):
    """Its twin for a screen of sw x sh, where the desktop would cover it: a shape within a third of its."""
    pw, ph = picture.size
    if pw * sh * 10 <= ph * sw * 13 and ph * sw * 10 <= pw * sh * 13 and pw * 2 >= sw:
        save_jpeg(covered(picture, sw, sh), os.path.join(OUT, 'wallpaper-%s@%dx%d.jpg' % (name, sw, sh)))
        return True
    return False


def keep(name, picture, shrink=True):
    if getattr(picture, 'is_animated', False):
        picture.seek(0)   # an animation's first frame
    picture = picture.convert('RGB')
    w, h = picture.size
    box = (1920, 1080) if w >= 1920 else (1280, 720) if w >= 1280 else None
    if box and shrink:
        picture.thumbnail(box, Image.LANCZOS)
    save_jpeg(picture, os.path.join(OUT, 'wallpaper-%s.jpg' % name))
    made = []
    pw, ph = picture.size
    for sw, sh in SCREENS:
        if twin(name, picture, sw, sh):
            made.append('%dx%d' % (sw, sh))
    print('wallpaper-%s.jpg: %dx%d from %dx%d, %d KB; for %s' % (name, pw, ph, w, h,
          os.path.getsize(os.path.join(OUT, 'wallpaper-%s.jpg' % name)) // 1024, ', '.join(made) or 'no screen'))


def from_ppm():
    """The PPM pictures an older build made (beside OUT, or in linux/prebuilt) as JPEG, names kept."""
    for folder in (OUT, os.path.join(ROOT, 'linux', 'prebuilt', 'root', 'usr', 'share')):
        for name in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
            m = re.fullmatch(r'wallpaper-(.+)\.ppm', name)
            if m and not os.path.exists(os.path.join(OUT, 'wallpaper-%s.jpg' % m.group(1))):
                keep(m.group(1), Image.open(os.path.join(folder, name)), shrink=False)   # (sized already)
    for name in os.listdir(OUT):
        if re.fullmatch(r'wallpaper-.+\.ppm', name):
            os.remove(os.path.join(OUT, name))


def twins():
    """The twins of SCREENS that a picture there has not, made from the picture."""
    for name in sorted(os.listdir(OUT)):
        m = re.fullmatch(r'wallpaper-([^@]+)\.jpg', name)
        if not m:
            continue
        picture = Image.open(os.path.join(OUT, name)).convert('RGB')
        for sw, sh in SCREENS:
            if not os.path.exists(os.path.join(OUT, 'wallpaper-%s@%dx%d.jpg' % (m.group(1), sw, sh))) and                     twin(m.group(1), picture, sw, sh):
                print('wallpaper-%s@%dx%d.jpg made' % (m.group(1), sw, sh))


def main():
    os.makedirs(OUT, exist_ok=True)
    if sys.argv[1:] == ['--from-ppm']:
        return from_ppm()
    if sys.argv[1:] == ['--twins']:
        return twins()
    for old in os.listdir(OUT):
        if old.startswith('wallpaper-'):
            os.remove(os.path.join(OUT, old))
    for line in open(LIST, encoding='utf-8'):
        words = line.split('#')[0].split()
        if len(words) < 2:
            continue
        try:
            keep(words[0], fetch(words[1]))
        except Exception as error:   # the network, mostly: the image has the other pictures
            print('wallpaper-%s.jpg not made: %s' % (words[0], error), file=sys.stderr)
    for folder in sys.argv[1:]:
        for n, name in enumerate(sorted(os.listdir(folder))):
            try:
                picture = Image.open(os.path.join(folder, name))
            except Exception:
                continue   # not a picture
            # the image's file system takes short plain names
            stem = re.sub(r'[^a-z0-9]+', '-', os.path.splitext(name)[0].lower()).strip('-')[:16].strip('-')
            keep(stem or 'picture%d' % n, picture)


if __name__ == '__main__':
    main()
