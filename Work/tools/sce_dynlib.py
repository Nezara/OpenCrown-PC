"""Dump the SCE dynamic linking info (imported libs, NID symbols, PLT relocs) from an unwrapped PS4 ELF."""
import struct, sys, collections

def parse(path):
    d = open(path, 'rb').read()
    phoff, = struct.unpack_from('<Q', d, 0x20)
    phnum, = struct.unpack_from('<H', d, 0x38)
    ph = [struct.unpack_from('<IIQQQQQQ', d, phoff + i * 56) for i in range(phnum)]
    dyn = next(p for p in ph if p[0] == 2)
    dl = next(p for p in ph if p[0] == 0x61000000)[2]
    tags = collections.defaultdict(list)
    for i in range(0, dyn[5], 16):
        t, v = struct.unpack_from('<qQ', d, dyn[2] + i)
        if t == 0: break
        tags[t].append(v)
    strtab = dl + tags[0x61000035][0]
    def s(o): return d[strtab + o:d.index(b'\0', strtab + o)].decode()
    symtab, symsz = dl + tags[0x61000039][0], tags[0x6100003F][0]
    syms = []
    for i in range(0, symsz, 24):
        name, info, other, shndx, val, size = struct.unpack_from('<IBBHQQ', d, symtab + i)
        syms.append((s(name), info, shndx, val))
    jmprel, pltsz = dl + tags[0x61000029][0], tags[0x6100002D][0]
    plt = []
    for i in range(0, pltsz, 24):
        off, info, add = struct.unpack_from('<QQq', d, jmprel + i)
        plt.append((off, syms[info >> 32][0]))
    libs = {v & 0xFFFF if False else (v >> 48): s(v & 0xFFFFFFFF) for v in tags[0x61000015]}
    mods = [s(v & 0xFFFFFFFF) for v in tags[0x6100000D]]
    needed = [s(v) for v in tags[1]]
    return dict(needed=needed, modules=mods, libs=libs, syms=syms, plt=plt)

if __name__ == '__main__':
    r = parse(sys.argv[1])
    print('DT_NEEDED:', r['needed'])
    print('modules:', r['modules'])
    print('import libs:', len(r['libs']), sorted(r['libs'].values()))
    print('symbols:', len(r['syms']), ' PLT imports:', len(r['plt']))
    for off, n in r['plt'][:8]: print(f'  GOT 0x{off:x} -> {n}')
    exported = [x for x in r['syms'] if x[2] != 0]
    print('defined (exported) symbols:', len(exported), exported[:5])
