"""Every program's own checks on the Linux image, under bin/rvc_cpu.exe, several at a time
(docs/cpu-harness.md, "The test set"). One PASS or FAIL line a case, then a summary; exit 1 on
any failure.

    python tools/test_all.py                    # the default set (some 5 minutes at most)
    python tools/test_all.py games doom-1 ...   # cases by name or tag (machine, games, desktop, media, long, ...)
    python tools/test_all.py --long             # the default set and the long cases
    python tools/test_all.py --races            # RVC_RACES on the cases with workers: 0 undone
    python tools/test_all.py --image DIR --jobs 8 --list

A case is the line typed at the guest's shell, what ends it, and what its console must say:
`has` lines (regular expressions), `sums` (a pattern's captures in order), `digest` (the first
8 hex of the md5 of every match, for long lists of state sums kept "as before" by the docs) and
`same` (another case's matches, which must be the same). The values are the docs' (each says
where) or, where a doc says only "as before", what the image of 15 October 2026 printed.
"""
import argparse, hashlib, os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DONE = "echo LX-''DONE"
X = 'nano-X -p & sleep 2; '
CC = 'mkdir -p /tmp/classicube; echo singleplayerphysics=false >> /tmp/classicube/options.txt; ' + X
TD = 'TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 '
Q3 = 'QUAKE3_STATS_FRAMES=100 quake3 +set fixedtime 33 +devmap q3dm17'
NES = X + 'NES_SUM=1 NES_PPU=inline '
NES_RUNS = (('thwaite-120', 'NES_FRAMES=120 nes thwaite', '31a18295', 'afcb4a98'),
            ('thwaite-180', 'NES_FRAMES=180 NES_PRESS="20 start 50 start 80 a 100 left 110 a" nes thwaite', '2f58587d', '39c90e07'),
            ('nova-120', 'NES_FRAMES=120 nes nova', '3a2e5c47', 'd8a04406'))
STATE = r'(tic \d+ state [0-9a-f]+)'
# media files with no ending, an ending in capitals, or only an ending, all with the execute bit (docs/open.md)
KINDS_T = ('mkdir -p /tmp/t; cd /tmp/t; cp "/root/Music/Grieg - Hall of the Mountain King.ogg" song; cp /usr/share/play-test.wav tone; '
           'cp "/root/Videos/Big Buck Bunny.avi" film; cp /root/Pictures/Fox.png fox; cp /usr/share/play-test.flac A.FLAC; '
           'for e in tga pnm pbm oga vqa xyz; do echo x > a.$e; done; chmod +x *; cd /; ')


def case(name, tags, cmd, workers=False, until='LX-DONE', more=(), wall=240, image=None, env=None, **checks):
    return dict(name=name, tags=set(tags.split()), cmd=cmd, workers=workers, until=until, more=list(more), wall=wall,
                image=image, env=env or {}, **checks)


