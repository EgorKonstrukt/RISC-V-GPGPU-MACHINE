"""The display stream that the GPU makes by itself (docs/world/share.md, "Units"), as a model in numpy.

  python tools/share_units.py FIRST.png NEXT.png ... [--room 1131] [--out FOLDER] [--against FOLDER]

Sends FIRST until nothing is left, then each NEXT for one packet (the last one until nothing
is left), as ShareUnits.shader does pass for pass: the same bytes in every packet, the same
state after it, the same picture at the receiver. --out writes packet_N.bin, state_N.bin and
picture_N.bin (5:6:5 as RGBA) as bin/share_test.exe does; --against compares with its files.

The picture is cut into blocks of four sizes: 4x4 samples of 1, 2, 4 or 8 pixels each (levels
0 to 3), two colours and a bit a sample. A unit is 4x4 blocks of one level on that level's
grid, with a bit a block that is there. The sender keeps for each 8x8 cell the level the
receiver has it at (4: stale) and the packet that carried it.
"""
import argparse
import os

import numpy as np
from PIL import Image

W, H = 1280, 960
CW, CH = 160, 120                      # cells
UW, UH = (80, 40, 20, 10), (60, 30, 15, 8)   # units of each level
UN = tuple(a * b for a, b in zip(UW, UH))
UY = (53, 23, 8, 0)                    # a level's first row in the units' texture (80 x 113)
COST16 = (192, 48, 12, 3)              # sixteenths of a byte a cell is reckoned at, by level
CHUNK, POSITIONS = 64, 2 * sum(UN)
SEG = []                               # the order units go in: (class, level, first position)
for _k in (0, 1):
    for _l in (3, 2, 1, 0):
        SEG.append((_k, _l, sum(UN[s[1]] for s in SEG)))


def pack(total, count, most):
    """A channel's mean (total over count samples of 0..255) as 0..most, rounded: in whole numbers."""
    d = count * 255
    return (2 * total * most + d) // (2 * d)


