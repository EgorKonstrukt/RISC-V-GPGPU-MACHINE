"""Whether any program fits after any other on the no-MMU desktop (docs/nommu.md, "Memory"), under
rvc_cpu, a machine each and all at once: each large program and then Quake III, the largest; and
all of them one after another, Quake III among them, in order and reversed. Prints free memory and
page cache at each step and any allocation that failed.   python linux/nommu/memory.py [IMAGE]"""
import os, re, subprocess, sys, time
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
img = sys.argv[1] if len(sys.argv) > 1 else 'nommu'
OUT = os.path.join(ROOT, 'build', 'nommu', 'memory')
os.makedirs(OUT, exist_ok=True)
M = 'echo "MEM {} $(grep -E "^(MemFree|Cached):" /proc/meminfo | tr -s " " | tr "\\n" " ")"'
FAIL = r'page allocation failure|failed to allocate|out of memory|cannot allocate'
Q3 = ('quake3', 'quake3')
BIG = [('doom', 'doom -1'), ('quake', 'quake'), ('tdawn', 'TDAWN_AUTO=1 tdawn'), ('ralert', 'TDAWN_AUTO=1 ralert'),
       ('classicube', 'classicube'), ('nes', 'nes'), ('nxray', 'nxray'), ('nxpath', 'nxpath'), ('nxweb', 'nxweb'),
       ('nxmicropolis', 'nxmicropolis')]
# (nxray and nxpath keep 255 cores busy, five times slower than the guest's time here: pairs only)
CHAIN = [('doom', 'doom -1'), ('quake', 'quake'), ('tdawn', 'TDAWN_AUTO=1 tdawn'), Q3, ('ralert', 'TDAWN_AUTO=1 ralert'),
         ('classicube', 'classicube'), ('nes', 'nes'), Q3, ('nxweb', 'nxweb'), ('nxmicropolis', 'nxmicropolis'),
         ('nxphys', 'nxphys'), ('imdemo', 'imdemo'), Q3]


def steps(seq, run_s):
    parts = ['sleep 15', M.format('start')]
    for name, run in seq:
        parts += [f'{run} </dev/null >/tmp/out.txt 2>&1 & sleep {run_s}', f'grep -h -i -E "{FAIL}" /tmp/out.txt',
                  M.format('with ' + name), 'kill $! 2>/dev/null; sleep 3', M.format('after ' + name)]
    return '; '.join(parts) + "; echo LX-''DONE"


runs = {f'pair-{name}': steps([(name, run), Q3], 8) for name, run in BIG}
runs['chain'] = steps(CHAIN, 6)
runs['chain-reversed'] = steps(list(reversed(CHAIN)), 6)
t = time.time()
procs = []
for name, cmd in runs.items():
    log = os.path.join(OUT, name + '.log')
    if os.path.exists(log):
        os.remove(log)
    procs.append((name, log, subprocess.Popen(
        [os.path.join(ROOT, 'bin', 'rvc_cpu.exe'), '--quiet', '--desktop', '--uart-log', log, '--wall', '240',
         '--image', 'build/images/' + img, '--expect', '/ # ', '--send', cmd + '\\n', '--until', 'LX-DONE'],
        cwd=ROOT, env=dict(os.environ, RVC_BARE_WORKERS='1'), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)))
bad = 0
for name, log, p in procs:
    p.wait()
    text = open(log, encoding='latin-1').read().replace('\r', '')
    done = '\nLX-DONE' in text
    # (the console's echo of the command, wrapped, has the pattern too, with its |)
    failed = [l for l in text.splitlines() if re.search(FAIL, l, re.I) and '|' not in l]
    bad += (not done) + bool(failed)
    print('==', name, 'done' if done else 'NOT DONE', f'{len(failed)} failed' if failed else '')
    for line in text.splitlines():
        at = re.search(r'MEM (start|with|after) [^$]*MemFree', line)   # (one may follow the echo on its line)
        if at or line in failed:
            print('  ', line[at.start() if at else 0:].rstrip()[:150])
print(f'{len(runs) - bad} of {len(runs)} fit, in {time.time() - t:.0f} s')
sys.exit(1 if bad else 0)
