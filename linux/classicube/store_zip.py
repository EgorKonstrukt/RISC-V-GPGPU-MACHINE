"""The texture pack with nothing to decode: the zip's entries stored, and each picture's pixels
as the game keeps them (the patch's Png_Decode reads them in one piece). A PNG of a kind not
taken here stays a PNG, its pixels in stored blocks with no row filters.

    python3 store_zip.py IN.zip OUT.zip
"""
import struct, sys, zipfile, zlib

def chunks(data):
    at = 8
    while at < len(data):
        n, kind = struct.unpack('>I4s', data[at:at + 8])
        yield kind, data[at + 8:at + 8 + n]
        at += 12 + n

def chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)

def unfiltered(raw, width, height, bpp):
    """The rows with filter 0, from rows with any of PNG's five."""
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
        out += b'\x00' + cur
        prev = cur
    return bytes(out)

def decoded(parts, raw, width, height, colour):
    """The picture as the game's words of 0xAARRGGBB, low byte first, or None for a kind left alone."""
    row = len(raw) // height
    rows = b''.join(raw[y * row + 1:(y + 1) * row] for y in range(height))
    out = bytearray(width * height * 4)
    if colour == 6:
        out[0::4], out[1::4], out[2::4], out[3::4] = rows[2::4], rows[1::4], rows[0::4], rows[3::4]
        return bytes(out)
    if colour == 3:
        # (an entry the palette lacks is opaque black, as in the game's decoder)
        palette = dict(parts).get(b'PLTE', b'')
        alpha = dict(parts).get(b'tRNS', b'')
        table = []
        for i in range(256):
            rgb = palette[i * 3:i * 3 + 3] if i * 3 + 3 <= len(palette) else bytes(3)
            table.append(bytes((rgb[2], rgb[1], rgb[0], alpha[i] if i < len(alpha) else 255)))
        return b''.join(table[i] for i in rows)
    return None

def stored_png(data):
    parts = list(chunks(data))
    width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', parts[0][1])
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    if depth != 8 or interlace:
        return data   # (none of the pack's are: left as they are)
    raw = zlib.decompress(b''.join(body for kind, body in parts if kind == b'IDAT'))
    raw = unfiltered(raw, width, height, channels)
    pixels = decoded(parts, raw, width, height, colour)
    if pixels:
        return b'SEPIXELS' + struct.pack('<II', width, height) + pixels
    out, done = data[:8], False
    for kind, body in parts:
        if kind != b'IDAT':
            out += chunk(kind, body)
        elif not done:
            out += chunk(b'IDAT', zlib.compress(raw, 0))
            done = True
    return out

source = zipfile.ZipFile(sys.argv[1])
with zipfile.ZipFile(sys.argv[2], 'w', zipfile.ZIP_STORED) as out:
    for info in source.infolist():
        data = source.read(info)
        if info.filename.lower().endswith('.png'):
            data = stored_png(data)
        entry = zipfile.ZipInfo(info.filename, info.date_time)
        if data[:8] == b'SEPIXELS':
            # the pixels start on a word of the file: the kernel then copies them by the word
            pad = -(out.fp.tell() + 30 + len(info.filename.encode()) + 16) % 4
            entry.extra = struct.pack('<HH', 0x4553, pad) + bytes(pad)
        out.writestr(entry, data)
