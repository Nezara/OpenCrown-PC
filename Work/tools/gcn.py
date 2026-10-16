"""Minimal GCN2 (PS4 Liverpool) disassembler for the eboot's embedded shader blobs (own code, reads the user's eboot).

    python gcn.py ps N          disassemble PS pool entry N (0..59)
    python gcn.py vs N          VS pool entry N (0..24)
    python gcn.py addr 0x7973e0 blob at an address
Blob: 'OrbShdr'-style header; code starts after the 'Shdr' header, length at header +0x10 (see render_cmds.md s5.1).
Only the opcodes these shaders use are named; anything else prints as raw.
"""
import json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
_argv = sys.argv; sys.argv = ['q']
import q
sys.argv = _argv
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SOP2 = {0: 's_add_u32', 1: 's_sub_u32', 2: 's_add_i32', 3: 's_sub_i32', 6: 's_min_i32', 7: 's_min_u32', 8: 's_max_i32',
        9: 's_max_u32', 0xa: 's_cselect_b32', 0xb: 's_cselect_b64', 0xe: 's_and_b32', 0xf: 's_and_b64', 0x10: 's_or_b32',
        0x11: 's_or_b64', 0x12: 's_xor_b32', 0x13: 's_xor_b64', 0x14: 's_andn2_b32', 0x15: 's_andn2_b64',
        0x1e: 's_lshl_b32', 0x20: 's_lshr_b32', 0x22: 's_ashr_i32', 0x26: 's_mul_i32', 0x27: 's_bfe_u32', 0x28: 's_bfe_i32'}
SOP1 = {3: 's_mov_b32', 4: 's_mov_b64', 7: 's_not_b32', 0xa: 's_wqm_b64', 0x1f: 's_swappc_b64', 0x20: 's_setpc_b64',
        0x24: 's_and_saveexec_b64', 0x25: 's_or_saveexec_b64'}
SOPC = {0: 's_cmp_eq_i32', 1: 's_cmp_lg_i32', 2: 's_cmp_gt_i32', 3: 's_cmp_ge_i32', 4: 's_cmp_lt_i32', 5: 's_cmp_le_i32',
        6: 's_cmp_eq_u32', 7: 's_cmp_lg_u32', 8: 's_cmp_gt_u32', 9: 's_cmp_ge_u32', 0xa: 's_cmp_lt_u32', 0xb: 's_cmp_le_u32'}
SOPP = {0: 's_nop', 1: 's_endpgm', 2: 's_branch', 4: 's_cbranch_scc0', 5: 's_cbranch_scc1', 6: 's_cbranch_vccz',
        7: 's_cbranch_vccnz', 8: 's_cbranch_execz', 9: 's_cbranch_execnz', 0xc: 's_waitcnt'}
SMRD = {0: 's_load_dword', 1: 's_load_dwordx2', 2: 's_load_dwordx4', 3: 's_load_dwordx8', 4: 's_load_dwordx16',
        8: 's_buffer_load_dword', 9: 's_buffer_load_dwordx2', 0xa: 's_buffer_load_dwordx4', 0xb: 's_buffer_load_dwordx8',
        0xc: 's_buffer_load_dwordx16'}
VOP2 = {0: 'v_cndmask_b32', 3: 'v_add_f32', 4: 'v_sub_f32', 5: 'v_subrev_f32', 7: 'v_mul_legacy_f32', 8: 'v_mul_f32',
        9: 'v_mul_i32_i24', 0xb: 'v_mul_u32_u24', 0xd: 'v_min_legacy_f32', 0xe: 'v_max_legacy_f32', 0xf: 'v_min_f32',
        0x10: 'v_max_f32', 0x11: 'v_min_i32', 0x12: 'v_max_i32', 0x13: 'v_min_u32', 0x14: 'v_max_u32', 0x15: 'v_lshr_b32',
        0x16: 'v_lshrrev_b32', 0x17: 'v_ashr_i32', 0x18: 'v_ashrrev_i32', 0x19: 'v_lshl_b32', 0x1a: 'v_lshlrev_b32',
        0x1b: 'v_and_b32', 0x1c: 'v_or_b32', 0x1d: 'v_xor_b32', 0x1f: 'v_mac_f32', 0x20: 'v_madmk_f32', 0x21: 'v_madak_f32',
        0x25: 'v_add_i32', 0x26: 'v_sub_i32', 0x27: 'v_subrev_i32', 0x2b: 'v_ldexp_f32', 0x2f: 'v_cvt_pkrtz_f16_f32'}
