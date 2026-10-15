"""Micropolis's pictures as the GPU reads them (docs/micropolis.md, "The pictures"):

  tiles.bin    16 palette words, then the 960 map tiles of 16 x 16 bytes, a tile after another
  sprites.bin  256 palette words, 64 x 16 cells, a tile count in 16 bytes, then the tiles (byte 0: a hole)
  tools.bin    the 16 tools' icons and their lit twins, 32 x 32 words each, one under another
  overview.bin each map tile's mean colour, a word (0xRRGGBB) for each of the 960
  sounds.bin   "MPSOUNDS", count, rate; a 32-byte entry each (name, offset, length); unsigned bytes

    python3 pack.py MICROPOLIS_CONTENT OUT_FOLDER [SOUNDS_FOLDER of NAME.raw, 16-bit mono]
"""
import os, struct, sys, zlib

TOOLS = ['residential', 'commercial', 'industrial', 'firestation', 'policestation', 'query', 'wire',
         'bulldozer', 'railroad', 'road', 'stadium', 'park', 'seaport', 'coalpower', 'nuclearpower', 'airport']
# frames of sprite_T_N.png for each type T (1 to 8), in the order the front end counts them
FRAMES = [5, 8, 11, 8, 16, 3, 6, 4]
FACE = (192, 192, 192)   # ui.h's UI_FACE, which the icons' transparent edges become
SLOT_CELLS, SLOTS_ACROSS = 3, 21   # a sprite frame has 3 x 3 cells; 21 of them in a row of 64


def png(path):
    """(width, height, rows of (r, g, b, a)) of an 8-bit RGB or RGBA PNG."""
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    at, idat = 8, b''
    while at < len(data):
        size, kind = struct.unpack('>I4s', data[at:at + 8])
        body = data[at + 8:at + 8 + size]
        if kind == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and colour in (2, 6) and not interlace, '%s: depth %d colour %d' % (path, depth, colour)
        elif kind == b'IDAT':
            idat += body
        at += 12 + size
    step = 3 if colour == 2 else 4
    raw, stride, rows, above = zlib.decompress(idat), width * step, [], bytearray(width * step)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - step] if i >= step else 0
            up, corner = above[i], above[i - step] if i >= step else 0
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        rows.append([tuple(line[x * step:x * step + step]) + ((255,) if step == 3 else ()) for x in range(width)])
        above = line
    return width, height, rows


def words(values):
    return struct.pack('<%dI' % len(values), *values)


def tiles(content, out):
    width, height, rows = png(os.path.join(content, 'images', 'tiles.png'))
    colours = sorted({p[:3] for row in rows for p in row})
    assert width == 256 and height == 960 and len(colours) <= 16, (width, height, len(colours))
    index = {c: i for i, c in enumerate(colours)}
    body = bytearray()
    for tile in range(960):
        tx, ty = tile % 16 * 16, tile // 16 * 16
        for y in range(16):
            body += bytes(index[p[:3]] for p in rows[ty + y][tx:tx + 16])
    palette = [r << 16 | g << 8 | b for r, g, b in colours] + [0] * (16 - len(colours))
    open(os.path.join(out, 'tiles.bin'), 'wb').write(words(palette) + body)
    mean = []
    for tile in range(960):
        tx, ty = tile % 16 * 16, tile // 16 * 16
        sums = [sum(rows[ty + y][tx + x][k] for y in range(16) for x in range(16)) for k in range(3)]
        r, g, b = (round(v / 256) for v in sums)
        mean.append(r << 16 | g << 8 | b)
    open(os.path.join(out, 'overview.bin'), 'wb').write(words(mean))


def sprites(content, out):
    pictures = []
    for kind, count in enumerate(FRAMES, 1):
        for n in range(count):
            pictures.append(png(os.path.join(content, 'images', 'sprite_%d_%d.png' % (kind, n))))
    colours = sorted({p[:3] for _, _, rows in pictures for row in rows for p in row if p[3] >= 128})
    assert len(colours) < 256
    index = {c: i + 1 for i, c in enumerate(colours)}
    cells, made = [0] * (64 * 16), [bytes(256)]   # tile 0: nothing, under every cell no sprite covers
    for slot, (width, height, rows) in enumerate(pictures):
        cx, cy = slot % SLOTS_ACROSS * SLOT_CELLS, slot // SLOTS_ACROSS * SLOT_CELLS
        for ty in range(0, height, 16):
            for tx in range(0, width, 16):
                tile = bytes(index[rows[ty + y][tx + x][:3]] if rows[ty + y][tx + x][3] >= 128 else 0
                             for y in range(16) for x in range(16))
                cells[(cy + ty // 16) * 64 + cx + tx // 16] = len(made)
                made.append(tile)
    assert len(made) <= 1024
    palette = [0] + [r << 16 | g << 8 | b for r, g, b in colours]
    palette += [0] * (256 - len(palette))
    open(os.path.join(out, 'sprites.bin'), 'wb').write(
        words(palette) + struct.pack('<%dH' % len(cells), *cells) + words([len(made), 0, 0, 0]) + b''.join(made))


def tools(content, out):
    body = []
    for name in TOOLS:
        for lit in ('', 'hi'):
            width, height, rows = png(os.path.join(content, 'images', 'tool_%s%s.png' % (name, lit)))
            assert width == 64 and height == 64, name
            for y in range(32):
                for x in range(32):
                    sum4 = [0, 0, 0]
                    for dy in (0, 1):
                        for dx in (0, 1):
                            r, g, b, a = rows[2 * y + dy][2 * x + dx]
                            for k, c in enumerate((r, g, b)):
                                sum4[k] += c * a + FACE[k] * (255 - a)
                    r, g, b = (round(s / (4 * 255)) for s in sum4)
                    body.append(r << 16 | g << 8 | b)
    open(os.path.join(out, 'tools.bin'), 'wb').write(words(body))


def sounds(folder, out, rate=22050):
    """Each NAME.raw as unsigned bytes, its quiet end cut; files with the same samples share them."""
    names = sorted(f[:-4] for f in os.listdir(folder) if f.endswith('.raw'))
    entries, data, seen = [], bytearray(), {}
    for name in names:
        raw = open(os.path.join(folder, name + '.raw'), 'rb').read()
        samples = bytes((v >> 8) + 128 for v in struct.unpack('<%dh' % (len(raw) // 2), raw))
        end = len(samples)
        while end > 0 and abs(samples[end - 1] - 128) <= 1:
            end -= 1
        samples = samples[:end]
        if samples not in seen:
            seen[samples] = len(data)
            data += samples + bytes(-len(samples) % 16)
        entries.append((name, seen[samples], len(samples)))
    head = 16 + 32 * len(entries)
    table = b''.join(struct.pack('<24sII', n.encode(), head + at, length) for n, at, length in entries)
    open(os.path.join(out, 'sounds.bin'), 'wb').write(b'MPSOUNDS' + struct.pack('<II', len(entries), rate) + table + data)


def main():
    content, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    tiles(content, out)
    sprites(content, out)
    tools(content, out)
    if len(sys.argv) > 3:
        sounds(sys.argv[3], out)


if __name__ == '__main__':
    main()
