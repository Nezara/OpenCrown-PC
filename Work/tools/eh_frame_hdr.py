"""Extract function start addresses from the PT_GNU_EH_FRAME (.eh_frame_hdr) binary search table."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('<Q', d, 0x20); phnum, = struct.unpack_from('<H', d, 0x38)
ph = [struct.unpack_from('<IIQQQQQQ', d, phoff + i * 56) for i in range(phnum)]
p = next(x for x in ph if x[0] == 0x6474e550)
off, vaddr, size = p[2], p[3], p[5]
ver, eh_enc, cnt_enc, tab_enc = d[off:off+4]
assert ver == 1 and cnt_enc == 0x03 and tab_enc == 0x3b, (ver, eh_enc, cnt_enc, tab_enc)  # udata4 count, sdata4|datarel table
count, = struct.unpack_from('<I', d, off + 8)
starts = []
for i in range(count):
    pc, fde = struct.unpack_from('<ii', d, off + 12 + i * 8)
    starts.append(vaddr + pc)
print(count, 'FDEs; first', hex(starts[0]), 'last', hex(starts[-1]), file=sys.stderr)
with open(sys.argv[2], 'w') as f:
    for s in starts: f.write(f'{s:x}\n')
