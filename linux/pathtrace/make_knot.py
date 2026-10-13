"""Writes a torus knot as a Wavefront .obj file: one of the example meshes of /root/Models
(docs/raytrace.md), the one that is made and not fetched.

    python3 make_knot.py OUT.obj
"""
import math
import sys


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def unit(a):
    length = math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
    return (a[0] / length, a[1] / length, a[2] / length)


def knot(turns_p=2, turns_q=3, around=180, across=14, tube=0.085):
    """A tube round the knot's curve: its points, and its faces by their points' numbers."""
    def curve(t):
        r = 1.0 + 0.45 * math.cos(turns_q * t)
        return (r * math.cos(turns_p * t), 0.45 * math.sin(turns_q * t) * 1.3, r * math.sin(turns_p * t))
    points, faces = [], []
    for i in range(around):
        t = 2 * math.pi * i / around
        p, ahead = curve(t), curve(t + 0.001)
        tangent = unit(sub(ahead, p))
        side = unit(cross(tangent, (0.0, 1.0, 0.0)))
        up = cross(side, tangent)
        for j in range(across):
            a = 2 * math.pi * j / across
            points.append(tuple(p[k] + tube * 3.0 * (math.cos(a) * side[k] + math.sin(a) * up[k]) for k in range(3)))
    for i in range(around):
        for j in range(across):
            a, b = i * across + j, i * across + (j + 1) % across
            c, d = ((i + 1) % around) * across + j, ((i + 1) % around) * across + (j + 1) % across
            faces += [(a, c, b), (b, c, d)]
    return points, faces


points, faces = knot()
with open(sys.argv[1], 'w', newline='\n') as out:
    out.write('# a torus knot (linux/pathtrace/make_knot.py)\n')
    for p in points:
        out.write('v %.5f %.5f %.5f\n' % p)
    for f in faces:
        out.write('f %d %d %d\n' % (f[0] + 1, f[1] + 1, f[2] + 1))
