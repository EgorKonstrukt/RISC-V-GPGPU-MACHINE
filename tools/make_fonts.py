"""Makes the image's fonts (docs/system/fonts.md): files the GPU draws glyphs from where they lie in the ROM.

    python tools\\make_fonts.py [OUT]     (OUT: build/images/linux/root/usr/share)

Liberation Sans, Serif and Mono, regular and bold, at nine sizes, as four bits of coverage a
pixel (FreeType's hinted outlines, through Pillow), and GNU Unifont, a bit a pixel, for every
character of the basic plane the others lack. The desktop's own text is Selawik (font-ui-N,
font-ui-b-N: Microsoft's open stand-in for Segoe UI), at the sizes of UI_SIZES. The fonts are
fetched once into build/fetch/fonts and never kept in this repository. `--check FILE` reads a
font back and prints what it holds; `--ui` makes the desktop's alone.

The outline faces are multi-channel distance fields (docs/system/fonts.md, "Distance fields"): one atlas
a face (font-NAME.msdf, made by msdf-atlas-gen, fetched into build/fetch/msdf) and at each size
a .sef with that size's metrics and a table into the atlas. `--bitmap` makes the bitmaps of
before; `--msdf-from N` keeps bitmaps below N pixels.
"""
import concurrent.futures, gzip, io, json, os, re, struct, subprocess, sys, tarfile, tempfile, time, urllib.request, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FETCH = os.path.join(ROOT, 'build', 'fetch', 'fonts')
LIBERATION = 'https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz'
UNIFONT = 'https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/unifont-16.0.04.hex.gz'
SIZES = [10, 11, 13, 15, 17, 20, 24, 28, 32]
SELAWIK = 'https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip'
UI_SIZES = [11, 12, 13, 15, 17]
UI_LARGE = [20, 24, 32]        # sizes the desktop's faces have as distance fields alone (a .sef of 6 KB each)
UI_FACES = [('ui', 'selawk.ttf'), ('ui-b', 'selawkb.ttf')]
FACES = [('sans', 'LiberationSans-Regular'), ('sans-b', 'LiberationSans-Bold'),
         ('serif', 'LiberationSerif-Regular'), ('serif-b', 'LiberationSerif-Bold'),
         ('mono', 'LiberationMono-Regular'), ('mono-b', 'LiberationMono-Bold')]
# what the outline faces are asked for: Latin, Greek, Cyrillic, punctuation, signs
MSDF_GEN = 'https://github.com/Chlumsky/msdf-atlas-gen/releases/download/v1.4/msdf-atlas-gen-1.4-win64.zip'
ATLAS_EM, ATLAS_RANGE = 24, 4   # the atlas's pixels an em, and its distance range in those pixels
SEF_MSDF = 24                   # a .sef's bits a pixel when its glyphs are an atlas's (three bytes)
FIT_UP_TO = 17                  # sizes whose quads are fitted to FreeType's hinted bitmaps of that size
FIT_TRIES = [(i / 8, 0.88 + 0.06 * j) for i in range(-4, 5) for j in range(5)]    # shifts (pixels), stretches
FIT_FINER = [(i / 16, 0.03 * j) for i in (-1, 0, 1) for j in (-1, 0, 1)]           # then round the best
WANTED = [(0x20, 0x7e), (0xa0, 0x24f), (0x2b0, 0x2ff), (0x370, 0x52f), (0x1e00, 0x1eff), (0x2000, 0x206f),
          (0x20a0, 0x20bf), (0x2100, 0x214f), (0x2190, 0x21ff), (0x2200, 0x22ff), (0x2500, 0x25ff)]


def fetched(name, address):
    path = os.path.join(FETCH, name)
    if not os.path.exists(path):
        os.makedirs(FETCH, exist_ok=True)
        with urllib.request.urlopen(address, timeout=60) as reply, open(path, 'wb') as out:
            out.write(reply.read())
    return path


def pad(data, to):
    return data + bytes(-len(data) % to)


