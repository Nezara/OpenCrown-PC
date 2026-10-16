# .bsb stage scene tree (full depth)

Supersedes the .bsb section of `formats_misc.md`. Evidence: `notes/raw/bsb/walk.py` (run `python -I notes/raw/bsb/walk.py notes/raw/misc/all`)
parses all 384 .bsb end-to-end and consumes exactly to end of file. Final line: **"384 files, 384 walked cleanly, 0 failures"**.
The same framing/walker also parses 16 of 17 .brb (RandomDungeon.brb differs, sec. 6). Addresses are eboot-relative; "(unverified by data)" =
read in the loader code only, no file in the dump uses it. Little-endian.
Rating: **routine** for a reader (fixed size rules, self-checking by exact EOF); careful only for the semantics of item fields.

## 1. Framing (reconciles the loader)
The loader buffer base is the file start (resource+0xf8). FUN_0005d120 reads:

| file off | size | meaning |
|---|---|---|
| 0x00 | u32 | version 0x47 (not checked by the loader) |
| 0x04 | s32 N | count of 0x40-byte string slots (copied to std::string-like records at obj+0x320). **N = 0 in all 384 files** |
| 0x08 | 0x40*N | the N slots (none in the dump) |
| o = 0x08+0x40N | 4 x s32 | stage bounds minX,minY,maxX,maxY -> obj+0x190..0x19c (10000000/-10000000 sentinel = unset) |
| o+0x10 | s32 K | art-name table count (0,3,6,9,12 per file; 1914 over the dump) |
| o+0x14 | 4*K | placeholder ints; the loader overwrites them with (string address - obj) offsets: obj+0x178 = K, obj+0x180 = array |
| o+0x14+4K | 0x40*K | K name strings, one per 0x40-byte slot, NUL-terminated (e.g. `stage/bg54c_00.mbs`; 429 slots empty) |
| s = o+0x14+4K+0x40K | | item stream start (obj+0x188 = s) |

(The earlier survey saw this as "N@0x18, entries at 0x30": with N=0 the K field is at 0x18, ints at 0x1c, strings at 0x34.)
FUN_0005d430 (set root): sizes the 256x128-unit region grid from the bounds via FUN_0003bf60 (unless unset), resets stage counters
(obj+0x1a0..0x1bc) and the per-type pointer table at obj+0x1c0+type*8, parses **one root item** with FUN_0005d5f0, then calls FUN_00062c90.
The stream is exactly one root record plus its descendants; the file ends there.

## 2. Item record (FUN_0005d5f0, recursive, cursor passed by pointer)
```
s32 id            ; -1 = none, else id + stage+0x310
s32 f1
s32 type          ; 0..0x27, switch
s32 f3            ; zeroed by the loader afterwards (runtime scratch)
s32 n             ; number of CHILD records following this item's payload
s32 slot[n]       ; scratch child slots (loader writes 0)
s32 x,y,z         ; position; made parent-relative (minus the parent's virtual getters at +0x2b0/+0x2b8/+0x2c8)
0x50-byte block   ; dword[1],[2] = floats 1.0,1.0 (scale); bytes 0x0c.. = NUL-terminated item name (<=32 B); dword[0x13] = c
s32 extra[c]      ; copied into a per-item vector (obj+0x288)
payload           ; type specific (sec. 3)
child records x n ; same format, parent = this item (not read for type 0x24)
```
Item objects store position floats at +0x1fc/+0x200/+0x204. The name is compared (strcmp/strncmp) to set flag bits 0x24e|0x40 and 0x24f|1.
Maximum nesting in the dump is 16. Types 1,0xf,0x1d,0x1f,0x26 create no object (their children are still parsed with a null parent).

