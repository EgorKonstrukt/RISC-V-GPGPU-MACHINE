# Leads laid by Blender's cloth solver: each is a thin square tube, straight when at rest, held
# at its two plugs and let fall on the things of its place. Unity's coordinates in and out.
import hashlib
import json
import math
import os
import random

import bmesh
import bpy
from mathutils import Vector

import lib

SCALE = 10.0     # the solver is unsteady with millimetres: the place is simulated ten times its size
STEP = 0.008     # a lead's rings are so far apart
PIN = 0.014      # so much of each end is held, straight out of its plug
FRAMES = 200
VERSION = 12      # of the solver's settings below: a change to them makes every kept result stale
# pull is the tube's wall: at 20 a lead stands as it was laid out, at 8 it holds an arc in the air, at 5 it crumples like string
TUNE = dict(quality=6, mass=0.1, pull=6.0, bend=1.0, damp=5.0, bend_damp=2.0, air=12.0, near=0.005, self_near=0.008,
            friction=30.0, gravity=1.0, outer=0.01)
TUNE.update(json.loads(os.environ.get("DRAPE_SET", "{}")))   # (to try settings from outside)
DEBUG = bool(os.environ.get("DRAPE_DEBUG"))
CACHE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "drape")


def _blender(p):
    return Vector((p[0], p[2], p[1])) * SCALE


def _resampled(path, step):
    points = [Vector(p) for p in path]
    total = sum((b - a).length for a, b in zip(points, points[1:]))
    n = max(2, int(round(total / step)))
    out, k, done = [points[0].copy()], 0, 0.0
    for i in range(1, n):
        want = total * i / n
        while done + (points[k + 1] - points[k]).length < want:
            done += (points[k + 1] - points[k]).length
            k += 1
        out.append(points[k].lerp(points[k + 1], (want - done) / (points[k + 1] - points[k]).length))
    out.append(points[-1].copy())
    return out


def _rings(points, radius):
    """Four corners round every point, carried along without a twist."""
    n, rings = len(points), []
    side = None
    for i in range(n):
        tangent = (points[min(i + 1, n - 1)] - points[max(i - 1, 0)]).normalized()
        side = tangent.orthogonal().normalized() if side is None else (side - tangent * side.dot(tangent)).normalized()
        other = tangent.cross(side)
        rings.append([points[i] + (side * math.cos(a) + other * math.sin(a)) * radius for a in (0.0, math.pi / 2, math.pi, 3 * math.pi / 2)])
    return rings


def _box(name, centre, size, turn=0.0):
    mesh = bpy.data.meshes.new(name)
    h = Vector(size) / 2
    corners = [Vector((x * h.x, y * h.y, z * h.z)) for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)]
    corners = [lib.place(centre, (0, turn, 0)) @ p for p in corners]
    mesh.from_pydata([_blender(p) for p in corners], [], [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)])
    return mesh


def _cylinder(name, centre, radius, height, segs=40):
    mesh = bpy.data.meshes.new(name)
    ring = [(math.cos(2 * math.pi * k / segs) * radius, math.sin(2 * math.pi * k / segs) * radius) for k in range(segs)]
    points = [Vector((centre[0] + x, centre[1] + y, centre[2] + z)) for y in (-height / 2, height / 2) for x, z in ring]
    faces = [(k, (k + 1) % segs, segs + (k + 1) % segs, segs + k) for k in range(segs)] + [tuple(range(segs)), tuple(range(segs, 2 * segs))]
    mesh.from_pydata([_blender(p) for p in points], [], faces)
    return mesh


def _simplified(points, tolerance):
    """Douglas and Peucker's: the fewest of the points that stay within `tolerance` of all."""
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    spans = [(0, len(points) - 1)]
    while spans:
        a, b = spans.pop()
        line = points[b] - points[a]
        far, at = 0.0, None
        for k in range(a + 1, b):
            d = (points[k] - points[a]).cross(line).length / line.length if line.length > 1e-9 else (points[k] - points[a]).length
            if d > far:
                far, at = d, k
        if at is not None and far > tolerance:
            keep[at] = True
            spans += [(a, at), (at, b)]
    return [p for p, k in zip(points, keep) if k]


