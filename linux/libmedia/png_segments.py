"""PNG files in segments, as libmedia's encoder writes them (docs/system/media.md), for the build's tools:
each IDAT is rows of its own in a deflate stream that refers to nothing before it and ends on a
byte, its first row filtered without the row above, every row with a filter quick to undo, and the
mdPR chunk lists the segments' first rows. Any PNG reader reads them; libmedia's decodes every segment on a core of its own.

    from png_segments import write_png
    write_png(path_or_None, width, height, rgba_bytes_or_rgb_bytes, channels, rows_a_segment=16)
"""
import struct, zlib


def _chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)


def _filtered(row, prev, bpp, first):
    """The row with None, Sub or Up, whichever has the least sum of magnitudes (None or Sub for a
    segment's first). Average and Paeth are left out: undoing them is most of a decoder's time,
    and they made the build's pictures 0.1 to 1.7% smaller."""
    n = len(row)
    tries = [bytes([0]) + row]
    sub = bytearray(row)
    for i in range(bpp, n):
        sub[i] = (row[i] - row[i - bpp]) & 255
    tries.append(bytes([1]) + bytes(sub))
    if not first:
        tries.append(bytes([2]) + bytes((row[i] - prev[i]) & 255 for i in range(n)))
    return min(tries, key=lambda t: sum(v if v < 128 else 256 - v for v in t[1:]))


def write_png(path, width, height, pixels, channels, rows_a_segment=16, level=9):
    """Writes (or returns, with path None) a PNG of 8-bit RGB (3) or RGBA (4) pixels in segments."""
    stride = width * channels
    starts = list(range(0, height, rows_a_segment))
    idats, adler = [], 1
    for k, y0 in enumerate(starts):
        y1 = min(height, y0 + rows_a_segment)
        raw = bytearray()
        prev = None
        for y in range(y0, y1):
            row = bytes(pixels[y * stride:(y + 1) * stride])
            raw += _filtered(row, prev, channels, y == y0)
            prev = row
        adler = zlib.adler32(bytes(raw), adler)
        z = zlib.compressobj(level, zlib.DEFLATED, -15)
        last = k == len(starts) - 1
        data = z.compress(bytes(raw)) + z.flush(zlib.Z_FINISH if last else zlib.Z_FULL_FLUSH)
        if k == 0:
            data = b'\x78\xda' + data
        if last:
            data += struct.pack('>I', adler & 0xffffffff)
        idats.append(_chunk(b'IDAT', data))
    out = b'\x89PNG\r\n\x1a\n' + _chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6 if channels == 4 else 2, 0, 0, 0))
    out += _chunk(b'mdPR', struct.pack('>I', len(starts)) + b''.join(struct.pack('>I', y) for y in starts))
    out += b''.join(idats) + _chunk(b'IEND', b'')
    if path:
        open(path, 'wb').write(out)
    return out
