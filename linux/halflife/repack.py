"""repack.py PAK OUT_DIR CACHE ENCODER: Uplink's pak0 made smaller for the machine (docs/games/halflife.md,
"The data"), into OUT_DIR/pak0.PAK and OUT_DIR/uplink.wad:
- a sound is the card's IMA ADPCM in its own WAV file (format 0x11, its cue chunks kept), played from
  the ROM; REPACK_SOUNDS=mp3 makes MP3 of them (format 0x55, decoded by libmedia when a level loads)
  and =wav leaves them;
- the maps' textures lie once in uplink.wad, one level and no mips (offsets[1] = 0);
- 24-bit pictures are JPEG in restart intervals under their own names; the sky's BMP copies are left out;
- maps, models and sprites are deflate in segments (libmedia's media_inflate; REPACK_SEGMENTS=0: not).
ENCODER is linux/classicube/sound/adpcm_encode.c built in WSL. Needs ffmpeg (for MP3) and Pillow.
CACHE keeps what was encoded, by its input's hash."""
import hashlib, io, os, struct, subprocess, sys, tempfile, zlib
import numpy as np
from concurrent.futures import ThreadPoolExecutor
from PIL import Image

MP3_RATE = {11025: '24k', 22050: '32k', 44100: '48k'}
JPEG_QUALITY = 70
CACHE = None

def cached(kind, data, make):
    key = hashlib.sha1(kind.encode() + data).hexdigest()
    path = os.path.join(CACHE, key) if CACHE else None
    if path and os.path.exists(path):
        return open(path, 'rb').read()
    out = make(data)
    if path:
        open(path + '.tmp', 'wb').write(out), os.replace(path + '.tmp', path)
    return out

def chunks(d):
    p = 12
    while p + 8 <= len(d):
        cid, cl = d[p:p + 4], struct.unpack('<I', d[p + 4:p + 8])[0]
        yield cid, d[p + 8:p + 8 + cl]
        p += 8 + cl + (cl & 1)

def chunk(cid, body):
    return cid + struct.pack('<I', len(body)) + body + (b'\0' if len(body) & 1 else b'')

# ---- sounds ----