## 3. Payloads (bytes after the extra list). Counts = occurrences in the 384 files
| type | count | payload size | class / ctor | notes |
|---|---|---|---|---|
| 0x02 | 384 | 0x38 | FUN_00464100 (0x378 B), stored at owner+0x9d*8 | root/stage item, exactly one per file; mostly bytes (colours) plus dwords at 0x1c,0x20,0x24,0x28,0x2c |
| 0x09 | 2767 | 12 | FUN_00462240 BSGItem_Lane | u32 flags (&1 main lane, &2, &4 = lanes remembered in owner), u32, u32 |
| 0x0a | 4725 | 16 | FUN_00458da0 BSGItem_Object | u32 kind (low byte), s32 (clamped >=0, short), u32 flags (&1 counted), u32 |
| 0x0b | 3282 | 4 | FUN_00459950 BSGItem_State | s32 on/off -> flag 8 at +0x24e; bumps the parent (type 0xa) counter |
| **0x0c** | 10514 | 0x28 + 12 (+6+len if flags&0x10) | FUN_00451bc0 DcBSGItem_Mone (0xa90 B) | **the art item**, sec. 4 |
| 0x0d | 154 | 0 | generic FUN_0044f340 | |
| 0x0e | 39 | 0x1c | FUN_0046e150 (0x360 B) | dword0, 4 dwords (converted to floats), dword5, dword6 |
| 0x10 | 8036 | 0x68 (+8 if sub 2/4) (+0x24 if flags&0x10) | FUN_00459b70 BSGItem_Floor (0x528; 0x530 for sub 2/4) | sec. 5 |
| 0x11 | 669 | 0 | generic | |
| 0x12 | 1965 | 8 | generic (own vtable 0xb9e9f0) | second dword kept as value |
| 0x13 | 1956 | 4 | generic "DcBSGItem_Logic" | one dword |
| 0x14 | 2317 | 4*D14[sub] | "DcBSGItem_Jyouken" family (condition items; ~20 sub-kinds, all FUN_0044f340 with a vtable swap) | dword0 = sub, dword1 = flag (-> byte +0x344). D14 in dwords: sub1:4 2:0x14 3:0x13 4:0x12 5:0x12 6:0x13 9:0x22 0xa:0x13 0xb:0x12 0xc:0x13 0xd:0x14 0xe:0x12 0xf:0x12 0x10:0x12 0x11:0x12 0x12:0x12 0x13:5 0x14:0x12; others (7, 8 ...) = 2. Dump sub counts: 1:564 2:1 3:7 4:365 6:33 7:177 8:113 9:43 0xa:552 0xb:166 0xc:1 0xd:88 0xe:65 0x12:60 0x13:60 0x14:22 |
| 0x15 | 3171 | 4*D15[sub] | enemy/hazard specialisations: sub1 FUN_00466e90 (0x3b8), 2 FUN_00467930, 4 FUN_00467fa0, 5 FUN_00468430, 6 FUN_004688a0, 7 FUN_00468d40, 8 FUN_00469220, 9 FUN_00469690, 0xa FUN_00469af0, 0xb name-switched (FUN_0046a4a0/a7c0/b410/b7d0/bbe0/bf60/c2f0/c680/ca10/cd50/d0d0) | dword0 = sub. D15 in dwords: 1:0x22 2:0x13 4:0x13 5:0x12 6:0x12 7:4 8:3 9:3 0xa:5 0xb:0x21; others = 1. Dump sub counts: 1:2337 4:255 5:9 7:184 8:33 0xa:157 0xb:196 |
| 0x16 | 415 | 0x30 | FUN_00461c00 (0x388) | 2 qwords (+0x360/+0x368) then 4 (x,y) int pairs = polygon merged with parent position; registered in owner list +0x70*8 |
| 0x17 | 144 | 0 | generic | |
| 0x18 | 672 | 0x24 | FUN_00462680 (0xa60 B) | 4 x u16 then 16 bytes of bytes/u16 fields |
| 0x19 | 559 | 0x40 | FUN_00460d60 BSGItem_ShadowFloor | 2 qwords, then 4 corner triples (x, ?, y); owner max shadow y tracked at owner+0x520 |
| 0x1b | 14 | 12 | FUN_00461860 BSGItem_ShadowLight | dword, float, float (angle quantised to 16 bit) |
| 0x1c | 235 | 0x54 | FUN_00463220 (0x3a8) | dword, then 4 x 0x14-byte records (x, y, byte-flags..., float) |
| 0x22 | 15 | 0x10 | generic (0x350 B) | 4 dwords, two copied to globals; registered in owner list +0x98*8 |
| 0x27 | 303 | 0x40 | FUN_0046d500 (0x5c0) | dwords 0,1,4..8, two qwords at 0x24/0x2c, dwords at 0x34..0x3c |