CASES = [
    # the machine (docs/multicore.md; programs/mc/mctest.c)
    case('mctest-3', 'machine', 'mctest 3', True, has=[r'mctest: PASS', r'\(0 pages brought for their start\)']),
    case('mctest-255', 'machine', 'mctest 255 30000', True, has=[r'mctest: PASS', r'\(0 pages brought for their start\)']),
    case('fptest', 'machine', 'fptest', has=[r'fptest: done, 0 of 15 instructions wrong']),   # docs/fpu.md
    # zerotest and tailtest are in no image: --long builds them into build/images/test_all (docs/boot.md)
    case('zero-tail', 'machine long', 'zerotest 12 4; tailtest /usr/share/* /usr/bin/* /etc/*', wall=400, image='zerotail',
         has=[r'zerotest: PASS', r'tailtest: PASS']),
    case('lazy', 'machine', None, image='lazy', until=' done', wall=60,   # docs/boot.md: twenty lines ok
         has=[r'lazy: sum 1a4a1226'], count=[(r'^lazy: .* ok', 20)]),
    # cc's outputs, the same bytes (docs/cc.md): cc, cc -g and tcc -c of the four examples
    case('cc-bytes', 'machine', 'cd /tmp; for e in hello cube home shader; do cc /usr/share/example-$e.c -o $e.x; '
         'cc -g /usr/share/example-$e.c -o $e.g; tcc -c /usr/share/example-$e.c -o $e.o; done; md5sum *.x *.g *.o', wall=300,
         digest=[(r'^([0-9a-f]{32}  \S+)', '6c8d7a0c')]),   # gles.c and mcw.c of 15 October: the .o files as before
    case('libmedia-installed', 'machine media', 'ls -1 /usr/lib/libmedia.a /usr/include/media.h /usr/bin/media_test | cat',   # docs/media.md
         has=[r'^/usr/lib/libmedia\.a', r'^/usr/include/media\.h'], never=[r'No such file']),
    # Doom (docs/doom.md, "Checking it"): the demos' state sums, the view told and checked, the port at rest
    *[case(f'doom-{d}', 'games doom', X + f'DOOM_HASH_TICS=100 doom -1 -timedemo demo{d}', True,
           digest=[(STATE, s)]) for d, s in ((1, '0ab4df93'), (2, 'c5123ba8'), (3, 'df43029a'))],
    case('doom-told', 'games doom', X + 'DOOM_VIEW=told DOOM_VIEW_CHECK=1 DOOM_HASH_TICS=100 doom -1 -timedemo demo1', True,
         has=[r'doomcheck: 5000 views, 0 not as told'], digest=[(STATE, '0ab4df93')]),
    case('doom-rest', 'games doom', X + 'DOOM_PORT=rest DOOM_HASH_TICS=100 doom -1 -timedemo demo1', True, digest=[(STATE, '0ab4df93')]),
    # Quake (docs/quake.md, "Checking it"): the server's sums in three modes, particles, light, a walk, an exit
    *[case(f'quake-server-{m}', 'games quake', X + f'QUAKE_SERVER={m} QUAKE_SUM=10 quake +map e1m5 +host_framerate 0.05', True,
           until='server frame 610:', more=['--fixed-dt', '0.004'], digest=[(r'(server frame (?:[1-9]|[1-5]\d|60)0: .* state [0-9a-f]{8})', '52463017')])
      for m in ('inline', 'late', 'wait')],
    case('quake-particles', 'games quake', X + 'QUAKE_PARTICLES=wait QUAKE_PARTICLE_SUM=1 quake +timedemo demo1 +host_framerate 0.05', True,
         until=' fps', digest=[(r'(particles at frame \d+: [0-9a-f]+)', '9c67f307')]),
    case('quake-light', 'games quake', X + 'QUAKE_LIGHTPOINT=check quake +timedemo demo1', True, until=' fps',
         has=[r'0 not as made here \(QUAKE_LIGHTPOINT=check\)'], never=[r'[1-9]\d* not as made here']),
    case('quake-walk', 'games quake', X + 'QUAKE_TRACK=5 quake +map e1m1 +host_framerate 0.05', True, until='track 150:',
         more=['--expect', 'track 30:', '--send', '+forward\\n'], has=[r'track 140: .* at 480 687 24']),
    case('quake-exit', 'games quake', X + 'QUAKE_TRACK=5 QUAKE_TEST_EXIT=40 quake +map e1m1', True, until='track 400:',
         more=['--expect', 'track 80:', '--send', '+attack\\n'], wall=300, has=[r'maps/e1m2\.bsp']),
    # Quake III (docs/quake3.md, "The client's frame on three cores"): typed the same length; the
    # client's game on its worker against the first core differs early, as before, so it stays on its worker
    case('quake3-workers', 'games quake3', X + 'QUAKE3_CLIENT=worker QUAKE3_SCENE=worker QUAKE3_SIDE=worker QUAKE3_SUM=1 QUAKE3_SERVER=inline ' + Q3,
         True, until='q3stat: frame 600:', count=[(r'^q3sum: ', (500, 600))]),
    case('quake3-inline', 'games quake3', X + 'QUAKE3_CLIENT=worker QUAKE3_SCENE=inline QUAKE3_SIDE=inline QUAKE3_SUM=1 QUAKE3_SERVER=inline ' + Q3,
         True, until='q3stat: frame 600:', same=[(r'^q3sum: (\d+) (.*)', 'quake3-workers', 500)]),
    case('quake3-server', 'games quake3', X + 'QUAKE3_SEED=7 QUAKE3_SERVER=inline QUAKE3_CLIENT=inline QUAKE3_SCENE=inline QUAKE3_SUM=1 ' + Q3 +
         ' +addbot sarge 3 +wait 100 +setviewpos 488 -968 378 90', True, until='q3stat: frame 700:', count=[(r'^q3srvsum: ', 223)],
         digest=[(r'^(q3srvsum: .*)', 'aaa1379a')]),
    case('quake3-bots-serial', 'games quake3 long', X + 'QUAKE3_BOTS=serial QUAKE3_SUM=1 QUAKE3_SEED=7 QUAKE3_SERVER=inline ' + Q3 + ' +addbot sarge 3' * 3,
         True, until='q3stat: frame 700:', wall=400, count=[(r'^q3srvsum: ', 223)]),
    case('quake3-bots', 'games quake3 long', X + 'QUAKE3_BOTX=serial QUAKE3_SUM=1 QUAKE3_SEED=7 QUAKE3_SERVER=inline ' + Q3 + ' +addbot sarge 3' * 3,
         True, until='q3stat: frame 700:', wall=400, same=[(r'^(q3srvsum: .*)', 'quake3-bots-serial')]),
    # ClassiCube (docs/classicube.md, "Checking it"): 52 meshes, 68,964 vertices at the start; the pack's sums the raw pack's
    case('classicube-meshes', 'games classicube', CC + 'CLASSICUBE_MESH=inline CLASSICUBE_MESH_SUM=1 CLASSICUBE_SEED=7 CLASSICUBE_STATS_MS=2000 classicube',
         True, until='classicube: steady', count=[(r'^ccmesh: ', 52)], digest=[(r'^(ccmesh: .*)', '7f763849')]),
    case('classicube-meshes-workers', 'games classicube', CC + 'CLASSICUBE_MESH=worker CLASSICUBE_MESH_SUM=1 CLASSICUBE_SEED=7 CLASSICUBE_STATS_MS=2000 classicube',
         True, until='classicube: steady', count=[(r'^ccmesh: ', 52)], digest=[(r'^ccmesh: at (\d+ \d+ \d+), \d+ vertices, sums \S+ (\S+)', '1f9d723c', 'sorted')]),
    case('classicube-pack', 'games classicube', CC + 'CLASSICUBE_TEXTURE_SUM=1 CLASSICUBE_SEED=7 CLASSICUBE_STATS_MS=2000 classicube',
         True, until='classicube: steady', count=[(r'^cctex: ', 42)], digest=[(r'^(cctex: .*)', 'e51865ae')]),
    case('classicube-kept', 'games classicube', CC + 'CLASSICUBE_CHECK_KEPT=1 CLASSICUBE_SEED=7 CLASSICUBE_STATS_MS=2000 classicube', True,
         until='cccheck:', more=['--expect', 'classicube: steady', '--send', 'turn 1200 0\\ndown W\\ndown Space\\n'],
         has=[r'cccheck: the outline kept \d+ times, 0 of them wrong; the held block kept \d+ times, 0 wrong']),
    # docs/holodeck.md, "Played from the seat": a desktop sitter breaks by the right trigger and places by
    # the left one, at the view's middle; in a headset each hand's beam picks its own block (small blocks)
    *[case(f'classicube-seat-{n}', 'games classicube', CC + 'CLASSICUBE_SEAT=1 CLASSICUBE_SEED=7 classicube', True, until='ccblock:',
           more=['--host', w], has=[h]) for n, w, h in (
        ('desk-break', '00000112,e0000000,0,0,0,0,40000000,0', r'ccblock: 32 40 31 2 -> 0'),
        ('desk-place', '00000112,e0000000,0,0,40000000,0,0,0', r'ccblock: 32 41 31 0 -> 1'),
        ('vr-break', '90000133,0,0,c0000000,04bea3da,0,44bea026,e0000000', r'ccblock: 34 40 28 2 -> 0'),
        ('vr-place', '90000133,0,0,c0000000,44bea3da,0,04bea026,e0000000', r'ccblock: 30 41 28 0 -> 1'))],
    # Tiberian Dawn and Red Alert (docs/tdawn.md, docs/ralert.md)
    case('tdawn-300', 'games tdawn', X + TD + 'TDAWN_SCEN=10 TDAWN_FRAMES=300 tdawn FROMINSTALL', True, has=[r'done at frame 300: SCG10EA, .* state f7650705']),
    case('tdawn-1300', 'games tdawn', X + TD + 'TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL', True, has=[r'done at frame 1300: SCG03EA, .* state 1a9a814b']),
    case('tdawn-paths', 'games tdawn', X + TD + 'TDAWN_SCEN=3 TDAWN_CHECK_PATHS=1 TDAWN_FRAMES=1300 tdawn FROMINSTALL', True,
         has=[r'searches not run again and \d+ another core.s, 0 of them wrong', r'edge walks made both ways, 0 of them wrong', r'state 1a9a814b']),
    case('tdawn-move', 'games tdawn long', X + TD + 'TDAWN_SCEN=3 TDAWN_MOVE=100,8,0 TDAWN_SUMS=1 TDAWN_FRAMES=600 tdawn FROMINSTALL', True, count=[(r'^tdsum: ', 600)]),
    case('tdawn-move-refresh', 'games tdawn long', X + TD + 'TDAWN_SCEN=3 TDAWN_MOVE=100,8,0 TDAWN_SUMS=1 TDAWN_REFRESH=1 TDAWN_FRAMES=600 tdawn FROMINSTALL', True,
         same=[(r'^(tdsum: .*)', 'tdawn-move')]),
    # docs/tdawn.md, "A mission won": the demo's first mission won, its score screen, the next mission played
    case('tdawn-won', 'games tdawn', X + TD + 'TDAWN_SCEN=1 TDAWN_WIN=100 TDAWN_FRAMES=300 tdawn FROMINSTALL', True, wall=400,
         has=[r'^tdawn: score: ends', r'done at frame 300: SCG10EA, .* state 325b1830'], never=[r'Segmentation fault', r'out of memory']),
    *[case(f'ralert-{side}-{f}', 'games ralert', X + f'TDAWN_AUTO=1 RALERT_SIDE={side} RALERT_SEED=7 RALERT_FRAMES={f} ralert', True,
           has=[rf'done at frame {f}: .* state {s}']) for side, f, s in (('soviet', 300, '01e0dd60'), ('allies', 300, 'ea72a5d1'),
                                                                        ('soviet', 1300, 'eb6c2265'), ('allies', 1300, 'dadd663d'))],
    case('ralert-scans', 'games ralert', X + 'TDAWN_AUTO=1 RALERT_SIDE=allies RALERT_SEED=7 RALERT_CHECK_SCANS=1 RALERT_FRAMES=1300 ralert', True,
         has=[r'scan bits kept in \d+ frames of 1280, 0 of them wrongly'], never=[r'[1-9]\d* of them wrongly']),
    case('ralert-movie', 'games ralert', X + 'RALERT_MOVIE_CHECK=1 TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7 RALERT_FRAMES=20 ralert', True,
         wall=400, has=[r'movie check: 0 of 156 shared frames differ']),
    # docs/ralert.md, "A base destroyed": a base attacked to the atlas's filling (the build before crashed in it), and
    # the atlas's shapes forgotten every 50 scenes giving the same pictures
    case('ralert-attack', 'games ralert', X + 'TDAWN_AUTO=1 RALERT_SIDE=allies RALERT_SEED=7 RALERT_NO_DELAY=1 RALERT_ATTACK=100,250 '
         'RALERT_FRAMES=3000 ralert', True, wall=400, has=[r'done at frame 3000: .* state dbb267c0']),
    *[case(f'ralert-atlas{tag}', 'games ralert', X + pre + 'RALERT_DRAW=here RALERT_REFRESH=1 TDAWN_SUMS=1 TDAWN_AUTO=1 RALERT_SIDE=allies '
           'RALERT_SEED=7 RALERT_NO_DELAY=1 RALERT_ATTACK=100,250 RALERT_FRAMES=300 ralert', True, **checks)
      for tag, pre, checks in (('', '', dict(count=[(r'^rasum: ', 300)])),
                               ('-forgotten', 'RALERT_ATLAS=50 ', dict(same=[(r'^(rasum: .*)', 'ralert-atlas')])))],
    # docs/ralert.md, "A mission won": won twice, the score screen's table and rows against the game's own, the
    # demo's menu again (it has no next mission: the build before asked for "out of memory"), then 300 frames
    case('ralert-won', 'games ralert', X + 'TDAWN_INTERPOLATION=check TDAWN_AUTO=1 RALERT_SIDE=allies RALERT_SEED=7 RALERT_WIN=100,2 '
         'RALERT_FRAMES=300 ralert', True, wall=400,
         has=[r'done at frame 300: SCG02EA\.INI, .* state ea72a5d1'],
         count=[(r"^ralert: the demo's menu again", 2), (r'^interpolation: 0 of 65536 entries differ', 2), (r'^interpolation: 0 of 399 rows differ', 22)],
         never=[r'^interpolation: [1-9]', r'Segmentation fault', r'out of memory']),
    # NES (docs/nes.md, "Testing"): RAM and picture with the lists on the first core and on a worker
    *[case(f'nes-{n}{tag}', 'games nes', NES + pre + c, True, sums=[(r"every frame's RAM ([0-9a-f]+)", [ram]), (r'done, picture ([0-9a-f]+)', [pic])])
      for n, c, ram, pic in NES_RUNS for tag, pre in (('', ''), ('-lists', 'NES_LIST=worker '))],
    case('nes-cells', 'games nes', X + 'NES_CELLS_CHECK=1 NES_LIST=worker NES_SOUND=0 NES_MARK=100 NES_FRAMES=1000 NES_PRESS="20 start 50 start 80 a '
         '100 left 110 a 200 a 210 +right 300 a 400 a 420 +up 500 a 800 a 900 -right 1000 a 1100 +left 1300 a" nes thwaite', True,
         has=[r'0 of 1000 frames differ']),
    case('nes-kill', 'games nes', 'mctest 3 120000; ' + X + 'nes nova & sleep 8; killall -9 nes.bin; sleep 1; mctest 3 120000', True,
         count=[(r'^mctest: PASS', 2)]),
    # nxphys (docs/physics.md): the wall with every worker, with none, the tower, the heap, a throw
    *[case(f'nxphys-{n}', 'games nxphys', X + pre + 'nxphys', True, has=[rf'sum {s}'] + extra) for n, pre, s, extra in (
        ('wall', 'PHYS_COUNT=1 PHYS_SCENE=1 PHYS_STEPS=120 ', 'b28b77b4', [r'0 pages brought for workers']),
        ('wall-7', 'PHYS_WORKERS=7 PHYS_SCENE=1 PHYS_STEPS=120 ', 'b28b77b4', []),
        ('tower', 'PHYS_SCENE=4 PHYS_BODIES=13 PHYS_STEPS=300 ', '3d3402c2', [r'highest 6899 mm']),
        ('heap', 'PHYS_WORKERS=0 PHYS_SCENE=3 PHYS_BODIES=150 PHYS_STEPS=90 ', 'ed123738', []),
        ('scene-2', 'PHYS_WORKERS=0 PHYS_SCENE=2 PHYS_STEPS=60 ', '51fae5a5', []),
        ('throw', 'PHYS_SCENE=1 PHYS_LAUNCH=40:1 PHYS_STEPS=120 ', '8cccf9ba', []))],
    case('nxphys-kill', 'games nxphys', X + 'nxphys & sleep 5; killall -9 nxphys; sleep 1; mctest 15 2000', True, has=[r'mctest: PASS']),
    case('nxphys-none', 'games nxphys long', X + 'PHYS_WORKERS=0 PHYS_SCENE=1 PHYS_STEPS=120 nxphys', has=[r'sum b28b77b4']),
    # Micropolis (docs/micropolis.md): a city and two scenarios with a fixed seed, the sums of the build computer's run
    case('micropolis', 'games', 'MICROPOLIS_SUM=/usr/share/micropolis/cities/haight.cty,1000 nxmicropolis; '
         'MICROPOLIS_SUM=1,500 nxmicropolis; MICROPOLIS_SUM=8,1500 nxmicropolis',
         sums=[(r'micropolis: .* ticks: .* sum ([0-9a-f]+)', ['909517ac', 'e14d1a50', '75e86f73'])]),
    # nxray and nxpath (docs/raytrace.md)
    case('nxray', 'workers', X + 'RAY_FRAMES=4 nxray', True, sums=[(r'raystat: picture \d.* sum ([0-9a-f]+)', ['9383472e', '226b5e92', 'c69f022b', '337a52f6'])],
         has=[r'with 255 workers']),
    case('nxray-none', 'workers long', X + 'RAY_WORKERS=0 RAY_FRAMES=4 nxray', wall=600,
         sums=[(r'raystat: picture \d.* sum ([0-9a-f]+)', ['9383472e', '226b5e92', 'c69f022b', '337a52f6'])]),
    case('nxpath', 'workers', X + 'NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SIZE=160x120 NXPATH_SAMPLES=2 nxpath', True, has=[r'02f213e9']),
    case('nxpath-none', 'workers long', X + 'NXPATH_CORES=0 NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SIZE=160x120 NXPATH_SAMPLES=2 nxpath', wall=600, has=[r'02f213e9']),
    case('nxpath-check', 'workers', X + 'NXPATH_CHECK=500 nxpath', True, until='thousandths of a pixel',
         has=[r'nxpath: 0 of 500 rays met another object']),
    # nxpath's example scenes (docs/raytrace.md, "Example scenes"): the same sum with every worker and with none
    *[case(f'nxpath-{n}{tag}', 'workers pathtrace', X + f'{pre}NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SIZE={size} NXPATH_SAMPLES=2 nxpath {n}',
           not pre, has=[rf'pathstat: {size.replace("x", " x ")}, 2 samples.* sum {s}'])
      for n, size, s in (('cornell', '120x120', '78c07525'), ('cornell-balls', '120x120', 'b962c063'), ('balls', '160x120', 'b85eb16e'),
                         ('materials', '160x120', '5ecc7a24'), ('beer', '160x120', '3c29787e'), ('mirrors', '160x120', '2f211b98'),
                         ('glow', '160x120', '2858a999'))
      for tag, pre in (('', ''), ('-none', 'NXPATH_CORES=0 '))],
    # a scene saved and opened again is the same scene: the start's and two examples' sums
    *[case(f'nxpath-saved-{n or "start"}', 'workers pathtrace', X + f'NXPATH_WRITE=/tmp/s.scene NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SIZE=8x8 NXPATH_SAMPLES=1 nxpath {n}'
           f' && NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SIZE={size} NXPATH_SAMPLES=2 nxpath /tmp/s.scene', True,
           has=[r'nxpath: /tmp/s\.scene written', rf'pathstat: {size.replace("x", " x ")}, 2 samples.* sum {s}'])
      for n, size, s in (('', '160x120', '02f213e9'), ('cornell', '120x120', '78c07525'), ('materials', '160x120', '5ecc7a24'))],
    case('nxpath-cornell-check', 'workers pathtrace', X + 'NXPATH_CHECK=500 nxpath cornell', True, until='thousandths of a pixel',
         has=[r'nxpath: 0 of 500 rays met another object', r'at most 0 thousandths of a pixel']),
    # libmedia and the players (docs/media.md, docs/play.md, docs/video.md)
    case('media-sound', 'media', '; '.join(f'media_test audio /usr/share/play-test.{e} /tmp/o.raw {w}' for e in ('flac', 'mp3', 'mod') for w in (0, 63)),
         True, wall=400, sums=[(r'audio /usr/share/play-test\.\w+: .* sum ([0-9a-f]+)', ['518c5ac8'] * 2 + ['aa07a2e5'] * 2 + ['c714a845'] * 2)]),
    case('media-mpeg', 'media', 'media_test video "/root/Videos/Big Buck Bunny.mpg" 8', True, wall=400,
         has=[r"pictures' sum d124dc14", r"sound's sum 96a296a2"]),
    case('media-avi', 'media', 'media_test video "/root/Videos/Big Buck Bunny.avi" 16', True, wall=400,
         has=[r"pictures' sum 3129e37f", r"sound's sum 6dd01fde"]),
    case('media-avi-none', 'media long', 'media_test video "/root/Videos/Big Buck Bunny.avi" 0', wall=600,
         has=[r"pictures' sum 3129e37f", r"sound's sum 6dd01fde"]),
    case('nxplay', 'media', '; '.join(f'NXPLAY_SUM=1 nxplay /usr/share/play-test.{e}' for e in ('wav', 'flac', 'mod', 'mp3')) +
         '; NXPLAY_WORKERS=0 NXPLAY_HALF=0 NXPLAY_SUM=1 nxplay /usr/share/play-test.mp3', True, wall=600,
         sums=[(r'nxplay: sum ([0-9a-f]+ of \d+) samples', ['e5fd8f8c of 141120', '1a14a562 of 176400', '98e66905 of 1354752',
                                                            '809816e7 of 1594368', '809816e7 of 1594368'])]),
    case('nxvideo', 'media', X + 'NXVIDEO_SUM=1 NXVIDEO_ONCE=1 nxvideo; NXVIDEO_SUM=1 NXVIDEO_ONCE=1 nxvideo "/root/Videos/Big Buck Bunny.avi"',
         True, wall=400, sums=[(r'nxvideo: sum ([0-9a-f]+ of \d+) pictures', ['d124dc14 of 480', '3129e37f of 96'])]),
    case('nxvideo-quit', 'media', X + 'nxvideo & sleep 9; nxkey q; sleep 4; pidof nxvideo; echo PIDOF=$?; mctest 255 20000', True,
         has=[r'^PIDOF=1', r'mctest: PASS']),
    # docs/video.md, "The window": a second start and Open... while a film plays, a smaller desktop, a codec not played,
    # a GL program while a film plays (refused in a window, the film on)
    case('nxvideo-second', 'media', X + 'nxvideo & sleep 10; nxvideo "/root/Videos/Big Buck Bunny.avi"; echo SECOND=$?; sleep 8; pidof nxvideo; echo PIDOF=$?',
         True, has=[r'^nxvideo: given to the player that is playing', r'^SECOND=0', r'^nxvideo: Big Buck Bunny\.avi, AVI 320 x 176', r'^PIDOF=0'],
         never=[r'the GPU is drawing for']),
    case('nxvideo-open', 'media', X + 'nxvideo & sleep 10; nxkey o; sleep 3; nxkey b enter; sleep 8; pidof nxvideo; echo PIDOF=$?', True,
         count=[(r'^nxvideo: Big Buck Bunny\.\w+, ', 2)], has=[r'^PIDOF=0'], never=[r'the GPU is drawing for']),
    case('nxvideo-fit', 'media', X + 'NXVIDEO_SCREEN=300x260 NXVIDEO_PRINT=1 NXVIDEO_SUM=1 NXVIDEO_ONCE=1 nxvideo', True, wall=400,
         has=[r'^nxvideo: 320 x 176 shown at 280 x 154, the window 280 x 194', r'nxvideo: sum d124dc14 of 480 pictures']),
    case('nxvideo-codec', 'media', X + 'cp "/root/Videos/Big Buck Bunny.avi" /tmp/x.avi; printf FMP4 | dd of=/tmp/x.avi bs=1 seek=188 conv=notrunc 2>/dev/null; '
         'UI_ASK_PRINT=1 nxvideo /tmp/x.avi & sleep 6; killall nxvideo',
         has=[r'^ask: Video: x\.avi MPEG-4 Part 2 \(FMP4\) pictures: not played \(this plays MJPEG in AVI\)']),
    case('nxvideo-gpu-taken', 'media', X + 'nxvideo & sleep 10; UI_ASK_PRINT=1 RAY_FRAMES=2 nxray; echo RAY=$?; sleep 4; pidof nxvideo; echo PIDOF=$?; killall nxask',
         True, has=[r'^RAY=1', r'^ask: Notice: The GPU is drawing for nxvideo ', r'^PIDOF=0']),
    # the desktop's programs (docs/nanox.md, docs/multicore.md, docs/web.md)
    case('nxmon', 'desktop', X + 'NXMON_CHECK=1 nxmon & sleep 20; killall nxmon', True, has=[r'nxmon: 0 of 51840 pixels differ'],
         never=[r'nxmon: [1-9]\d* of 51840']),
    case('nxview', 'desktop media', 'NXVIEW_SUM=1 nxview --decode /usr/share/picture-fox.png /tmp/a.ppm; '
         'NXVIEW_SUM=1 nxview --decode /usr/share/picture-fox.jpg /tmp/b.ppm', True,
         sums=[(r'nxview: .* sum ([0-9a-f]+) of', ['72ae6c0d', '95b8fd40'])]),
    case('nxpaint', 'desktop', X + 'NXPAINT_SCRIPT="new:200x120 fg:ff0000 tool:Rect size:2 drag:20,20:120,90 sum tool:Fill fg:00ff00 '
         'press:60,50 pixel:60,50 undo sum quit quit" nxpaint; NXPAINT_SCRIPT="new:40x30 saveas:/tmp/x sum open:/tmp/x.png sum quit quit" nxpaint',
         has=[r'pixel 60,50 is 00ff00', r'saved as /tmp/x\.png'], pairs=[r'nxpaint: (\d+ x \d+, sum [0-9a-f]+),']),
    case('nxask', 'desktop', X + "(nxkey 2500 n) & nxask 'Sure?' Yes No; echo ASK=$?", has=[r'^ASK=1']),
    case('nxfiles', 'desktop', X + 'NXFILES_PRINT=1 nxfiles /usr/share & sleep 6; nxkey drag:643,86:343,196; sleep 4',
         has=[r'nxfiles: the box selected 8 ']),
    # what opens a file and its icon (docs/open.md, "Checking it"): by first bytes, else by ending in any case
    case('open-kinds', 'desktop media', KINDS_T + 'for f in /root/Music/* /root/Videos/* /root/Pictures/* /tmp/t/*; do open --dry "$f"; done',
         count=[(r'^open: /root/Music/.* is sound: nxplay ', 6), (r'^open: /root/Videos/.* is video: nxvideo ', 2),
                (r'^open: /root/Pictures/.* is picture: nxview ', 7)],
         has=[rf'^open: /tmp/t/{n} is {k}: {p} ' for n, k, p in (
             ('song', 'sound', 'nxplay'), ('tone', 'sound', 'nxplay'), ('A.FLAC', 'sound', 'nxplay'), ('a.oga', 'sound', 'nxplay'),
             ('film', 'video', 'nxvideo'), ('a.vqa', 'video', 'nxvideo'), ('fox', 'picture', 'nxview'), ('a.tga', 'picture', 'nxview'),
             ('a.pnm', 'picture', 'nxview'), ('a.pbm', 'picture', 'nxview'))] + [r'^open: /tmp/t/a\.xyz: unknown kind']),
    case('nxfiles-kinds', 'desktop media', KINDS_T + 'for d in /root/Music /root/Videos /root/Models /tmp/t /usr/bin; do nxfiles --kinds $d; done',
         has=[r'^nxfiles: Beethoven - Moonlight Sonata\.ogg: Sound, icon tune', r'^nxfiles: Big Buck Bunny\.avi: Video, icon video',
              r'^nxfiles: teapot\.obj: File, icon file', r'^nxfiles: a\.tga: Picture, icon picture', r'^nxfiles: a\.vqa: Video, icon video',
              r'^nxfiles: song: File, icon file', r'^nxfiles: nxplay: Program, icon nxplay', r'^nxfiles: nxgame: Program, icon nxgame'],
         count=[(r'^nxfiles: .*: Sound, ', 8)], never=[r'^nxfiles: (song|tone|film|fox|[^:]*\.(ogg|oga|flac|mp3|mod|wav|avi|mpg|vqa|tga|pnm|pbm|obj|ply|xyz|FLAC)): Program']),
    case('nxview-unnamed', 'desktop media', X + 'cp /root/Pictures/Earthrise.jpg /tmp/pic; NXVIEW_PRINT=1 nxview /tmp/pic & sleep 8; killall nxview',
         has=[r'^nxview: /tmp/pic, 480 x 480$']),
    case('nxweb', 'desktop', X + 'NXWEB_DUMP=1 nxweb & sleep 30; killall nxweb',   # tools/web_test.c gives the same
         has=[r'nxweb: /usr/share/web-index\.html: 122 nodes, 4 rules, 586 x 872; 77 texts \(1836 characters\), 2 boxes, 0 pictures, 0 fields']),
    # docs/web.md, "Loading, and what did not come": each failure and why, a box for each picture
    case('nxweb-missing', 'desktop', X + 'NXWEB_DUMP=1 nxweb /usr/share/web-missing.html & sleep 20; killall nxweb', True,
         has=[r'nxweb: /usr/share/web-missing\.html: 36 nodes, 0 rules, 586 x 480; 10 texts \(520 characters\), 0 boxes, 5 pictures',
              r'nxweb: loading: Loading web-missing\.html: 0 of 1 picture \(5 not loaded\)',
              r'nxweb: not loaded: style sheet /usr/share/web-gone\.css: not found$',
              r'nxweb: not loaded: picture /usr/share/web-gone\.png: not found$', r'nxweb: not loaded: picture /usr/share/web-gone\.gif: not found$',
              r'nxweb: not loaded: picture /usr/share/web-test\.html: unsupported format', r'nxweb: /usr/share/web-missing\.html: all here, 5 not loaded']),
    # (no host answers rvc_cpu's fetches: the picture is given up after 30 s, its box laid out)
    case('nxweb-no-answer', 'desktop', X + 'NXWEB_DUMP=1 nxweb /usr/share/web-test.html & sleep 45; killall nxweb',
         has=[r'nxweb: loading: Loading web-test\.html: 0 of 1 picture$', r'nxweb: not loaded: picture https://example\.com/x\.png: no answer in 30 s',
              r'nxweb: /usr/share/web-test\.html: 88 nodes, 1 rules, 586 x 636; 40 texts \(520 characters\), 1 boxes, 1 pictures']),
    # a short first page ("No such page"): its strips were never made, nothing was shown
    case('nxweb-short', 'desktop', X + 'NXWEB_TIME=1 nxweb /usr/share/web-nowhere.html & sleep 10; killall nxweb', has=[r'nxweb: the first picture \d+ ms after']),
    case('launch-note', 'desktop', 'export UI_LAUNCH=print; nx; sleep 4; nxkey 266,94 266,94; sleep 4', wall=120,
         has=[r'^launch: Calculator', r'^launch: a window shown after \d+ ms']),   # docs/nanox.md, "A program starting"
]


