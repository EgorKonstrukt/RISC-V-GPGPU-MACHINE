"""Fuzzing the two Command & Conquer games under bin/rvc_cpu.exe: random but plausible input from a seed,
through the machine's own input words (pointer, buttons, key ring; docs/machine/input.md), never the real
mouse. Each run boots the image, starts the game as tools/test_all.py does, plays N frames and is judged:
crashes (the process dying, the kernel's or the C library's lines), hangs (no end by the guest's time),
wrong state (the game's own checks: "N of them wrong", RVC_RACES "N changes undone") and long frames
(TDAWN_FRAME_LOG / RALERT_FRAME_LOG). A run is --threads 1, so a seed is the same run every time.

    python tools/cc_fuzz.py run --game td --seeds 1-200 [--jobs 8] [--frames 1200] [--out DIR]
    python tools/cc_fuzz.py replay --game td --seed 17 [--steps N] [--env K=V ...] [--keep]
    python tools/cc_fuzz.py shrink --game td --seed 17      the fewest steps that still give the same failure
    python tools/cc_fuzz.py gen --game td --seed 17         the run's settings and input script, printed
    python tools/cc_fuzz.py report DIR                      summary of a run's results.jsonl again

The input needs rvc_cpu's --input-script ("MS p X Y", "MS b B", "MS k|u CODE", "MS w +1|-1"; MS after
--input-from): --cpu names a build that has it. A "step" is one action (a click, a drag, a key, a burst);
--steps N gives only the first N, for a replay.
"""
import argparse, json, os, random, re, statistics, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
X = 'nano-X -p & sleep 2; '

# Linux key codes (linux/input-event-codes.h)
K = dict(esc=1, enter=28, ctrl=29, shift=42, alt=56, space=57, tab=15, backspace=14, home=102, up=103, pgup=104,
         left=105, right=106, end=107, down=108, pgdn=109, delete=111, **{f'f{i}': 58 + i for i in range(1, 11)})
for _row, _code in (('1234567890', 2), ('qwertyuiop', 16), ('asdfghjkl', 30), ('zxcvbnm', 44)):
    for _i, _c in enumerate(_row):
        K[_c] = _code + _i
K[' '] = K['space']