class Capture:
    def __init__(self, path):
        # (a picture's file, or its pixels)
        rgb = (path if isinstance(path, np.ndarray) else np.asarray(Image.open(path).convert('RGB'))).astype(np.int64)
        self.h, self.w = rgb.shape[:2]
        self.px = np.zeros((H, W, 3), np.int64)
        self.px[:self.h, :self.w] = rgb
        self.lo, self.hi, self.mask, self.shown = [], [], [], []
        for level in range(4):
            n = 1 << level
            s = self.px.reshape(H // n, n, W // n, n, 3).sum(axis=(1, 3))
            bh, bw = H // (4 * n), W // (4 * n)
            b = s.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 1, 3, 4).reshape(bh, bw, 16, 3)
            luma = b @ np.array([299, 587, 114])
            high = 16 * luma > luma.sum(axis=2, keepdims=True)
            nh = high.sum(axis=2)
            low_sum, high_sum = (b * ~high[..., None]).sum(axis=2), (b * high[..., None]).sum(axis=2)
            c = lambda t, k: (pack(t[..., 0], k * n * n, 31) << 11 | pack(t[..., 1], k * n * n, 63) << 5 | pack(t[..., 2], k * n * n, 31))
            lo = c(low_sum, 16 - nh)
            hi = np.where(nh > 0, c(high_sum, np.maximum(nh, 1)), lo)
            mask = (high * (1 << np.arange(16))).sum(axis=2)
            flat = (lo == hi) | (mask == 0)
            hi, mask = np.where(flat, lo, hi), np.where(flat, 0, mask)
            self.lo.append(lo)
            self.hi.append(hi)
            self.mask.append(mask)
            bits = (mask[..., None] >> np.arange(16)) & 1
            shown = np.where(bits == 1, hi[..., None], lo[..., None]).reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)
            self.shown.append(np.repeat(np.repeat(shown, n, axis=0), n, axis=1))
        # the coarsest level that shows a cell as level 0 does: its block exact, and each finer one
        self.need = np.zeros((CH, CW), np.int64)
        for level in (1, 2, 3):
            b = 4 << level
            same = (self.shown[level] == self.shown[0]).reshape(H // b, b, W // b, b).all(axis=(1, 3))
            k = b // 8
            self.need[np.repeat(np.repeat(same, k, axis=0), k, axis=1) & (self.need == level - 1)] = level


def block_bytes(cap, level, by, bx, state):
    """A block's kind and bytes, after the blocks before it in its unit (state: last flat, last pair)."""
    lo, hi, mask = int(cap.lo[level][by, bx]), int(cap.hi[level][by, bx]), int(cap.mask[level][by, bx])
    if mask == 0:
        kind, data = (0, b'') if lo == state[0] else (1, bytes([lo & 255, lo >> 8]))
        state[0] = lo
    else:
        m = bytes([mask & 255, mask >> 8])
        kind, data = (2, m) if (lo, hi) == state[1] else (3, bytes([lo & 255, lo >> 8, hi & 255, hi >> 8]) + m)
        state[1] = (lo, hi)
    return kind, data


def unit_bytes(cap, level, index, mask):
    """A unit's record: its level and number, a bit a block, two bits a block of kinds, the blocks."""
    v = level << 13 | index
    uy, ux = index // UW[level], index % UW[level]
    kinds, data, state, n = 0, b'', [-1, None], 0
    for b in range(16):
        if mask >> b & 1:
            kind, part = block_bytes(cap, level, uy * 4 + (b >> 2), ux * 4 + (b & 3), state)
            kinds |= kind << (2 * n)
            data += part
            n += 1
    return bytes([v & 255, v >> 8, mask & 255, mask >> 8]) + kinds.to_bytes((n + 3) // 4, 'little') + data


class Sender:
    def __init__(self):
        self.have = np.full((CH, CW), 4, np.int64)
        self.seq = np.zeros((CH, CW), np.int64)
        self.cap = None
        self.turn = 0                  # where the last packet stopped, as a unit of level 0

    def marks(self, cap, reset=False, lost=(0, 0)):
        """Pass "marks": a cell that changed since the last capture, or went with a lost packet, is stale."""
        inside = (np.arange(CH)[:, None] * 8 < cap.h) & (np.arange(CW)[None, :] * 8 < cap.w)
        if self.cap is None or reset:
            stale = np.ones((CH, CW), bool)
        else:
            stale = (cap.px != self.cap.px).any(axis=2).reshape(CH, 8, CW, 8).any(axis=(1, 3))
            stale |= (self.have < 4) & (((self.seq - lost[0]) & 255) < lost[1])
        self.have = np.where(inside, np.where(stale, 4, self.have), 0)
        self.seq = np.where(inside & ~stale, self.seq, 0)
        self.cap = cap

    def floors(self, room):
        """Pass "stats": the stale cells as sharp as one packet can carry all of them (what keeps
        changing is then whole in every packet, if coarse), the rest as sharp as two can."""
        have, need = self.have, self.cap.need
        stale, todo = have == 4, (have > need) & (have < 4)
        cost = np.array(COST16)

        def first(sums, room, packets):
            for level in range(4):
                if sums[level] <= packets * room * 16:
                    return level
            return 3
        es = [int(cost[np.maximum(level, need)][stale].sum()) for level in range(4)]
        et = [int(cost[np.maximum(level, need)][todo & (np.maximum(level, need) < have)].sum()) for level in range(4)]
        fs = first(es, room, 1)
        room_t = room - min(room, es[fs] // 16)
        ft = first(et, room_t, 2)
        if todo.any():
            ft = min(ft, int(have[todo].max()) - 1)
        return fs, ft

    def plan(self, fs, ft):
        """Pass "plan": the level each wanted cell goes at: the coarsest whose block spoils nothing."""
        have, need = self.have, self.cap.need
        cls = np.where(have == 4, 0, np.where(have > need, 1, -1))
        t0 = np.maximum(np.where(cls == 0, fs, ft), need)
        cls = np.where((cls >= 0) & (t0 < have), cls, -1)
        out = np.full((CH, CW), -1, np.int64)
        for k in (0, 1):
            t = np.where(cls == k, t0, -1)
            for level, n in ((3, 4), (2, 2)):
                g = lambda a: a.reshape(CH // n, n, CW // n, n)
                ok = ((g(t) >= level) | (g(need) >= level) | (g(have) >= level)).all(axis=(1, 3))
                ok = np.repeat(np.repeat(ok, n, axis=0), n, axis=1)
                t = np.where((t >= level) & ~ok, level - 1, t)
            out = np.where(cls == k, t, out)
        return out, cls

    def units(self, level_of, cls):
        """Pass "units": for each unit of each level a bit a block that goes, its bytes, its class."""
        cap, out = self.cap, []
        for level in range(4):
            n = max(1, (4 << level) // 8)
            goes = (level_of == level).reshape(CH // n, n, CW // n, n)
            first = ((cls == 0) & (level_of == level)).reshape(CH // n, n, CW // n, n).any(axis=(1, 3))
            goes = goes.any(axis=(1, 3))
            if level == 0:
                goes, first = (np.repeat(np.repeat(a, 2, axis=0), 2, axis=1) for a in (goes, first))
            mask, size, kind = np.zeros(UN[level], np.int64), np.zeros(UN[level], np.int64), np.zeros(UN[level], np.int64)
            pad = np.zeros((UH[level] * 4, UW[level] * 4), bool)
            pad[:goes.shape[0], :goes.shape[1]] = goes
            padf = np.zeros_like(pad)
            padf[:goes.shape[0], :goes.shape[1]] = first
            for uy, ux in zip(*np.nonzero(pad.reshape(UH[level], 4, UW[level], 4).any(axis=(1, 3)))):
                i = uy * UW[level] + ux
                bits = pad[uy * 4:uy * 4 + 4, ux * 4:ux * 4 + 4].ravel()
                mask[i] = int((bits * (1 << np.arange(16))).sum())
                size[i] = len(unit_bytes(cap, level, i, int(mask[i])))
                kind[i] = 1 if padf[uy * 4:uy * 4 + 4, ux * 4:ux * 4 + 4].any() else 2
            out.append((mask, size, kind))
        return out

    def order(self, units, k):
        """The units of one class (0: the stale cells', 1: the rest's) in the order they go: coarse
        before sharp, each level from the place the last packet stopped at, and round."""
        seq = []
        for level in (3, 2, 1, 0):
            mask, size, kind = units[level]
            rot = self.turn * UN[level] // UN[0]
            idx = (np.arange(UN[level]) + rot) % UN[level]
            for i in idx[kind[idx] == k + 1]:
                seq.append((level, int(i), int(mask[i]), int(size[i])))
        return seq

    def build(self, room, number):
        """A packet's bytes; the state is as if it has gone. Passes "prefix", "cut", "packet"
        and "state": the stale cells' units while they fit in the room less a quarter (kept for
        the rest, when there is any: what stands still sharpens while something else moves),
        then the rest's while they fit; the next packet starts where the first cut fell."""
        fs, ft = self.floors(room)
        level_of, cls = self.plan(fs, ft)
        units = self.units(level_of, cls)
        stale, rest = self.order(units, 0), self.order(units, 1)
        packet, sent, stop = b'', np.full((CH, CW), 9, np.int64), None
        for part, limit in ((stale, room - room // 4 if rest else room), (rest, room)):
            for level, i, mask, size in part:
                if len(packet) + size > limit:
                    if stop is None:
                        stop = (level, i)
                    break
                packet += unit_bytes(self.cap, level, i, mask)
                # a cell is at the sharpest level a block of the packet covers it with
                n, uy, ux = max(1, (4 << level) // 8), i // UW[level], i % UW[level]
                for b in range(16):
                    if mask >> b & 1:
                        by, bx = uy * 4 + (b >> 2), ux * 4 + (b & 3)
                        y, x = (by // 2, bx // 2) if level == 0 else (by * n, bx * n)
                        sent[y:y + n, x:x + n] = np.minimum(sent[y:y + n, x:x + n], level)
        if stop:
            self.turn = stop[1] * UN[0] // UN[stop[0]]
        self.have = np.where(sent < 9, sent, self.have)
        self.seq = np.where(sent < 9, number & 255, self.seq)
        return packet


def records(packet):
    at = 0
    while at + 4 <= len(packet):
        v, mask = packet[at] | packet[at + 1] << 8, packet[at + 2] | packet[at + 3] << 8
        n = bin(mask).count('1')
        kinds = int.from_bytes(packet[at + 4:at + 4 + (n + 3) // 4], 'little')
        yield v >> 13, v & 0x1fff, mask, kinds, at + 4 + (n + 3) // 4
        at += 4 + (n + 3) // 4 + sum((0, 2, 2, 6)[kinds >> 2 * j & 3] for j in range(n))


def receive(picture, packet):
    """Draws a packet into a picture of 5:6:5 values: where units overlap the sharpest shows."""
    for level, index, mask, kinds, at in sorted(records(packet), key=lambda r: -r[0]):
        side, uy, ux = 4 << level, index // UW[level], index % UW[level]
        flat, pair, n = 0, (0, 0), 0
        for b in range(16):
            if not mask >> b & 1:
                continue
            kind = kinds >> 2 * n & 3
            n += 1
            two = lambda a: packet[a] | packet[a + 1] << 8
            if kind == 1:
                flat = two(at)
            if kind == 3:
                pair = (two(at), two(at + 2))
            bits = 0 if kind < 2 else two(at if kind == 2 else at + 4)
            at += (0, 2, 2, 6)[kind]
            y, x = (uy * 4 + (b >> 2)) * side, (ux * 4 + (b & 3)) * side
            which = (bits >> np.arange(16) & 1).reshape(4, 4)
            block = np.where(which == 1, pair[1], pair[0]) if kind >= 2 else np.full((4, 4), flat)
            block = np.repeat(np.repeat(block, side // 4, axis=0), side // 4, axis=1)
            part = picture[y:y + side, x:x + side]
            part[...] = block[:part.shape[0], :part.shape[1]]


def as_rgba(v):
    """5:6:5 values as a render target of bytes keeps them."""
    r, g, b = (v >> 11) * 255 * 2 + 31, ((v >> 5) & 63) * 255 * 2 + 63, (v & 31) * 255 * 2 + 31
    return np.stack([r // 62, g // 126, b // 62, np.full(v.shape, 255)], axis=-1).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('frames', nargs='+')
    ap.add_argument('--room', type=int, default=1131)
    ap.add_argument('--out')
    ap.add_argument('--against')
    ap.add_argument('--most', type=int, default=400, help='packets at most')
    ap.add_argument('--lost-at', type=int, default=0, help='when this packet is made, the two from three before it are said to be lost')
    ap.add_argument('--no-pictures', action='store_true', help='(as share_test: none were written)')
    a = ap.parse_args()
    if a.out:
        os.makedirs(a.out, exist_ok=True)
    s, picture, number, total, same = Sender(), np.zeros((H, W), np.int64), 0, 0, [0, 0, 0, 0]
    for f, path in enumerate(a.frames):
        cap = Capture(path)
        hold = f == 0 or f == len(a.frames) - 1
        sent = 0
        while number < a.most:
            number += 1
            s.marks(cap, lost=((number - 3) & 255, 2 if number == a.lost_at else 0))
            packet = s.build(a.room, number)
            receive(picture, packet)
            state = np.stack([s.have, s.seq, np.zeros_like(s.have), np.zeros_like(s.have)], axis=-1).astype(np.uint8)
            if a.out:
                open(os.path.join(a.out, 'packet_%d.bin' % number), 'wb').write(packet)
                state.tofile(os.path.join(a.out, 'state_%d.bin' % number))
                as_rgba(picture).tofile(os.path.join(a.out, 'picture_%d.bin' % number))
            if a.against:
                read = lambda name: open(os.path.join(a.against, name % number), 'rb').read()
                same[0] += 1
                same[1] += read('packet_%d.bin') == packet
                same[2] += read('state_%d.bin')[:CH * CW * 4] == state.tobytes()
                if os.path.exists(os.path.join(a.against, 'picture_%d.bin' % number)):   # (share_test --no-pictures)
                    theirs = np.frombuffer(read('picture_%d.bin'), np.uint8).reshape(H, W, 4)
                    same[3] += bool((theirs[..., :3] == as_rgba(picture)[..., :3]).all())
                else:
                    same[3] += 1
            total += len(packet)
            sent += 1
            if not hold or not packet:
                break
        off = int((picture != cap.shown[0])[:cap.h, :cap.w].sum())
        print('%-28s %3d packets, %6d bytes so far, %6d pixels not as sharp as they get' % (os.path.basename(path), sent - (hold and not packet), total, off))
    if a.against:
        print('against %s: of %d packets %d the same bytes, %d the same state after, %d the same picture' % (a.against, same[0], same[1], same[2], same[3]))


if __name__ == '__main__':
    main()