def matches(text, pattern):
    return [m.group(0) if m.lastindex is None else ' '.join(m.groups()) for m in re.finditer(pattern, text, re.M)]


def digest(items):
    return hashlib.md5('\n'.join(items).encode()).hexdigest()[:8]


def check(c, text, err, races, done):
    """The reasons the case failed (an empty list if it passed)."""
    why = []
    if '--until matched' not in err:
        why.append(f'did not reach "{c["until"]}"')
    for p in c.get('has', []):
        if not re.search(p, text, re.M):
            why.append(f'no line /{p}/')
    for p in c.get('never', []):
        if re.search(p, text, re.M):
            why.append(f'a line /{p}/')
    for p, n in c.get('count', []):
        got = len(re.findall(p, text, re.M))
        if not (n[0] <= got <= n[1] if isinstance(n, tuple) else got == n):
            why.append(f'{got} lines /{p}/, not {n}')
    for p, want in c.get('sums', []):
        got = matches(text, p)
        if got != want:
            why.append(f'/{p}/ gave {got}, not {want}')
    for p, want, *how in c.get('digest', []):
        got = matches(text, p)
        d = digest(sorted(got) if how else got)
        if d != want:
            why.append(f'/{p}/: {len(got)} lines, digest {d}, not {want}')
    for p in c.get('pairs', []):   # every second match the one before it (an undo, a file read back)
        got = matches(text, p)
        if len(got) < 2 or any(got[i] != got[i + 1] for i in range(0, len(got) - 1, 2)):
            why.append(f'/{p}/ not in equal pairs: {got}')
    for p, other, *least in c.get('same', []):
        if other not in done:
            why.append(f'{other} was not run')
            continue
        if least:   # keyed by the first group: compared where both have the key
            a = {m[1]: m.groups()[1:] for m in re.finditer(p, text, re.M)}
            b = {m[1]: m.groups()[1:] for m in re.finditer(p, done[other], re.M)}
            both = [k for k in a if k in b]
            n = sum(a[k] == b[k] for k in both)
            if n != len(both) or n < least[0]:
                why.append(f'/{p}/: {n} of {len(both)} as in {other} (at least {least[0]})')
            continue
        a, b = matches(text, p), matches(done[other], p)
        if a != b:
            n = sum(x == y for x, y in zip(a, b))
            why.append(f'/{p}/: {n} of {max(len(a), len(b))} as in {other}')
    if races:
        m = re.search(r'(\d+) changes undone', err)
        if not m or m[1] != '0':
            why.append(f'RVC_RACES: {m[1] if m else "no"} changes undone')
    return why


