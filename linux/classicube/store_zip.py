"""The texture pack for the machine (docs/games/classicube.md, "The texture pack's pictures"): entries
stored, and the pictures the start reads in one pack of PNGs, the first entry, each picture's own
entry a stub naming its place in it. A PNG not packed is a PNG in segments (png_segments.py).

    python3 store_zip.py IN.zip OUT.zip
"""
import os, struct, sys, zipfile, zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'libmedia'))
from png_segments import write_png, _chunk

PACK_NAME = 'shaderemu-pack.png'
PACK_WIDTH = 256	# pixels a row: 1,024 bytes, four rows a page
# Parts of under 1 MB, whose working memory libmedia keeps from one to the next; the first is
# decoded by the game's own thread, the rest beside it (8 against 4, 12 and 16: the quickest start)
PACK_PARTS = 8
# A part's segments: the game's 15 workers and the first core, which mcwx gives the first
# segment, a row stored, so that the thread that decodes the pack takes little of the game's core
SHARES = 16
# (the touch screen's buttons: no start reads them)
ON_THEIR_OWN = {'touch.png'}


def chunks(data):
    at = 8
    while at < len(data):
        n, kind = struct.unpack('>I4s', data[at:at + 8])
        yield kind, data[at + 8:at + 8 + n]
        at += 12 + n


def unfiltered(raw, width, height, bpp):
    """The rows' bytes without their filters, from rows with any of PNG's five."""
    row = width * bpp
    out = bytearray()
    prev = bytearray(row)
    for y in range(height):
        kind = raw[y * (row + 1)]
        cur = bytearray(raw[y * (row + 1) + 1:(y + 1) * (row + 1)])
        for i in range(row):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if kind == 1:
                cur[i] = (cur[i] + a) & 255
            elif kind == 2:
                cur[i] = (cur[i] + b) & 255
            elif kind == 3:
                cur[i] = (cur[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += cur
        prev = cur
    return bytes(out)


def rgba(parts, rows, colour):
    """The picture's red, green, blue and alpha bytes, or None for a kind left alone."""
    if colour == 6:
        return rows
    if colour == 3:
        # (an entry the palette lacks is opaque black, as in the game's decoder)
        palette = dict(parts).get(b'PLTE', b'')
        alpha = dict(parts).get(b'tRNS', b'')
        table = []
        for i in range(256):
            rgb = palette[i * 3:i * 3 + 3] if i * 3 + 3 <= len(palette) else bytes(3)
            table.append(rgb + bytes((alpha[i] if i < len(alpha) else 255,)))
        return b''.join(table[i] for i in rows)
    return None


def picture(data):
    """(width, height, RGBA bytes) of a PNG, or None for a kind not taken here."""
    parts = list(chunks(data))
    width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', parts[0][1])
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    if depth != 8 or interlace:
        return None   # (none of the pack's are)
    raw = zlib.decompress(b''.join(body for kind, body in parts if kind == b'IDAT'))
    pixels = rgba(parts, unfiltered(raw, width, height, channels), colour)
    return (width, height, pixels) if pixels else None




def part_png(pixels, segments=SHARES):
    """Rows of PACK_WIDTH words as a PNG in segments, the first a row, every row filtered with None
    (smaller here than with Sub or Up, and the quickest to undo)."""
    stride = PACK_WIDTH * 4
    height = len(pixels) // stride
    starts = [0] + [1 + (height - 1) * k // (segments - 1) for k in range(segments - 1)]
    starts = sorted(set(starts))
    idats, adler = [], 1
    for k, y0 in enumerate(starts):
        y1 = starts[k + 1] if k + 1 < len(starts) else height
        raw = b''.join(b'\0' + pixels[y * stride:(y + 1) * stride] for y in range(y0, y1))
        adler = zlib.adler32(raw, adler)
        z = zlib.compressobj(0 if k == 0 else 9, zlib.DEFLATED, -15)   # (the first core's: no codes to make)
        last = k == len(starts) - 1
        data = z.compress(raw) + z.flush(zlib.Z_FINISH if last else zlib.Z_FULL_FLUSH)
        if k == 0:
            data = b'\x78\xda' + data
        if last:
            data += struct.pack('>I', adler & 0xffffffff)
        idats.append(_chunk(b'IDAT', data))
    out = b'\x89PNG\r\n\x1a\n' + _chunk(b'IHDR', struct.pack('>IIBBBBB', PACK_WIDTH, height, 8, 6, 0, 0, 0))
    out += _chunk(b'mdPR', struct.pack('>I', len(starts)) + b''.join(struct.pack('>I', y) for y in starts))
    return out + b''.join(idats) + _chunk(b'IEND', b'')


def pack_pngs(pixels, parts=PACK_PARTS):
    """The pack: its rows in `parts` PNGs of whole pages, one after another."""
    pages = len(pixels) // 4096
    cuts = [pages * k // parts * 4096 for k in range(parts + 1)]
    return b''.join(part_png(pixels[cuts[k]:cuts[k + 1]]) for k in range(parts) if cuts[k + 1] > cuts[k])


def main():
    source = zipfile.ZipFile(sys.argv[1])
    entries, pack, placed = [], bytearray(), {}
    for info in source.infolist():
        data = source.read(info)
        name = info.filename
        if name.lower().endswith('.png'):
            got = picture(data)
            if got and os.path.basename(name).lower() not in ON_THEIR_OWN:
                width, height, rgba_bytes = got
                words = bytearray(len(rgba_bytes))
                words[0::4], words[1::4], words[2::4], words[3::4] = rgba_bytes[2::4], rgba_bytes[1::4], rgba_bytes[0::4], rgba_bytes[3::4]
                placed[name] = (len(pack), width, height)
                pack += words + bytes(-len(words) % 4096)
            elif got:
                data = write_png(None, got[0], got[1], got[2], 4)
        entries.append((info, data))
    pngs = pack_pngs(bytes(pack))
    name_, = struct.unpack('>I', pngs[-16:-12])
    with zipfile.ZipFile(sys.argv[2], 'w', zipfile.ZIP_STORED) as out:
        out.writestr(zipfile.ZipInfo(PACK_NAME, entries[0][0].date_time), pngs)
        for info, data in entries:
            if info.filename in placed:
                data = b'SEPACKED' + struct.pack('<IIII', name_, *placed[info.filename])
            out.writestr(zipfile.ZipInfo(info.filename, info.date_time), data)
    print(f'store_zip: {len(placed)} pictures packed, {len(pack)} bytes of pixels in {len(pngs)}')


if __name__ == '__main__':
    main()
