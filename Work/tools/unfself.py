"""Unwrap a decrypted (fake-signed) PS4 SELF into a plain ELF."""
import struct, sys

PT_SCE_VERSION = 0x6FFFFF01

def unfself(src, dst):
    d = open(src, 'rb').read()
    magic, = struct.unpack_from('<I', d, 0)
    assert magic == 0x1D3D154F, 'not a PS4 SELF'
    nseg, = struct.unpack_from('<H', d, 0x18)
    segs = [struct.unpack_from('<QQQQ', d, 0x20 + i * 32) for i in range(nseg)]
    eoff = 0x20 + nseg * 32
    assert d[eoff:eoff + 4] == b'\x7fELF'
    e_type, = struct.unpack_from('<H', d, eoff + 0x10)
    e_phoff, = struct.unpack_from('<Q', d, eoff + 0x20)
    e_phentsize, e_phnum = struct.unpack_from('<HH', d, eoff + 0x36)
    phdrs = [struct.unpack_from('<IIQQQQQQ', d, eoff + e_phoff + i * e_phentsize) for i in range(e_phnum)]
    size = max(p[2] + p[6] for p in phdrs)
    out = bytearray(size)
    out[0:e_phoff + e_phnum * e_phentsize] = d[eoff:eoff + e_phoff + e_phnum * e_phentsize]
    for idx, (ptype, pflags, poff, vaddr, paddr, filesz, memsz, align) in enumerate(phdrs):
        if ptype == PT_SCE_VERSION:
            out[poff:poff + filesz] = d[len(d) - filesz:]
            continue
        for fl, off, csz, dsz in segs:
            if fl & 0x800 and (fl >> 20) & 0xFFF == idx:
                assert not fl & 0xA, 'segment encrypted/compressed'
                out[poff:poff + csz] = d[off:off + csz]
    open(dst, 'wb').write(out)
    print(f'e_type=0x{e_type:04x}, {e_phnum} phdrs, wrote {len(out):#x} bytes -> {dst}')
    for i, p in enumerate(phdrs):
        print(f'  [{i}] type=0x{p[0]:08x} flags={p[1]} off=0x{p[2]:x} vaddr=0x{p[3]:x} filesz=0x{p[5]:x} memsz=0x{p[6]:x}')

if __name__ == '__main__':
    unfself(sys.argv[1], sys.argv[2])
