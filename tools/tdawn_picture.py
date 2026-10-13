"""Tiberian Dawn's picture as the program has it against what the GPU drew (docs/tdawn.md, "Checking it").

    python tools/tdawn_picture.py UART.log CAPTURE.bmp [OUT.png]

UART.log is a run with TDAWN_PICTURE=1 held at a frame (the "tdpic:" lines), CAPTURE.bmp the
harness's --gpu-capture of that run. A pixel something darkens (a shadow, the shroud's edge) is
not compared: the program has no colour for it. Prints how many of the others differ, and
where. OUT.png: the program's picture, written for a page.
"""
import sys
import numpy as np
from PIL import Image

rows, palette, size = {}, None, None
for line in open(sys.argv[1], encoding='latin1'):
    if not line.startswith('tdpic: '):
        continue
    part = line.split()
    if palette is None:
        size = int(part[1]), int(part[2])
        palette = np.array([[int(c[0:2], 16), int(c[2:4], 16), int(c[4:6], 16)] for c in part[3:259]], dtype=np.int16)
    elif len(part) == 4 and len(part[2]) == 2 * size[0]:
        rows[int(part[1])] = (np.frombuffer(bytes.fromhex(part[2]), dtype=np.uint8), np.array([c == '1' for c in part[3]]))
if palette is None or len(rows) != size[1]:
    sys.exit('no whole picture in %s (%d rows)' % (sys.argv[1], len(rows)))
index = np.stack([rows[y][0] for y in range(size[1])])
dark = np.stack([rows[y][1] for y in range(size[1])])
ours = palette[index]
shot = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
best = None
for scale in (1, 2, 3, 4):   # the window is a whole multiple of the game's screen
    theirs = shot[:size[1] * scale:scale, :size[0] * scale:scale]
    if theirs.shape[:2] != ours.shape[:2]:
        continue
    d = np.abs(theirs - ours).max(axis=2)
    d[dark] = 0
    count = int((d > 8).sum())
    if best is None or count < best[0]:
        best = (count, scale, d)
count, scale, d = best
print('%s: %dx%d at %dx, %d pixels darkened and left out; of the other %d, %d differ by more than 8 levels' % (
    sys.argv[2], size[0], size[1], scale, int(dark.sum()), int((~dark).sum()), count))
ys, xs = np.where(d > 8)
if count:
    print('   in x %d..%d, y %d..%d of the game\'s pixels' % (xs.min(), xs.max(), ys.min(), ys.max()))
    seen = np.zeros(d.shape, dtype=bool)
    for y, x in zip(ys, xs):   # by 24-pixel squares of the screen
        seen[y // 24 * 24, x // 24 * 24] = True
    cells = ['%d,%d:%d' % (x, y, int((d[y:y + 24, x:x + 24] > 8).sum())) for y, x in zip(*np.where(seen))]
    print('   by 24-pixel squares (x,y:pixels): ' + ' '.join(cells[:40]))
if len(sys.argv) > 3:
    Image.fromarray(ours.astype(np.uint8)).resize((size[0] * scale, size[1] * scale), Image.NEAREST).save(sys.argv[3])
