"""A game's instructions a frame on core 0 and on all cores, under rvc_cpu (docs/multicore.md,
"Every core's share of a game's frame").

    python tools/game_par.py GAME [--image DIR] [--log FILE] [--calls FILE]
    python tools/game_par.py all [--runs 3]     (a mean over three timings: --salt)
    python tools/game_par.py GAME --profile PC.LOG; python tools/pc_profile.py PC.LOG kernel=K.nm game=G.nm

Each game runs a fixed workload from power-on (bin/rvc_cpu.exe, no card). rvc_cpu --count-from
counts every core from when the console says the game's start mark until --until; the frames
in that window come from the game's own lines. Factor = all cores' instructions / core 0's.
"""
import argparse, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DONE = "echo LX-''DONE"   # (the quotes keep the echo of the command from matching --until)


def doom_frames(log):
    # every "doom: N frames" line after the mark is a window; the first began before it
    lines = re.findall(r'doom: (\d+) frames in', log)
    total = int(re.search(r'timed (\d+) gametics', log).group(1))
    return total - int(lines[0])


GAMES = {
    'doom': dict(
        what='-timedemo demo1, one tic a frame, from its first rate line to the end',
        cmd='nano-X -p & sleep 2; doom -1 -timedemo demo1',
        mark='doom: ', frames=doom_frames),
    'quake': dict(
        what='+timedemo demo1, from the level\'s first frame to the timedemo\'s line',
        cmd='nano-X -p & sleep 2; quake +timedemo demo1', until=' fps',
        mark="a level's first frame is frame", frames=lambda log: int(re.search(r'(\d+) frames +[\d.]+ seconds', log)[1])),
    'quake3': dict(
        what='q3dm17 with one bot, fixedtime 33, put at 488 -968 378 looking north at frame 100, frames 300 to 700',
        cmd='nano-X -p & sleep 2; QUAKE3_STATS_FRAMES=100 quake3 +set fixedtime 33 +devmap q3dm17 +addbot sarge 3',
        more=['--expect', 'q3stat: frame 100:', '--send', 'setviewpos 488 -968 378 90\\n'],
        mark='q3stat: frame 300:', until='q3stat: frame 700:', frames=lambda log: 400),
    'classicube': dict(
        what='seed 7, block physics off, walking and jumping into a hill, five 2 s windows from "steady"',
        cmd='mkdir -p /tmp/classicube; echo singleplayerphysics=false >> /tmp/classicube/options.txt; '
            'nano-X -p & sleep 2; CLASSICUBE_SEED=7 CLASSICUBE_STATS_MS=2000 classicube',
        more=['--expect', 'classicube: steady', '--send', 'turn 1200 0\\ndown W\\ndown Space\\n', '--until-times', '5'],
        mark='classicube: steady', until='ccstat: a frame:', frames=lambda log: sum(
            int(n) for n in re.findall(r'ccstat: (\d+) frames', log[log.find('classicube: steady'):])[:5])),
    'tdawn': dict(
        what='mission 3, seed 7, frames 301 to 1200',
        cmd='nano-X -p & sleep 2; TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=3 TDAWN_FRAMES=1300 '
            'TDAWN_MARK=301,1201 tdawn FROMINSTALL',
        mark=': mark begins', until=': mark ends', frames=lambda log: 900),
    'ralert': dict(
        what='Soviet mission, seed 7, frames 301 to 1200',
        cmd='nano-X -p & sleep 2; TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7 RALERT_FRAMES=1300 RALERT_MARK=301,1201 ralert',
        mark=': mark begins', until=': mark ends', frames=lambda log: 900),
    'ralert-allies': dict(
        what='Allied mission, seed 7, frames 301 to 1200',
        cmd='nano-X -p & sleep 2; TDAWN_AUTO=1 RALERT_SIDE=allies RALERT_SEED=7 RALERT_FRAMES=1300 RALERT_MARK=301,1201 ralert',
        mark=': mark begins', until=': mark ends', frames=lambda log: 900),
    'nes': dict(
        what='Thwaite, the 1,000-frame run of docs/nes.md, the list by a worker, NES frames 100 to 1000',
        cmd='nano-X -p & sleep 2; NES_LIST=worker NES_SOUND=0 NES_MARK=100 NES_FRAMES=1000 NES_PRESS="20 start 50 start '
            '80 a 100 left 110 a 200 a 210 +right 300 a 400 a 420 +up 500 a 800 a 900 -right 1000 a 1100 +left 1300 a" nes thwaite',
        mark='nesmark', frames=lambda log: 900),
    'nxphys': dict(
        what='the wall (scene 1), 120 substeps; a frame here is a substep',
        cmd='nano-X -p & sleep 2; PHYS_SCENE=1 PHYS_STEPS=120 nxphys',
        mark='shares on ', frames=lambda log: int(re.search(r'physstat: done: \d+ boxes, (\d+) substeps', log)[1])),
}