All payload sizes above are verified by the walker (exact EOF on 384 files). Types absent from the dump, sizes from the code only
**(unverified by data)**: 6: 0 (FUN_0044f340, resets counters); 7: 0x94 = 4 x 0x20-byte names + 6 x u16 + 2 u32 (FUN_00457690, "Chara" family);
8: 4 (dword -> +0x340); 0x1a: as floor 0x10; 0x1e: 4; 0x20: 0x30 (12 ints, a rect with offsets; FUN_0044f340); 0x21: 0x14;
0x23: 0x60 + 0x14 (+4 if the flags dword at 0x70 has bit 4) + 8 + 4 (DcBSGItem_Chara FUN_00454f10; special-cases a "Treasurebox" id 0x62);
0x24: 0x28 + ceil4(w*h) with s32 w@0x20, h@0x24 (BSGItem_Chara_Slime FUN_004568d0, tile bytes; children not read);
0x25: 0x80 (+8+4a+8b if flags@0x70 has 0x40; a,b = s32 at 0x80,0x84), FUN_0044ef10 (0x448 B). Types 0,3,4,5 and 0x17,0x0d,0x11: generic, no payload.

## 4. Binding to art (type 0x0c, "Mone")
Payload dwords: [0] **mbs** name index, [1] **ftx** name index, [2] **mcb** name index (-1 = none), [3] (not traced), [4] flags
(0x10 = extra block below), [5],[6],[7], bytes at 0x1d,0x1e,0x1f, dword [8], bytes 0x21,0x22,0x23, byte@0x24 (1,2,3 -> internal ids 0xe2..0xe4);
if flags&0x10: u8@0x28 (layer number; owner+0x534 |= 1<<it), u8@0x29, u8@0x2a (has string), u16 len@0x2c, string of len bytes@0x2e;
then always: 2 dwords (-> FUN_00454bd0 and +0xa10), 1 dword (<<4 -> +0xa3c).
The indices select entries of the **K name table of sec. 1** (obj+0x180 array + obj base). The ctor path: FUN_005770f0(item+0x370, mbs name, ftx name)
loads the sprite-bank + texture pair; FUN_000544e0(item+0x370, mcb name) attaches the collision bank. Checked over the dump: all 9125
non-empty 0x0c items resolve to (.mbs, .ftx, .mcb) in that order; 1389 reference an empty slot (no art). K slots always come in (mbs, ftx, mcb) triples.
The initial motion is not in the payload (default; the extra[c] list and the sibling state items 0x0a/0x0b drive it; unverified).
Other item types name no art: they place lanes, walk floors, shadows, triggers and enemies.

## 5. Floor (types 0x10/0x1a, FUN_00459b70)
Head 0x28: u8 a, u8 sub (2 or 4 = larger subclass with 8 extra trailing bytes), u16 flags (0x10 = trailing 0x24-byte name; 0x4000 = not added to owner list
+0x79*8), dwords at 4,8,0xc,0x10,0x14, qwords at 0x18 and 0x20. Then 4 corners x 16 bytes (s32 x, s32 z, s32 flag, 3 bytes + pad); the
floor bounds (min/max x,z) are accumulated while reading. Extra 8 bytes for sub 2/4: flag byte (bit0,1,2,3 -> bools), bytes at 4,5,6.

## 6. .brb and others
.brb uses the identical framing and walker (bounds = sentinel, K = 0): 16/17 walk cleanly. RandomDungeon.brb has a nonzero N at +4 (4 slots) so
the walker's generic N rule applies, but it fails at an item with c = -1 near 0xe28c4 (deeper block not decoded). Open.

## 7. Function table
| addr | proposed name | role | conf |
|---|---|---|---|
| 0x5d120 | Bsb_ParseHeader | N-table, bounds, K name table | high |
| 0x5d430 | Bsb_SetRoot | region grid from bounds, reset counters, parse root, post pass | high |
| 0x5d5f0 | BsgItem_ParseRecord | recursive item reader, 0x28-way switch | high |
| 0x5d270 | NameVec_Resize | resize helper for the N-table | medium |
| 0x44f340 / 0x44ef10 | BSGItem base ctors | generic item | high |
| 0x462240 / 0x458da0 / 0x459950 / 0x451bc0 / 0x459b70 / 0x460d60 / 0x461860 / 0x464100 | Lane / Object / State / Mone / Floor / ShadowFloor / ShadowLight / Stage-root ctors | types 9/0xa/0xb/0xc/0x10/0x19/0x1b/2 | high |
| 0x457690 / 0x454f10 / 0x4568d0 / 0x46e150 / 0x461c00 / 0x462680 / 0x463220 / 0x46d500 | ctors for types 7 / 0x23 / 0x24 / 0xe / 0x16 / 0x18 / 0x1c / 0x27 | addresses high, roles low |

## 8. Open questions
Semantics of the 0x0c head dwords [3],[5..8] and the extra[c] list; meaning of the 0x14/0x15 sub-kinds (condition vs enemy classes);
how motions are chosen for 0x0c; RandomDungeon.brb; types absent from the dump rely on code-derived sizes only.
