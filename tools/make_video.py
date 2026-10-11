"""Makes the Linux image's sample video, /root/Videos/Big Buck Bunny.mpg, into
build/images/linux/root (tools/make_linux_image.py then puts it in the image).

    python tools/make_video.py [WIDTHxHEIGHT [SECONDS [VIDEO_KBITS]]]

It is the first seconds of the trailer of Big Buck Bunny, (c) copyright 2008, Blender
Foundation / www.bigbuckbunny.org, licensed Creative Commons Attribution 3.0, fetched once from
Wikimedia Commons into build/fetch/media. MPEG-1 as nxvideo wants it (docs/video.md): closed
groups of six pictures with no B pictures, so that every group is a worker's job by itself,
and layer II sound. Needs ffmpeg and, the first time, the network.
"""
import hashlib, os, subprocess, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'build', 'images', 'linux', 'root', 'root', 'Videos')
FETCH = os.path.join(ROOT, 'build', 'fetch', 'media')
SOURCE = 'https://upload.wikimedia.org/wikipedia/commons/b/b3/Big_Buck_Bunny_Trailer_400p.ogv'
SHA1 = '75c0cfaa0c287b0a5ca4d932fd9b095e2a1ef7d3'
AGENT = 'ShaderEmu-media-fetch/1.0 (https://github.com/MichaelMoroz/ShaderEmu)'


def main():
    size = sys.argv[1] if len(sys.argv) > 1 else '320x176'
    seconds = sys.argv[2] if len(sys.argv) > 2 else '20'
    kbits = sys.argv[3] if len(sys.argv) > 3 else '300'
    width, height = (int(v) for v in size.split('x'))
    if width % 16 or height % 16:
        raise SystemExit('the width and the height must be multiples of 16')
    cached = os.path.join(FETCH, 'Big_Buck_Bunny_Trailer_400p.ogv')
    if not os.path.exists(cached):
        os.makedirs(FETCH, exist_ok=True)
        request = urllib.request.Request(SOURCE, headers={'User-Agent': AGENT})
        open(cached, 'wb').write(urllib.request.urlopen(request, timeout=180).read())
    if hashlib.sha1(open(cached, 'rb').read()).hexdigest() != SHA1:
        raise SystemExit(cached + ' is not the file this was written for')
    os.makedirs(OUT, exist_ok=True)
    to = os.path.join(OUT, 'Big Buck Bunny.mpg')
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-t', seconds, '-i', cached,
                    '-vf', 'scale=%d:%d,fps=24' % (width, height),
                    '-c:v', 'mpeg1video', '-b:v', kbits + 'k', '-maxrate', kbits + 'k', '-bufsize', '400k',
                    '-g', '6', '-bf', '0', '-flags', '+cgop+bitexact', '-sc_threshold', '1000000000',
                    '-c:a', 'mp2', '-ar', '32000', '-ac', '1', '-b:a', '48k',
                    '-map_metadata', '-1', '-fflags', '+bitexact', '-f', 'mpeg', to], check=True)
    print('%s: %d x %d, %s s, %d bytes' % (os.path.relpath(to, ROOT), width, height, seconds, os.path.getsize(to)))


if __name__ == '__main__':
    main()
