# .ftx texture bank (and its pairing with .mbs)

Mapper notes. Addresses are eboot-relative (as in q.py). "(unverified)" = inferred, not read in asm.

## 1. Summary
An .ftx is a flat container ("FTEX") of N child chunks ("FTX0"), each holding one Sony **GNF** texture file
(PS4 GNM `T#` descriptor + raw tiled pixel data). The loader copies/defers each GNF's pixels into GPU memory, patches
the `T#` base address and keeps a 32-byte copy of the descriptor in a 0x58-byte per-texture record. There is no
format conversion in the eboot: the PC port must decode BC7 + GNM thin-1D tiling itself (section 3).
Every sample inspected (12 files across effect/player/stage/other, incl. fonts, Interface, common, 4K manual pages)
is BC7 UNORM, 1 mip, 1 slice, tiling index 13. The layout below is verified against real files, and a detiled
sample decoded to clean glyphs.

.mbs ("FMBS") is a separate effect/sprite-animation data file (header 0x120 bytes, parsed by FUN_005727f0). The
effect object stores the two paths side by side (FUN_00090df0: mbs path at obj+0x7c0, ftx path at obj+0x7e8, variant
index at obj+0x840) and the resource system loads both; pairing is by path convention (same stem, e.g.
`effect/OnmemoryEffect00.mbs` + `.ftx`). How MBS sprites select an FTX texture (index vs name) is NOT yet mapped.

Rating: **routine** for the on-disk format (small, fully understood, all samples consistent); **careful** only for
the GNM detiling (verified for tile index 13 / 128-bit blocks only; other tile modes never seen in samples).

