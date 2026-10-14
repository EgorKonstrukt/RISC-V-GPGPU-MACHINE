# The games in their boxes on the shelf and the desk, from scans of real boxes:
#   python world/gameboxes.py
# Downloads go to build/gameboxes. Written: Textures/World/GameBoxes.png (every box's six faces)
# and GameBoxes_p.png (their gloss), and world/gameboxes.json (each box's size and faces).
import json
import os
import struct
import subprocess
import urllib.request
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "..", "build", "gameboxes")
OUT = os.path.join(HERE, "..", "unity", "ShaderEmu", "Textures", "World")
FACES = ("front", "back", "left", "right", "top", "bottom")
ATLAS = 4096
GUTTER = 8   # pixels between faces, filled with each face's own edge

# A face's picture as it is seen from outside: the top's top edge is at the back, the bottom's at
# the front, a side's left edge at the front (right) or the back (left). Sizes are millimetres.
BBC = "https://bigboxcollection.com/images/"
BOXES = {
    # bigboxcollection.com: six pictures and a gloss map a face; three.js's box takes them as they are
    "doom": dict(bbc="DoomShareware", gloss="_default/14", mm=(167, 211, 19)),
    "quake": dict(bbc="Quake1", gloss="Quake1", mm=(182, 232, 43)),
    "steel_sky": dict(bbc="BeneathASteelSky", gloss="BeneathASteelSky", mm=(184, 232, 47)),
    # bigboxdb.com: a box's model, its faces in one KTX2 picture
    "red_alert": dict(bbdb="command-and-conquer-red-alert", variant=47, gloss=0.85),
    # gamebox.museum: photographs of the six faces on grass, in one picture. Each face's corners
    # there, upper left, lower left, lower right, upper right of the face as it is seen
    "command_conquer": dict(museum="GRSjvfkvzUKCsGVbIuFrOA", gloss=0.85, corners={
        "front": ((34, 48), (34, 2100), (1500, 2100), (1500, 48)),
        "back": ((1557, 30), (1557, 2102), (3057, 2102), (3057, 30)),
        "left": ((35, 2603), (1655, 2603), (1655, 2150), (35, 2150)),
        "right": ((35, 3062), (1655, 3062), (1655, 2615), (35, 2615)),
        "top": ((3042, 2603), (3042, 2155), (1718, 2155), (1718, 2603)),
        "bottom": ((1718, 2618), (1718, 3060), (3044, 3060), (3044, 2618))}),
}


def get(url, name, data=None):
    path = os.path.join(CACHE, name)
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path
    os.makedirs(os.path.dirname(path), exist_ok=True)
    request = urllib.request.Request(url, data=data, headers={"User-Agent": "Mozilla/5.0 ShaderEmu-world-build"})
    with urllib.request.urlopen(request, timeout=120) as response, open(path + ".part", "wb") as out:
        out.write(response.read())
    os.replace(path + ".part", path)
    return path


def quad(img, corners, size):
    """The face whose corners in `img` are `corners`, drawn upright at `size` pixels."""
    (a, b, c, d) = corners
    return img.transform(size, Image.QUAD, (*a, *b, *c, *d), Image.BICUBIC)


def from_bbc(box):
    faces, gloss = {}, {}
    for face in FACES:
        faces[face] = Image.open(get(BBC + "textures/%s/%s.webp" % (face, box["bbc"]), "%s/%s.webp" % (box["bbc"], face))).convert("RGB")
        g = box["gloss"] if box["gloss"].startswith("_") else "%s/%s" % (face, box["gloss"])
        gloss[face] = Image.open(get(BBC + "specularmaps/%s.webp" % g, "%s/gloss_%s.webp" % (box["bbc"], face))).convert("L")
    return faces, gloss


