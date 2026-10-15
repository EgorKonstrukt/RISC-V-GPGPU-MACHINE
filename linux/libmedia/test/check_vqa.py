"""libmedia's Westwood VQA against ffmpeg's (docs/media.md, "VQA"): every picture of real files
(Tiberian Dawn's 4x2 blocks, Red Alert's 4x4) decoded by media_test, made colours by its own
palette, against ffmpeg's decoding to RGB. The sound against the games' own decoder
(vqa_sound_ref.cpp, its FILE.VQA.game.raw beside the file: --game), not ffmpeg's, whose IMA steps
are rounded otherwise (they differ from the games' by 1 or 2).

    python linux/libmedia/test/check_vqa.py MEDIA_TEST FFMPEG FILE.VQA... [--wsl] [--workers N] [--game]
"""
import os, struct, subprocess, sys


def to_wsl(p):
    return '/mnt/' + p[0].lower() + p[2:].replace('\\', '/') if len(p) > 2 and p[1] == ':' else p


def ffmpeg_size(ffmpeg, path):
    probe = subprocess.run([ffmpeg, '-hide_banner', '-i', path], capture_output=True, text=True).stderr
    for word in probe.replace(',', ' ').split():
        if 'x' in word and word.split('x')[0].isdigit() and word.split('x')[1].isdigit():
            return int(word.split('x')[0]), int(word.split('x')[1])
    raise SystemExit('no size from ffmpeg for ' + path)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    exe, ffmpeg, files = args[0], args[1], args[2:]
    wsl = '--wsl' in sys.argv
    workers = sys.argv[sys.argv.index('--workers') + 1] if '--workers' in sys.argv else '8'
    if workers in args:
        files = [f for f in files if f != workers]
    bad = 0
    for path in files:
        w, h = ffmpeg_size(ffmpeg, path)
        want = subprocess.run([ffmpeg, '-v', 'error', '-i', path, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                              capture_output=True).stdout
        game = path + '.game.raw'
        sound = open(game, 'rb').read() if '--game' in sys.argv and os.path.exists(game) else b''
        frames = path + '.frames'
        if os.path.exists(frames):
            os.remove(frames)
        cmd = ['env', 'MEDIA_TEST_QUIET=1', 'MEDIA_TEST_FRAMES=' + (to_wsl(frames) if wsl else frames), exe, 'video', path, workers]
        if wsl:
            cmd = ['wsl', '--'] + [to_wsl(c) for c in cmd]
        out = subprocess.run(cmd, capture_output=True, text=True).stdout
        raw = open(frames, 'rb').read() if os.path.exists(frames) else b''
        each = w * h + 1024
        count = len(want) // (w * h * 3)
        if len(raw) != each * count:
            print('%s: %d bytes of pictures, not %d for %d (%s)' % (os.path.basename(path), len(raw), each * count, count, out.strip()))
            bad += 1
            continue
        exact, most, total = 0, 0, 0
        for n in range(count):
            idx = raw[n * each:n * each + w * h]
            pal = struct.unpack('<256I', raw[n * each + w * h:(n + 1) * each])
            mine = bytearray(w * h * 3)
            for i, p in enumerate(idx):
                c = pal[p]
                mine[3 * i], mine[3 * i + 1], mine[3 * i + 2] = (c >> 16) & 255, (c >> 8) & 255, c & 255
            theirs = want[n * w * h * 3:(n + 1) * w * h * 3]
            if bytes(mine) == theirs:
                exact += 1
            d = max(abs(a - b) for a, b in zip(mine, theirs))
            most = max(most, d)
            total += d
        # (a palette is 6 bits a channel: ffmpeg widens it one way, the player another)
        ok = most <= 3
        fnv = 2166136261
        for b in sound:
            fnv = ((fnv ^ b) * 16777619) & 0xffffffff
        sound_ok = not sound or ('fnv %08x' % fnv) in out
        bad += (not ok) + (not sound_ok)
        print('%-14s %dx%d, %d pictures: %s, %d exact, most off %d; sound %d samples %s   %s' % (
            os.path.basename(path), w, h, count, 'ok ' if ok else 'BAD', exact, most, len(sound) // 2,
            'the same' if sound_ok else 'DIFFERS (fnv %08x)' % fnv,
            ' | '.join(l.split(' (')[0] for l in out.splitlines() if 'sum' in l)))
    print('%d bad' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