def curve(points, steps=8):
    """Catmull and Rom's curve through every point, `steps` to a span."""
    points = [Vector(p) for p in points]
    ends = [points[0] * 2 - points[1]] + points + [points[-1] * 2 - points[-2]]
    out = []
    for i in range(1, len(ends) - 2):
        p0, p1, p2, p3 = ends[i - 1], ends[i], ends[i + 1], ends[i + 2]
        for k in range(steps):
            t = k / steps
            out.append(0.5 * ((2 * p1) + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (3 * p1 - p0 - 3 * p2 + p3) * t * t * t))
    return out + [points[-1]]


def ruffled(path, rng, up=0.05, wander=0.006, held=0.04):
    """A laid-out lead made one of its own before it falls: lifted in a few humps of chance
    heights (slack, which falls to one side or the other) and wandering a little, but for its ends."""
    points = _resampled(path, 0.01)
    total = 0.01 * (len(points) - 1)
    waves = [(rng.uniform(0.3, 1.0), total / rng.uniform(0.12, 0.4), rng.uniform(0, 2 * math.pi)) for _ in range(3)]
    out = []
    for k, p in enumerate(points):
        along = k * 0.01
        near = min(along, total - along)
        hold = min(1.0, max(0.0, (near - 0.02) / held)) ** 2
        t = along / total
        lift = up * waves[0][0] * (0.5 + 0.5 * math.sin(2 * math.pi * waves[0][1] * t + waves[0][2]))
        out.append(p + hold * Vector((wander * math.sin(2 * math.pi * waves[1][1] * t + waves[1][2]), lift,
                                      wander * math.sin(2 * math.pi * waves[2][1] * t + waves[2][2]))))
    return out


def check(leads, boxes, cylinders, ends=0.03, within=0.0025, step=0.003, quiet=False):
    """Says which lead (name, path, radius) lies in a box or a cylinder, in itself further along
    or in another lead; not of its ends, which are in their plugs. Nothing said: none does."""
    said, sampled = [], []

    def say(what):
        if what not in said:
            said.append(what)
            if not quiet:
                print("LEADS", what)
    frames = [(lib.place(centre, (0, turn, 0)).inverted(), Vector(size) / 2) for centre, size, turn in boxes]
    for name, path, radius in leads:
        points, along, total = [], [], 0.0
        for a, c in zip(path, path[1:]):
            n = max(1, int((c - a).length / step))
            for k in range(n):
                points.append(a.lerp(c, k / n))
                along.append(total + (c - a).length * k / n)
            total += (c - a).length
        sampled.append((name, [q for q, far in zip(points, along) if ends <= far <= total - ends], along, radius))
        for q, far in zip(points, along):
            if far < ends or far > total - ends:
                continue
            for k, (frame, half) in enumerate(frames):
                local = frame @ q
                if min(half[i] + radius - abs(local[i]) for i in range(3)) > within:
                    say("%s is in box %d" % (name, k))
            for k, (centre, round_, height) in enumerate(cylinders):
                if abs(q.y - centre[1]) < height / 2 + radius - within and math.hypot(q.x - centre[0], q.z - centre[2]) < round_ + radius - within:
                    say("%s is in cylinder %d" % (name, k))
        for k, q in enumerate(points):
            for m in range(k + 1, len(points)):
                if along[m] - along[k] > 0.03 and (points[m] - q).length < 1.7 * radius:
                    say("%s crosses itself" % name)
                    break
    for i in range(len(sampled)):
        for j in range(i + 1, len(sampled)):
            near = min((q - w).length for q in sampled[i][1] for w in sampled[j][1])
            if near < 0.85 * (sampled[i][3] + sampled[j][3]):
                say("%s and %s cross (their middles %.1f mm apart)" % (sampled[i][0], sampled[j][0], near * 1000))
    return said


