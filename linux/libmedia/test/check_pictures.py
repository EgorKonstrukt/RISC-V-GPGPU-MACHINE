"""libmedia's pictures against Pillow's (docs/system/media.md): makes a set of pictures of every kind
the library reads, decodes each with media_test (built for this computer, or run under WSL), and
compares. PNG, GIF, BMP, PNM and TGA must be the same pixels; JPEG within what two decoders'
rounding and colour stretching differ by. Then the encoders: written, read back by Pillow.

    python linux/libmedia/test/check_pictures.py MEDIA_TEST [WORKDIR] [--wsl]
"""
import os, struct, subprocess, sys, zlib
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
FOX = os.path.join(HERE, '..', '..', 'apps', 'pictures', 'picture-fox.png')


def to_wsl(p):
    if len(p) > 2 and p[1] == ':':
        return '/mnt/' + p[0].lower() + p[2:].replace('\\', '/')
    return p


def run(cmd, wsl):
    if wsl:
        cmd = ['wsl', '--'] + [to_wsl(c) for c in cmd]
    return subprocess.run(cmd, capture_output=True, text=True).stdout.strip()


def png16(path, im):
    """A 16-bit colour PNG, by hand (Pillow writes none)."""
    raw = bytearray()
    for y in range(im.size[1]):
        raw.append(0)
        for x in range(im.size[0]):
            for v in im.getpixel((x, y)):
                raw += bytes((v, (v * 7) & 255))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', im.size[0], im.size[1], 16, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b''))


def corpus(out):
    fox = Image.open(FOX).convert('RGB')
    fox = fox.resize((333, 221), Image.LANCZOS)   # sizes not a multiple of 8 or 16
    rgba = fox.convert('RGBA')
    rgba.putalpha(Image.linear_gradient('L').resize(fox.size))
    made = []

    def keep(name, im, **kw):
        path = os.path.join(out, name)
        im.save(path, **kw)
        made.append(path)

    keep('rgb.png', fox)
    keep('rgba.png', rgba)
    keep('gray.png', fox.convert('L'))
    keep('gray16.png', fox.convert('I;16'))
    keep('bits1.png', fox.convert('1'))
    keep('pal.png', fox.convert('P', palette=Image.ADAPTIVE, colors=200))
    keep('pal4.png', fox.convert('P', palette=Image.ADAPTIVE, colors=16), bits=4)
    keep('paltrns.png', fox.convert('P', palette=Image.ADAPTIVE, colors=64), transparency=3)
    keep('interlaced.png', fox, interlace=1)
    keep('interlaced_rgba.png', rgba, interlace=1)
    png16(os.path.join(out, 'rgb16.png'), fox)
    made.append(os.path.join(out, 'rgb16.png'))
    keep('base444.jpg', fox, quality=92, subsampling=0)
    keep('base420.jpg', fox, quality=80, subsampling=2)
    keep('base422.jpg', fox, quality=85, subsampling=1)
    keep('gray.jpg', fox.convert('L'), quality=85)
    keep('progressive.jpg', fox, quality=85, progressive=True)
    keep('progressive444.jpg', fox, quality=90, progressive=True, subsampling=0)
    keep('restart.jpg', fox, quality=85, restart_marker_rows=1)
    keep('restart_blocks.jpg', fox, quality=85, restart_marker_blocks=5)
    keep('cmyk.jpg', fox.convert('CMYK'), quality=90)
    keep('still.gif', fox.convert('P', palette=Image.ADAPTIVE))
    keep('24.bmp', fox)
    keep('8.bmp', fox.convert('P', palette=Image.ADAPTIVE))
    keep('1.bmp', fox.convert('1'))
    keep('32.bmp', rgba)
    keep('p6.ppm', fox)
    keep('p5.pgm', fox.convert('L'))
    keep('p4.pbm', fox.convert('1'))
    keep('rgb.tga', fox)
    keep('rle.tga', fox, compression='tga_rle')
    keep('rgba.tga', rgba)
    keep('gray.tga', fox.convert('L'))
    # an animation that moves a box and uses each way of leaving a frame
    frames = []
    for i in range(6):
        f = Image.new('RGB', (64, 48), (20 * i, 100, 200))
        f.paste((255, 255, 0), (i * 8, 10, i * 8 + 16, 26))
        frames.append(f.convert('P', palette=Image.ADAPTIVE))
    path = os.path.join(out, 'anim.gif')
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=[40, 80, 120, 40, 40, 200], loop=0, disposal=[0, 1, 2, 1, 2, 1])
    made.append(path)
    return made


