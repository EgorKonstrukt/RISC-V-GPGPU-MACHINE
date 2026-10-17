"""libmedia's animated GIF encoder against Pillow (docs/system/media.md, "GIF encoding"): animations made
with Pillow are decoded by media_test and written again as one (media_gif_anim_encode); Pillow
reads both, and every frame must be the same pixels, with the same delays and loop count.

    python linux/libmedia/test/check_gif_anim.py MEDIA_TEST [WORKDIR] [--wsl]
"""
import os, random, subprocess, sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
FOX = os.path.join(HERE, '..', '..', 'apps', 'pictures', 'picture-fox.png')


def to_wsl(p):
    return '/mnt/' + p[0].lower() + p[2:].replace('\\', '/') if len(p) > 2 and p[1] == ':' else p


def frames_of(path):
    im = Image.open(path)
    out = []
    for n in range(im.n_frames):
        im.seek(n)
        out.append((im.convert('RGBA'), im.info.get('duration', 0)))
    return out, im.info.get('loop', None)


def makes(work):
    fox = Image.open(FOX).convert('RGB')
    made = []
    # a pan over the fox, 12 frames, its own 256 colours each
    pan = [fox.crop((x, 40, x + 320, 280)).quantize(256).convert('RGB') for x in range(0, 300, 25)]
    p = os.path.join(work, 'pan.gif')
    pan[0].save(p, save_all=True, append_images=pan[1:], duration=[40 + 10 * (n % 3) for n in range(len(pan))], loop=0)
    made.append(p)
    # a small drawing with see-through parts, played 3 times
    rnd = random.Random(2)
    frames = []
    for n in range(8):
        im = Image.new('RGBA', (96, 64), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        for k in range(6):
            x, y = rnd.randrange(80), rnd.randrange(48)
            d.ellipse((x, y, x + 16, y + 16), fill=(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), 255))
        frames.append(im)
    p = os.path.join(work, 'dots.gif')
    frames[0].save(p, save_all=True, append_images=frames[1:], duration=70, loop=3, disposal=2)
    made.append(p)
    return made


def main():
    exe = sys.argv[1]
    work = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else os.path.join(os.environ.get('TEMP', '/tmp'), 'gif_anim')
    wsl = '--wsl' in sys.argv
    os.makedirs(work, exist_ok=True)
    bad = 0
    for src in makes(work):
        want, loop = frames_of(src)
        for workers in ('0', '4', '16'):
            out = src[:-4] + '.again%s.gif' % workers
            if os.path.exists(out):
                os.remove(out)
            cmd = [exe, 'animenc', src, out, workers]
            if wsl:
                cmd = ['wsl', '--'] + [to_wsl(c) for c in cmd]
            line = subprocess.run(cmd, capture_output=True, text=True).stdout.strip()
            try:
                got, got_loop = frames_of(out)
            except Exception as e:
                print('%-10s %2s workers: BAD (%s) %s' % (os.path.basename(src), workers, e, line))
                bad += 1
                continue
            same = len(got) == len(want) and all(a[0].tobytes() == b[0].tobytes() for a, b in zip(got, want))
            # (transparent pixels: only that they are, not what colour they hide)
            if not same and len(got) == len(want):
                same = all([px if px[3] else (0, 0, 0, 0) for px in a[0].get_flattened_data()] ==
                           [px if px[3] else (0, 0, 0, 0) for px in b[0].get_flattened_data()] for a, b in zip(got, want))
            times = [d for _, d in got] == [d for _, d in want]
            ok = same and times and got_loop == loop
            bad += not ok
            print('%-10s %2s workers: %s frames %d, pixels %s, delays %s, loop %s/%s   %s' % (
                os.path.basename(src), workers, 'ok ' if ok else 'BAD', len(got), 'same' if same else 'DIFFER',
                'same' if times else 'DIFFER', got_loop, loop, line.split(': ', 1)[-1]))
    print('%d bad' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