VOP1 = {0: 'v_nop', 1: 'v_mov_b32', 2: 'v_readfirstlane_b32', 5: 'v_cvt_f32_i32', 6: 'v_cvt_f32_u32', 7: 'v_cvt_u32_f32',
        8: 'v_cvt_i32_f32', 0xa: 'v_cvt_f16_f32', 0xb: 'v_cvt_f32_f16', 0x11: 'v_cvt_f32_ubyte0', 0x12: 'v_cvt_f32_ubyte1',
        0x13: 'v_cvt_f32_ubyte2', 0x14: 'v_cvt_f32_ubyte3', 0x20: 'v_fract_f32', 0x21: 'v_trunc_f32', 0x22: 'v_ceil_f32',
        0x23: 'v_rndne_f32', 0x24: 'v_floor_f32', 0x25: 'v_exp_f32', 0x27: 'v_log_f32', 0x2a: 'v_rcp_f32',
        0x2e: 'v_rsq_f32', 0x33: 'v_sqrt_f32', 0x35: 'v_sin_f32', 0x36: 'v_cos_f32', 0x37: 'v_not_b32'}
VOP3 = {0x140: 'v_mad_legacy_f32', 0x141: 'v_mad_f32', 0x142: 'v_mad_i32_i24', 0x143: 'v_mad_u32_u24', 0x148: 'v_bfe_u32',
        0x149: 'v_bfe_i32', 0x14a: 'v_bfi_b32', 0x14b: 'v_fma_f32', 0x151: 'v_min3_f32', 0x154: 'v_max3_f32',
        0x157: 'v_med3_f32', 0x16a: 'v_mul_lo_u32', 0x16b: 'v_mul_hi_u32', 0x16c: 'v_mul_lo_i32'}
CMP = ['f', 'lt', 'eq', 'le', 'gt', 'lg', 'ge', 'o', 'u', 'nge', 'nlg', 'ngt', 'nle', 'neq', 'nlt', 'tru']
MIMG = {0: 'image_load', 0x20: 'image_sample', 0x24: 'image_sample_l', 0x25: 'image_sample_b', 0x27: 'image_sample_lz',
        0x28: 'image_sample_c', 0x2f: 'image_sample_c_lz'}
INTRP = {0: 'v_interp_p1_f32', 1: 'v_interp_p2_f32', 2: 'v_interp_mov_f32'}
CONST = {240: '0.5', 241: '-0.5', 242: '1.0', 243: '-1.0', 244: '2.0', 245: '-2.0', 246: '4.0', 247: '-4.0'}


def vopc_name(op):
    if op < 0x20: return f"v_cmp{'x' if op >= 0x10 else ''}_{CMP[op & 15]}_f32"
    if 0x80 <= op < 0x100:
        t = ['i32', 'i64', 'u32', 'u64'][(op - 0x80) >> 5 & 3] if True else ''
        return f"v_cmp{'x' if op & 0x10 else ''}_{['f','lt','eq','le','gt','lg','ge','t'][op & 7]}_{t}"
    return f'vopc_{op:x}'


def src(v, lit):
    if v < 104: return f's{v}'
    if v == 106: return 'vcc_lo'
    if v == 107: return 'vcc_hi'
    if v == 124: return 'm0'
    if v == 126: return 'exec_lo'
    if v == 127: return 'exec_hi'
    if 128 <= v <= 192: return str(v - 128)
    if 193 <= v <= 208: return str(128 - v - 64 + 64 - 1 - (v - 193)) if False else str(-(v - 192))
    if v in CONST: return CONST[v]
    if v == 255:
        f = struct.unpack('<f', struct.pack('<I', lit))[0]
        return f'0x{lit:08x}({f:g})'
    if v >= 256: return f'v{v - 256}'
    return f'?{v}'