def images(a):
    """The image folder of each kind of case: the one given, a private one with zerotest, the lazy test's."""
    out = {None: a.image}
    if any(c['image'] == 'zerotail' for c in a.run):
        name = 'test_all'
        bin_ = os.path.join(ROOT, 'build', 'images', name, 'overlay', 'usr', 'bin')
        os.makedirs(bin_, exist_ok=True)
        repo = '/mnt/' + ROOT[0].lower() + ROOT[2:].replace('\\', '/')
        script = f'source {repo}/linux/userland/toolchain.sh && cd {repo} && ' + ' && '.join(
            f'rv32-cc -O2 linux/kernel/{t}.c -o build/images/{name}/overlay/usr/bin/{t}' for t in ('zerotest', 'tailtest'))
        subprocess.run(['wsl', '--', 'bash', '-c', script], check=True)
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'private_image.py'), name], check=True, capture_output=True)
        out['zerotail'] = f'build/images/{name}'
    if any(c['image'] == 'lazy' for c in a.run):
        folder = os.path.join(ROOT, 'build', 'test_all_lazy')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'make_lazy_image.py'), folder], check=True, capture_output=True)
        out['lazy'] = os.path.join(folder, 'build', 'images', 'linux')
    return out


def run(c, a, image, logs):
    log = os.path.join(logs, c['name'] + '.log')
    if os.path.exists(log):
        os.remove(log)
    args = [os.path.join(ROOT, 'bin', 'rvc_cpu.exe'), '--quiet', '--uart-log', log, '--wall', str(c['wall']), '--image', image]
    if c['cmd'] is not None:
        args += ['--expect', '/ # ', '--send', c['cmd'] + '; ' + DONE + '\\n']
    args += ['--until', c['until']] + c['more']
    env = dict(os.environ)
    env.pop('RVC_RACES', None)
    if a.card_model:
        env.update(RVC_SOUND='1', RVC_WCACHE='1', RVC_ADFAULT='1')
    races = a.races and c['workers']
    if races:
        env['RVC_RACES'] = log + '.races'
    env.update(c['env'])
    t = time.time()
    for _ in range(60):   # the harness lock refuses a CPU harness beside a GPU one: wait for it
        p = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True, errors='replace')
        if '[lock]' not in p.stderr or 'refused' not in p.stderr:
            break
        time.sleep(20)
    text = open(log, encoding='latin-1').read() if os.path.exists(log) else ''
    return text, p.stderr, races, time.time() - t


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('select', nargs='*', help='case names or tags (default: every case not tagged long)')
    ap.add_argument('--long', action='store_true', help='the long cases too')
    ap.add_argument('--races', action='store_true', help='RVC_RACES on the cases that use workers: 0 changes undone')
    ap.add_argument('--card-model', action='store_true', help="rvc_cpu as the card: RVC_SOUND, RVC_WCACHE, RVC_ADFAULT")
    ap.add_argument('--image', default='build/images/linux', help='the image folder (default build/images/linux)')
    ap.add_argument('--jobs', type=int, default=max(2, (os.cpu_count() or 4) // 2), help='cases at once')
    ap.add_argument('--logs', default=os.path.join(ROOT, 'build', 'test_all'), help='where the consoles go')
    ap.add_argument('--list', action='store_true', help='list the cases and stop')
    a = ap.parse_args()
    if a.select:
        a.run = [c for c in CASES if c['name'] in a.select or c['tags'] & set(a.select)]
    else:
        a.run = [c for c in CASES if a.long or 'long' not in c['tags']]
    if a.list or not a.run:
        for c in a.run or CASES:
            print(f"{c['name']:28} {' '.join(sorted(c['tags']))}")
        return 0 if a.run else 1
    os.makedirs(a.logs, exist_ok=True)
    where = images(a)
    done, failed, t0 = {}, [], time.time()
    # a case compared with another runs after it
    first = [c for c in a.run if not c.get('same')]
    later = [c for c in a.run if c.get('same')]
    for group in (first, later):
        with ThreadPoolExecutor(a.jobs) as ex:
            futures = [(c, ex.submit(run, c, a, where[c['image']], a.logs)) for c in group]
            for c, f in futures:
                text, err, races, secs = f.result()
                done[c['name']] = text
                why = check(c, text, err, races, done)
                print(f"{'PASS' if not why else 'FAIL'} {c['name']:28} {secs:5.0f} s" + ('' if not why else '  ' + '; '.join(why)), flush=True)
                if why:
                    failed.append(c['name'])
    print(f'{len(a.run) - len(failed)} of {len(a.run)} passed in {time.time() - t0:.0f} s' + (f'; failed: {" ".join(failed)}' if failed else '') +
          f' (consoles in {a.logs})')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