# A game: how it is started, where its screen lies on the 800x600 display of `nano-X -p`, its regions in
# the game's own pixels, its hotkeys, and the console lines that judge it.
GAMES = {
    'td': dict(
        missions=[1, 3, 5, 6, 10],
        # TDAWN_MARK=1,F-1 says "tdawn: mark begins" at frame 1: the input starts there
        cmd=lambda m, f, extra: f'TDAWN_AUTO=1 TDAWN_SEED={m["gseed"]} TDAWN_INPUT_LOG=1 TDAWN_FRAME_LOG=1 TDAWN_STATS_MS=2000 '
                                f'TDAWN_MARK=1,{f - 1} TDAWN_SCEN={m["mission"]} TDAWN_FRAMES={f} {extra}tdawn FROMINSTALL',
        start='tdawn: mark begins',
        done=r'^tdawn: (?:done|stopped) at frame (\d+): (\S+), (\d+) units, (\d+) infantry, (\d+) buildings, (\d+) overlays, state ([0-9a-f]+)',
        stat=r'^tdstat: frame (\d+): ',
        origin=(10, 10), scale=2, screen=(320, 200),   # the window's corner on the display, display pixels a game pixel
        map=(0, 10, 239, 199),                          # x0, y0, x1, y1 of the map in game pixels
        sidebar=dict(radar=(244, 10, 316, 72), buttons=[(258, 82), (286, 82), (306, 82)],
                     cameos=[(c, r) for r in (102, 126, 150, 174) for c in (264, 299)], arrows=[(264, 192), (299, 192)]),
        tabs=[(40, 3), (280, 3)],                       # Options, Sidebar
        menu=(80, 30, 240, 170),                        # where a dialog's buttons are
        hotkeys='hgsxe1234567890 ',
        checks={'paths': {'TDAWN_CHECK_PATHS': '1'}, 'occupy': {'TDAWN_CHECK_OCCUPY': '1'}, 'sort': {'TDAWN_CHECK_SORT': '1'},
                'ready': {'TDAWN_CHECK_READY': '1'}, 'enter': {'TDAWN_CHECK_ENTER': '1'}},
        resize=['TDAWN_RESIZE=794x545 ', 'TDAWN_RESIZE=640x400 ', 'TDAWN_RESIZE=400x300 '],
        speed_env=None,
    ),
    # fuzz-ra's: 640x400 at 1:1; the window's client corner at 43,46 (TDAWN_INPUT_LOG: display 400,300 is 357,254).
    # Sidebar from redalert/sidebar.h doubled: buttons y 158, strips from y 180, cameos 64x48, columns at x 496 and 566.
    'ra': dict(
        missions=['allies', 'soviet'],
        cmd=lambda m, f, extra: f'TDAWN_AUTO=1 RALERT_SIDE={m["mission"]} RALERT_SEED={m["gseed"]} TDAWN_INPUT_LOG=1 RALERT_FRAME_LOG=1 '
                                f'RALERT_STATS_MS=2000 RALERT_MARK=1,{f - 1} RALERT_FRAMES={f} {extra}ralert',
        start='ralert: mark begins',
        done=r'^ralert: (?:done|stopped) at frame (\d+): (\S+), (\d+) units, (\d+) infantry, \d+ vessels, (\d+) buildings, (\d+) overlays, state ([0-9a-f]+)',
        stat=r'^rastat: frame (\d+): ',
        origin=(43, 46), scale=1, screen=(640, 400),
        map=(0, 16, 479, 399),
        sidebar=dict(radar=(484, 20, 636, 150), buttons=[(514, 166), (572, 166), (616, 166)],
                     cameos=[(c, r) for r in (204, 252, 300, 348) for c in (528, 598)], arrows=[(528, 386), (598, 386)]),
        tabs=[(80, 6), (560, 6)],
        menu=(160, 60, 480, 340),
        hotkeys='hgsxe1234567890 ',
        checks={'paths': {'RALERT_CHECK_PATHS': '1'}, 'occupy': {'RALERT_CHECK_OCCUPY': '1'}, 'sort': {'RALERT_CHECK_SORT': '1'},
                'scans': {'RALERT_CHECK_SCANS': '1'}, 'zones': {'RALERT_CHECK_ZONES': '1'}},
        resize=[],
        speed_env=None,
    ),
}

# what a console line is when it is no failure: everything else after the command is listed as odd
BENIGN = re.compile(r'^(tdstat|rastat|tdinput|tdworkers|raworkers|frame \d+:|tdawn: (mark|done|stopped)|ralert: (mark|done|stopped)'
                    r'|nano-X: (client closed|GsHandleClient read failed)|\[\d+\]\+? +(Done|Running)|\[\d+\] \d+ |ShaderEmu display|LX-DONE'
                    r'|CCFUZZ-EXIT|\s*$|.*TDAWN_AUTO=|\[ *[\d.]+\] random: crng init done|tdawn: \d+ scenes. objects drawn|.* 0 of them (wrong|differ))')
CRASH = re.compile(r'Segmentation fault|Illegal instruction|Bus error|Aborted|out of memory|Oops|Kernel panic|BUG:|Unable to handle'
                   r'|Unhandled|page allocation failure|Killed|core dumped|terminate called|pure virtual|assert', re.I)
WRONG = re.compile(r'(\d+) of them (?:wrong|differ)|(\d+) cells\' occupancy bits were wrong')


def parse_seeds(text):
    out = []
    for part in text.split(','):
        a, _, b = part.partition('-')
        out += list(range(int(a), int(b) + 1)) if b else [int(a)]
    return out


