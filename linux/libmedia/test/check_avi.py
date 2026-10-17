"""libmedia's MJPEG in AVI against Pillow (docs/system/media.md, "MJPEG in AVI"): writes films with
tools/make_avi.py, has media_test decode every picture (MEDIA_TEST_FRAMES) and compares each
with Pillow's decoding of the same JPEG, within what two decoders' colour stretching differs
by (a mean under 1.5 levels, under 0.5% of the values off by more than 24),
and the sound sample for sample. Also the image's sample, when it is there.

    python linux/libmedia/test/check_avi.py MEDIA_TEST [WORKDIR] [--wsl]
"""
import io, os, struct, subprocess, sys
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import make_avi
from check_pictures import FOX, differ, read_out, run, to_wsl


def chunks(path):
    """The pictures' JPEGs and the sound's bytes of an AVI, by its movi list."""
    data = open(path, 'rb').read()
    at = data.index(b'movi') + 4
    pictures, sound = [], b''
    while at + 8 <= len(data) and data[at:at + 4] != b'idx1':
        kind, n = data[at:at + 4], struct.unpack('<I', data[at + 4:at + 8])[0]
        if kind == b'00dc':
            pictures.append(data[at + 8:at + 8 + n])
        elif kind == b'01wb':
            sound += data[at + 8:at + 8 + n]
        at += 8 + n + (n & 1)
    return pictures, sound


def check(exe, path, workers, wsl, work, bits=16):
    frames = os.path.join(work, os.path.basename(path) + '.frames')
    cmd = ['env', 'MEDIA_TEST_QUIET=1', 'MEDIA_TEST_FRAMES=' + (to_wsl(frames) if wsl else frames), exe, 'video', path, workers]
    if wsl:
        cmd = ['wsl', '--'] + [to_wsl(c) for c in cmd]
    if os.path.exists(frames):
        os.remove(frames)   # (a run that fails must not be judged by the last one's pictures)
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    pictures, sound = chunks(path)
    first = Image.open(io.BytesIO(pictures[0]))
    w, h = first.size
    size = (w * h * 4 + 15) & ~15
    raw = open(frames, 'rb').read() if os.path.exists(frames) else b''
    bad, worst, mean, far = 0, 0, 0.0, 0.0
    if len(raw) != size * len(pictures):
        print('%s: %d bytes of pictures, not %d (%s)' % (os.path.basename(path), len(raw), size * len(pictures), out.strip()))
        return 1
    for n, jpg in enumerate(pictures):
        mine = Image.frombuffer('RGBX', (w, h), raw[n * size:n * size + w * h * 4], 'raw', 'BGRX', 0, 1).convert('RGB')
        top = set(raw[n * size + 3:n * size + w * h * 4:4])
        theirs = Image.open(io.BytesIO(jpg)).convert('RGB')
        what = differ(mine, theirs)
        # (a colour at half size is the nearest sample here, smoothed in Pillow: its edges differ)
        off = sum(abs(x - y) > 24 for x, y in zip(mine.tobytes(), theirs.tobytes())) / (w * h * 3)
        worst, mean, far = max(worst, what[1]), max(mean, what[2]), max(far, off)
        bad += what[2] >= 1.5 or off >= 0.005 or top != {0}
    print('%-22s %2s workers: %d pictures, %s; worst %d, mean at most %.2f, at most %.3f%% off by more than 24   %s' % (
        os.path.basename(path), workers, len(pictures), 'ok' if not bad else '%d BAD' % bad, worst, mean, far * 100,
        ' | '.join(l.split(' (')[0] for l in out.splitlines() if 'sum' in l)))
    if sound:
        want = sound if bits == 16 else b''.join(struct.pack('<h', (b - 128) << 8) for b in sound)
        fnv = 2166136261
        for b in want:
            fnv = ((fnv ^ b) * 16777619) & 0xffffffff
        ok = ('fnv %08x' % fnv) in out
        print('    sound: %d samples, fnv %08x: %s' % (len(want) // 2, fnv, 'ok' if ok else 'BAD'))
        bad += not ok
    return bad


def main():
    exe = sys.argv[1]
    work = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else os.path.join(os.environ.get('TEMP', '/tmp'), 'avi_check')
    wsl = '--wsl' in sys.argv
    os.makedirs(work, exist_ok=True)
    fox = Image.open(FOX).convert('RGB').resize((333, 221), Image.LANCZOS)
    pictures = [fox.rotate(n * 9, Image.BILINEAR, fillcolor=(20 * n, 90, 200)) for n in range(8)]
    stereo8 = bytes((128 + int(100 * ((i // 2 * (3 + i % 2)) % 50 - 25) / 25)) & 255 for i in range(2 * 11025))
    mono16 = b''.join(struct.pack('<h', (i * 37) % 60000 - 30000) for i in range(16000))
    films = []
    path = os.path.join(work, 'fox.avi')
    make_avi.write_avi(path, [make_avi.jpeg(p, 80) for p in pictures], 333, 221, 8, stereo8, 11025, 2, 8)
    films.append((path, 8))
    path = os.path.join(work, 'fox_notables.avi')
    make_avi.write_avi(path, [make_avi.jpeg(p.resize((320, 240)), 60, tables=False) for p in pictures], 320, 240, 12, mono16, 16000, 1, 16)
    films.append((path, 16))
    sample = os.path.join(ROOT, 'build', 'images', 'linux', 'root', 'root', 'Videos', 'Big Buck Bunny.avi')
    if os.path.exists(sample):
        films.append((sample, 16))
    bad = 0
    # the film's pictures are the still decoder's (check_pictures.py) to the byte
    check(exe, films[0][0], '4', wsl, work, films[0][1])
    pictures, _ = chunks(films[0][0])
    raw = open(os.path.join(work, 'fox.avi.frames'), 'rb').read()
    same = 0
    for n, jpg in enumerate(pictures):
        one = os.path.join(work, 'still%d.jpg' % n)
        open(one, 'wb').write(jpg)
        run([exe, 'decode', one, one + '.out', '4', '4'], wsl)
        size = (333 * 221 * 4 + 15) & ~15
        mine = Image.frombuffer('RGBX', (333, 221), raw[n * size:n * size + 333 * 221 * 4], 'raw', 'BGRX', 0, 1).convert('RGB')
        same += mine.tobytes() == read_out(one + '.out').convert('RGB').tobytes()
    print('fox.avi: %d of %d pictures the same as the still decoder\'s' % (same, len(pictures)))
    bad += same != len(pictures)
    for path, bits in films:
        for workers in ('0', '4', '16'):
            bad += check(exe, path, workers, wsl, work, bits)
    print('%d bad' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