def settle(leads, boxes, cylinders, stiff=1.0, again=2):
    """leads: (name, path, radius). boxes: (centre, size, turn about y). cylinders: (centre, radius,
    height), upright. Gives each lead's path as it comes to lie; kept in build/drape by what was asked."""
    asked = repr([VERSION, SCALE, STEP, PIN, FRAMES, stiff, sorted(TUNE.items()),
                  [(name, [tuple(round(v, 5) for v in p) for p in path], round(radius, 5)) for name, path, radius in leads],
                  [[tuple(round(v, 5) for v in part) if isinstance(part, (tuple, list)) else round(part, 5) for part in thing] for thing in list(boxes) + list(cylinders)]])
    kept = os.path.join(CACHE, hashlib.sha1(asked.encode()).hexdigest()[:20] + ".json")
    if os.path.exists(kept) and not DEBUG:
        return [[Vector(p) for p in path] for path in json.load(open(kept))]

    scene = bpy.context.scene
    made = []

    def linked(name, mesh):
        bm = bmesh.new()   # faces outwards: the solver pushes a lead to the side a face looks to
        bm.from_mesh(mesh)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        bm.to_mesh(mesh)
        bm.free()
        ob = bpy.data.objects.new(name, mesh)
        scene.collection.objects.link(ob)
        made.append(ob)
        return ob
    for k, (centre, size, turn) in enumerate(boxes):
        ob = linked("drape box %d" % k, _box("drape box", centre, size, turn))
        ob.modifiers.new("Collision", 'COLLISION')
        ob.collision.thickness_outer, ob.collision.cloth_friction, ob.collision.damping = TUNE['outer'], TUNE['friction'], 0.8
    for k, (centre, radius, height) in enumerate(cylinders):
        ob = linked("drape cylinder %d" % k, _cylinder("drape cylinder", centre, radius, height))
        ob.modifiers.new("Collision", 'COLLISION')
        ob.collision.thickness_outer, ob.collision.cloth_friction, ob.collision.damping = TUNE['outer'], TUNE['friction'], 0.8

    start, rest, faces, pinned, spans = [], [], [], [], []
    for index, (name, path, radius) in enumerate(leads):
        points = _resampled(path, STEP)
        length = sum((b - a).length for a, b in zip(points, points[1:]))
        straight = [Vector((length * k / (len(points) - 1), 0.0, index * 0.1)) for k in range(len(points))]
        base = len(start)
        hold = int(math.ceil(PIN / STEP)) + 1
        for k, (ring, flat) in enumerate(zip(_rings(points, radius), _rings(straight, radius))):
            start += [_blender(p) for p in ring]
            rest += [_blender(p) for p in flat]
            if k < hold or k >= len(points) - hold:
                pinned += range(base + 4 * k, base + 4 * k + 4)
            if k:
                faces += [(base + 4 * (k - 1) + s, base + 4 * (k - 1) + (s + 1) % 4, base + 4 * k + (s + 1) % 4, base + 4 * k + s) for s in range(4)]
        faces += [tuple(range(base, base + 4)), tuple(range(base + 4 * len(points) - 4, base + 4 * len(points)))]
        spans.append((base, len(points)))
    mesh = bpy.data.meshes.new("drape leads")
    mesh.from_pydata(start, [], faces)
    ob = linked("drape leads", mesh)
    ob.shape_key_add(name="Basis")
    straightened = ob.shape_key_add(name="rest")
    for k, p in enumerate(rest):
        straightened.data[k].co = p
    straightened.value = 0.0
    group = ob.vertex_groups.new(name="pin")
    group.add(list(pinned), 1.0, 'REPLACE')
    cloth = ob.modifiers.new("Cloth", 'CLOTH')
    s = cloth.settings
    s.quality = int(TUNE['quality'])
    s.mass = TUNE['mass']
    s.tension_stiffness = s.compression_stiffness = s.shear_stiffness = TUNE['pull'] * stiff
    s.bending_stiffness = TUNE['bend'] * stiff
    s.tension_damping = s.compression_damping = s.shear_damping = TUNE['damp']
    s.bending_damping = TUNE['bend_damp']
    s.air_damping = TUNE['air']
    s.effector_weights.gravity = TUNE['gravity']
    s.vertex_group_mass = "pin"
    s.rest_shape_key = straightened
    if DEBUG:
        print("DRAPE rest shape", s.rest_shape_key, "pinned", len(pinned), "of", len(start))
    c = cloth.collision_settings
    c.use_collision, c.collision_quality, c.distance_min, c.friction = True, 5, TUNE['near'], TUNE['friction']
    c.use_self_collision, c.self_distance_min, c.self_friction = TUNE['self_near'] > 0, max(TUNE['self_near'], 0.001), 10.0
    cloth.point_cache.frame_start, cloth.point_cache.frame_end = 1, FRAMES
    was = (scene.frame_start, scene.frame_end, scene.frame_current)
    scene.frame_start, scene.frame_end = 1, FRAMES

    def lying():
        shaped = ob.evaluated_get(bpy.context.evaluated_depsgraph_get()).data.vertices
        out = []
        for base, count in spans:
            line = []
            for k in range(count):
                p = sum((shaped[base + 4 * k + q].co for q in range(4)), Vector()) / 4 / SCALE
                line.append(Vector((p.x, p.z, p.y)))
            out.append(line)
        return out
    before = None
    for frame in range(1, FRAMES + 1):
        scene.frame_set(frame)
        if frame == FRAMES - 10:
            before = lying()
        if DEBUG and frame % 10 == 0:
            now = lying()
            low = ["%s %.1f" % (lead[0], 1000 * min([p.y - lead[2] for p in line if p.y > -0.1] or [0])) for lead, line in zip(leads, now)]
            print("DRAPE frame %d, lowest over the table (mm): %s" % (frame, ", ".join(low)))
    paths = lying()
    moved = max((p - q).length for a, b in zip(paths, before) for p, q in zip(a, b))
    print("DRAPE %d leads, %d frames: still moving %.1f mm in the last ten" % (len(leads), FRAMES, moved * 1000))
    if DEBUG:
        for (name, path, radius), line in zip(leads, paths):
            was_long = sum((b - a).length for a, b in zip(path, path[1:]))
            print("DRAPE %s: %.0f mm long, now %.0f, moving %.1f" % (name, was_long * 1000, 1000 * sum((b - a).length for a, b in zip(line, line[1:])),
                                                                   1000 * max((p - q).length for p, q in zip(line, before[leads.index((name, path, radius))]))))
    for thing in made:
        data = thing.data
        bpy.data.objects.remove(thing)
        bpy.data.meshes.remove(data)
    scene.frame_start, scene.frame_end = was[0], was[1]
    scene.frame_set(was[2])

    out = []
    for line in paths:
        for _ in range(2):   # the solver's last tremble out of it
            line = [line[0]] + [(a + b * 2 + c) / 4 for a, b, c in zip(line, line[1:], line[2:])] + [line[-1]]
        out.append(_simplified(line, 0.0003))
    wrong = check([(name, line, radius) for (name, _, radius), line in zip(leads, out)], boxes, cylinders, quiet=True)
    if wrong and again:   # a lead came to lie in something: shaken a little, they fall another way
        print("DRAPE again:", "; ".join(wrong))
        rng = random.Random(again)
        shaken = []
        for name, path, radius in leads:
            points = _resampled(path, 0.01)
            shaken.append((name, [p + Vector((0, rng.uniform(0, 0.004), 0)) if 5 < k < len(points) - 6 else p for k, p in enumerate(points)], radius))
        out = settle(shaken, boxes, cylinders, stiff, again - 1)
    os.makedirs(CACHE, exist_ok=True)
    json.dump([[tuple(p) for p in line] for line in out], open(kept, "w"))
    return out