def write_font(path, height, ascent, bpp, em, glyphs):
    """glyphs: {codepoint: (advance, dx, dy, width, height, packed rows)} -> a .sef file."""
    codes = sorted(glyphs)
    ranges, first = [], 0
    for i, cp in enumerate(codes):
        if i and cp != codes[i - 1] + 1:
            ranges.append((codes[first], i - first, first))
            first = i
    ranges.append((codes[first], len(codes) - first, first))
    missing = codes.index(0xfffd) if 0xfffd in glyphs else codes.index(0x3f) if 0x3f in glyphs else 0xffffffff
    ranges_off = 64
    adv_off = ranges_off + 12 * len(ranges)
    table_off = (adv_off + len(codes) + 15) & ~15
    bits_off = table_off + 16 * len(codes)
    advances, table, bits = bytearray(), bytearray(), bytearray()
    for cp in codes:
        advance, dx, dy, w, h, rows = glyphs[cp]
        advances.append(min(advance, 255))
        table += struct.pack('<IIII', bits_off + len(bits) if w and h else 0, w, h, (dx & 255) | (dy & 255) << 8)
        bits += pad(rows, 4)
    head = struct.pack('<4sHHHHIIIIIIII', b'SEF1', height, ascent, bpp, em, len(codes), len(ranges), ranges_off,
                       adv_off, table_off, bits_off, bits_off + len(bits), missing)
    body = pad(head, 64) + b''.join(struct.pack('<III', *r) for r in ranges) + bytes(advances)
    with open(path, 'wb') as out:
        out.write(pad(body, 16) + bytes(table) + bytes(bits))
    return bits_off + len(bits)


