"""Two runs of one game of Tiberian Dawn or Red Alert drawn two ways, frame for frame (docs/tdawn.md, "A scrolled picture").

    python tools/tdawn_sums.py A.log B.log

Both are console logs of runs with TDAWN_SUMS=1 (the "tdsum:" or "rasum:" lines: the frame, where
the view is, a sum of the map's part of the picture as the program has it, and one of what the
page has over it). Says in how many frames the views are the same and the pictures are not.
"""
import re, sys
def load(p):
    d = {}
    for m in re.finditer(r'(?:td|ra)sum: frame (\d+) view ([0-9a-f]+) sum ([0-9a-f]+) over ([0-9a-f]+)', open(p, encoding='latin1').read()):
        d[int(m.group(1))] = m.groups()[1:]
    return d
a, b = load(sys.argv[1]), load(sys.argv[2])
frames = sorted(set(a) & set(b))
views = [f for f in frames if a[f][0] != b[f][0]]
bad = [f for f in frames if a[f][0] == b[f][0] and a[f][1] != b[f][1]]
over = [f for f in frames if a[f][0] == b[f][0] and a[f][2] != b[f][2]]
print('%d frames in both (%d and %d), %d distinct views; view differs in %d; picture differs in %d, the page over the map in %d' % (
    len(frames), len(a), len(b), len(set(a[f][0] for f in frames)), len(views), len(bad), len(over)))
if bad:
    print('   first: ' + ' '.join('%d@%s' % (f, a[f][0]) for f in bad[:12]))
