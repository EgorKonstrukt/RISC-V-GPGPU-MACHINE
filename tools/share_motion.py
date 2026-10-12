"""The display stream while the picture keeps changing (docs/share.md, "Judged while it moves").

  rvc_harness --cpu --cpu-ips 3000000 --desktop ... --screen-log SCENE.scr --screen-every 33 --screen-frames 300
  python tools/share_motion.py SCENE.scr ... [--rates 0.15,1.0] [--only tiles,units] [--video FOLDER]

Plays a recording of the guest's display through a model of each sender on the recording's own
clock: a packet every 0.15 s (VRChat) or every second (the editor), each made of the frame that
was current when its capture was taken, and the receiver's picture held against the frame that
is current at each recorded moment. Prints the mean difference (levels of 255) over the pixels
that change at all during the recording: what a watcher's eye is on. Lower is better.
--video writes the recording and what each receiver showed as SCENE.mp4 and SCENE_RATE_SENDER.mp4
(ffmpeg must be on the path), and the numbers as SCENE.json.

  tiles   EmuShareHub's tiles (PutTiles), its capture one sending old
  units   ShareUnits.shader as tools/share_units.py models it: a packet goes the frame it has
          been read back (EmuShareHub.SendUnits), its capture 0.04 s old, and while the link
          is busy it is made again every tick (UnitsWait): 0.15 s old at a packet a second
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import share_units as u

PACKET, HEAD = 1150, 19


class Recording:
    """A --screen-log file: 16 bytes (width, height, frame, the guest's milliseconds), then red, green, blue a pixel."""

    def __init__(self, path):
        head = np.fromfile(path, np.uint32, 4)
        self.w, self.h = int(head[0]), int(head[1])
        self.size = 16 + self.w * self.h * 3
        self.n = os.path.getsize(path) // self.size
        self.raw = np.memmap(path, np.uint8, 'r')
        ms = np.array([int(np.frombuffer(self.raw[i * self.size + 12:i * self.size + 16], np.uint32)[0]) for i in range(self.n)])
        self.seconds = (ms - ms[0]) / 1000.0

    def frame(self, i):
        at = i * self.size + 16
        return np.asarray(self.raw[at:at + self.w * self.h * 3]).reshape(self.h, self.w, 3)

    def at(self, t):
        return int(np.searchsorted(self.seconds, max(t, 0.0), side='right') - 1)

    def changing(self):
        """The pixels that change at all, and 8 round them."""
        mask, prev = np.zeros((self.h, self.w), bool), self.frame(0)
        for i in range(1, self.n):
            f = self.frame(i)
            mask |= (f != prev).any(axis=2)
            prev = f
        grown = mask.copy()
        for dy in (-8, 0, 8):
            for dx in (-8, 0, 8):
                grown |= np.roll(np.roll(mask, dy, axis=0), dx, axis=1)
        return grown


# ---- the tiles, as ShareEncode.shader codes them and EmuShareHub.PutTiles picks them ----

def split(px):
    """4x4 blocks of [H, W, 3]: low and high 5:6:5, 16 bits, and the picture they show."""
    h, w = px.shape[:2]
    b = px.reshape(h // 4, 4, w // 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(h // 4, w // 4, 16, 3).astype(np.float64)
    luma = b @ np.array([0.299, 0.587, 0.114])
    hi = luma > luma.mean(axis=2, keepdims=True) + 0.0255
    nh = hi.sum(axis=2)
    low = (b * ~hi[..., None]).sum(axis=2) / np.maximum(16 - nh, 1)[..., None]
    high = np.where(nh[..., None] > 0, (b * hi[..., None]).sum(axis=2) / np.maximum(nh, 1)[..., None], low)
    p = lambda c: ((c[..., 0] * 31 + 127) // 255) << 11 | ((c[..., 1] * 63 + 127) // 255) << 5 | (c[..., 2] * 31 + 127) // 255
    un = lambda v: np.stack([(v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31], axis=-1)
    lo5, hi5 = p(np.floor(low + 0.5).astype(np.int64)), p(np.floor(high + 0.5).astype(np.int64))
    mask = (hi * (1 << np.arange(16))).sum(axis=2)
    shown = np.where(hi[..., None], un(hi5)[:, :, None, :], un(lo5)[:, :, None, :])
    return lo5, hi5, mask, shown.reshape(h // 4, w // 4, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(h, w, 3)


def quarter_bytes(lo5, hi5, mask):
    """Pass 4's bytes for a quarter's 64 blocks: colours that repeat are left out."""
    flat, pair, size = -1, None, 16
    for l, h, m in zip(lo5, hi5, mask):
        if l == h or m == 0:
            size += 0 if l == flat else 2
            flat = l
        else:
            size += 2 if (l, h) == pair else 6
            pair = (l, h)
    return size


class TileCapture:
    def __init__(self, picture, before):
        h, w = picture.shape[:2]
        self.fine = np.zeros(((h + 63) // 64 * 64, (w + 63) // 64 * 64, 3), np.int64)
        self.fine[:h, :w] = picture
        H, W = self.fine.shape[:2]
        self.rows, self.cols = H // 64, W // 64
        self.lo, self.hi, self.mask, self.sharp = split(self.fine)
        box = lambda a: (a.reshape(a.shape[0] // 2, 2, a.shape[1] // 2, 2, 3).sum(axis=(1, 3)) + 2) // 4
        mip = box(self.fine)
        t = mip.reshape(self.rows, 32, self.cols, 32, 3).transpose(0, 2, 1, 3, 4)
        q8, q16 = t.reshape(self.rows, self.cols, 4, 8, 4, 8, 3), t.reshape(self.rows, self.cols, 2, 16, 2, 16, 3)
        flat8 = (q8 == q8[:, :, :, :1, :, :1]).all(axis=(2, 3, 4, 5, 6))
        flat16 = (q16 == q16[:, :, :, :1, :, :1]).all(axis=(2, 3, 4, 5, 6))
        self.flat = np.where(flat16, 2, np.where(flat8, 1, 0)).ravel()
        self.level = []
        for k in range(3):
            shown = split(mip)[3]
            self.level.append(np.repeat(np.repeat(shown, 2 << k, axis=0), 2 << k, axis=1))
            mip = box(mip)
        px = np.ones((H, W), bool) if before is None else (before.fine != self.fine).any(axis=2)
        self.tile_changed = px.reshape(self.rows, 64, self.cols, 64).any(axis=(1, 3)).ravel()
        self.quarter = {}

    def quarter_size(self, t, part):
        if (t, part) not in self.quarter:
            y, x = (t // self.cols) * 16 + (part >> 1) * 8, (t % self.cols) * 16 + (part & 1) * 8
            self.quarter[t, part] = quarter_bytes(self.lo[y:y + 8, x:x + 8].ravel(), self.hi[y:y + 8, x:x + 8].ravel(), self.mask[y:y + 8, x:x + 8].ravel())
        return self.quarter[t, part]


class Tiles:
    """EmuShareHub.PutTiles for a display over 640 x 480: tiles of the half-size stream, then quarters at the fine level."""

    def __init__(self):
        self.cap = None
        self.cursor = 0
        self.work = False
        self.picture = None

    def packet(self, picture):
        cap = TileCapture(picture, self.cap)
        if self.cap is None:
            n = cap.rows * cap.cols
            self.have, self.fine_have, self.picture = [3] * n, [0] * n, np.zeros_like(cap.fine)
            self.work = True
        for t in np.flatnonzero(cap.tile_changed):
            self.have[int(t)], self.fine_have[int(t)], self.work = 3, 0, True
        self.cap = cap
        if not self.work:
            return 0
        at, start, used, full = HEAD, HEAD, len(self.have), False
        stale = sum(1 for h in self.have if h == 3)

        def put(t, level):
            nonlocal at
            size = (384, 96, 24)[level]
            if at + 2 + size > PACKET:
                return False
            y, x = (t // cap.cols) * 64, (t % cap.cols) * 64
            self.picture[y:y + 64, x:x + 64] = cap.level[level][y:y + 64, x:x + 64]
            self.have[t] = level
            at += 2 + size
            return True
        if stale:
            room = PACKET - at
            level = 0 if stale * 386 <= 2 * room else 1 if stale * 98 <= 2 * room else 2
            for i in range(used):
                t = (self.cursor + i) % used
                if self.have[t] == 3 and not put(t, max(int(cap.flat[t]), level)):
                    self.cursor = t
                    break
        if at + 26 <= PACKET:
            mark = at
            for frm in (2, 1):
                if at != mark:
                    break
                for i in range(used):
                    t = (self.cursor + i) % used
                    if self.have[t] != frm or cap.flat[t] >= frm:
                        continue
                    if not put(t, max(int(cap.flat[t]), frm - 1)):
                        self.cursor, full = t, True
                        break
            for i in range(used):
                t = (self.cursor + i) % used
                if full:
                    break
                if self.have[t] != 0 or self.fine_have[t] == 15 or cap.flat[t] != 0:
                    continue
                for part in range(4):
                    if full or self.fine_have[t] >> part & 1:
                        continue
                    size = cap.quarter_size(t, part)
                    if at + 4 + size > PACKET:
                        self.cursor, full = t, True
                        continue
                    y, x = (t // cap.cols) * 64 + (part >> 1) * 32, (t % cap.cols) * 64 + (part & 1) * 32
                    self.picture[y:y + 32, x:x + 32] = cap.sharp[y:y + 32, x:x + 32]
                    self.fine_have[t] |= 1 << part
                    at += 4 + size
        if at == start:
            self.work = False
        return at - start

    def shown(self, h, w):
        return self.picture[:h, :w]


# ---- the units ----

class Units:
    def __init__(self):
        self.s, self.picture, self.number = u.Sender(), np.zeros((u.H, u.W), np.int64), 0

    def packet(self, picture):
        self.s.marks(u.Capture(picture))
        self.number += 1
        p = self.s.build(PACKET - HEAD, self.number)
        u.receive(self.picture, p)
        return len(p)

    def shown(self, h, w):
        return u.as_rgba(self.picture[:h, :w])[..., :3].astype(np.int64)


def grid(w, h):
    """The model's grid just large enough for the picture (the shader's is 1280 x 960: the same
    units from the same corner, numbered otherwise), which is three times less to work out."""
    u.W, u.H = (w + 127) // 128 * 128, (h + 127) // 128 * 128
    u.CW, u.CH = u.W // 8, u.H // 8
    u.UW, u.UH = tuple(u.W // (16 << k) for k in range(4)), tuple(u.H // (16 << k) for k in range(4))
    u.UN = tuple(a * b for a, b in zip(u.UW, u.UH))
    u.SEG[:] = []
    for k in (0, 1):
        for level in (3, 2, 1, 0):
            u.SEG.append((k, level, sum(u.UN[s[1]] for s in u.SEG)))


class Video:
    """Pictures into an .mp4 at the recording's own speed."""

    def __init__(self, path, rec):
        fps = (rec.n - 1) / max(rec.seconds[-1], 0.001)
        self.pipe = subprocess.Popen(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', '%dx%d' % (rec.w, rec.h),
                                      '-r', '%.3f' % fps, '-i', '-', '-c:v', 'libx264', '-preset', 'fast', '-crf', '15', '-pix_fmt', 'yuv420p', path],
                                     stdin=subprocess.PIPE)

    def add(self, picture):
        self.pipe.stdin.write(np.ascontiguousarray(picture, np.uint8).tobytes())

    def close(self):
        self.pipe.stdin.close()
        self.pipe.wait()


METHODS = {'tiles': (Tiles, lambda period: period), 'units': (Units, lambda period: 0.04 if period < 0.2 else 0.15)}


def play(rec, make, period, lag, mask, video=None):
    """The mean difference over `mask` at each recorded frame, and the bytes sent."""
    s = make()
    while s.packet(rec.frame(0)):   # the receiver has the first frame whole
        pass
    errors, total, due = [], 0, 0.0
    for i in range(rec.n):
        while due <= rec.seconds[i]:
            total += s.packet(rec.frame(rec.at(due - lag)))
            due += period
        shown = s.shown(rec.h, rec.w)
        if video:
            video.add(np.clip(shown, 0, 255))
        errors.append(float(np.abs(shown - rec.frame(i).astype(np.int64))[mask].mean()))
    if video:
        video.close()
    return float(np.mean(errors)), total, errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('recordings', nargs='+')
    ap.add_argument('--rates', default='0.15,1.0', help='seconds between packets')
    ap.add_argument('--only', default='tiles,units')
    ap.add_argument('--video')
    a = ap.parse_args()
    for path in a.recordings:
        rec = Recording(path)
        grid(rec.w, rec.h)
        mask = rec.changing()
        scene, numbers = os.path.splitext(os.path.basename(path))[0], {}
        if a.video:
            os.makedirs(a.video, exist_ok=True)
            v = Video(os.path.join(a.video, scene + '.mp4'), rec)
            for i in range(rec.n):
                v.add(rec.frame(i))
            v.close()
        for period in (float(v) for v in a.rates.split(',')):
            out = []
            for name in a.only.split(','):
                make, lag = METHODS[name]
                video = Video(os.path.join(a.video, '%s_%s_%s.mp4' % (scene, period, name)), rec) if a.video else None
                error, total, errors = play(rec, make, period, lag(period), mask, video)
                numbers['%s %s' % (period, name)] = {'error': error, 'bytes': total, 'errors': errors}
                out.append('%s %5.2f (%d bytes)' % (name, error, total))
            print('%-10s %d frames, %4.1f%% of the screen changes; a packet every %.2f s: %s' % (
                scene, rec.n, 100 * mask.mean(), period, ', '.join(out)), flush=True)
        if a.video:
            json.dump({'frames': rec.n, 'seconds': float(rec.seconds[-1]), 'changes': float(mask.mean()), 'runs': numbers}, open(os.path.join(a.video, scene + '.json'), 'w'))


if __name__ == '__main__':
    main()
