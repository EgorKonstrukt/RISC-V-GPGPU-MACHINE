"""What of a program's own work on core 0 would pay on a worker (docs/machine/multicore.md, "Finding the work").

    RVC_CALLS=calls.txt bin/rvc_cpu.exe ...        then
    python tools/mc_candidates.py calls.txt PROGRAM_ELF [--min 20000] [--races races.txt]

rvc_cpu counts, for every call core 0 makes, its own instructions until the call returns
(inclusive of what it calls, not of the kernel's work for it). This names the functions from
the program's symbols and ranks them by instructions a call: a job is worth a worker from
some tens of thousands (handing it out and taking the answer back is about a pass each way).
The program is the context whose calls land in the ELF's functions most. With --races, the
places RVC_RACES found are named too.
"""
import argparse
import bisect
import struct
import sys


def elf_functions(path):
    """(address, name) of an ELF32's function symbols, sorted, and the end of the last."""
    data = open(path, 'rb').read()
    if data[:4] != b'\x7fELF' or data[4] != 1:
        sys.exit(f'{path}: not a 32-bit ELF')
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x2e)
    sections = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
    funcs = []
    for sh in sections:
        if sh[1] != 2:   # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        for at in range(sh[4], sh[4] + sh[5], 16):
            name, value, size, info = struct.unpack_from('<IIIB', data, at)
            if info & 15 != 2 or not value:   # STT_FUNC
                continue
            end = data.index(b'\0', strtab[4] + name)
            funcs.append((value, size, data[strtab[4] + name:end].decode('latin-1')))
    if not funcs:
        sys.exit(f'{path}: no symbols (a stripped binary: link the unstripped one)')
    funcs.sort()
    return funcs


def namer(funcs):
    starts = [f[0] for f in funcs]

    def name(pc):
        i = bisect.bisect_right(starts, pc) - 1
        if i < 0:
            return None
        start, size, n = funcs[i]
        if size and pc >= start + size:
            return None
        return n if pc == start else f'{n}+{pc - start:#x}'
    return name


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('calls')
    ap.add_argument('elf')
    ap.add_argument('--min', type=int, default=20000, help='instructions a call that pays for a worker')
    ap.add_argument('--context', help="the program's page table as rvc_cpu wrote it (hex); found when not given")
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--races')
    a = ap.parse_args()

    name = namer(elf_functions(a.elf))
    totals, fns = {}, []
    for line in open(a.calls):
        w = line.split()
        if w[:1] == ['ctx']:
            totals[int(w[1], 16)] = int(w[2])
        elif w[:1] == ['fn']:
            fns.append((int(w[1], 16), int(w[2], 16), int(w[3]), int(w[4]), int(w[5])))
    if a.context:
        ctx = int(a.context, 16)
    else:
        mine = {}
        for c, target, calls, incl, most in fns:
            if c and name(target):
                mine[c] = mine.get(c, 0) + incl
        if not mine:
            sys.exit('no context calls the functions of this ELF')
        ctx = max(mine, key=mine.get)
    total = totals.get(ctx, 0)
    rows = [(incl / calls, name(t) or f'{t:08x}', calls, incl, most) for c, t, calls, incl, most in fns if c == ctx and calls]
    rows.sort(reverse=True)
    print(f'context {ctx:08x}: {total:,} instructions of its own on core 0')
    print(f'{"function":40s} {"calls":>8s} {"a call":>12s} {"most":>12s} {"of all":>7s}')
    shown = 0
    for per, n, calls, incl, most in rows:
        if shown >= a.top:
            break
        if per < a.min and shown >= 5:
            break
        share = incl / total if total else 0
        mark = '  <- a job' if per >= a.min and n not in ('main', '_start', '__libc_start_main') else ''
        print(f'{n[:40]:40s} {calls:8,d} {per:12,.0f} {most:12,d} {share:6.1%}{mark}')
        shown += 1

    if a.races:
        print()
        for line in open(a.races):
            w = line.split()
            # kind count core N pc P satp S other M pc Q satp T phys A
            kind, count, core, pc, other, opc, phys = w[0], int(w[1]), w[3], int(w[5], 16), w[9], int(w[11], 16), w[15]
            if kind == 'undone':
                what = f'undoes what core {other} changed in the same 16 bytes in the same pass at'
            else:
                what = f'is given as they were bytes core {other} changed in the same pass at'
            print(f'{count:8,d} x  core {core} at {name(pc) or hex(pc)} {what} {name(opc) or hex(opc)} (e.g. {phys})')


if __name__ == '__main__':
    main()