def dis(code, base=0):
    out, i = [], 0
    while i + 4 <= len(code):
        w = struct.unpack_from('<I', code, i)[0]
        w2 = struct.unpack_from('<I', code, i + 4)[0] if i + 8 <= len(code) else 0
        n, txt = 4, None
        uses_lit = False
        if w >> 23 == 0x17d:  # SOP1
            op, d, s0 = w >> 8 & 0xff, w >> 16 & 0x7f, w & 0xff
            uses_lit = s0 == 255
            txt = f"{SOP1.get(op, f'sop1_{op:x}')} {src(d, 0)}, {src(s0, w2)}"
        elif w >> 23 == 0x17e:
            op = w >> 16 & 0x7f; uses_lit = (w & 0xff) == 255 or (w >> 8 & 0xff) == 255
            txt = f"{SOPC.get(op, f'sopc_{op:x}')} {src(w & 0xff, w2)}, {src(w >> 8 & 0xff, w2)}"
        elif w >> 23 == 0x17f:
            op = w >> 16 & 0x7f
            imm = w & 0xffff
            if op == 2 or 4 <= op <= 9:
                imm = (i + 4 + 4 * (imm - 0x10000 if imm & 0x8000 else imm)) + base
                txt = f"{SOPP.get(op)} 0x{imm:x}"
            else:
                txt = f"{SOPP.get(op, f'sopp_{op:x}')} 0x{imm:x}"
        elif w >> 28 == 0xb:
            op = w >> 23 & 0x1f
            txt = f"sopk_{op:x} {src(w >> 16 & 0x7f, 0)}, 0x{w & 0xffff:x}"
        elif w >> 30 == 2:
            op, d, s1, s0 = w >> 23 & 0x7f, w >> 16 & 0x7f, w >> 8 & 0xff, w & 0xff
            uses_lit = s0 == 255 or s1 == 255
            txt = f"{SOP2.get(op, f'sop2_{op:x}')} {src(d, 0)}, {src(s0, w2)}, {src(s1, w2)}"
        elif w >> 27 == 0x18:
            op, d, sb, imm, off = w >> 22 & 0x1f, w >> 15 & 0x7f, (w >> 9 & 0x3f) * 2, w >> 8 & 1, w & 0xff
            o = f'0x{off * 4:x}' if imm else src(off, w2)
            uses_lit = not imm and off == 255
            txt = f"{SMRD.get(op, f'smrd_{op:x}')} s{d}, s[{sb}:{sb + 1}], {o}   ; byte offset{' (dword*4)' if imm else ''}"
        elif w >> 25 == 0x3f:  # VOP1
            op, d, s0 = w >> 9 & 0xff, w >> 17 & 0xff, w & 0x1ff
            uses_lit = s0 == 255
            txt = f"{VOP1.get(op, f'vop1_{op:x}')} v{d}, {src(s0, w2)}"
        elif w >> 25 == 0x3e:  # VOPC
            op, s1, s0 = w >> 17 & 0xff, w >> 9 & 0xff, w & 0x1ff
            uses_lit = s0 == 255
            txt = f"{vopc_name(op)} vcc, {src(s0, w2)}, v{s1}"
        elif w >> 31 == 0:  # VOP2
            op, d, s1, s0 = w >> 25 & 0x3f, w >> 17 & 0xff, w >> 9 & 0xff, w & 0x1ff
            if op in (0x20, 0x21):
                n = 8
                k = src(255, w2)
                txt = f"{VOP2[op]} v{d}, {src(s0, 0)}, v{s1}, {k}"
            else:
                uses_lit = s0 == 255
                txt = f"{VOP2.get(op, f'vop2_{op:x}')} v{d}, {src(s0, w2)}, v{s1}" + (', vcc' if op == 0 else '')
        elif w >> 26 == 0x34:  # VOP3
            n = 8
            op, d, ab, cl = w >> 17 & 0x1ff, w & 0xff, w >> 8 & 7, w >> 11 & 1
            s = [w2 & 0x1ff, w2 >> 9 & 0x1ff, w2 >> 18 & 0x1ff]
            omod, neg = w2 >> 27 & 3, w2 >> 29 & 7
            def opnd(k):
                t = src(s[k], 0)
                if ab >> k & 1: t = f'|{t}|'
                if neg >> k & 1: t = '-' + t
                return t
            if op < 0x100:
                name, args = vopc_name(op), [src(d, 0), opnd(0), opnd(1)]
            elif op < 0x140:
                name, args = VOP2.get(op - 0x100, f'vop2_{op - 0x100:x}'), [f'v{d}', opnd(0), opnd(1)]
                if op - 0x100 == 0: args.append(src(s[2], 0))
            elif op >= 0x180:
                name, args = VOP1.get(op - 0x180, f'vop1_{op - 0x180:x}'), [f'v{d}', opnd(0)]
            else:
                name, args = VOP3.get(op, f'vop3_{op:x}'), [f'v{d}', opnd(0), opnd(1), opnd(2)]
            txt = f"{name}{' clamp' if cl else ''}{f' omod{omod}' if omod else ''} " + ', '.join(args)
        elif w >> 26 == 0x32:  # VINTRP
            d, op, at, ch, vs = w >> 18 & 0xff, w >> 16 & 3, w >> 10 & 0x3f, w >> 8 & 3, w & 0xff
            txt = f"{INTRP[op]} v{d}, v{vs}, attr{at}.{'xyzw'[ch]}"
        elif w >> 26 == 0x3c:  # MIMG
            n = 8
            op, dm = w >> 18 & 0x7f, w >> 8 & 0xf
            va, vd, rs, ss = w2 & 0xff, w2 >> 8 & 0xff, (w2 >> 16 & 0x1f) * 4, (w2 >> 21 & 0x1f) * 4
            txt = f"{MIMG.get(op, f'mimg_{op:x}')} v{vd}.. dmask 0x{dm:x}, v{va}.., T# s[{rs}:{rs + 7}], S# s[{ss}:{ss + 3}]"
        elif w >> 26 == 0x3e:  # EXP
            n = 8
            en, tgt, compr, done, vm = w & 0xf, w >> 4 & 0x3f, w >> 10 & 1, w >> 11 & 1, w >> 12 & 1
            v = [w2 >> (8 * k) & 0xff for k in range(4)]
            tname = f'mrt{tgt}' if tgt < 8 else 'mrtz' if tgt == 8 else 'null' if tgt == 9 else f'pos{tgt - 12}' if 12 <= tgt < 16 else f'param{tgt - 32}'
            txt = f"exp {tname} en 0x{en:x} {' '.join(f'v{x}' for x in v)}{' compr' if compr else ''}{' done' if done else ''}{' vm' if vm else ''}"
        elif w >> 26 in (0x38, 0x3a):  # MUBUF / MTBUF
            n = 8
            op = w >> 18 & 0x7f if w >> 26 == 0x38 else w >> 16 & 7
            off = w & 0xfff
            va, vd, rs, so = w2 & 0xff, w2 >> 8 & 0xff, (w2 >> 16 & 0x1f) * 4, w2 >> 24 & 0xff
            kind = 'buffer' if w >> 26 == 0x38 else f'tbuffer dfmt{w >> 19 & 0xf} nfmt{w >> 23 & 7}'
            txt = f"{kind}_op{op:x} v{vd}, v{va}, s[{rs}:{rs + 3}], {src(so, 0)} off 0x{off:x} idxen{w >> 13 & 1}"
        else:
            txt = f'.raw 0x{w:08x}'
        if uses_lit: n = 8
        out.append(f'  {base + i:6x}: {txt}')
        if (w >> 23 == 0x17f) and (w >> 16 & 0x7f) == 1: break
        i += n
    return out


