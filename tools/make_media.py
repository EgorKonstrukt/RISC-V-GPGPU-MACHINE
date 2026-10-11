"""Makes the pictures and music of the Linux image's /root/Pictures and /root/Music from the
list in linux/media/media.txt, into build/images/linux/root/root (tools/make_linux_image.py
then makes the folders in the ROM and puts them there).

    python tools/make_media.py

Originals come from Wikimedia Commons into build/fetch/media, once, and must have the SHA-1
the list names. Needs Pillow, ffmpeg and, the first time, the network.
"""
import hashlib, io, os, subprocess, sys, urllib.parse, urllib.request
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIST = os.path.join(ROOT, 'linux', 'media', 'media.txt')
OUT = os.path.join(ROOT, 'build', 'images', 'linux', 'root', 'root')
FETCH = os.path.join(ROOT, 'build', 'fetch', 'media')
AGENT = 'ShaderEmu-media-fetch/1.0 (https://github.com/MichaelMoroz/ShaderEmu)'


def original(name, sha1):
    """A file of Wikimedia Commons by its name, from build/fetch/media if it is there."""
    cached = os.path.join(FETCH, name.replace(' ', '_'))
    if not os.path.exists(cached):
        os.makedirs(FETCH, exist_ok=True)
        under = name.replace(' ', '_')
        md5 = hashlib.md5(under.encode()).hexdigest()   # where MediaWiki keeps an upload
        address = 'https://upload.wikimedia.org/wikipedia/commons/%s/%s/%s' % (md5[0], md5[:2], urllib.parse.quote(under))
        data = urllib.request.urlopen(urllib.request.Request(address, headers={'User-Agent': AGENT}), timeout=120).read()
        open(cached, 'wb').write(data)
    data = open(cached, 'rb').read()
    if hashlib.sha1(data).hexdigest() != sha1:
        raise SystemExit('%s is not the file the list names (SHA-1 %s)' % (cached, hashlib.sha1(data).hexdigest()))
    return cached


def picture(source, to):
    image = Image.open(source).convert('RGB')
    image.thumbnail((640, 480), Image.LANCZOS)
    image.save(to, 'JPEG', quality=82, progressive=False, optimize=False, subsampling=2)


def tune(source, to, how):
    options = dict(word.split('=') for word in how.split())
    kind = ['-c:a', 'flac', '-ar', '22050', '-ac', '1', '-sample_fmt', 's16'] if to.endswith('.flac') else \
           ['-c:a', 'libmp3lame', '-b:a', '96k', '-ar', '44100', '-ac', '2', '-write_xing', '0', '-id3v2_version', '0']
    # no tags and no encoder's date: the same bytes from every build
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-ss', options.get('from', '0'), '-t', options.get('seconds', '30'), '-i', source,
                    '-vn', '-map_metadata', '-1', '-fflags', '+bitexact', '-flags:a', '+bitexact'] + kind + [to], check=True)


def main():
    made = []
    for line in open(LIST, encoding='utf-8'):
        fields = [field.strip() for field in line.split('#')[0].split('|')]
        if len(fields) < 2 or not fields[0]:
            continue
        to = os.path.join(OUT, *fields[0].split('/'))
        os.makedirs(os.path.dirname(to), exist_ok=True)
        kind, _, what = fields[1].partition(':')
        if kind == 'link':
            open(to + '.romlink', 'w', newline='\n').write(what + '\n')
            made.append((fields[0], 0))
            continue
        source = original(what, fields[2])
        if fields[0].startswith('Pictures/'):
            picture(source, to)
        else:
            tune(source, to, fields[3])
        made.append((fields[0], os.path.getsize(to)))
    for name, size in made:
        print('%8d  %s' % (size, name))
    print('%8d bytes in all' % sum(size for _, size in made))


if __name__ == '__main__':
    main()
