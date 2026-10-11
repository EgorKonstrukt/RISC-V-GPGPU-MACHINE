#!/usr/bin/env python3
"""The path tracer's models as C: triangles ready for the ray's test, and a tree of boxes over
them (docs/raytrace.md, "Meshes").

    python3 make_meshes.py TEAPOT.obj BUNNY.ply OUT.c

A model is put where it stands in the box (the unit cube, y up, the eye before z = 0) here, so
the tracer has no transform to do. The tree is split where the boxes' surfaces times their
triangles are least (twelve places an axis are tried), down to four triangles a leaf.
"""
import math
import struct
import sys

LEAF = 4
BINS = 12


def read_obj(path):
    points, faces = [], []
    for line in open(path, encoding='latin1'):
        part = line.split()
        if not part:
            continue
        if part[0] == 'v':
            points.append(tuple(float(x) for x in part[1:4]))
        elif part[0] == 'f':
            corner = [int(x.split('/')[0]) - 1 for x in part[1:]]
            for k in range(1, len(corner) - 1):
                faces.append((corner[0], corner[k], corner[k + 1]))
    return points, faces


def read_ply(path):
    lines = open(path, encoding='latin1').read().split('\n')
    points_n = faces_n = 0
    at = 0
    for at, line in enumerate(lines):
        part = line.split()
        if part[:2] == ['element', 'vertex']:
            points_n = int(part[2])
        elif part[:2] == ['element', 'face']:
            faces_n = int(part[2])
        elif part[:1] == ['end_header']:
            break
    body = lines[at + 1:]
    points = [tuple(float(x) for x in body[i].split()[:3]) for i in range(points_n)]
    faces = []
    for i in range(points_n, points_n + faces_n):
        part = [int(x) for x in body[i].split()]
        for k in range(2, part[0]):
            faces.append((part[1], part[k], part[k + 1]))
    return points, faces


def knot(turns_p=2, turns_q=3, around=180, across=14, tube=0.085):
    """A torus knot, a tube round its curve."""
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


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def unit(a):
    d = math.sqrt(dot(a, a))
    return (a[0] / d, a[1] / d, a[2] / d) if d > 1e-20 else (0.0, 1.0, 0.0)


def placed(points, turn, size, centre_x, centre_z, floor, flip_z=False):
    """Turned about y, made `size` across its widest of x and z, stood on y = floor (or centred there with floor < 0)."""
    c, s = math.cos(math.radians(turn)), math.sin(math.radians(turn))
    out = [(p[0] * c + p[2] * s, p[1], (-p[0] * s + p[2] * c) * (-1.0 if flip_z else 1.0)) for p in points]
    low = [min(p[k] for p in out) for k in range(3)]
    high = [max(p[k] for p in out) for k in range(3)]
    by = size / max(high[0] - low[0], high[2] - low[2])
    mid = [(low[k] + high[k]) / 2 for k in range(3)]
    base = low[1] if floor >= 0 else mid[1]
    return [((p[0] - mid[0]) * by + centre_x, (p[1] - base) * by + abs(floor), (p[2] - mid[2]) * by + centre_z) for p in out]


def outward(points, faces):
    """Every triangle wound so that its normal leaves the model: by the sign of its volume."""
    volume = 0.0
    for a, b, c in faces:
        volume += dot(points[a], cross(points[b], points[c]))
    return faces if volume > 0 else [(a, c, b) for a, b, c in faces]


def smooth_normals(points, faces):
    """A corner's normal: the mean of the faces round its point, points in one place counted one."""
    key = {}
    place = [key.setdefault((round(p[0], 5), round(p[1], 5), round(p[2], 5)), len(key)) for p in points]
    total = [[0.0, 0.0, 0.0] for _ in key]
    for a, b, c in faces:
        n = cross(sub(points[b], points[a]), sub(points[c], points[a]))   # (as long as the face is large)
        for corner in (a, b, c):
            for k in range(3):
                total[place[corner]][k] += n[k]
    return [unit(total[place[i]]) for i in range(len(points))]


