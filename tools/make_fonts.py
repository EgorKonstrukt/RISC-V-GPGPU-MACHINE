"""Makes the image's fonts (docs/system/fonts.md): files the GPU draws glyphs from where they lie in the ROM.

    python tools\\make_fonts.py [OUT]     (OUT: build/images/linux/root/usr/share)

Liberation Sans, Serif and Mono, regular and bold, at nine sizes, as four bits of coverage a
pixel (FreeType's hinted outlines, through Pillow), and GNU Unifont, a bit a pixel, for every
character of the basic plane the others lack. The fonts are fetched once into build/fetch/fonts
and never kept in this repository. `--check FILE` reads a font back and prints what it holds.
"""
import gzip, io, os, struct, sys, tarfile, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FETCH = os.path.join(ROOT, 'build', 'fetch', 'fonts')
LIBERATION = 'https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz'
UNIFONT = 'https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/unifont-16.0.04.hex.gz'
SIZES = [10, 11, 13, 15, 17, 20, 24, 28, 32]
FACES = [('sans', 'LiberationSans-Regular'), ('sans-b', 'LiberationSans-Bold'),
         ('serif', 'LiberationSerif-Regular'), ('serif-b', 'LiberationSerif-Bold'),
         ('mono', 'LiberationMono-Regular'), ('mono-b', 'LiberationMono-Bold')]
# what the outline faces are asked for: Latin, Greek, Cyrillic, punctuation, signs
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


def outline_font(ttf, em):
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
    for g in range(count):
        at, w, h, _ = struct.unpack_from('<IIII', data, table_off + 16 * g)
        row = (w + 7) // 8 if bpp == 1 else (w + 1) // 2
        assert at % 4 == 0 and (not w or bits_off <= at and at + row * h <= size), 'glyph %d' % g
    print('%s: %d glyphs in %d ranges, height %d, ascent %d, %d bit, %d bytes' %
          (os.path.basename(path), count, nranges, height, ascent, bpp, size))


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--check':
        for path in sys.argv[2:]:
            check(path)
        return
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'images', 'linux', 'root', 'usr', 'share')
    os.makedirs(out, exist_ok=True)
    began, total = time.time(), 0
    archive = tarfile.open(fetched('liberation.tar.gz', LIBERATION))
    members = {os.path.basename(m.name): m for m in archive.getmembers()}
    for name, face in FACES:
        ttf = archive.extractfile(members[face + '.ttf']).read()
        for em in SIZES:
            height, ascent, glyphs = outline_font(ttf, em)
            total += write_font(os.path.join(out, 'font-%s-%d.sef' % (name, em)), height, ascent, 4, em, glyphs)
    total += write_font(os.path.join(out, 'font-uni-16.sef'), 16, 14, 1, 16, unifont())
    print('%d fonts, %d bytes, %.1f s' % (len(FACES) * len(SIZES) + 1, total, time.time() - began))


main()
