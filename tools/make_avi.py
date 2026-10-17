"""Writes Motion JPEG in AVI (docs/system/media.md, "MJPEG in AVI"): every picture a baseline JPEG of
Pillow's in restart intervals, as libmedia decodes on every core, and PCM sound if given.

    python tools/make_avi.py OUT.avi PICTURE... [--fps N] [--quality Q] [--wav FILE] [--no-tables]
    python tools/make_avi.py --sample [SECONDS]

--no-tables leaves the Huffman tables out of every picture, as a camera's MJPEG does (AVI1).
--sample makes /root/Videos/Big Buck Bunny.avi of the image (build/images/linux/root/root/Videos)
from the trailer tools/make_video.py fetched: its first seconds at 320 x 176, 24 pictures a
second, 22,050 Hz mono sound. Needs Pillow, and ffmpeg for --sample.
"""
import io, os, struct, subprocess, sys, tempfile, wave
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def jpeg(image, quality=75, tables=True):
    """A picture as a JPEG in some 128 restart intervals: every core takes some."""
    image = image.convert('RGB')
    mcus = -(-image.size[0] // 16) * -(-image.size[1] // 16)
    out = io.BytesIO()
    image.save(out, 'JPEG', quality=quality, subsampling=2, optimize=False, progressive=False,
               restart_marker_blocks=max(1, -(-mcus // 128)))
    data = out.getvalue()
    if not tables:
        at, kept = 2, bytearray(data[:2])
        while data[at + 1] != 0xda:
            n = 2 + struct.unpack('>H', data[at + 2:at + 4])[0]
            if data[at + 1] != 0xc4:
                kept += data[at:at + n]
            at += n
        data = bytes(kept + data[at:])
    return data


def chunk(kind, data):
    return kind + struct.pack('<I', len(data)) + data + (b'\0' if len(data) & 1 else b'')


def listed(kind, data):
    return chunk(b'LIST', kind + data)


def write_avi(path, frames, width, height, fps, pcm=None, rate=0, channels=0, bits=16):
    """frames: JPEG files' bytes; pcm: interleaved little-endian samples, `bits` 8 or 16."""
    align = channels * bits // 8 if pcm else 0
    sound = len(pcm) // align if pcm else 0
    streams = 2 if pcm else 1
    biggest = max(len(f) for f in frames)
    avih = struct.pack('<10I4I', round(1e6 / fps), (biggest + 4096) * fps, 0, 0x10, len(frames), 0, streams,
                       biggest + 4096, width, height, 0, 0, 0, 0)
    vids = struct.pack('<4s4sIHHIIIIIIIIhhhh', b'vids', b'MJPG', 0, 0, 0, 0, 1, fps, 0, len(frames), biggest, 0xffffffff, 0,
                       0, 0, width, height)
    bih = struct.pack('<IiiHH4sIiiII', 40, width, height, 1, 24, b'MJPG', width * height * 3, 0, 0, 0, 0)
    strl = [listed(b'strl', chunk(b'strh', vids) + chunk(b'strf', bih))]
    if pcm:
        auds = struct.pack('<4s4sIHHIIIIIIIIhhhh', b'auds', b'\0\0\0\0', 0, 0, 0, 0, 1, rate, 0, sound, rate * align, 0xffffffff,
                           align, 0, 0, 0, 0)
        wfx = struct.pack('<HHIIHHH', 1, channels, rate, rate * align, align, bits, 0)
        strl.append(listed(b'strl', chunk(b'strh', auds) + chunk(b'strf', wfx)))
    hdrl = listed(b'hdrl', chunk(b'avih', avih) + b''.join(strl))
    # the movie: each picture with the sound of its time after it
    movi, index, at = [], [], 4
    for n, f in enumerate(frames):
        pieces = [(b'00dc', f, 0x10)]
        if pcm:
            first, last = n * sound // len(frames), (n + 1) * sound // len(frames)
            if last > first:
                pieces.append((b'01wb', pcm[first * align:last * align], 0x10))
        for kind, data, flags in pieces:
            index.append(struct.pack('<4sIII', kind, flags, at, len(data)))
            c = chunk(kind, data)
            movi.append(c)
            at += len(c)
    body = b'AVI ' + hdrl + listed(b'movi', b''.join(movi)) + chunk(b'idx1', b''.join(index))
    open(path, 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)


def sample(seconds):
    source = os.path.join(ROOT, 'build', 'fetch', 'media', 'Big_Buck_Bunny_Trailer_400p.ogv')
    if not os.path.exists(source):
        raise SystemExit('run tools/make_video.py first: it fetches the trailer')
    out = os.path.join(ROOT, 'build', 'images', 'linux', 'root', 'root', 'Videos', 'Big Buck Bunny.avi')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with tempfile.TemporaryDirectory() as work:
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-t', str(seconds), '-i', source, '-vf', 'scale=320:176,fps=24',
                        os.path.join(work, '%04d.png')], check=True)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-t', str(seconds), '-i', source, '-vn', '-ac', '1', '-ar', '22050',
                        '-f', 's16le', '-fflags', '+bitexact', os.path.join(work, 'sound.raw')], check=True)
        names = sorted(n for n in os.listdir(work) if n.endswith('.png'))
        frames = [jpeg(Image.open(os.path.join(work, n)), 70) for n in names]
        pcm = open(os.path.join(work, 'sound.raw'), 'rb').read()
    write_avi(out, frames, 320, 176, 24, pcm, 22050, 1, 16)
    print('%s: %d pictures, %d bytes' % (os.path.relpath(out, ROOT), len(frames), os.path.getsize(out)))


def main():
    args = sys.argv[1:]
    if args and args[0] == '--sample':
        sample(float(args[1]) if len(args) > 1 else 4)
        return
    fps, quality, wav, tables, files = 24, 75, None, True, []
    while args:
        a = args.pop(0)
        if a == '--fps':
            fps = int(args.pop(0))
        elif a == '--quality':
            quality = int(args.pop(0))
        elif a == '--wav':
            wav = args.pop(0)
        elif a == '--no-tables':
            tables = False
        else:
            files.append(a)
    if len(files) < 2:
        raise SystemExit(__doc__)
    pictures = [Image.open(f).convert('RGB') for f in files[1:]]
    size = pictures[0].size
    frames = [jpeg(p if p.size == size else p.resize(size), quality, tables) for p in pictures]
    pcm, rate, channels, bits = None, 0, 0, 16
    if wav:
        with wave.open(wav) as w:
            pcm, rate, channels, bits = w.readframes(w.getnframes()), w.getframerate(), w.getnchannels(), 8 * w.getsampwidth()
    write_avi(files[0], frames, size[0], size[1], fps, pcm, rate, channels, bits)


if __name__ == '__main__':
    main()
