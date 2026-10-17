"""How many bytecode instructions the tick runs for one emulated instruction.

    python3 tools/tick_path.py TICK.asm [TICK2.asm]

TICK.asm is the tick's pixel shader disassembled (dxbc-spirv's dxbc_disasm, docs/world/linux.md, on
the largest .cso of a shader cache folder). The fast loop is found (the largest loop inside the
pass's outer one) and walked once for each kind of instruction along the path that kind takes,
which is given below as the turn taken at every `if` it meets, in order: t into the if, e
past it (into its else). What is counted is every instruction on the path but the bare
else / endif / endloop marks. The kinds are then weighed as a shell loop under Linux runs
them (rvc_harness --profile on the benchmark snapshot: x_alu, x_load, x_store, x_branch,
x_jump, x_refetch, ram_read_l1_hit, ram_read_bloom_pass).

With two files the second is compared with the first: emulated instructions per bytecode
instruction is the inverse of the weighted mean.
"""
import re
import sys

OPEN = ('if_nz', 'if_z', 'loop', 'switch')

# The start of every path: the instruction is in the texel the last one was fetched from (S),
# or in the next one, which was read ahead (R: into the fetch block, no page change, a new
# texel, and it is the one read ahead).
PREFIX = {'S': 'e', 'R': 'tett'}
# After the prefix: the tree of the kinds' tests (the group of arithmetic, loads, stores and
# floats; in it arithmetic and loads; then the kind), and what the kind's own block meets; the
# last turn is the step's "was it ours" test.
KINDS = {
    'arithmetic':                   'ttt' + 'e',
    'load, from the write cache':   'tte' + 'e' + 't' + 't' + 'te' + 't' + 'e',
    'load, a texture read':         'tte' + 'e' + 't' + 't' + 'tte' + 'e' + 'e',
    'load, nothing stored near':    'tte' + 'e' + 't' + 'e' + 'e' + 'e' + 'e',
    'store, texel in the cache':    'tet' + 'e' + 't' + 'e' + 'e' + 'ttt' + 'e',
    'store, a new texel':           'tet' + 'e' + 't' + 'e' + 'tee' + 'ee' + 't' + 't' + 'et' + 'e',
    'branch':                       'et' + 'e',
    'jump':                         'eet' + 'e',
}
# Shares of the instructions a shell loop under Linux runs (--profile, October 2026).
MIX = {
    'arithmetic': 0.359,
    'load, from the write cache': 0.242 * 0.46, 'load, a texture read': 0.242 * 0.43, 'load, nothing stored near': 0.242 * 0.11,
    'store, texel in the cache': 0.144 * 0.6, 'store, a new texel': 0.144 * 0.4,
    'branch': 0.159, 'jump': 0.087,
}
REFETCH = 0.343


def fast_loop(path):
    text = re.sub(r'\x1b\[[0-9;]*m', '', open(path, encoding='utf-8', errors='replace').read())
    ins = []
    for line in text.splitlines():
        m = re.match(r'\s*(\d+):\s+(\S+)\s*(.*)$', line)
        if m:
            ins.append(m.group(2).lower())
    stack, loops = [], []
    for i, op in enumerate(ins):
        if op == 'loop':
            stack.append(i)
        elif op == 'endloop':
            s = stack.pop()
            loops.append((len(stack), s, i))
    inner = max((l for l in loops if l[0] == 1), key=lambda l: l[2] - l[1])
    return ins[inner[1] + 1:inner[2]]


def walk(body, turns):
    """Instructions run from the top of the loop to its end with these turns."""
    turns = list(turns)
    count, i = 0, 0
    while i < len(body):
        op = body[i]
        if op in ('if_nz', 'if_z'):
            count += 1
            turn = turns.pop(0) if turns else 'e'
            # find this if's else and endif
            depth, j, els = 1, i + 1, None
            while depth:
                o = body[j]
                if o in OPEN:
                    depth += 1
                elif o in ('endif', 'endloop', 'endswitch'):
                    depth -= 1
                elif o == 'else' and depth == 1:
                    els = j
                j += 1
            end = j - 1
            if turn == 't':
                # run the if-part; at its else, jump to the endif
                sub = body[i + 1:els if els is not None else end]
                c, left = walk_part(sub, turns)
                count += c
                turns = left
            elif els is not None:
                sub = body[els + 1:end]
                c, left = walk_part(sub, turns)
                count += c
                turns = left
            i = end + 1
        elif op in ('else', 'endif', 'endswitch', 'default', 'case'):
            i += 1
        elif op in ('break',):
            count += 1
            return count
        else:
            count += 1
            i += 1
    return count + 1   # the loop's own jump back


def walk_part(body, turns):
    """A block inside an if: the count, and the turns left."""
    turns = list(turns)
    count, i = 0, 0
    while i < len(body):
        op = body[i]
        if op in ('if_nz', 'if_z'):
            count += 1
            turn = turns.pop(0) if turns else 'e'
            depth, j, els = 1, i + 1, None
            while depth:
                o = body[j]
                if o in OPEN:
                    depth += 1
                elif o in ('endif', 'endloop', 'endswitch'):
                    depth -= 1
                elif o == 'else' and depth == 1:
                    els = j
                j += 1
            end = j - 1
            sub = body[i + 1:els if els is not None else end] if turn == 't' else (body[els + 1:end] if els is not None else [])
            c, turns = walk_part(sub, turns)
            count += c
            i = end + 1
        elif op in ('else', 'endif', 'endswitch', 'default', 'case', 'loop', 'endloop', 'switch'):
            i += 1
        else:
            count += 1
            i += 1
    return count, turns


def measure(path):
    body = fast_loop(path)
    out = {}
    for kind, turns in KINDS.items():
        out[kind] = (walk(body, PREFIX['S'] + turns), walk(body, PREFIX['R'] + turns))
    return len(body), out


def mean(paths):
    total = sum(MIX.values())
    return sum(MIX[k] * ((1 - REFETCH) * s + REFETCH * r) for k, (s, r) in paths.items()) / total


def main():
    results = [measure(p) for p in sys.argv[1:3]]
    for p, (size, paths) in zip(sys.argv[1:3], results):
        print('%s: the fast loop is %d instructions' % (p, size))
    print('%-30s %s' % ('bytecode instructions for one', '   '.join('%-22s' % p.split('/')[-1][:22] for p in sys.argv[1:3])))
    for kind in KINDS:
        print('  %-28s %s' % (kind, '   '.join('%4d (%4d with a fetch)' % results[i][1][kind] for i in range(len(results)))))
    means = [mean(r[1]) for r in results]
    print('weighted as a shell loop runs them: ' + ', '.join('%.1f' % m for m in means))
    if len(means) == 2:
        print('emulated instructions per bytecode instruction: %.5f to %.5f, %+.1f%%' % (1 / means[0], 1 / means[1], (means[0] / means[1] - 1) * 100))


main()