def from_bbdb(box):
    """The model's six faces, from where its corners lie in its picture."""
    slug = box["bbdb"]
    meta = json.load(open(get("https://www.bigboxdb.com/api/variants/%d" % box["variant"], slug + "/variant.json")))
    glb = open(get("https://www.bigboxdb.com" + meta["textureFileName"], slug + "/box.glb"), "rb").read()
    n = struct.unpack("<I", glb[12:16])[0]
    gltf, blob = json.loads(glb[20:20 + n]), glb[28 + n:]

    def view(i):
        v = gltf["bufferViews"][i]
        return blob[v.get("byteOffset", 0):v.get("byteOffset", 0) + v["byteLength"]]
    ktx2 = os.path.join(CACHE, slug, "box.ktx2")
    open(ktx2, "wb").write(view(gltf["images"][0]["bufferView"]))
    basis = os.path.join(CACHE, "basis")
    for name in ("basis_transcoder.js", "basis_transcoder.wasm"):
        get("https://unpkg.com/three@0.160.0/examples/jsm/libs/basis/" + name, "basis/" + name)
    w, h = map(int, subprocess.check_output(["node", os.path.join(HERE, "gamebox_ktx2.cjs"), basis, ktx2, ktx2 + ".rgba"], text=True).split())
    img = Image.frombytes("RGBA", (w, h), open(ktx2 + ".rgba", "rb").read()).convert("RGB")
    prim = gltf["meshes"][0]["primitives"][0]["attributes"]
    count = gltf["accessors"][prim["POSITION"]]["count"]
    pos = np.frombuffer(view(gltf["accessors"][prim["POSITION"]]["bufferView"]), "<f4").reshape(count, 3) * (1, 1, -1)   # glTF's front is +z, ours -z
    nrm = np.frombuffer(view(gltf["accessors"][prim["NORMAL"]]["bufferView"]), "<f4").reshape(count, 3) * (1, 1, -1)
    uv = np.frombuffer(view(gltf["accessors"][prim["TEXCOORD_0"]]["bufferView"]), "<f4").reshape(count, 2)
    half = pos.max(0)
    faces = {}
    for face, (normal, right, up) in AXES.items():
        mine = [i for i in range(count) if np.dot(nrm[i], normal) > 0.9]
        def corner(su, sv):
            want = np.array(normal) * half + su * np.array(right) * half + sv * np.array(up) * half
            i = min(mine, key=lambda i: np.abs(pos[i] - want).sum())
            return tuple(uv[i] * (w, h))
        faces[face] = (img, (corner(-1, 1), corner(-1, -1), corner(1, -1), corner(1, 1)))
    mm = tuple(float(x) * 25.4 for x in (meta["w"], meta["h"], meta["d"]))   # inches
    return faces, mm


def from_museum(box):
    xml = json.load(open(get("https://www.gamebox.museum/api/scans.php", box["museum"] + "/scan.json",
                             ("action=getScan&UID=" + box["museum"]).encode())))["message"]
    scan = ET.fromstring(xml)
    base = next(f for f in scan.iter("Feature") if f.findtext("UVTemplate") == "defaultBaseBox")
    img = Image.open(get("https://www.gamebox.museum/files/" + base.findtext("TextureFile"), box["museum"] + "/base.jpg")).convert("RGB")
    mm = tuple(float(scan.findtext(k)) * 10 for k in "XYZ")   # centimetres
    return {face: (img, box["corners"][face]) for face in FACES}, mm


# a face's outward normal, and the box's axes along its picture's right and up (x right, y up, the front at -z)
AXES = {"front": ((0, 0, -1), (1, 0, 0), (0, 1, 0)), "back": ((0, 0, 1), (-1, 0, 0), (0, 1, 0)),
        "left": ((-1, 0, 0), (0, 0, -1), (0, 1, 0)), "right": ((1, 0, 0), (0, 0, 1), (0, 1, 0)),
        "top": ((0, 1, 0), (1, 0, 0), (0, 0, 1)), "bottom": ((0, -1, 0), (1, 0, 0), (0, 0, -1))}


def face_mm(face, mm):
    w, h, d = mm
    return {"front": (w, h), "back": (w, h), "left": (d, h), "right": (d, h), "top": (w, d), "bottom": (w, d)}[face]


