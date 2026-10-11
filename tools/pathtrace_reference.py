"""Checks the path tracer's tree of boxes (docs/raytrace.md, "Meshes"): every eye ray against
every triangle of the model, no tree, and the nearest compared with what the guest found.

    python tools/pathtrace_reference.py MESHES.c MODEL LOG [OUT.png]

MESHES.c is what linux/pathtrace/make_meshes.py wrote (in ~/shaderemu-linux/src/pathtrace-build),
MODEL its name there (teapot, bunny, knot), LOG the console of
`NXPATH_DEPTH=1 NXPATH_PRINT=1 NXPATH_SAMPLES=1 NXPATH_SCENE=N nxpath`. A ray that misses the
model is not compared (the room and the ball are the tracer's own arithmetic).
"""
import re
import struct
import sys
import numpy as np


def triangles(path, name):
    text = open(path, encoding='latin1').read()
    body = text[text.index('%s_triangles[][18]' % name):]
    body = body[:body.index('};')]
    words = [int(w, 16) for w in re.findall(r'0x([0-9a-f]{8})', body)]
    return np.frombuffer(struct.pack('<%dI' % len(words), *words), dtype=np.float32).reshape(-1, 18).astype(np.float64)


def main():
    tri = triangles(sys.argv[1], sys.argv[2])
    rows = {}
    for line in open(sys.argv[3], encoding='latin1'):
        part = line.split()
        if part[:1] == ['pathrow'] and len(part) == 3:
            rows[int(part[1])] = part[2]
    height, width = max(rows) + 1, len(rows[0]) // 6
    guest = np.array([[int(rows[y][i:i + 6], 16) for i in range(0, width * 6, 6)] for y in range(height)]) / 4000000.0
    eye = np.array([0.5, 0.5, -1.35])
    zoom = 0.5 / (height / 2)
    xs, ys = np.meshgrid(np.arange(width), np.arange(height))
    way = np.stack([(xs + 0.5 - width / 2) * zoom, (height / 2 - ys - 0.5) * zoom, np.full(xs.shape, 1.35)], axis=-1).reshape(-1, 3)
    way /= np.linalg.norm(way, axis=1, keepdims=True)
    corner, e1, e2 = tri[:, 0:3], tri[:, 3:6], tri[:, 6:9]
    s = eye - corner
    q = np.cross(s, e1)                                   # (the same for every ray: they all leave the eye)
    best = np.full(len(way), np.inf)
    for start in range(0, len(way), 512):
        d = way[start:start + 512, None, :]
        p = np.cross(d, e2[None])
        det = (e1[None] * p).sum(-1)
        with np.errstate(divide='ignore', invalid='ignore'):
            inv = 1.0 / det
            u = (s[None] * p).sum(-1) * inv
            w = (d * q[None]).sum(-1) * inv
            t = (e2 * q).sum(-1)[None] * inv
        hit = (np.abs(det) > 1e-12) & (u >= 0) & (u <= 1) & (w >= 0) & (u + w <= 1) & (t > 0.0005)
        best[start:start + 512] = np.where(hit, t, np.inf).min(axis=1)
    best = best.reshape(height, width)
    on = np.isfinite(best)
    # the tracer's nearest thing is the model wherever the model is nearer than the room's walls and the ball
    nearer = on & (best < guest + 1e-3)
    off = np.abs(guest - best)
    same = nearer & (off < 2e-4)
    print('%s: %d triangles; %d of %d rays meet it, %d of those are what the guest has nearest' % (sys.argv[2], len(tri), on.sum(), on.size, nearer.sum()))
    print('  the same distance (within 0.0002): %d; not: %d (largest difference %.5f)' % (same.sum(), (nearer & ~same).sum(), off[nearer].max() if nearer.any() else 0))
    # and where the guest sees the model but no triangle is there, or the other way round
    behind = on & (guest > best + 1e-3)
    print('  rays whose triangle the guest went past: %d' % behind.sum())
    if len(sys.argv) > 4:
        from PIL import Image
        shade = np.clip((guest - 1.2) / 1.4, 0, 1)
        picture = np.stack([255 - shade * 235] * 3, axis=-1).astype(np.uint8)
        picture[guest == 0] = (20, 20, 60)
        picture[behind | (nearer & ~same)] = (255, 0, 0)
        Image.fromarray(picture).save(sys.argv[4])
    return 0 if behind.sum() == 0 and (nearer & ~same).sum() == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