def pixels(path, rgba):
    im = Image.open(path)
    if im.mode in ('I;16', 'I;16B', 'I'):
        im = im.point(lambda v: v / 256).convert('L')
    if rgba:
        return im.convert('RGBA')
    if im.mode in ('RGBA', 'LA', 'PA') or (im.mode == 'P' and 'transparency' in im.info):
        back = Image.new('RGBA', im.size, (255, 255, 255, 255))
        back.alpha_composite(im.convert('RGBA'))
        return back.convert('RGB')
    return im.convert('RGB')


def read_out(path):
    data = open(path, 'rb').read()
    if data.startswith(b'P7'):
        head, _, body = data.partition(b'ENDHDR\n')
        words = dict(l.split(b' ', 1) for l in head.split(b'\n')[1:] if b' ' in l)
        return Image.frombytes('RGBA', (int(words[b'WIDTH']), int(words[b'HEIGHT'])), body)
    return Image.open(path).convert('RGB')


def differ(a, b):
    if a.size != b.size:
        return 'size %s against %s' % (a.size, b.size), 999, 999
    diffs = [abs(x - y) for x, y in zip(a.tobytes(), b.tobytes())]
    return 'mean %.2f, most %d' % (sum(diffs) / len(diffs), max(diffs)), max(diffs), sum(diffs) / len(diffs)


def gif_fixed(im):
    """The 6 x 7 x 6 colours with a 4 x 4 ordered dither, as m_gif_enc.c makes them."""
    bayer = (0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5)
    out = []
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            d = (bayer[(y & 3) * 4 + (x & 3)] * 2 + 1) * 255 // 32
            pixel = []
            for v, levels in zip(im.getpixel((x, y)), (6, 7, 6)):
                l = min((v * (levels - 1) + d) // 255, levels - 1)
                pixel.append(l * 51 if levels == 6 else (l * 255 + 3) // 6)
            out.append(bytes(pixel))
    return Image.frombytes('RGB', im.size, b''.join(out))


def main():
    exe = sys.argv[1]
    work = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else os.path.join(os.environ.get('TEMP', '/tmp'), 'media_check')
    wsl = '--wsl' in sys.argv
    workers = '4'
    os.makedirs(work, exist_ok=True)
    bad = 0
    for path in corpus(work):
        name = os.path.basename(path)
        if name == 'anim.gif':
            continue
        for ch in (3, 4):
            out = path + '.%d.out' % ch
            line = run([exe, 'decode', path, out, workers, str(ch)], wsl)
            if 'sum' not in line:
                print('%-22s %d: FAILED %s' % (name, ch, line))
                bad += 1
                continue
            what = differ(read_out(out), pixels(path, ch == 4))
            ok = what[1] == 0 or (name.endswith('.jpg') and what[1] <= 40 and what[2] < 1.5)
            print('%-22s %d: %s %s   %s' % (name, ch, 'ok  ' if ok else 'BAD ', what[0], line.split(': ', 1)[1]))
            bad += not ok
    path = os.path.join(work, 'anim.gif')
    line = run([exe, 'anim', path, workers], wsl)
    sums = [l for l in line.splitlines() if l.startswith('frame')]
    want = []
    im = Image.open(path)
    for n in range(6):
        im.seek(n)
        want.append(im.convert('RGBA'))
    print('anim.gif: %d frames (%s)' % (len(sums), line.splitlines()[0] if line else 'nothing'))
    bad += len(sums) != 6
    src = os.path.join(work, 'rgb.png')
    for out, q, *named in (('enc.png', '6'), ('enc1.png', '1'), ('enc9.png', '9'), ('enc.jpg', '90'), ('enc70.jpg', '70'), ('enc.bmp', '0'),
                           ('enc.ppm', '0'), ('enc.gif', '0', 'pal.png'), ('enc16.gif', '0', 'pal4.png'), ('enc2.gif', '0', 'bits1.png'),
                           ('encgray.gif', '0', 'gray.png'), ('encfixed.gif', '0', 'rgb.png')):
        source = os.path.join(work, named[0]) if named else src
        target = os.path.join(work, out)
        line = run([exe, 'encode', source, target, q, workers], wsl)
        try:
            # (a GIF of more than 256 colours: the fixed colours, dithered as m_gif_enc.c does)
            want = gif_fixed(Image.open(source).convert('RGB')) if out == 'encfixed.gif' else Image.open(source).convert('RGB')
            what = differ(Image.open(target).convert('RGB'), want)
            ok = what[1] == 0 or (out.endswith('.jpg') and what[2] < 4)
        except Exception as e:
            what, ok = (str(e), 0, 0), False
        print('%-22s %s %s (%d bytes) %s' % (out, 'ok  ' if ok else 'BAD ', what[0], os.path.getsize(target) if os.path.exists(target) else 0, line))
        bad += not ok
        print('    read back: %s' % run([exe, 'decode', target, target + '.out', workers], wsl))
    print('%d bad' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
