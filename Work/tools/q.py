"""Query the eboot without opening Ghidra (uses Work/index/*.tsv from `ghidra.ps1 index` + Work/eboot.elf).

    python q.py fn 0x42b470|NAME        function info (size, callers, callees)
    python q.py dis 0x42b470|NAME       disassemble the whole function; calls / strings / imports annotated
    python q.py dis 0x42b470 0x42b500   disassemble a raw range
    python q.py callers X [depth]       who calls X (depth>1 walks up)
    python q.py callees X               what X calls
    python q.py str REGEX               code-referenced strings matching REGEX + the functions using them
    python q.py strs X                  strings referenced by function X
"""
import bisect, os, re, struct, sys
from collections import defaultdict

WORK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDX = os.path.join(WORK, 'index')


def load():
    funcs = {}
    for line in open(os.path.join(IDX, 'functions.tsv'), encoding='utf-8'):
        a, size, name, nin, nout = line.rstrip('\n').split('\t')
        funcs[int(a, 16)] = (int(size), name)
    calls, callers = defaultdict(set), defaultdict(set)
    for line in open(os.path.join(IDX, 'calls.tsv')):
        a, b = (int(x, 16) for x in line.split())
        calls[a].add(b); callers[b].add(a)
    strings, by_fn = {}, defaultdict(list)
    for line in open(os.path.join(IDX, 'strings.tsv'), encoding='utf-8'):
        a, fns, text = line.rstrip('\n').split('\t', 2)
        a = int(a, 16)
        strings[a] = (text, [int(x, 16) for x in fns.split(',')])
        for f in strings[a][1]: by_fn[f].append(a)
    return funcs, calls, callers, strings, by_fn


FUNCS, CALLS, CALLERS, STRINGS, STR_BY_FN = load()
STARTS = sorted(FUNCS)
NAMES = {v[1]: k for k, v in FUNCS.items()}
IMPORTS = {}
for line in open(os.path.join(WORK, 'imports.tsv')):
    got, name, lib, _ = line.rstrip('\n').split('\t')
    IMPORTS[int(got, 16)] = f'{lib}::{name}'


def resolve(s):
    if s.lower().startswith('0x'):
        a = int(s, 16)
        i = bisect.bisect_right(STARTS, a) - 1
        return STARTS[i] if i >= 0 and a < STARTS[i] + max(FUNCS[STARTS[i]][0], 1) else a
    if s in NAMES: return NAMES[s]
    m = re.fullmatch(r'FUN_([0-9a-fA-F]+)', s)
    if m: return int(m.group(1), 16)
    sys.exit(f'unknown function {s}')


def label(a):
    if a in FUNCS: return FUNCS[a][1]
    return f'0x{a:x}'


def segments():
    d = open(os.path.join(WORK, 'eboot.elf'), 'rb').read()
    phoff, = struct.unpack_from('<Q', d, 0x20); phnum, = struct.unpack_from('<H', d, 0x38)
    segs = []
    for i in range(phnum):
        t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('<IIQQQQQQ', d, phoff + i * 56)
        if t == 1: segs.append((va, off, fsz))
    return d, segs


def read(d, segs, a, n):
    for va, off, fsz in segs:
        if va <= a < va + fsz: return d[off + a - va: off + a - va + n]
    return b''


def dis(start, end):
    import capstone
    d, segs = segments()
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    for ins in md.disasm(read(d, segs, start, end - start), start):
        note = ''
        op = ins.op_str
        m = re.fullmatch(r'0x([0-9a-f]+)', op)
        if ins.mnemonic in ('call', 'jmp') and m and int(m.group(1), 16) in FUNCS:
            note = label(int(m.group(1), 16))
        rip = re.search(r'\[rip ([+-]) 0x([0-9a-f]+)\]', op)
        if rip:
            tgt = ins.address + ins.size + (1 if rip.group(1) == '+' else -1) * int(rip.group(2), 16)
            if tgt in STRINGS: note = repr(STRINGS[tgt][0][:60])
            elif tgt in IMPORTS: note = IMPORTS[tgt]
            elif tgt in FUNCS: note = label(tgt)
            else: note = f'=0x{tgt:x}'
        print(f'{ins.address:8x}  {ins.mnemonic:6} {op}' + (f'   ; {note}' if note else ''))


def main(argv):
    if len(argv) < 2: sys.exit(__doc__)
    cmd, args = argv[0], argv[1:]
    if cmd == 'fn':
        a = resolve(args[0]); size, name = FUNCS.get(a, (0, '?'))
        print(f'{a:x} {name} size={size} callers={len(CALLERS[a])} callees={len(CALLS[a])} strings={len(STR_BY_FN[a])}')
    elif cmd == 'dis':
        a = resolve(args[0])
        end = int(args[1], 16) if len(args) > 1 else a + FUNCS[a][0]
        dis(a, end)
    elif cmd == 'callers':
        depth = int(args[1]) if len(args) > 1 else 1
        frontier, seen = {resolve(args[0])}, set()
        for lvl in range(depth):
            nxt = set()
            for f in sorted(frontier):
                for c in sorted(CALLERS[f]):
                    if c in seen: continue
                    seen.add(c); nxt.add(c)
                    print('  ' * lvl + f'{c:x} {label(c)}  (-> {label(f)})')
            frontier = nxt
    elif cmd == 'callees':
        for c in sorted(CALLS[resolve(args[0])]): print(f'{c:x} {label(c)}')
    elif cmd == 'str':
        rx = re.compile(args[0])
        for a, (text, fns) in sorted(STRINGS.items()):
            if rx.search(text): print(f'{a:x} {text[:100]!r}  <- ' + ', '.join(label(f) for f in fns[:6]))
    elif cmd == 'strs':
        for s in sorted(STR_BY_FN[resolve(args[0])]): print(f'{s:x} {STRINGS[s][0][:100]!r}')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
