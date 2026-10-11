# The lightmap's layout of an object that brings its own (lib.Builder.chart): no face over another.
#   blender -b build/world.blend --python world/check_charts.py -- "Station 0 rest" build/charts.npz
#   python world/check_charts.py build/charts.npz [OUT.png]
# The faces are drawn into a picture of the layout in their order and again backwards: where the
# two differ, two faces lie on one place. (A line a texel wide where two faces meet does not count.)
import sys
import numpy as np

try:
    import bpy
except ImportError:
    bpy = None

if bpy is not None:
    name, out = sys.argv[sys.argv.index('--') + 1:][:2]
    mesh = bpy.data.objects.get(name).data
    layer = mesh.uv_layers.get("UV2")
    if layer is None:
        print("CHARTS", name, "has no layout of its own")
        sys.exit(1)
    uv = np.array([d.uv[:] for d in layer.data])
    charts = mesh.attributes.get("chart")
    np.savez(out, uv=uv, start=np.array([p.loop_start for p in mesh.polygons]), count=np.array([p.loop_total for p in mesh.polygons]),
             chart=np.array([d.value for d in charts.data]) if charts is not None else np.zeros(len(mesh.polygons), dtype=np.int64))
    print("CHARTS %s: %d faces written" % (name, len(mesh.polygons)))
    sys.exit(0)

from PIL import Image, ImageDraw

SIZE = 2048
data = np.load(sys.argv[1])
uv, ids = data["uv"], data["chart"]
faces = [[tuple(p) for p in uv[s:s + c] * SIZE] for s, c in zip(data["start"], data["count"])]
print("%d faces, %d charts, layout from (%.3f, %.3f) to (%.3f, %.3f)" % (len(faces), len(set(ids.tolist())), uv[:, 0].min(), uv[:, 1].min(), uv[:, 0].max(), uv[:, 1].max()))


def drawn(order):
    picture = Image.new("I", (SIZE, SIZE), 0)
    draw = ImageDraw.Draw(picture)
    for k in order:
        draw.polygon(faces[k], fill=k + 1)
    return np.array(picture)


first, last = drawn(range(len(faces))), drawn(range(len(faces) - 1, -1, -1))
differ = first != last
inside = differ.copy()   # only places whose neighbours differ too: a face's inside, not its edge
for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1), (2, 0), (-2, 0), (0, 2), (0, -2)):
    inside &= np.roll(np.roll(differ, dx, axis=1), dy, axis=0)
covered = first > 0
print("%.1f%% of the square is used; faces over one another: %d texels of %d (%.3f%%)" % (100.0 * covered.mean(), inside.sum(), covered.sum(), 100.0 * inside.sum() / max(1, covered.sum())))
by_chart = np.concatenate([[0], ids])[first]
if inside.sum():
    which, counts = np.unique(by_chart[inside], return_counts=True)
    print("  by chart:", ", ".join("%d: %d" % (w, c) for w, c in sorted(zip(which, counts), key=lambda t: -t[1])[:8]))
    areas = {int(w): int((by_chart == w).sum()) for w in which}
    print("  those charts' own texels:", areas)
if len(sys.argv) > 2:
    rgb = np.stack([(by_chart * 97) % 200 + 55, (by_chart * 57) % 200 + 55, (by_chart * 23) % 200 + 55], axis=-1).astype(np.uint8)
    rgb[~covered] = 16
    rgb[inside] = (255, 0, 0)
    Image.fromarray(rgb[::-1]).resize((1024, 1024)).save(sys.argv[2])