def mp3_of(wav):
    fmt = next(b for c, b in chunks(wav) if c == b'fmt ')
    tag, channels, rate, _, align, bits = struct.unpack('<HHIIHH', fmt[:16])
    data = next(b for c, b in chunks(wav) if c == b'data')
    if tag != 1 or channels != 1 or rate not in MP3_RATE or len(data) < align * 1200:
        return wav   # (a sound shorter than two frames is no smaller as MP3)
    with tempfile.TemporaryDirectory() as t:
        src, dst = os.path.join(t, 'in.wav'), os.path.join(t, 'out.mp3')
        open(src, 'wb').write(wav)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', src, '-c:a', 'libmp3lame', '-b:a', MP3_RATE[rate],
                        '-write_xing', '0', '-id3v2_version', '0', dst], check=True)
        mp3 = open(dst, 'rb').read()
    samples = len(data) // align
    # fmt 0x55 with the original's bits (8 or 16: what the card plays it as), fact: the samples
    body = chunk(b'fmt ', struct.pack('<HHIIHH', 0x55, 1, rate, len(mp3) * rate // max(samples, 1), 1, bits))
    body += chunk(b'fact', struct.pack('<I', samples))
    for c, b in chunks(wav):
        if c in (b'cue ', b'LIST'):
            body += chunk(c, b)
    body += chunk(b'data', mp3)
    out = b'RIFF' + struct.pack('<I', 4 + len(body)) + b'WAVE' + body
    return out if len(out) < len(wav) else wav

ENCODER = None   # linux/classicube/sound/adpcm_encode.c built in WSL (image.py)

def adpcm_of(wav):
    """the card's IMA ADPCM (fmt 0x11, 4 bits; 'ckpt': a word every 32 samples, 4-aligned), played from the ROM"""
    fmt = next(b for c, b in chunks(wav) if c == b'fmt ')
    tag, channels, rate, _, align, bits = struct.unpack('<HHIIHH', fmt[:16])
    data = next(b for c, b in chunks(wav) if c == b'data')
    if tag != 1 or channels != 1 or bits not in (8, 16):
        return wav
    if bits == 8:
        pcm = ((np.frombuffer(data, np.uint8).astype(np.int16) - 128) << 8).astype('<i2').tobytes()
    else:
        pcm = data[:len(data) // 2 * 2]
    samples = len(pcm) // 2
    out = subprocess.run(['wsl', '--', ENCODER], input=pcm, capture_output=True, check=True).stdout
    codes_bytes = ((samples + 1) // 2 + 3) & ~3
    codes, marks = out[:codes_bytes], out[codes_bytes:]
    body = chunk(b'fmt ', struct.pack('<HHIIHH', 0x11, 1, rate, rate // 2, 1, 4))
    body += chunk(b'fact', struct.pack('<I', samples))
    for c, b in chunks(wav):
        if c in (b'cue ', b'LIST'):
            body += chunk(c, b)
    if (12 + len(body) + 8) & 3:
        body += chunk(b'JUNK', b'\0\0')
    body += chunk(b'ckpt', marks) + chunk(b'data', codes)
    out = b'RIFF' + struct.pack('<I', 4 + len(body)) + b'WAVE' + body
    return out if len(out) < len(wav) else wav

# ---- pictures ----

def jpeg_of(data):
    img = Image.open(io.BytesIO(data))
    if img.mode in ('RGBA', 'LA') or 'transparency' in img.info or img.mode == 'P' or img.mode == 'L':
        return data   # (alpha or a palette: the GPU keeps those as they are)
    out = io.BytesIO()
    img.convert('RGB').save(out, 'JPEG', quality=JPEG_QUALITY, restart_marker_blocks=16)
    return out.getvalue() if out.tell() < len(data) else data

# ---- maps ----

LUMP_ENTITIES, LUMP_TEXTURES = 0, 2

def bsp_lumps(d):
    return [d[o:o + n] for o, n in (struct.unpack('<ii', d[4 + 8 * i:12 + 8 * i]) for i in range(15))]

def bsp_join(version, lumps):
    out, at = bytearray(4 + 15 * 8), 4 + 15 * 8
    struct.pack_into('<i', out, 0, version)
    for i, l in enumerate(lumps):
        struct.pack_into('<ii', out, 4 + 8 * i, at, len(l))
        out += l + b'\0' * (-len(l) & 3)
        at = len(out)
    return bytes(out)

def miptexes(lump):
    n = struct.unpack('<i', lump[:4])[0]
    for o in struct.unpack('<%di' % n, lump[4:4 + 4 * n]):
        if o < 0:
            yield None
            continue
        name, w, h = lump[o:o + 16].split(b'\0')[0], *struct.unpack('<II', lump[o + 16:o + 24])
        offs = struct.unpack('<4I', lump[o + 24:o + 40])
        if not offs[0]:
            yield name, w, h, None
            continue
        pal = o + offs[0] + w * h * 85 // 64
        yield name, w, h, (lump[o + offs[0]:o + offs[0] + w * h], lump[pal:pal + 2 + 768])

def one_level(name, w, h, pixels, palette):
    return name.ljust(16, b'\0')[:16] + struct.pack('<II4I', w, h, 40, 0, 0, 0) + pixels + palette + b'\0\0'

def wad_of(entries):
    out, table = bytearray(b'WAD3' + b'\0' * 8), b''
    for name, body in entries:
        table += struct.pack('<iiibbh16s', len(out), len(body), len(body), 0x43, 0, 0, name.ljust(16, b'\0')[:16])
        out += body + b'\0' * (-len(body) & 3)
    struct.pack_into('<ii', out, 4, len(entries), len(out))
    return bytes(out + table)

def repack_maps(maps):
    """the maps' textures into one WAD where every map that has a name has the same picture"""
    seen = {}
    for name, d in maps.items():
        for t in miptexes(bsp_lumps(d)[LUMP_TEXTURES]):
            if t and t[3]:
                seen.setdefault(t[0].lower(), set()).add((t[1], t[2]) + t[3])
    shared = {k: next(iter(v)) for k, v in seen.items() if len(v) == 1}
    wad = [(k, one_level(k, *v[:2], *v[2:])) for k, v in sorted(shared.items())]
    out = {}
    for name, d in maps.items():
        lumps = bsp_lumps(d)
        lump, at = lumps[LUMP_TEXTURES], []
        n = struct.unpack('<i', lump[:4])[0]
        body = bytearray()
        for o, t in zip(struct.unpack('<%di' % n, lump[4:4 + 4 * n]), miptexes(lump)):
            if t is None:
                at.append(-1)
                continue
            at.append(4 + 4 * n + len(body))
            if t[3] and t[0].lower() in shared:   # a header only: its picture is the WAD's
                body += t[0].ljust(16, b'\0')[:16] + struct.pack('<II4I', t[1], t[2], 0, 0, 0, 0)
            elif t[3]:   # (a name with two pictures stays the map's own, as it was)
                body += lump[o:o + 40 + t[1] * t[2] * 85 // 64 + 2 + 768]
            else:
                body += lump[o:o + 40]
            body += b'\0' * (-len(body) & 3)
        lumps[LUMP_TEXTURES] = struct.pack('<i%di' % n, n, *at) + bytes(body)
        ents = lumps[LUMP_ENTITIES].rstrip(b'\0')
        key = ents.find(b'"wad"')
        if key >= 0:
            q = ents.index(b'"', key + 5) + 1
            ents = ents[:q] + b'uplink.wad;' + ents[q:]
        else:
            ents = ents.replace(b'{', b'{\n"wad" "uplink.wad"', 1)
        lumps[LUMP_ENTITIES] = ents + b'\0'
        out[name] = bsp_join(struct.unpack('<i', d[:4])[0], lumps)
    return out, wad_of(wad)

# ---- deflate in segments (libmedia's media_inflate): each 8 KB by itself, a job each ----

SEGMENT = 8192
SMALLEST = 32768   # (a smaller file read as it is: inflating costs a segment's wait, ~40 ms on the card)

def segments_of(data):
    parts = []
    for i in range(0, len(data), SEGMENT):
        z = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
        parts.append(z.compress(data[i:i + SEGMENT]) + z.flush())
    head = 16 + 4 * (len(parts) + 1)
    offsets, at = [], head
    for p in parts:
        offsets.append(at)
        at += len(p)
    offsets.append(at)
    out = b'MDZ1' + struct.pack('<III', len(data), SEGMENT, len(parts)) + struct.pack('<%dI' % len(offsets), *offsets) + b''.join(parts)
    return out if len(out) < len(data) * 15 // 16 and len(data) >= SMALLEST else data

# ---- the pack ----

def read_pak(path):
    f = open(path, 'rb')
    _, o, n = struct.unpack('<4sii', f.read(12))
    f.seek(o)
    table = [struct.unpack('<56sii', f.read(64)) for _ in range(n // 64)]
    files = []
    for name, off, size in table:
        f.seek(off)
        files.append((name.split(b'\0')[0].decode(), f.read(size)))
    return files

def write_pak(path, files):
    out, table = bytearray(b'PACK' + b'\0' * 8), b''
    for name, d in files:
        table += struct.pack('<56sii', name.encode(), len(out), len(d))
        out += d + b'\0' * (-len(d) & 15)   # (16: a sound's samples where a copy of the ROM starts)
    struct.pack_into('<ii', out, 4, len(out), len(table))
    open(path, 'wb').write(bytes(out + table))

def main():
    global CACHE, ENCODER
    src, dst, CACHE, ENCODER = sys.argv[1:5]
    if CACHE:
        os.makedirs(CACHE, exist_ok=True)
    files = read_pak(src)
    names = {n.lower() for n, _ in files}
    files = [(n, d) for n, d in files if not (n.lower().startswith('gfx/env/') and n.lower().endswith('.bmp')
                                              and n.lower()[:-4] + '.tga' in names)]
    def convert(item):
        n, d = item
        low = n.lower()
        sounds = os.environ.get('REPACK_SOUNDS', 'adpcm')
        if low.endswith('.wav') and sounds == 'mp3':
            return n, cached('mp3 ' + str(MP3_RATE), d, mp3_of)
        if low.endswith('.wav') and sounds == 'adpcm':
            return n, cached('adpcm', d, adpcm_of)
        if low.endswith(('.tga', '.bmp')) and low.startswith('gfx/'):
            return n, cached('jpeg %d' % JPEG_QUALITY, d, jpeg_of)
        return n, d
    with ThreadPoolExecutor(os.cpu_count()) as pool:
        files = list(pool.map(convert, files))
    maps, wad = repack_maps({n: d for n, d in files if n.lower().endswith('.bsp')})
    files = [(n, maps.get(n, d)) for n, d in files]
    with ThreadPoolExecutor(os.cpu_count()) as pool:
        packed = ('.bsp', '.mdl', '.spr') if os.environ.get('REPACK_SEGMENTS', '1') == '1' else ()
        files = list(pool.map(lambda f: (f[0], segments_of(f[1]) if f[0].lower().endswith(packed) and packed else f[1]), files))
    os.makedirs(dst, exist_ok=True)
    write_pak(os.path.join(dst, 'pak0.PAK'), files)
    open(os.path.join(dst, 'uplink.wad'), 'wb').write(wad)
    total = sum(len(d) for _, d in files)
    print('repack: pak0 %.2f MB (%s), uplink.wad %.2f MB' % (total / 1e6, ', '.join(
        '%s %.2f' % (k, sum(len(d) for n, d in files if n.lower().endswith(k)) / 1e6) for k in ('.wav', '.bsp', '.mdl', '.spr', '.tga', '.bmp')), len(wad) / 1e6))

if __name__ == '__main__':
    main()
