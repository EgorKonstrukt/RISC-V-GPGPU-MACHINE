"""Two real clients' logs of the display stream, held together (docs/share.md, "Judged while it moves").

  python tools/share_clients.py LOG_A.txt LOG_B.txt

With `netTest` set on the hub, the first client's machine shows five scenes that keep changing
(0 a game, 1 windows with activity, 2 text scrolling, 3 a window dragged, 4 the pointer) and
sends each as units and as tiles, 40 s each. Both clients write lines to their logs
(LocalLow/VRChat/VRChat/output_log_*.txt):

  [ShareLag]  for every packet the instance's clock when its picture was captured, when it was
              sent, and (the other client) when it arrived
  [ShareSig]  three times a second, 24 x 18 pixels: of the display, by the client whose machine
              is on (role 1), and of its picture of that display, by the one that watches (role 2)

This prints, by what was being sent: how old a packet's picture is when it goes and how long
from capture to arrival (the two clients' clocks may differ by a constant: only the difference
between tiles and units says anything there); and for each scene the mean difference (levels of
255) between the watcher's pixels and the display's at the same moment, over the pixels that
change at all in the scene, and over all of them. The 12 s after a change of what is sent do
not count: the picture is arriving anew.
"""
import re
import sys

import numpy as np

SENT = re.compile(r'\[ShareLag\] sent seq=(\d+) ms=(-?\d+) captured=(-?\d+) units=(\d) bytes=(\d+)')
GOT = re.compile(r'\[ShareLag\] got seq=(\d+) ms=(-?\d+)')
SIG = re.compile(r'\[ShareSig\] ms=(-?\d+) role=(\d) units=(\d) scene=(-?\d+) ([0-9A-F]+)')
SCENES = ('a game', 'windows with activity', 'text scrolling', 'a window dragged', 'the pointer')


def lags(paths):
    sent, got = {}, {}
    for path in paths:
        for line in open(path, encoding='utf-8', errors='replace'):
            m = SENT.search(line)
            if m:
                sent[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))
            m = GOT.search(line)
            if m:
                got.setdefault(int(m.group(1)), int(m.group(2)))
    for u, name in ((0, 'tiles'), (1, 'units')):
        rows = np.array([(s[0] - s[1], got[q] - s[1], s[3]) for q, s in sent.items() if s[2] == u and q in got and s[1] != 0], np.float64)
        if not len(rows):
            print('%s: no packets with both ends' % name)
            continue
        print('%s: %4d packets, %.0f bytes each; the picture %.0f ms old when it goes (9 in 10 under %.0f); %.0f ms from capture to arrival by the two clocks' % (
            name, len(rows), rows[:, 2].mean(), np.median(rows[:, 0]), np.percentile(rows[:, 0], 90), np.median(rows[:, 1])))


def read(paths):
    sent, got = [], []
    for path in paths:
        for line in open(path, encoding='utf-8', errors='replace'):
            m = SIG.search(line)
            if m:
                sig = np.frombuffer(bytes.fromhex(m.group(5)), np.uint8).astype(np.float64)
                (sent if m.group(2) == '1' else got).append((int(m.group(1)), int(m.group(3)), int(m.group(4)), sig))
    return sorted(sent, key=lambda r: r[0]), sorted(got, key=lambda r: r[0])


def main():
    lags(sys.argv[1:])
    sent, got = read(sys.argv[1:])
    print('%d samples of the display, %d of the watcher\'s picture' % (len(sent), len(got)))
    if not sent or not got:
        return
    times = np.array([r[0] for r in sent])
    mode = np.array([r[1] for r in sent])
    scene = np.array([r[2] for r in sent])
    sigs = np.stack([r[3] for r in sent])
    changes = times[1:][(np.diff(mode) != 0) | (np.diff(scene) != 0)]
    gt = np.array([r[0] for r in got])
    gs = np.stack([r[3] for r in got])
    k = np.searchsorted(times, gt, side='right') - 1
    ok = (k >= 0) & (gt - times[np.maximum(k, 0)] < 1000)
    for c in changes:
        ok &= ~((gt >= c) & (gt < c + 12000))
    for sc in range(5):
        of = scene == sc
        if not of.any():
            continue
        # the display's pixels that change at all while the scene is shown
        moving = (np.abs(np.diff(sigs[of], axis=0)).reshape(-1, sigs.shape[1] // 3, 3).max(axis=2) > 0).any(axis=0)
        out = []
        for u in (0, 1):
            sel = ok & (scene[np.maximum(k, 0)] == sc) & (mode[np.maximum(k, 0)] == u)
            if not sel.any():
                out.append('-')
                continue
            d = np.abs(gs[sel] - sigs[k[sel]]).reshape(int(sel.sum()), -1, 3).mean(axis=2)
            out.append('%.2f over the %d pixels that change, %.2f over all (%d samples)' % (d[:, moving].mean() if moving.any() else 0, int(moving.sum()), d.mean(), int(sel.sum())))
        print('%-22s tiles %s | units %s' % (SCENES[sc] + ':', out[0], out[1]))


if __name__ == '__main__':
    main()