def outline_font(ttf, em, gap=0):
    from PIL import ImageFont
    from fontTools.ttLib import TTFont
    font = ImageFont.truetype(io.BytesIO(ttf), em)
    have = TTFont(io.BytesIO(ttf), lazy=True).getBestCmap()
    ascent, descent = font.getmetrics()
    glyphs = {}
    for lo, hi in WANTED:
        for cp in range(lo, hi + 1):
            if cp not in have:
                continue
            ch = chr(cp)
            advance = int(font.getlength(ch) + 0.5)
            box = font.getbbox(ch)
            mask = font.getmask(ch, mode='L')
            w, h = mask.size
            if not w or not h or not box:
                glyphs[cp] = (advance, 0, 0, 0, 0, b'')
                continue
            # sixteen levels, two pixels a byte, the left one in the high half
            rows = bytearray()
            for y in range(h):
                level = [(mask.getpixel((x, y)) * 15 + 127) // 255 for x in range(w)]
                level.append(0)
                rows += bytes(level[x] << 4 | level[x + 1] for x in range(0, w, 2))
            # (gap: at least so many pixels after a glyph's ink, so a small colon is not run into its neighbours)
            advance = max(advance, box[0] + w + gap) if gap else advance
            glyphs[cp] = (advance, max(-128, min(127, box[0])), max(-128, min(127, box[1])), w, h, bytes(rows))
    return ascent + descent, ascent, glyphs


def unifont():
    glyphs = {}
    with gzip.open(fetched('unifont.hex.gz', UNIFONT), 'rt') as lines:
        for line in lines:
            code, _, rows = line.strip().partition(':')
            cp = int(code, 16)
            if cp < 0x20 or cp > 0xffff or 0xd800 <= cp < 0xf900 or not rows:    # (no surrogates, no private use)
                continue
            w = len(rows) // 4      # 32 digits: 8 wide; 64: 16 wide
            glyphs[cp] = (w, 0, 0, w, 16, bytes.fromhex(rows))
    return glyphs


def msdf_tool():
    """msdf-atlas-gen 1.4 (Viktor Chlumsky, MIT licence), fetched once."""
    exe = os.path.join(ROOT, 'build', 'fetch', 'msdf', 'v1.4', 'msdf-atlas-gen', 'msdf-atlas-gen.exe')
    if not os.path.exists(exe):
        path = os.path.join(ROOT, 'build', 'fetch', 'msdf', 'msdf-atlas-gen-1.4-win64.zip')
        if not os.path.exists(path):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with urllib.request.urlopen(MSDF_GEN, timeout=60) as reply, open(path, 'wb') as out:
                out.write(reply.read())
        zipfile.ZipFile(path).extractall(os.path.join(ROOT, 'build', 'fetch', 'msdf', 'v1.4'))
    return exe


def msdf_atlas(ttf, codes, path, sizes):
    """The face's distance fields as font-NAME.msdf: {codepoint: (offset, w, h, plane bounds in ems)}.
    A glyph is its box's rows of three bytes a pixel; its plane bounds go from the centre of the
    box's first pixel to that of its last."""
    with tempfile.TemporaryDirectory() as work:
        font, charset = os.path.join(work, 'face.ttf'), os.path.join(work, 'charset.txt')
        open(font, 'wb').write(ttf)
        open(charset, 'w').write(', '.join(map(str, codes)))
        subprocess.run([msdf_tool(), '-font', font, '-charset', charset, '-type', 'msdf', '-format', 'bin',
                        '-imageout', os.path.join(work, 'atlas.bin'), '-json', os.path.join(work, 'atlas.json'),
                        '-size', str(ATLAS_EM), '-pxrange', str(ATLAS_RANGE), '-yorigin', 'top'],
                       check=True, capture_output=True)
        layout = json.load(open(os.path.join(work, 'atlas.json')))
        width = layout['atlas']['width']
        image = open(os.path.join(work, 'atlas.bin'), 'rb').read()
    data, glyphs = bytearray(), {}
    for g in layout['glyphs']:
        box, plane = g.get('atlasBounds'), g.get('planeBounds')
        if not box or not plane:
            continue
        x0, y0 = int(box['left']), int(min(box['top'], box['bottom']))
        w, h = int(box['right']) - x0 + 1, int(max(box['top'], box['bottom'])) - y0 + 1
        at = 64 + len(data)
        for y in range(y0, y0 + h):
            data += image[3 * (y * width + x0):3 * (y * width + x0 + w)]
        data += bytes(-len(data) % 4)
        glyphs[g['unicode']] = (at, w, h, plane, g['advance'])
    head = struct.pack('<4sHHII', b'SEM1', ATLAS_EM, ATLAS_RANGE, len(glyphs), len(sizes))
    head += b''.join(struct.pack('<HH', em, height) for em, height in sizes)
    body = pad(head, 64) + pad(bytes(data), 16) + bytes(16)   # (a pixel's pair may read a word past a glyph)
    with open(path, 'wb') as out:
        out.write(body)
    return glyphs, body


def msdf_quad(entry, glyph, em, ascent):
    """A glyph's quad at size em, in pixels from the pen and the line's top: x, y, width, height.
    The hinted advance's rounding is shared out on both sides of the outline."""
    b, advance = entry[3], entry[4]
    return (b['left'] * em + (glyph[0] - advance * em) / 2, ascent + b['top'] * em,
            (b['right'] - b['left']) * em, (b['bottom'] - b['top']) * em)


def fit_quads(task):
    """{cp: (box, quad, bitmap)} -> {cp: quad} fitted to the hinted bitmap: the outline's coverage over
    each pixel's box (from the fields, 4 samples an atlas pixel) against the bitmap's, by a shift and
    a stretch of the quad, across then down, twice. box: bytes of rows of 3 a pixel, its width and
    height; bitmap: dx, dy, w, h, rows."""
    import numpy as np

    def median(s):
        return np.maximum(np.minimum(s[..., 0], s[..., 1]), np.minimum(np.maximum(s[..., 0], s[..., 1]), s[..., 2]))

    def at(edges, t):
        t = np.clip(t, edges[0], edges[-1])
        i = np.clip(np.searchsorted(edges, t) - 1, 0, len(edges) - 2)
        return i, (t - edges[i]) / (edges[i + 1] - edges[i])

    def cover(sat, eu, ev, qx, qw, qy, qh, cols, rows):
        # rows x tries x cols: the summed area over each pixel's box, for quads (qx, qw) and (qy, qh)
        vi0, vf0 = at(ev, (rows - qy) / qh)
        vi1, vf1 = at(ev, (rows + 1 - qy) / qh)
        t = sat[vi1] * (1 - vf1)[:, None] + sat[vi1 + 1] * vf1[:, None] - sat[vi0] * (1 - vf0)[:, None] - sat[vi0 + 1] * vf0[:, None]
        ui0, uf0 = at(eu, (cols[None, :] - qx[:, None]) / qw[:, None])
        ui1, uf1 = at(eu, (cols[None, :] + 1 - qx[:, None]) / qw[:, None])
        val = t[:, ui1] * (1 - uf1) + t[:, ui1 + 1] * uf1 - t[:, ui0] * (1 - uf0) - t[:, ui0 + 1] * uf0
        return val * (qw * qh)[None, :, None]

    out = {}
    for cp, ((data, aw, ah), quad, (bx, by, bw, bh, rows)) in task.items():
        box = np.frombuffer(data, np.uint8).reshape(ah, aw, 3) / 255
        kv, ku = (ah - 1) * 4 + 1, (aw - 1) * 4 + 1
        ay, ax = np.linspace(0, ah - 1, kv), np.linspace(0, aw - 1, ku)
        xi, yi = np.minimum(ax.astype(int), aw - 2), np.minimum(ay.astype(int), ah - 2)
        fx, fy = (ax - xi)[None, :, None], (ay - yi)[:, None, None]
        s = (box[yi][:, xi] * (1 - fx) + box[yi][:, xi + 1] * fx) * (1 - fy) + \
            (box[yi + 1][:, xi] * (1 - fx) + box[yi + 1][:, xi + 1] * fx) * fy
        sat = np.zeros((kv + 1, ku + 1))
        sat[1:, 1:] = np.clip(ATLAS_RANGE * 4 * (median(s) - 0.5) + 0.5, 0, 1).cumsum(0).cumsum(1) / ((kv - 1) * (ku - 1))
        eu, ev = (np.arange(ku + 1) - 0.5) / (ku - 1), (np.arange(kv + 1) - 0.5) / (kv - 1)
        packed = np.frombuffer(rows, np.uint8).reshape(bh, (bw + 1) // 2)
        bitmap = np.stack([packed >> 4, packed & 15], 2).reshape(bh, -1)[:, :bw] / 15
        qx, qy, qw, qh = quad
        x0, y0 = int(np.floor(min(qx, bx))) - 1, int(np.floor(min(qy, by))) - 1
        x1, y1 = int(np.ceil(max(qx + qw, bx + bw))) + 1, int(np.ceil(max(qy + qh, by + bh))) + 1
        want = np.zeros((y1 - y0, x1 - x0))
        want[by - y0:by - y0 + bh, bx - x0:bx - x0 + bw] = bitmap
        cols, lines = np.arange(x0, x1, dtype=float), np.arange(y0, y1, dtype=float)
        for _ in range(4):
            across = _ % 2 == 0
            tries, p, n = np.array(FIT_TRIES), (qx if across else qy), (qw if across else qh)
            for finer in (0, 1):
                mp, mn = p + n / 2 * (1 - tries[:, 1]) + tries[:, 0], n * tries[:, 1]
                got = cover(sat, eu, ev, mp, mn, qy, qh, cols, lines) - want[:, None, :] if across else \
                    cover(sat.T, ev, eu, mp, mn, qx, qw, lines, cols) - want.T[:, None, :]
                k = np.argmin((got ** 2).sum(axis=(0, 2)) + 0.02 * (tries[:, 0] ** 2 + (tries[:, 1] - 1) ** 2) * want.sum())
                best, tries = (mp[k], mn[k]), tries[k] + np.array(FIT_FINER)
            qx, qw, qy, qh = best + (qy, qh) if across else (qx, qw) + best
        out[cp] = (qx, qy, qw, qh)
    return out


def fit_on(pool, fonts, atlas, body):
    """[(em, {cp: quad} to come)] for the sizes up to FIT_UP_TO, fitted on the pool in parts of 200 glyphs."""
    parts = []
    for em, height, ascent, glyphs in fonts:
        if em > FIT_UP_TO:
            continue
        task = {}
        for cp in sorted(glyphs):
            g = glyphs[cp]
            if cp in atlas and g[3] and g[4]:
                at, w, h = atlas[cp][:3]
                task[cp] = ((bytes(body[at:at + 3 * w * h]), w, h), msdf_quad(atlas[cp], g, em, ascent), g[1:])
            if len(task) == 200:
                parts.append((em, pool.submit(fit_quads, task)))
                task = {}
        if task:
            parts.append((em, pool.submit(fit_quads, task)))
    return parts


def write_msdf_font(path, height, ascent, em, glyphs, atlas, quads):
    """A .sef whose table is into the face's atlas: a texel a glyph of where its pixels are,
    their width | height << 16, the quad's width | height << 16 and where its corner is from the
    character's (x | y << 16, signed), the last two in 64ths of a pixel at this size. quads: the
    fitted ones (else msdf_quad's)."""
    def fixed(v):
        return int(round(v * 64))
    codes = sorted(glyphs)
    ranges, first = [], 0
    for i, cp in enumerate(codes):
        if i and cp != codes[i - 1] + 1:
            ranges.append((codes[first], i - first, first))
            first = i
    ranges.append((codes[first], len(codes) - first, first))
    missing = codes.index(0xfffd) if 0xfffd in glyphs else codes.index(0x3f) if 0x3f in glyphs else 0xffffffff
    adv_off = 64 + 12 * len(ranges)
    table_off = (adv_off + len(codes) + 15) & ~15
    size = table_off + 16 * len(codes)
    advances, table = bytearray(), bytearray()
    for cp in codes:
        advances.append(min(glyphs[cp][0], 255))
        if cp not in atlas:
            table += bytes(16)
            continue
        at, w, h = atlas[cp][:3]
        dx, dy, qw, qh = (fixed(v) for v in quads.get(cp) or msdf_quad(atlas[cp], glyphs[cp], em, ascent))
        table += struct.pack('<IIII', at, w | h << 16, qw | qh << 16, (dx & 0xffff) | (dy & 0xffff) << 16)
    head = struct.pack('<4sHHHHIIIIIIIII', b'SEF1', height, ascent, SEF_MSDF, em, len(codes), len(ranges), 64,
                       adv_off, table_off, size, size, missing, ATLAS_RANGE * em * 65536 // ATLAS_EM)
    body = pad(head, 64) + b''.join(struct.pack('<III', *r) for r in ranges) + bytes(advances)
    with open(path, 'wb') as out:
        out.write(pad(body, 16) + bytes(table))
    return size


def check(path):
    data = open(path, 'rb').read()
    magic, height, ascent, bpp, em, count, nranges, ranges_off, adv_off, table_off, bits_off, size, missing = \
        struct.unpack_from('<4sHHHHIIIIIIII', data)
    assert magic == b'SEF1' and size == len(data) and table_off % 16 == 0, 'not a font of ours'
    covered = 0
    for i in range(nranges):
        first, n, glyph = struct.unpack_from('<III', data, ranges_off + 12 * i)
        assert glyph == covered and n > 0
        covered += n
    assert covered == count
    atlas = open(re.sub(r'-\d+\.sef$', '.msdf', path), 'rb').read() if bpp == SEF_MSDF else None
    for g in range(count):
        at, w, h, _ = struct.unpack_from('<IIII', data, table_off + 16 * g)
        if atlas is not None:
            assert not at or at % 4 == 0 and at + 3 * (w & 0xffff) * (w >> 16) <= len(atlas), 'glyph %d' % g
            continue
        row = (w + 7) // 8 if bpp == 1 else (w + 1) // 2
        assert at % 4 == 0 and (not w or bits_off <= at and at + row * h <= size), 'glyph %d' % g
    print('%s: %d glyphs in %d ranges, height %d, ascent %d, %s, %d bytes' %
          (os.path.basename(path), count, nranges, height, ascent,
           'distance fields (an atlas of %d bytes)' % len(atlas) if atlas else '%d bit' % bpp, size))


def make_face(out, name, ttf, sizes, gap, msdf_from, pool):
    """A face at its sizes: bitmaps below msdf_from, else the atlas and a table a size. What writes
    the files once the pool has fitted the quads, and returns their bytes."""
    fonts = [(em,) + outline_font(ttf, em, gap) for em in sizes]
    atlas, body, parts = None, b'', []
    if any(em >= msdf_from for em in sizes):
        atlas, body = msdf_atlas(ttf, sorted(fonts[0][3]), os.path.join(out, 'font-%s.msdf' % name),
                                 [(em, height) for em, height, _, _ in fonts if em >= msdf_from])
        parts = fit_on(pool, [f for f in fonts if f[0] >= msdf_from], atlas, body)
    elif os.path.exists(os.path.join(out, 'font-%s.msdf' % name)):
        os.remove(os.path.join(out, 'font-%s.msdf' % name))

    def finish():
        total, quads = len(body), {}
        for em, part in parts:
            quads.setdefault(em, {}).update(part.result())
        for em, height, ascent, glyphs in fonts:
            path = os.path.join(out, 'font-%s-%d.sef' % (name, em))
            if em >= msdf_from:
                total += write_msdf_font(path, height, ascent, em, glyphs, atlas, quads.get(em, {}))
            else:
                total += write_font(path, height, ascent, 4, em, glyphs)
        return total
    return finish


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--check':
        for path in sys.argv[2:]:
            check(path)
        return
    argv, msdf_from = [a for a in sys.argv[1:] if a not in ('--ui', '--bitmap')], 0
    if '--msdf-from' in argv:
        msdf_from = int(argv.pop(argv.index('--msdf-from') + 1))
        argv.remove('--msdf-from')
    if '--bitmap' in sys.argv:
        msdf_from = 1000
    out = argv[0] if argv else os.path.join(ROOT, 'build', 'images', 'linux', 'root', 'usr', 'share')
    os.makedirs(out, exist_ok=True)
    began, total, count, finish = time.time(), 0, 0, []
    pool = concurrent.futures.ProcessPoolExecutor(min(8, os.cpu_count() or 1))
    selawik = zipfile.ZipFile(fetched('Selawik_Release.zip', SELAWIK))
    for name, face in UI_FACES:
        sizes = UI_SIZES + [em for em in UI_LARGE if em >= msdf_from]
        finish.append(make_face(out, name, selawik.read(face), sizes, 1, msdf_from, pool))
        count += len(sizes)
    if '--ui' in sys.argv:
        total += sum(f() for f in finish)
        print('%d fonts, %d bytes, %.1f s' % (count, total, time.time() - began))
        return
    archive = tarfile.open(fetched('liberation.tar.gz', LIBERATION))
    members = {os.path.basename(m.name): m for m in archive.getmembers()}
    for name, face in FACES:
        finish.append(make_face(out, name, archive.extractfile(members[face + '.ttf']).read(), SIZES, 0, msdf_from, pool))
    total += sum(f() for f in finish)
    total += write_font(os.path.join(out, 'font-uni-16.sef'), 16, 14, 1, 16, unifont())
    print('%d fonts, %d bytes, %.1f s' % (count + len(FACES) * len(SIZES) + 1, total, time.time() - began))


if __name__ == '__main__':
    main()
