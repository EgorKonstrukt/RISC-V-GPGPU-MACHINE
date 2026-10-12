"""What a pass's time is made of: a cost fitted to each kind of event.

    python tools/pass_regress.py logs/pass_costs.csv

The .csv is tools/pass_costs.ps1's: a line a pass, its time in milliseconds and the profiling
build's counts for it. The fit is least squares with no cost below nothing. The kinds are few
on purpose: counts that rise and fall together (a load and a load's texture read, a branch and
a branch taken) cannot be told apart by twenty passes, and a fit with more terms gives each
of them whatever makes the sum come out.
"""
import sys

import numpy as np

rows = []
for line in open(sys.argv[1], encoding='utf-8-sig'):
    line = line.strip()
    if not line:
        continue
    name, at, ms, counts = line.split(',', 3)
    c = dict((k, float(v)) for k, v in (kv.split('=') for kv in counts.split(';') if kv))
    if float(ms) > 0 and c:
        rows.append((name + '+' + at, float(ms), c))


def g(c, k):
    return c.get(k, 0.0)


FEATURES = [
    ('not a load or a store', lambda c: g(c, 'fast_step') - g(c, 'x_load') - g(c, 'x_store')),
    ('load', lambda c: g(c, 'x_load')),
    ('  ... that searches the cache', lambda c: g(c, 'l1_load_search') if 'l1_load_search' in c else g(c, 'ram_read_bloom_pass')),
    ('store', lambda c: g(c, 'x_store')),
    ('  ... that searches the cache', lambda c: g(c, 'l1_store_search')),
    ('general path', lambda c: g(c, 'tick') - g(c, 'fast_step')),
]
A = np.array([[f(c) for _, f in FEATURES] for _, _, c in rows])
y = np.array([ms * 1e6 for _, ms, _ in rows])   # nanoseconds


def nnls(A, y, steps=200000):
    """Least squares with x >= 0, by projected gradient steps."""
    x = np.zeros(A.shape[1])
    L = np.linalg.norm(A, 2) ** 2
    for _ in range(steps):
        x = np.maximum(0, x - A.T @ (A @ x - y) / L)
    return x


scale = np.maximum(A.max(axis=0), 1)
x = nnls(A / scale, y) / scale
print('%-32s %10s %10s' % ('', 'ns each', 'of the time'))
total = (A * x).sum(axis=0)
for (n, _), xi, t in zip(FEATURES, x, total):
    print('%-32s %10.1f %9.1f%%' % (n, xi, 100 * t / y.sum()))
print()
for (n, ms, c), p in zip(rows, A @ x):
    print('%-12s %.3f ms measured, %.3f fitted (%+.1f%%), %d instructions' % (n, ms, p / 1e6, 100 * (p / 1e6 / ms - 1), g(c, 'tick')))