class Tree:
    def __init__(self, points, faces):
        self.nodes = []      # (low, high, a, b)
        self.order = []      # the triangles, a leaf's together
        boxes = []
        for f in faces:
            corner = [points[i] for i in f]
            low = tuple(min(p[k] for p in corner) for k in range(3))
            high = tuple(max(p[k] for p in corner) for k in range(3))
            boxes.append((low, high, tuple((low[k] + high[k]) / 2 for k in range(3))))
        self.boxes = boxes
        self.nodes.append(None)
        self.build(0, list(range(len(faces))))

    @staticmethod
    def around(boxes):
        low = tuple(min(b[0][k] for b in boxes) for k in range(3))
        high = tuple(max(b[1][k] for b in boxes) for k in range(3))
        return low, high

    @staticmethod
    def surface(low, high):
        d = [high[k] - low[k] for k in range(3)]
        return d[0] * d[1] + d[1] * d[2] + d[2] * d[0]

    def build(self, at, items):
        low, high = self.around([self.boxes[i] for i in items])
        if len(items) <= LEAF:
            self.nodes[at] = (low, high, len(self.order), len(items))
            self.order += items
            return
        best = None
        for axis in range(3):
            a = min(self.boxes[i][2][axis] for i in items)
            b = max(self.boxes[i][2][axis] for i in items)
            if b - a < 1e-9:
                continue
            bins = [[] for _ in range(BINS)]
            for i in items:
                bins[min(BINS - 1, int((self.boxes[i][2][axis] - a) / (b - a) * BINS))].append(i)
            for cut in range(1, BINS):
                left = [i for part in bins[:cut] for i in part]
                right = [i for part in bins[cut:] for i in part]
                if not left or not right:
                    continue
                cost = (self.surface(*self.around([self.boxes[i] for i in left])) * len(left)
                        + self.surface(*self.around([self.boxes[i] for i in right])) * len(right))
                if best is None or cost < best[0]:
                    best = (cost, left, right)
        if best is None:   # (all in one place: halves)
            best = (0, items[:len(items) // 2], items[len(items) // 2:])
        first = len(self.nodes)
        self.nodes += [None, None]
        self.nodes[at] = (low, high, first, 0)
        self.build(first, best[1])
        self.build(first + 1, best[2])


def words(values):
    return ', '.join('0x%08x' % struct.unpack('<I', struct.pack('<f', v))[0] for v in values)


def write(out, name, points, faces):
    faces = outward(points, [f for f in faces if len(set(f)) == 3])
    normals = smooth_normals(points, faces)
    tree = Tree(points, faces)
    out.write('static union pt_word %s_nodes[][8] = {\n' % name)
    for low, high, a, b in tree.nodes:
        # (a hair wider: the tracer's test is in floats, and a ray along a box's side must not miss it)
        out.write('\t{{%s}, {%du}, {%s}, {%du}},\n' % ('}, {'.join(words([x - 1e-5 for x in low]).split(', ')), a,
                                                       '}, {'.join(words([x + 1e-5 for x in high]).split(', ')), b))
    out.write('};\nstatic union pt_word %s_triangles[][18] = {\n' % name)
    for i in tree.order:
        a, b, c = faces[i]
        row = list(points[a]) + list(sub(points[b], points[a])) + list(sub(points[c], points[a]))
        row += list(normals[a]) + list(normals[b]) + list(normals[c])
        out.write('\t{{%s}},\n' % '}, {'.join(words(row).split(', ')))
    out.write('};\n')
    deepest = [0]

    def depth(at, d):
        deepest[0] = max(deepest[0], d)
        if tree.nodes[at][3] == 0:
            depth(tree.nodes[at][2], d + 1)
            depth(tree.nodes[at][2] + 1, d + 1)
    depth(0, 1)
    assert deepest[0] <= 60, deepest[0]
    print('%s: %d triangles, %d boxes, %d deep' % (name, len(faces), len(tree.nodes), deepest[0]))
    return len(faces), len(tree.nodes)


def main():
    teapot, bunny, path = sys.argv[1:4]
    models = []
    with open(path, 'w') as out:
        out.write('/* Made by make_meshes.py: the path tracer\'s models, where they stand in the box. */\n#include "tracer.h"\n\n')
        points, faces = read_obj(teapot)
        models.append(('teapot', 'a teapot (Newell\'s, of Utah)', (0.80, 0.78, 0.74)) + write(out, 'teapot', placed(points, 28, 0.80, 0.5, 0.56, 0.0), faces))
        points, faces = read_ply(bunny)
        models.append(('bunny', 'a rabbit (Stanford\'s scan)', (0.78, 0.66, 0.55)) + write(out, 'bunny', placed(points, -20, 0.62, 0.52, 0.56, 0.0), faces))
        points, faces = knot()
        models.append(('knot', 'a knot', (0.85, 0.70, 0.35)) + write(out, 'knot', placed(points, 20, 0.72, 0.5, 0.56, -0.42), faces))
        out.write('\nstruct pt_mesh pt_meshes[] = {\n')
        for name, label, colour, triangles, nodes in models:
            out.write('\t{"%s", (const struct pt_node *)%s_nodes, (const struct pt_triangle *)%s_triangles, %d, %d, {%s}},\n'
                      % (label, name, name, nodes, triangles, ', '.join('%.2ff' % c for c in colour)))
        out.write('};\nint pt_mesh_count = %d;\n' % len(models))


if __name__ == '__main__':
    main()
