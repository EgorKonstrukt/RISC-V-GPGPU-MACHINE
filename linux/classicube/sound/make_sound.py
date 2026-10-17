"""ClassiCube's sounds and its music for the machine (docs/games/classicube.md, "Sound").

  python3 make_sound.py OUT_DIR RESOURCES.C MEDIA_TEST ENCODER CACHE

The game's launcher fetches Mojang's files by their SHA-1 from resources.download.minecraft.net;
the list of them is its own (src/Resources.c). They are fetched once into CACHE and decoded by
libmedia on this computer (media_test).

- classicube-sound.pak: the dig and step sounds as IMA ADPCM with checkpoints (ENCODER), which
  the sound card plays where they lie in the ROM. 'CCSN', count, then count entries sorted by
  name: name (24 bytes, NUL-padded), rate, samples, data offset, checkpoints offset.
- classicube-music.mp3: one tune (TUNE), made an MP3 by ffmpeg once (kept in CACHE), which the
  game decodes on worker cores at half its rate.
"""
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.request

import numpy as np

SERVER = 'https://resources.download.minecraft.net/'
TUNE = 'calm3'


def fetch(cache, sha1):
    path = os.path.join(cache, sha1)
    if not os.path.exists(path):
        data = urllib.request.urlopen(SERVER + sha1[:2] + '/' + sha1, timeout=60).read()
        if hashlib.sha1(data).hexdigest() != sha1:
            sys.exit('%s: not the file asked for' % sha1)
        open(path + '.part', 'wb').write(data)
        os.replace(path + '.part', path)
    return path


def decode(media_test, path):
    """16-bit samples (frames x channels) and their rate."""
    raw = path + '.raw'
    said = subprocess.run([media_test, 'audio', path, raw], capture_output=True, text=True, check=True).stdout
    found = re.search(r' (\d+) Hz (\d) ch', said)
    if not found:
        sys.exit('%s: %s' % (path, said.strip()))
    rate, channels = int(found.group(1)), int(found.group(2))
    pcm = np.fromfile(raw, dtype='<i2').reshape(-1, channels)
    os.remove(raw)
    return pcm, rate


def encode(encoder, pcm):
    samples = np.clip(np.round(pcm.mean(axis=1)), -32768, 32767).astype('<i2')
    out = subprocess.run([encoder], input=samples.tobytes(), capture_output=True, check=True).stdout
    data = ((len(samples) + 1) // 2 + 3) & ~3
    return len(samples), out[:data], out[data:]


def tune(media_test, cache, sha1):
    mp3 = os.path.join(cache, sha1 + '.mp3')
    if not os.path.exists(mp3):
        ffmpeg = shutil.which('ffmpeg') or shutil.which('ffmpeg.exe')
        if not ffmpeg:
            sys.exit('ffmpeg is wanted once, to make the tune an MP3')
        pcm, rate = decode(media_test, fetch(cache, sha1))
        out = subprocess.run([ffmpeg, '-v', 'error', '-f', 's16le', '-ar', str(rate), '-ac', str(pcm.shape[1]), '-i', 'pipe:0',
                              '-c:a', 'libmp3lame', '-b:a', '128k', '-f', 'mp3', 'pipe:1'],
                             input=pcm.astype('<i2').tobytes(), capture_output=True, check=True).stdout
        open(mp3 + '.part', 'wb').write(out)
        os.replace(mp3 + '.part', mp3)
    return mp3


def main():
    out, resources, media_test, encoder, cache = sys.argv[1:6]
    os.makedirs(cache, exist_ok=True)
    text = open(resources, encoding='latin-1').read()
    effects = re.findall(r'\{\s*"((?:dig|step)_\w+)\.wav",\s*"([0-9a-f]{40})"\s*\}', text)
    music = dict(re.findall(r'\{\s*"(\w+)\.ogg",\s*"([0-9a-f]{40})",\s*\d+\s*\}', text))
    if len(effects) < 40 or TUNE not in music:
        sys.exit('%s: the lists of sounds were not found' % resources)
    coded = {}
    for name, sha1 in effects:
        if sha1 not in coded:
            pcm, rate = decode(media_test, fetch(cache, sha1))
            coded[sha1] = (rate,) + encode(encoder, pcm)
    effects.sort()
    at, body, places = 8 + 40 * len(effects), b'', {}
    for sha1, (rate, samples, data, marks) in coded.items():
        places[sha1] = (at + len(body), at + len(body) + len(data))
        body += data + marks + b'\0' * (-len(data + marks) % 16)
    index = b''.join(struct.pack('<24s4I', name.encode(), coded[sha1][0], coded[sha1][1], *places[sha1]) for name, sha1 in effects)
    pak = os.path.join(out, 'classicube-sound.pak')
    open(pak, 'wb').write(b'CCSN' + struct.pack('<I', len(effects)) + index + body)
    shutil.copyfile(tune(media_test, cache, music[TUNE]), os.path.join(out, 'classicube-music.mp3'))
    print('%s: %d sounds (%d files, %.1f s), %d bytes; %s.ogg as an MP3 of %d bytes' % (
        pak, len(effects), len(coded), sum(c[1] / c[0] for c in coded.values()), at + len(body), TUNE,
        os.path.getsize(os.path.join(out, 'classicube-music.mp3'))))


if __name__ == '__main__':
    main()
