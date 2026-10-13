"""clean.py TCC STAGE EXAMPLE.c ...: the sums (nxide.c's text_sums) of the examples the
compiler says nothing of, each read as the IDE reads it and compiled as its check compiles
it, by the compiler built for the host with the image's headers (docs/ide.md)."""
import os, subprocess, sys, tempfile


def as_read(data):
    """A file's lines as tx_load leaves them."""
    lines, line = [], []
    for ch in data:
        if ch == 10:
            lines.append(bytes(line))
            line = []
        elif ch == 9:
            line += [32] * (4 - len(line) % 4)
        elif ch != 13:
            line.append(ch if 32 <= ch <= 126 else 32)
    lines.append(bytes(line))
    if len(lines) > 1 and not lines[-1]:
        lines.pop()
    return lines


def sums(lines):
    a, b = 2166136261, 5381
    for line in lines:
        for ch in line + b'\n':
            a = ((a ^ ch) * 16777619) & 0xffffffff
            b = (b * 33 + ch) & 0xffffffff
    return a, b


tcc, stage = sys.argv[1:3]
for path in sys.argv[3:]:
    lines = as_read(open(path, 'rb').read())
    with tempfile.TemporaryDirectory() as folder:
        source = os.path.join(folder, 'ide-check.c')
        with open(source, 'wb') as file:
            file.write(b''.join(line + b'\n' for line in lines))
        run = subprocess.run([tcc, '-static', '-mfpu', '-nostdinc', '-I' + stage + '/usr/include', '-I' + stage + '/usr/lib/tcc/include',
                              '-c', source, '-o', os.path.join(folder, 'ide-check.o')], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if run.returncode == 0 and not run.stdout.strip():
        print('%08x %08x' % sums(lines))
    else:
        sys.stderr.write('%s: not clean, the IDE will check it: %s\n' % (path, run.stdout.decode('latin1')[:200]))