class Script:
    """The input as timed events, grouped into steps."""

    def __init__(self, g, resize=''):
        self.g, self.events, self.steps, self.t = g, [], [], 0.0
        size = re.search(r'=(\d+)x(\d+)', resize)
        self.resized = (int(size[1]) / g['screen'][0], int(size[2]) / g['screen'][1]) if size else None

    def disp(self, x, y):
        ox, oy = self.g['origin']
        sx = sy = self.g['scale']
        if self.resized and self.t > 4000:   # (TDAWN_RESIZE is the window's size from its 60th frame: the picture stretched over it)
            sx, sy = self.resized
        return int(ox + (x + 0.5) * sx), int(oy + (y + 0.5) * sy)

    def step(self, name):
        self.steps.append((name, len(self.events), self.t))

    def ev(self, *a):
        self.events.append((round(self.t, 1),) + a)

    def wait(self, ms):
        self.t += ms

    def move(self, x, y, raw=False):
        self.ev('p', *((x, y) if raw else self.disp(x, y)))

    def click(self, x, y, button=1, hold=40, raw=False):
        self.move(x, y, raw)
        self.wait(20)
        self.ev('b', button)
        self.wait(hold)
        self.ev('b', 0)
        self.wait(20)

    def drag(self, a, b, button=1, n=6, dt=30):
        self.move(*a)
        self.wait(20)
        self.ev('b', button)
        for i in range(1, n + 1):
            self.wait(dt)
            self.move(a[0] + (b[0] - a[0]) * i // n, a[1] + (b[1] - a[1]) * i // n)
        self.wait(30)
        self.ev('b', 0)
        self.wait(20)

    def key(self, code, mods=()):
        for m in mods:
            self.ev('k', m)
        self.ev('k', code)
        self.wait(30)
        self.ev('u', code)
        for m in mods:
            self.ev('u', m)
        self.wait(30)


def guest_limit(m):
    """The guest's seconds: boot, start, and the frames at no fewer than ten a second (four with a check of every cell)."""
    return 25 + m['frames'] / (4 if m['check'] == 'occupy' else 10)


def plan(game, seed, frames, mix='all'):
    """The run's settings and input from its seed: (settings dict, Script)."""
    g = GAMES[game]
    r = random.Random(seed * 7919 + (0 if game == 'td' else 1))
    m = dict(seed=seed, game=game, mission=r.choice(g['missions']), gseed=r.randrange(1, 1000), frames=frames, mix=mix)
    m['check'] = r.choice(['none', 'none'] + sorted(g['checks']) + ['races'])
    m['resize'] = r.choice(g['resize']) if g['resize'] and r.random() < 0.15 else ''
    m['desk'] = r.random() < 0.15   # another program's window on the desktop beside the game
    m['style'] = r.choice(['player', 'player', 'player', 'spammer', 'menus'])
    s = Script(g, m['resize'])
    x0, y0, x1, y1 = g['map']
    sb = g['sidebar']

    def mp():
        return r.randint(x0, x1), r.randint(y0, y1)

    def edge():
        side = r.randrange(4)
        return ((x0, r.randint(y0, y1)), (x1, r.randint(y0, y1)), (r.randint(x0, x1), y0), (r.randint(x0, x1), y1))[side]

    acts = {
        'click_map': (10, lambda: s.click(*mp())),
        'order': (10, lambda: (s.drag(mp(), mp()), s.wait(r.randint(50, 400)), s.click(*mp()))),
        'select_all_order': (5, lambda: (s.drag((x0 + 1, y0 + 1), (x1 - 1, y1 - 1), n=8), s.wait(100), s.click(*edge()))),
        'drag_select': (8, lambda: s.drag(mp(), mp(), n=r.randint(2, 10))),
        'double_click': (5, lambda: (lambda p: (s.click(*p), s.wait(r.randint(30, 200)), s.click(*p)))(mp())),
        'right_click': (6, lambda: s.click(*mp(), button=2)),
        'map_drag': (5, lambda: s.drag(mp(), mp(), button=2, n=r.randint(3, 12))),
        'build': (10, lambda: s.click(*r.choice(sb['cameos']), button=r.choice([1, 1, 1, 2]))),
        'place': (8, lambda: (s.click(*r.choice(sb['cameos'])), s.wait(r.randint(100, 3000)), s.click(*r.choice(sb['cameos'])),
                              s.wait(200), s.click(*mp()))),
        'side_button': (5, lambda: (s.click(*r.choice(sb['buttons'])), s.wait(r.randint(50, 500)), s.click(*mp()))),
        'side_arrow': (3, lambda: s.click(*r.choice(sb['arrows']))),
        'radar': (3, lambda: s.click(r.randint(sb['radar'][0], sb['radar'][2]), r.randint(sb['radar'][1], sb['radar'][3]),
                                     button=r.choice([1, 2]))),
        'tab': (2, lambda: (s.click(*r.choice(g['tabs'])), s.wait(r.randint(200, 1500)),
                            [s.click(r.randint(g['menu'][0], g['menu'][2]), r.randint(g['menu'][1], g['menu'][3])) for _ in range(r.randint(1, 4))],
                            s.wait(300), s.key(K['esc']), s.wait(300), s.key(K['esc']))),
        'escape_menu': (2, lambda: (s.key(K['esc']), s.wait(r.randint(200, 1500)),
                                    [s.click(r.randint(g['menu'][0], g['menu'][2]), r.randint(g['menu'][1], g['menu'][3])) for _ in range(r.randint(1, 5))],
                                    s.wait(300), s.key(r.choice([K['esc'], K['enter']])), s.wait(300), s.key(K['esc']))),
        'hotkey': (8, lambda: s.key(K[r.choice(g['hotkeys'])] if r.random() < 0.8 else r.choice(list(K.values())),
                                    mods=r.choice([(), (), (), (K['ctrl'],), (K['alt'],), (K['shift'],)]))),
        'scroll_keys': (3, lambda: [s.key(r.choice([K['up'], K['down'], K['left'], K['right'], K['home'], K['pgup'], K['pgdn']]))
                                    for _ in range(r.randint(1, 8))]),
        'wheel': (1, lambda: (s.move(*mp()), s.ev('w', r.choice([1, -1])))),
        'leave': (2, lambda: (s.move(r.randint(0, 799), r.randint(560, 599), raw=True), s.wait(r.randint(100, 1500)),
                              s.click(r.randint(700, 799), r.randint(450, 599), raw=True), s.wait(200), s.move(*mp()))),
        'burst': (3, lambda: [s.click(*mp(), button=r.choice([1, 1, 2]), hold=5) for _ in range(r.randint(50, 300))]),
        'key_burst': (2, lambda: [s.key(r.choice(list(K.values()))) for _ in range(r.randint(20, 80))]),
        'idle': (6, lambda: s.wait(r.randint(300, 3000))),
    }
    weights = dict((k, v[0]) for k, v in acts.items())
    if m['style'] == 'spammer':
        weights.update(burst=15, key_burst=8, select_all_order=10, idle=1)
    elif m['style'] == 'menus':
        weights.update(escape_menu=12, tab=10, hotkey=12, idle=3)
    if mix == 'play':   # no menus or dialogs on purpose: more of the game itself
        weights.update(tab=0, escape_menu=0)
        if m['style'] == 'menus':
            weights.update(hotkey=12, order=14, build=14)
    names = list(weights)
    end = (guest_limit(m) - 10) * 1000   # input until the run's end: a game that stops while input comes is in a dialog
    s.wait(300)
    while s.t < end:
        name = r.choices(names, [weights[n] for n in names])[0]
        s.step(name)
        acts[name][1]()
        s.wait(r.randint(30, 700))
    return m, s


def write_script(s, path, steps=None):
    upto = s.steps[steps][1] if steps is not None and steps < len(s.steps) else len(s.events)
    marks = {i: (k, n) for k, (n, i, _) in enumerate(s.steps)}
    with open(path, 'w') as f:
        for i, e in enumerate(s.events[:upto]):
            if i in marks:
                f.write(f'# step {marks[i][0]} {marks[i][1]}\n')
            f.write(' '.join(str(v) for v in e) + '\n')
    return upto


def command(game, m):
    g = GAMES[game]
    extra = m['resize'] + ''.join(f'{k}={v} ' for k, v in g['checks'].get(m['check'], {}).items())
    pre = X + ('(sleep 6; nxclock) & ' if m['desk'] else '')
    return pre + g['cmd'](m, m['frames'], extra) + '; echo CCFUZZ-EXIT=$?'


def judge(game, m, s, text, err, upto, secs):
    g = GAMES[game]
    lines = text.splitlines()
    try:
        at = next(i for i, l in enumerate(lines) if 'CCFUZZ-EXIT' in l and 'echo' in l)
    except StopIteration:
        at = 0
    out = lines[at + 1:]
    res = dict(m)
    res['secs'] = round(secs, 1)
    exit_m = re.search(r'^CCFUZZ-EXIT=(\d+)', text, re.M)
    done = re.search(g['done'], text, re.M)
    res['exit'] = int(exit_m[1]) if exit_m else None
    res['done'] = done.groups() if done else None
    stats = [int(x) for x in re.findall(g['stat'], text, re.M)]
    res['last_stat_frame'] = stats[-1] if stats else None
    gi = re.search(r'^\[input\] (\d+) of (\d+) events given', err, re.M)
    given = int(gi[1]) if gi else 0
    res['events_given'] = given
    res['steps_given'] = sum(1 for (_, i, _) in s.steps if i < given)
    res['steps_total'] = len(s.steps)
    res['inputs_seen'] = dict(buttons=len(re.findall(r'^tdinput: button', text, re.M)), keys=len(re.findall(r'^tdinput: key', text, re.M)))
    res['odd'] = [l.strip()[:200] for l in out if not BENIGN.match(l)][:20]
    crash = [l.strip()[:200] for l in out if CRASH.search(l)]
    wrong = []
    for l in out:
        w = WRONG.search(l)
        if w and int(w[1] or w[2]) > 0:
            wrong.append(l.strip()[:200])
    rv = re.search(r'(\d+) changes undone', err)
    if m['check'] == 'races':
        res['races_undone'] = int(rv[1]) if rv else None
        if not rv or rv[1] != '0':
            wrong.append(f'RVC_RACES: {rv[1] if rv else "no"} changes undone')
    wf = re.search(r"workers' faults (\d+)", err)
    res['worker_faults'] = int(wf[1]) if wf else None
    cpu = re.search(r'^\[cpu\] (\d+) passes in ([\d.]+)s, (\d+) guest instructions \((\d+) on worker cores\), ([\d.]+) s of guest time', err, re.M)
    if cpu:
        res['core0'], res['workers'], res['guest_s'] = int(cpu[3]), int(cpu[4]), float(cpu[5])
    if '[lock]' in err and 'refused' in err:
        res['verdict'] = 'refused'
    elif crash or (res['exit'] not in (None, 0)):
        res['verdict'] = 'crash'
    elif wrong:
        res['verdict'] = 'wrong'
    elif not done and res['exit'] is None:
        # frames stopped while the game still took input: a dialog it waits in (stuck), else a hang
        tail_after = text[text.rfind('stat: frame'):] if stats else text
        res['verdict'] = 'stuck' if re.search(r'^tdinput: ', tail_after, re.M) and stats else 'hang'
    elif not done:
        res['verdict'] = 'left'   # the game ended before its frames, without an error (a menu's exit, a lost mission)
    else:
        res['verdict'] = 'ok'
    res['crash_lines'] = crash[:10]
    res['wrong_lines'] = wrong[:10]
    res['tail'] = [l.rstrip()[:200] for l in out if not re.match(r'^(frame \d+:|tdinput|rastat|tdstat)', l)][-12:]
    # frames: "frame N: all logic draw map cells objects shroud drawn N put N N"
    fr = [(int(a), int(b), int(c), int(d)) for a, b, c, d in re.findall(r'^frame (\d+): (\d+) (\d+) (\d+) map', text, re.M)]
    skip = set()
    for f in stats:
        skip |= {f, f + 1}
    body = [f for f in fr if f[0] > 4 and f[0] not in skip]
    if body:
        allv = sorted(f[1] for f in body)
        med = allv[len(allv) // 2]
        # by logic and drawing: a frame's rest holds the time a dialog is open (the game's loop waits inside it)
        worst = [list(f) for f in sorted(body, key=lambda f: -(f[2] + f[3]))[:5]]
        for w in worst:
            w.append(step_at(s, frame_ms(stats, w[0])))
        res['perf'] = dict(n=len(fr), median=med, p99=allv[min(len(allv) - 1, int(len(allv) * 0.99))], max=worst[0][2] + worst[0][3],
                           max_all=allv[-1],
                             over150k=sum(1 for v in allv if v > 150000), worst=[list(w) for w in worst])
    return res


def frame_ms(stats, frame):
    """Roughly when a frame was, in the input's milliseconds: the stat lines come every 2,000 ms of the game's."""
    t0, f0 = 0, 1
    for k, f in enumerate(stats):
        if f >= frame:
            return t0 + (frame - f0) * 2000 / max(1, f - f0)
        t0, f0 = (k + 1) * 2000, f
    return t0


def step_at(s, ms):
    name = '-'
    for k, (n, i, t) in enumerate(s.steps):
        if t > ms:
            break
        name = f'{k} {n}'
    return name


_line_times = {}


def has_line_times(cpu):
    """Whether this rvc_cpu writes --line-times (each console line with the guest's milliseconds)."""
    if cpu not in _line_times:
        p = subprocess.run([cpu, '--help'], capture_output=True, text=True, errors='replace')
        _line_times[cpu] = '--line-times' in p.stdout + p.stderr
    return _line_times[cpu]


def run_one(a, game, seed, steps=None, env_extra=None, tag=None, keep=False):
    m, s = plan(game, seed, a.frames, a.mix)
    out = a.out
    name = tag or f'{game}-{seed}'
    path_in = os.path.join(out, name + '.in')
    upto = write_script(s, path_in, steps)
    log = os.path.join(out, name + '.log')
    if os.path.exists(log):
        os.remove(log)
    env = dict(os.environ)
    env.pop('RVC_RACES', None)
    if m['check'] == 'races':
        env['RVC_RACES'] = os.path.join(out, name + '.races')
    env.update(env_extra or {})
    limit = guest_limit(m)
    args = [a.cpu, '--quiet', '--threads', '1', '--uart-log', log, '--wall', str(a.wall), '--seconds', str(limit), '--image', a.image,
            '--input-script', path_in, '--input-from', GAMES[game]['start'],
            '--expect', '/ # ', '--send', command(game, m) + '; echo LX-\'\'DONE\\n', '--until', 'LX-DONE'] + (a.more or [])
    times = os.path.join(out, name + '.times')
    if has_line_times(a.cpu):
        args[1:1] = ['--line-times', times]
    for _ in range(90):   # (the harness lock refuses a CPU harness beside a GPU one: wait for it)
        t = time.time()
        p = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, text=True, errors='replace')
        if '[lock]' not in p.stderr or 'refused' not in p.stderr:
            break
        time.sleep(20)
    text = open(log, encoding='latin-1').read() if os.path.exists(log) else ''
    res = judge(game, m, s, text, p.stderr, upto, time.time() - t)
    res['steps_limit'] = steps
    if os.path.exists(times):   # a game that still made frames as the guest's time ran out was slow, not stopped
        stat_ms = [float(l.split(' ', 1)[0]) for l in open(times, encoding='latin-1') if re.search(r'stat: frame \d+', l)]
        if stat_ms and res.get('guest_s'):
            res['silent_s'] = round(res['guest_s'] - stat_ms[-1] / 1000, 1)
            if res['verdict'] in ('hang', 'stuck') and res['silent_s'] < 15:
                res['verdict'] = 'slow'
    if res['verdict'] == 'ok' and not keep:
        for f in (log, path_in, os.path.join(out, name + '.races'), times):
            if os.path.exists(f):
                os.remove(f)
    else:
        open(os.path.join(out, name + '.err'), 'w').write(p.stderr if keep else p.stderr[-6000:])
    return res


def summarize(results, file=sys.stdout):
    by = {}
    for r in results:
        by.setdefault(r['verdict'], []).append(r)
    p = lambda *x: print(*x, file=file)
    p(f'{len(results)} runs: ' + ', '.join(f'{k} {len(v)}' for k, v in sorted(by.items())))
    miss = {}
    for r in results:
        key = f"{r['game']} {r['mission']}"
        miss[key] = miss.get(key, 0) + 1
    p('missions: ' + ', '.join(f'{k} {v}' for k, v in sorted(miss.items())))
    p('checks: ' + ', '.join(f'{k} {v}' for k, v in sorted({c: sum(1 for r in results if r['check'] == c) for c in {r['check'] for r in results}}.items())))
    p(f"steps given {sum(r.get('steps_given', 0) for r in results)}, buttons the game saw {sum(r['inputs_seen']['buttons'] for r in results)}, "
      f"keys {sum(r['inputs_seen']['keys'] for r in results)}; guest seconds {sum(r.get('guest_s', 0) for r in results):.0f}, "
      f"host seconds {sum(r['secs'] for r in results):.0f}")
    for v in ('crash', 'hang', 'wrong', 'stuck', 'slow', 'left'):
        for r in by.get(v, [])[:30]:
            p(f"{v.upper()} seed {r['seed']} ({r['mission']}, {r['check']}, {r['style']}) steps {r.get('steps_given')} of {r.get('steps_total')}, "
              f"last stat frame {r.get('last_stat_frame')}, exit {r.get('exit')}: {(r['crash_lines'] or r['wrong_lines'] or r['tail'][-1:] or [''])[0]}")
    odd = {}
    for r in results:
        for l in r.get('odd', []):
            k = re.sub(r'\d+', 'N', l)
            odd.setdefault(k, []).append(r['seed'])
    for k, v in sorted(odd.items(), key=lambda kv: -len(kv[1]))[:20]:
        p(f'odd line in {len(v)} runs (seeds {v[:5]}): {k}')
    fr = [r for r in results if r.get('perf') and r['check'] == 'none']   # (a check's own work is in its frames)
    if fr:
        # a frame heavy in three runs or more of one mission is the mission's own (its script, its AI), not the input's
        seen = {}
        for r in fr:
            for w in r['perf']['worst']:
                seen.setdefault((r['mission'], w[0]), []).append(w[2] + w[3])
        own = {k: v for k, v in seen.items() if len(v) >= 3}
        p(f'frames heavy in many runs of a mission (logic + drawing, median of the runs), of the {len(fr)} runs with no check:')
        for (mis, f), v in sorted(own.items(), key=lambda kv: -statistics.median(kv[1]))[:12]:
            p(f'  {statistics.median(v):>11,.0f} mission {mis} frame {f}, in {len(v)} runs')
        worst = sorted(((w[2] + w[3], r['seed'], r['mission'], w) for r in fr for w in r['perf']['worst']
                        if (r['mission'], w[0]) not in own), reverse=True)[:20]
        p('the longest other frames (core 0 instructions):')
        for v, seed, mis, w in worst:
            p(f'  {v:>11,} seed {seed} mission {mis} frame {w[0]}: logic {w[2]:,} draw {w[3]:,}, all {w[1]:,} (step {w[4] if len(w) > 4 else "?"})')
        meds = [r['perf']['median'] for r in fr]
        p(f"frame median over runs {statistics.median(meds):,.0f}; runs with frames over 150k: "
          f"{sum(1 for r in fr if r['perf']['over150k'])} of {len(fr)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('what', choices=['run', 'replay', 'shrink', 'gen', 'report'])
    ap.add_argument('dir', nargs='?')
    ap.add_argument('--game', choices=sorted(GAMES), default='td')
    ap.add_argument('--seeds', default='1-8')
    ap.add_argument('--seed', type=int)
    ap.add_argument('--steps', type=int)
    ap.add_argument('--frames', type=int, default=1200)
    ap.add_argument('--mix', choices=['all', 'play'], default='all', help="the actions: all, or play (no menus on purpose)")
    ap.add_argument('--jobs', type=int, default=max(1, min(8, (os.cpu_count() or 4) // 2)))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'cc_fuzz'))
    ap.add_argument('--cpu', default=os.path.join(ROOT, 'bin', 'rvc_cpu.exe'))
    ap.add_argument('--image', default='build/images/nommu')
    ap.add_argument('--wall', type=float, default=240)
    ap.add_argument('--env', action='append', default=[], help='K=V for the replay (TDAWN_SLOW=200, RVC_PROF=DIR, ...)')
    ap.add_argument('--more', nargs=argparse.REMAINDER, help="rvc_cpu's own options after everything else")
    ap.add_argument('--keep', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    if a.what == 'gen':
        m, s = plan(a.game, a.seed, a.frames, a.mix)
        print(json.dumps(m))
        print(command(a.game, m))
        for k, (n, i, t) in enumerate(s.steps):
            print(f'step {k} {n} at {t:.0f} ms (event {i})')
        return
    if a.what == 'report':
        res = [json.loads(l) for l in open(os.path.join(a.dir or a.out, 'results.jsonl'))]
        summarize(res)
        return
    if '--input-script' not in subprocess.run([a.cpu, '--help'], capture_output=True, text=True, errors='replace').stdout:
        sys.exit(f'{a.cpu} has no --input-script: --cpu names an rvc_cpu built with it')
    env = dict(e.split('=', 1) for e in a.env)
    if a.what == 'replay':
        r = run_one(a, a.game, a.seed, a.steps, env, tag=f'{a.game}-{a.seed}-replay', keep=True)
        print(json.dumps(r, indent=1))
        return
    if a.what == 'shrink':
        first = run_one(a, a.game, a.seed, None, env, tag=f'{a.game}-{a.seed}-shrink')
        want = first['verdict']
        sig = lambda r: r['verdict'] == want
        lo, hi = 0, first.get('steps_given') or first['steps_total']
        print(f'seed {a.seed}: {want} with {hi} steps; searching', flush=True)
        while lo + 1 < hi:
            mid = (lo + hi) // 2
            r = run_one(a, a.game, a.seed, mid, env, tag=f'{a.game}-{a.seed}-shrink')
            print(f'  {mid} steps: {r["verdict"]}', flush=True)
            if sig(r):
                hi = mid
            else:
                lo = mid
        print(f'seed {a.seed}: {want} needs its first {hi} steps (python tools/cc_fuzz.py replay --game {a.game} --seed {a.seed} --steps {hi} --frames {a.frames})')
        m, s = plan(a.game, a.seed, a.frames, a.mix)
        for k in range(max(0, hi - 5), hi):
            print(f'  step {k}: {s.steps[k][0]} at {s.steps[k][2]:.0f} ms')
        return
    seeds = parse_seeds(a.seeds)
    est = len(seeds) * (3 + a.frames / 200) / a.jobs
    print(f'{len(seeds)} seeds of {a.game}, {a.frames} frames, {a.jobs} at once: roughly {est / 60:.1f} minutes', flush=True)
    t0 = time.time()
    results = []
    with open(os.path.join(a.out, 'results.jsonl'), 'a') as f, ThreadPoolExecutor(a.jobs) as pool:
        futs = {pool.submit(run_one, a, a.game, sd, None, env): sd for sd in seeds}
        for fu in as_completed(futs):
            try:
                r = fu.result()
            except Exception as e:   # (a fault of this script: the batch goes on)
                print(f'seed {futs[fu]}: {e!r}', flush=True)
                continue
            results.append(r)
            f.write(json.dumps(r) + '\n')
            f.flush()
            if r['verdict'] != 'ok' or len(results) % 25 == 0:
                print(f"[{time.time() - t0:5.0f}s] {len(results)}/{len(seeds)} seed {r['seed']}: {r['verdict']} "
                      f"({r['mission']}, {r['check']}, {r['style']}, {r['secs']}s){' ' + (r['crash_lines'] or r['wrong_lines'] or [''])[0] if r['verdict'] != 'ok' else ''}", flush=True)
    summarize(results)
    print(f'{time.time() - t0:.0f} s')


if __name__ == '__main__':
    main()