## 2. Function table
| addr | name (proposed) | role | conf |
|---|---|---|---|
| 5a3470 | FtxBank_Update (vtable +0xe8 of resource class) | when load finished (state byte +0x5c == 0): iterates children, appends a 0x58 record per texture, calls FtxTex_FromGnf; then advances state via vtable+0xa0 | high |
| 5a31d0 | FtxTex_FromGnf(rec, gnf, gnfSize) | validates 'GNF ' (0x20464e47), copies header, checks size, allocs GPU mem, copies or defers pixel data, stores T# | high |
| 5a5000 | Chunk_GetChild(chunk, i) | generic chunk iterator (also used by FUN_0056e520, a by-name lookup on a different, name-bearing bank) | high |
| 637600 | Gnf_PatchTextures(gnf, idx, n, ptrs) | for each tex: sets base address from ptrs[k]>>8 via FUN_00563ad0, zeroes T# dword7; returns &T#[idx] | high |
| 6375d0 | Gnf_GetPixelOffset(gnf, idx) | returns T# dword0 (pixel data offset relative to end of GNF contents) | high |
| 6375e0 | Gnf_GetSizeAlign(gnf, idx) | returns low32 = T# dword7 (size), high32 = 1 << (T# dword1 & 0xff) (alignment) | high |
| 563ad0 | Texture_SetBaseAddress256 | Gnm setBaseAddress256ByteBlocks incl. pipe/bank swizzle bits for macro-tiled modes | med (bit details unverified) |
| 563f40 | Texture_SetMtype(t#, 0x10) | patches memory-type bits in T# dwords 1 and 3 | med |
| 55e850 | PackDataFormat | packs (dfmt, nfmt, 4 swizzle selectors) into a 24-bit value | med |
| 59ac20 / 568510 | GpuHeap_Alloc(size) / GpuHeap_Get | allocator object at DAT_e63cb8; alloc via FUN_00571090 | med |
| 5a3370 | FtxRec_Release | frees rec+0x48 GPU memory (59ac40), then handle rec+0x50 (59afe0) | med |
| 594a70 | FtxBank_dtor | destroys vector of 0x58 records (vector at obj+0x160/0x168/0x170) | med |
| 594b90 | FtxBank_ClearRecords | same loop, resets end ptr (reload) | med |
| 57b740 | Resource_ReleaseBuffer | releases raw file buffer at obj+0xf8 (size obj+0x100) to one of two pools | med |
| 58dbc0 | Flag_TestClear(obj, clear) | tests/clears bit 0x800 of obj+0x50 | med |
| 58da30 | State_Advance (vtable +0xa0) | sets state byte obj[0x5b+i], sets flag, chains to next stage | med |
| 5727f0 | FmbsHeader_Read | copies 0x120-byte FMBS header, checks magic 'FMBS' (0x53424d46), byte-swaps all fields if the file's byte +0x17 bit0 is set (big-endian flag), requires u16 at +0x14 == 0x72 (version) | high |
| 572c30 | MbsLoad | sets obj+0x158 = obj+0xf8 (file buffer), calls FmbsHeader_Read, then FUN_0057a520 (not read) | med |
| 90df0 | Effect_SetResourcePaths(obj, mbs, ftx, variant) | stores both path strings and variant; remaps variant through table at c1dc80 | med |
| f220 (also bc40/c080/d190/eeb0) | per-effect create (OnmemoryEffect00) | f220 read in asm: allocs 0x860-byte effect object, calls FUN_00090df0 with the .mbs and .ftx path strings | high (f220), low (others) |
| 25820 / 257e0 / 258a0 / 25920 | UiFtxPath_* | return `char*` for STAGE/INTERFACE(_US).FTX etc. (language index from FUN_00016b10()+0x130, minus 2, table at 8c01b0, 5 entries) | high |
| 16da0 | SnapTexture | string consumer only; not read | low |

FTX resource class vtables found via the relocation table: 0x8c8200, 0xbc03d0, 0xbc0c40 (method +0xe8 / +0xe0 =
FUN_005a3470). Dtor wrappers FUN_005a36d0 / 5a3710 -> 594a70.

## 3. On-disk layout (little endian; offsets from file start)
All chunks share a 16-byte prefix: `+0 magic[4]`, `+4 u32 payloadSize`, `+8 u32 headerSize`, `+0xc u16 count`.
FUN_005a5000 walk: first child at `base + base[8]`; next child at `child + child[8] + child[4]`. The eboot has no
immediate for 'FTEX'/'FTX0' (no magic check found), so the game does not validate them; the port reader may.

### FTEX (file)
- `0x00` "FTEX"; `0x04` u32 total child bytes (= file size - headerSize - 0x10); `0x08` u32 headerSize (= 0x20 +
  0x30*count rounded up to 0x40: count 1 or 2 -> 0x80, 3 -> 0xc0, 4 -> 0x100, 10 -> 0x200);
  `0x0c` u16 count; rest of 0x10..0x1f zero.
- `0x20 + 0x30*i` name table entry (consumer not found; the loader ignores it): `char name[0x20]`
  (pattern `<Stem>_arranged000.gnf`), `u32 0`, `u32 payloadSize of child i` (equals child +4), then 8 bytes: build
  timestamp `u16 year, u8 month, u8 day, u8 hour, u8 min, u8 sec, u8 0`.
- Children from headerSize; then a 0x10-byte footer: "FEOC", u32 0, u32 0x10, u32 0.

### FTX0 (child chunk)
- `+4` u32 payloadSize (= GNF header block + pixel bytes), `+8` u32 headerSize (always 0x40 seen), `+0xc` u16 0, rest
  of the 0x40 header zero. GNF starts at `child + headerSize`; FtxTex_FromGnf gets size = payloadSize.

### GNF inside FTX0
- `+0` "GNF "; `+4` u32 contentsSize (0xf8 seen). Contents start at +8 (loader copies contentsSize bytes).
- Contents: `+0 u8 version` (1 or 2; 2 seen), `+1 u8 numTextures` (1 seen), `+2 u8 alignment log2` (< 0x20; 8 seen),
  `+3 u8 0`, `+4 u32 whole GNF file size` (header block included; equals the FTX0 payload - verified on all 2,312
  textures by dcp_tex; earlier read as "total pixel bytes").
  Size check: v1: `numTextures*32 + 8 == contentsSize`; v2: `numTextures*32 + 8 + pad == contentsSize` where pad rounds
  `numTextures*32 + 0x10` up to a multiple of `1<<alignment`. With 1 texture the table is padded to 0xf8.
- T# table at contents+8, 32 bytes (8 dwords) per texture. Fields (standard GCN T#; loader-read ones marked *):
  - d0*: pixel data offset from the end of the contents block (0 seen). Absolute = `gnf + 8 + contentsSize + d0`.
  - d1: low byte* = alignment log2 for the GPU allocation (8 -> 256 B); bits 20..25 DATA_FORMAT; bits 26..29 NUM_FORMAT.
  - d2: bits 0..13 width-1; bits 14..27 height-1.
  - d3: bits 0..11 four 3-bit dst_sel (0xfac = identity RGBA); bits 16..19 last mip level; bits 20..24 tiling index;
    bits 28..31 type (9 = 2D).
  - d4: bits 0..12 depth-1; bits 13..26 pitch-1 (pitch in pixels).
  - d7*: total pixel data size in bytes; cleared to 0 by Gnf_PatchTextures after the base address is patched.
  - GPU allocation size = d7 rounded up to the alignment (FUN_006375e0 + rounding in FtxTex_FromGnf).
- Pixel data follows the contents block (file offset GNF + 0x100 for all samples), d7 bytes.

### DATA_FORMAT / NUM_FORMAT
Seen: DATA_FORMAT 41 = BC7, NUM_FORMAT 0 = UNORM (every sample). The GCN numbering used elsewhere (from memory, unverified):
1=8, 2=16, 3=8_8, 4=32, 10=8_8_8_8, 35=BC1, 36=BC2, 37=BC3, 38=BC4, 39=BC5, 40=BC6, 41=BC7.
Sizes seen: 4096x4096, 4096x2048, 2048x2048, 1024x512, 1024x1024, 3840x2160 (manual pages), 512x512 ... 64x32.
Mips: lastLevel always 0 (one level).

### Tiling (verified on a real font texture)
Tile index 13 = GNM Thin_1dThin (name from memory; behaviour verified): 1D micro tiling, no macro tiles.
For BC7 (16 B per 4x4 block) the micro tile is 8x8 blocks (32x32 px) = 1024 B. Micro tiles are stored row-major
across `pitch/32` tiles per row; the tile rows cover the height rounded up to a multiple of 32 px (3840x2160 ->
2176 rows; data size 3840*2176 == d7). Within a micro tile, block index i (0..63) maps to block (x,y) by plain
Morton order: `x = i0 | i2<<1 | i4<<2`, `y = i1 | i3<<1 | i5<<2`. Determined by brute-forcing all 720 bit orders on
smallfontUTF16.ftx; the result decodes to clean glyphs. The eboot does NOT detile: the GPU samples tiled data natively,
so the port needs its own detile (new code) before upload as BC7.

## 4. Loaded-bank structures (as the loader builds them)
Per-texture record, 0x58 bytes, vector at bank+0x160 begin / +0x168 end / +0x170 capacity:
+0x08 GPU ptr (deferred path), +0x10 source pointer into the file buffer (deferred), +0x18 aligned size,
+0x20 byte flags (bit 3 = deferred copy; FtxBank_Update sets it on every record, so FtxTex_FromGnf records pointers
instead of memcpy), +0x24..0x43 copy of the 32-byte T# with patched base address, +0x48 GPU allocation pointer,
+0x50 i32 allocation handle (<0 = none). Bytes 0x44..0x47 and 0x51..0x57 not read.
Bank object: +0x5c state byte (0 = buffer ready -> build records; 1 = still loading; 2 = discard via FUN_0057b740),
+0xf8 file buffer pointer, +0x100 size.

## 5. Dependencies / open questions
- Imports in these paths: libc memcpy, strlen, strncasecmp, memcmp. No libSceGnm calls here (T# is plain memory;
  the render backend consumes the copies).
- Open: (1) who reads the FTEX name table and how users pick a texture (index vs name); (2) how FMBS sprite/frame
  records reference a texture index (FMBS body after the 0x120-byte header; FUN_0057a520); (3) render path that
  consumes the 0x58 records (readers of bank+0x160); (4) whether non-BC7 / multi-mip / other tile-index ftx exist in
  the ~570 ftx files (only 12 sampled); (5) FUN_00016da0 SnapTexture purpose; (6) enum names from memory;
  (7) deferred-upload (flag 8) completion path.
- Raw dumps: notes/raw/ftx/ (a.c, b.c, c.c Ghidra output; sample extracts). No game data is copied into this file.