def layout(sizes, scale):
    """Each box a block: its sides, front and back in a row, its top and bottom under the front and
    back; blocks in shelves. The faces' pixel rectangles, or None if they do not fit."""
    px = lambda v: int(round(v * scale))
    rects, x, y, row = {}, 0, 0, 0
    for name, (w, h, d) in sorted(sizes.items(), key=lambda kv: -kv[1][1]):
        cols = [("left", px(d)), ("front", px(w)), ("right", px(d)), ("back", px(w))]
        wide = sum(c for _, c in cols) + GUTTER * 4
        high = px(h) + px(d) + GUTTER * 2
        if x + wide > ATLAS:
            x, y, row = 0, y + row, 0
        if y + high > ATLAS or wide > ATLAS:
            return None
        cx = x + GUTTER // 2
        for face, c in cols:
            rects[name, face] = (cx, y + GUTTER // 2, c, px(h))
            if face in ("front", "back"):
                rects[name, "top" if face == "front" else "bottom"] = (cx, y + GUTTER // 2 + px(h) + GUTTER, c, px(d))
            cx += c + GUTTER
        x += wide
        row = max(row, high)
    return rects


def bleed(atlas, x, y, w, h):
    """Spread a face's edge pixels into the gutter round it, so mip maps do not mix in a neighbour."""
    g = GUTTER // 2
    a = atlas
    a[y:y + h, max(x - g, 0):x] = a[y:y + h, x:x + 1]
    a[y:y + h, x + w:x + w + g] = a[y:y + h, x + w - 1:x + w]
    a[max(y - g, 0):y, max(x - g, 0):x + w + g] = a[y:y + 1, max(x - g, 0):x + w + g]
    a[y + h:y + h + g, max(x - g, 0):x + w + g] = a[y + h - 1:y + h, max(x - g, 0):x + w + g]


def main():
    sources, sizes = {}, {}
    for name, box in BOXES.items():
        if "bbc" in box:
            faces, gloss = from_bbc(box)
            sources[name] = ({f: (faces[f], None) for f in FACES}, gloss)
            sizes[name] = box["mm"]
        else:
            faces, sizes[name] = (from_bbdb if "bbdb" in box else from_museum)(box)
            sources[name] = (faces, box["gloss"])
    scale = 6.0
    while layout(sizes, scale) is None:
        scale -= 0.05
    rects = layout(sizes, scale)
    colour = np.full((ATLAS, ATLAS, 3), 128, np.uint8)
    shine = np.zeros((ATLAS, ATLAS), np.uint8)
    out = {}
    for name, (faces, gloss) in sources.items():
        out[name] = {"size": [v / 1000 for v in sizes[name]], "faces": {}}
        for face in FACES:
            x, y, w, h = rects[name, face]
            img, corners = faces[face]
            pic = img.resize((w, h), Image.LANCZOS) if corners is None else quad(img, corners, (w, h))
            colour[y:y + h, x:x + w] = np.asarray(pic)
            g = gloss[face].resize((w, h), Image.BILINEAR) if isinstance(gloss, dict) else Image.new("L", (w, h), int(gloss * 255))
            shine[y:y + h, x:x + w] = np.asarray(g)
            bleed(colour, x, y, w, h)
            bleed(shine, x, y, w, h)
            # (v up from the atlas's bottom, as atlas.json's; half a texel in from the edges)
            out[name]["faces"][face] = [(x + 0.5) / ATLAS, 1 - (y + h - 0.5) / ATLAS, (x + w - 0.5) / ATLAS, 1 - (y + 0.5) / ATLAS]
    Image.fromarray(colour).save(os.path.join(OUT, "GameBoxes.png"))
    # gloss: a laminated print where the map is white, a matt card where it is black
    smooth = 0.30 + 0.35 * (shine.astype(np.float32) / 255)
    packed = np.dstack([np.ones_like(smooth), 1 - smooth, np.ones_like(smooth)])   # occlusion, roughness, metal (as textures.save)
    Image.fromarray((packed * 255 + 0.5).astype(np.uint8)).resize((ATLAS // 4, ATLAS // 4), Image.BILINEAR).save(os.path.join(OUT, "GameBoxes_p.png"))
    json.dump(out, open(os.path.join(HERE, "gameboxes.json"), "w"), indent=1)
    print("game boxes: %.2f pixels a millimetre, %s" % (scale, ", ".join("%s %.0f x %.0f x %.0f mm" % ((n,) + tuple(s)) for n, s in sizes.items())))


if __name__ == "__main__":
    main()