def blob(addr):
    d, segs = q.segments()
    hdr = q.read(d, segs, addr, 0x200)
    p = hdr.find(b'Shdr')
    # 'Shdr' header: +0x14 (from 'Shdr' -4) code length ... the code starts at the first 0xbeeb03ff-like word after the
    # header; layout: [OrbShdr-ish prefix][Shdr hdr 0x50 B][code]
    sh = p - 4 + 0  # address of the Shdr block start relative to hdr (4 bytes before the tag)
    clen = struct.unpack_from('<I', hdr, p + 0x10)[0]
    code_off = hdr.find(struct.pack('<I', 0xbeeb03ff), p)  # PS4 compiler prologue: s_mov_b32 vcc_hi, lit
    if code_off < 0: code_off = p + 0x50
    return addr, hdr[:code_off], q.read(d, segs, addr + code_off, 0x2000), addr + code_off


def main():
    lst = json.load(open(os.path.join(ROOT, 'notes/raw/render/shaders.json')))
    kind, n = sys.argv[1], sys.argv[2]
    if kind == 'ps': addr = lst[2 + int(n)][2]
    elif kind == 'vs': addr = lst[62 + int(n)][2]
    else: addr = int(n, 16)
    a, h, code, cbase = blob(addr)
    print(f'; blob 0x{a:x} code 0x{cbase:x} len {len(code)}')
    d, segs = q.segments()
    tail = q.read(d, segs, cbase + len(code), 0x180)
    strs = [s.decode('latin1') for s in tail.split(b'\0') if len(s) >= 3 and all(32 <= c < 127 for c in s)]
    print('; names:', ' | '.join(strs[:20]))
    for l in dis(code, cbase): print(l)


if __name__ == '__main__':
    main()