def run(name, g, image, log, calls, salt, show, profile=None):
    exe = os.path.join(ROOT, 'bin', 'rvc_cpu.exe')
    if os.path.exists(log):
        os.remove(log)
    args = [exe, '--quiet', '--uart-log', log, '--wall', str(g.get('wall', 280)), '--expect', '/ # ',
            '--send', (f': {salt}; ' if salt else '') + g['cmd'] + '; ' + DONE + '\\n', '--until', g.get('until', 'LX-DONE'), '--count-from', g['mark']] + g.get('more', [])
    if image:
        args += ['--image', image]
    if profile:   # (fine samples change the timing: a profile's counts are not the baseline's)
        args += ['--pc-log', profile, '--pc-from', g['mark'], '--ticks', '256', '--pass-ms', '0.0078']
    env = dict(os.environ)
    env.update(g.get('env', {}))
    if calls:
        env['RVC_CALLS'] = calls
    if show:
        print(subprocess.list2cmdline(args))
    t = time.time()
    p = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True, errors='replace')
    wall = time.time() - t
    err = p.stderr
    m = re.search(r'COUNT from "(?:[^"]*)": core 0 (\d+), all cores (\d+), (\d+) cores ran, the busiest worker (\d+), (\d+) passes, ([\d.]+) s', err)
    if not m or '--until matched' not in err:
        why = 'the mark was never said' if 'COUNT never' in err else 'it did not reach LX-DONE' if '--until NOT' in err else 'no COUNT line'
        print(f'{name}: not measured, {why} ({wall:.0f} s; console in {log})')
        return None
    c0, allc, ran, busiest, passes, secs = int(m[1]), int(m[2]), int(m[3]), int(m[4]), int(m[5]), float(m[6])
    text = open(log, encoding='latin-1').read()
    text = text[text.rfind('=== rvc_cpu ==='):]
    frames = g['frames'](text)
    r = dict(game=name, frames=frames, core0=c0 / frames, all=allc / frames, factor=allc / c0, workers=ran - 1,
             busiest=busiest / frames, seconds=secs, wall=wall)
    print(f"{name}: {frames} frames ({g['what']}); core 0 {r['core0'] / 1e3:.1f}k a frame, all cores {r['all'] / 1e3:.1f}k, "
          f"factor {r['factor']:.2f}; {r['workers']} workers ran, the busiest {r['busiest'] / 1e3:.1f}k a frame; "
          f"{secs:.1f} s of guest time, {wall:.0f} s here")
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('game', help='|'.join(GAMES) + '|all')
    ap.add_argument('--image', help='an image folder (default build/images/linux)')
    ap.add_argument('--log', help='the console (default build/game_par/GAME.log)')
    ap.add_argument('--calls', help='RVC_CALLS=FILE for tools/mc_candidates.py')
    ap.add_argument('--salt', type=int, default=0, help='N > 0: the command typed after ": N;", which shifts its timing')
    ap.add_argument('--runs', type=int, default=1, help='runs at salts SALT.. SALT+RUNS-1, and their mean')
    ap.add_argument('--profile', help="core 0's pc every 256 instructions from the mark, for tools/pc_profile.py")
    ap.add_argument('--show', action='store_true', help="print rvc_cpu's command line")
    a = ap.parse_args()
    names = list(GAMES) if a.game == 'all' else [a.game]
    os.makedirs(os.path.join(ROOT, 'build', 'game_par'), exist_ok=True)
    rows = []
    for n in names:
        if n not in GAMES:
            sys.exit(f'no game {n}: ' + ', '.join(GAMES))
        log = a.log or os.path.join(ROOT, 'build', 'game_par', n + '.log')
        got = [r for r in (run(n, GAMES[n], a.image, log, a.calls, a.salt + k, a.show, a.profile) for k in range(a.runs)) if r]
        if got:
            mean = {k: sum(r[k] for r in got) / len(got) for k in ('core0', 'all', 'factor', 'workers', 'frames')}
            mean.update(game=n, spread=(max(r['core0'] for r in got) - min(r['core0'] for r in got)) / mean['core0'])
            rows.append(mean)
    if len(rows) > 1 or a.runs > 1:
        print(f'\n| Game | Frames | Core 0 a frame | All cores a frame | Factor | Workers |' + (' Core 0 spread |' if a.runs > 1 else ''))
        print('|---|---|---|---|---|---|' + ('---|' if a.runs > 1 else ''))
        for r in rows:
            print(f"| {r['game']} | {r['frames']:.0f} | {r['core0'] / 1e3:.1f}k | {r['all'] / 1e3:.1f}k | {r['factor']:.2f} | {r['workers']:.0f} |"
                  + (f" {100 * r['spread']:.1f}% |" if a.runs > 1 else ''))


if __name__ == '__main__':
    main()
