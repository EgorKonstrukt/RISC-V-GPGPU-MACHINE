"""The loops of a compiled shader, and whether two blobs have the same loop.

    python tools/cso_loops.py A.cso                 instructions, registers, and every loop over 60 instructions
    python tools/cso_loops.py A.cso AT B.cso AT     the loops that start at those instructions, compared

A blob is a file of build/shadercache (the tick's is the one over 500 KB). The comparison
renames registers in order of appearance, so two loops that differ only in which registers
the compiler chose are the same. The tick's fast loop is the first loop inside its main one.
"""
import ctypes, difflib, re, sys

dll = ctypes.WinDLL(r'C:\Windows\System32\d3dcompiler_47.dll')


def listing(path):
    data = open(path, 'rb').read()
    blob = ctypes.c_void_p()
    hr = dll.D3DDisassemble(data, ctypes.c_size_t(len(data)), 0, None, ctypes.byref(blob))
    if hr != 0:
        raise SystemExit('%s: D3DDisassemble failed, %08x' % (path, hr & 0xffffffff))
    vt = ctypes.cast(ctypes.cast(blob, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    text = ctypes.string_at(pointer(blob), size(blob)).decode('latin1')
    return [l.strip() for l in text.splitlines() if l.strip() and not l.strip().startswith('//')]


def loops(lines):
    open_at, found = [], []
    for i, l in enumerate(lines):
        op = l.split()[0]
        if op == 'loop':
            open_at.append(i)
        elif op == 'endloop':
            start = open_at.pop()
            found.append((start, i - start, len(open_at)))
    return sorted(found)


def renamed(lines, at):
    length = next(n for s, n, d in loops(lines) if s == at)
    names = {}
    return [re.sub(r'\br(\d+)\b', lambda m: 'R%d' % names.setdefault(m.group(1), len(names)), l) for l in lines[at:at + length + 1]], len(names)


args = sys.argv[1:]
if len(args) == 1:
    lines = listing(args[0])
    print('%d instructions, %s' % (len(lines), next(l for l in lines if l.startswith('dcl_temps'))))
    for start, length, depth in loops(lines):
        if length > 60:
            print('  ' * depth + 'loop at %d, %d instructions' % (start, length))
elif len(args) == 4:
    a, na = renamed(listing(args[0]), int(args[1]))
    b, nb = renamed(listing(args[2]), int(args[3]))
    print('%d and %d instructions, %d and %d registers' % (len(a), len(b), na, nb))
    if a == b:
        print('the same loop')
    else:
        d = [l for l in difflib.unified_diff(a, b, lineterm='', n=0) if not l.startswith(('@@', '---', '+++'))]
        print('%d lines differ; the first:' % len(d))
        print('\n'.join(d[:20]))
else:
    print(__doc__)
