# Misc stage/data formats of DRACRO.CPK: .bsb .brb .mcb .otb .bin .h

Mapper notes (survey depth). Addresses are eboot-relative (q.py). "(unverified)" = inferred, not read in asm.
Companions: `mbs.md`, `ftx.md`. All files below are plain bytes once the CPK layer is undone (`dcp_cpk` already
handles CRILAYLA; there is no second packing layer: extracted files start directly with the magics below). All integers
little-endian. Layouts were checked against all 384 .bsb / 17 .brb / 307 .mcb files of the dump unless noted.

## 0. Overview: how the pieces fit
A "stage" is a hierarchy: **.brb** (a "round" = a map of several stages, e.g. Town00, Catacomb) references **.bsb**
(one stage = a scrolling background scene: background layers + floors + lanes + objects + spawn points). Each .bsb
carries a file table of (.mbs, .ftx, .mcb) triples: the .mbs/.ftx are the sprite/animation bank and textures (see
mbs.md / ftx.md) for background art and props, the .mcb is the matching per-motion hit/collision data. Characters,
players and effects pair `.mbs + .ftx + .mcb` the same way (via hard-coded path tables; FUN_0029d380(obj, mbs, ftx)
+ FUN_000544e0(obj_sub, mcb) for the effect case, e.g. FUN_001e4370). **.otb** is a single global bank of named
chunks, one per .mbs. **.bin** are tiny boss shot tables. **.h** are CRI ADX2 cue-id C headers (never parsed).

Resource plumbing common to bsb/brb/mcb: files are resources registered in a resource manager (FUN_00568800()+0x48),
looked up **by name** with FUN_0057c9c0(mgr, "dir/name.ext") (case-insensitive strcasecmp over a hash of the
upper-cased byte-sum of the name, bucket array at mgr+0x90, 0x20 B per bucket). The loaded file buffer is at
resource+0xf8; "post-load" fixups (below) run once the buffer is resident.

> **Superseded by `bsb.md`** (exact framing and per-type payloads, validated over all 384 files).

## 1. .bsb  (stage / background scene)   rating: CAREFUL (framing decoded, ~40 item types only identified, payloads not)
Count 384, all in `stage/`: `AL_0_st00`, `MT_0_st01`, `bg54c`, `R_bg..`, `T_Castle00`, `E_Arena` ... (names: area code
+ stage number; `bgNN?` backgrounds; `R_` = round variant (unverified); `T_` town; `E_` event).
It is a **scene description tree**, not pixels: it lists the art banks and instantiates "BSGItem" objects
(BSG = background-stage-graph (unverified expansion; strings "RoundBSG %s", "StageBSG %s" in FUN_00017800)).

### Layout
| off | size | meaning |
|---|---|---|
| 0x00 | u32 | version = 0x47 (71) in every .bsb/.brb |
| 0x04 | u32 | 0 |
| 0x08 | 4 x s32 | stage bounds (minX, minY, maxX, maxY) as seen in samples (MT_0_st01: -907,-981,852,484); the code compares against the sentinel pair 10000000 / -10000000 = "unset" (that is what .brb carries). Read in FUN_0005d120 into obj+0x190..0x19c and used by FUN_0005d430 to set up a tile/region grid (FUN_0003bf60, 256x128 cell units) |
| 0x18 | s32 | N = number of file-table slots (observed 0,3,6,9,12; always multiple of 3) |
| 0x30 + 0x40*k | 0x40 | file entry k: 4-byte lead (0 in samples) + NUL-terminated path (e.g. `stage/bg54c_00.mbs`). Slots come in triples **(.mbs, .ftx, .mcb)**; 429 of the slots in the dump are empty strings. Entries are copied into 0x28-byte std::string-like records at obj+0x320 (FUN_0005d120) |
| 0x30 + 0x40*N + 4 | stream | item stream (see below). In the samples the first record starts at 0x34+0x40*N (checked on the two samples + N=12 file) |

NOTE (open): FUN_0005d120 as decompiled reads count at buffer+4, entries at buffer+8, then 4 ints of bounds + a
u32 K + K ints before the stream. Its buffer base is therefore offset from the file start by a fixed amount I did not
pin down (header words 0x08..0x14 and N@0x18 above are what the file actually contains). The "K ints" are the
0x1c4 = 5 in MT_0_st01 (unverified relationship). Anyone implementing should treat 0x1b4-style framing below as
empirical and verify with a walker before relying on it.

### Item stream (FUN_0005d5f0 = "BsgItem_ParseRecord(stage, cursor**, parentItem, owner)")
Recursive, cursor-based (cursor in param_2, advanced as fields are consumed). Child positions are made relative to the
parent by subtracting the parent's virtual +0x2b0/+0x2b8/+0x2c8 (x/y/z). Common head:
- s32 id (-1 = none; otherwise id + stage+0x310 base)
- s32 (repeats id in samples)
- s32 **type** (0..0x27, switch in FUN_0005d5f0)
- s32 ?
- s32 n, then n x s32 (list; for type 2 n=5)
- 3 x s32 position (x,y,z)
- 0x50-byte common block (starts with two floats 1.0,1.0 = scale; dword 0x13 = count c)
- c x s32 extra (c used for per-item list)
- type-specific payload (e.g. type 2: 0x38 bytes of mostly bytes/flags + 3 dwords; type 7: strncpy of 4 names, 0x94 B)
Walking children recurses; sibling ids run 0,1,2,3.. at the start of the stream (MT_0_st01: types 2, 9, 10, 12 with
record sizes 0xbc/0x80/0x84).

### Type -> class (constructor functions, all base class FUN_0044f340 / FUN_0044ef10 "BSGItem", object 0x340+ bytes)
| type | class / ctor (eboot) | meaning (from the class name string) |
|---|---|---|
| 0, 6, 8, 0x12, 0x13 | FUN_0044f340 (generic) | markers / triggers (unverified) |
| 2 | FUN_00464100 | stored in owner+0x9d*8: stage "start / camera" item (unverified) |
| 7 | FUN_00457690 | "BSGItem_Chara..." family: 4 name strings (char/enemy spawn) + counters |
| 9 | FUN_00462240 | BSGItem_Lane |
| 10 (0xa) | FUN_00458da0 | BSGItem_Object |
| 0xb | FUN_00459950 | BSGItem_State |
| 0xc | FUN_00451bc0 (0xa90 B) | DcBSGItem_Mone (money / score item) |
| 0xe | FUN_0046e150 | ? |
| 0x10, 0x1a | FUN_00459b70 | BSGItem_Floor (floor / walkable area) |
| 0x14, 0x15 | big sub-switch, ~35 classes (FUN_00466e90, 467930, 467fa0, 468430 ...) | enemy / hazard / interactive specialisations |
| 1,0xf,0x1d,0x1f,0x26 | skipped (no object) | |
Also strings only: BSGItem_ShadowFloor (FUN_00460d60), BSGItem_ShadowLight (FUN_00461860), DcBSGItem_Chara (FUN_00454f10),
BSGItem_Chara_Slime (FUN_004568d0). Names embedded in the stage data also carry item names ("HIDDEN_ITEM", ...).

### Loaders / users
FUN_00479710 (stage-object update, state 2): FUN_0057c9c0(resMgr, name) -> FUN_0005d120 (header/file table) ->
FUN_0005d430 (set bounds, reset counters, FUN_0005d5f0 on the root, then FUN_00062c90). FUN_0047c300 does the same for a
second class. Path tables: FUN_00041510 (huge, stage id -> names), FUN_000654c0 / FUN_000660a0 / FUN_00074630 /
FUN_00151270 (per-stage special-casing by name compare).

### Relation to .mbs/.ftx
Each triple = background/prop bank (.mbs motions, sprite meshes) + its textures + .mcb. Items (type 10 Object, etc.)
select an mbs by index (unverified) and play motions; the stage itself holds no geometry.

## 2. .brb  (round = map of stages)   rating: CAREFUL (header only)
Count 17 in `round/` (Town00, Tutorial, Arena, Camp_A/B/C, LostZone, AncientLabyrinth, Catacomb, ForgottenRealms,
GhostshipCove, LostWoods, MageTower, MoldyDungeon, MysticRuins, UnderCitadel, RandomDungeon). Table of 18 round names at
data 0xbe0f90 (stride 0xd0) searched by FUN_0004aab0 (name -> round index). Loaded by FUN_00396e80/397350 (Town),
FUN_003a69e0 (Tutorial), FUN_003c3130 etc.
Same framing as .bsb: version 0x47, bounds both = unset sentinel (+0x08 = 10000000,10000000,-10000000,-10000000),
+0x24 = 6, +0x2c = number of top-level items (1..22) then the same BSGItem record stream (types named "Stage", "Floor",
"Entrance", "Portal*", "Enter", "EventLock" in embedded strings; stage items reference `bgNNx.bsb` by name, e.g. Town00 ->
bg25a.bsb, bg01a.bsb, plus NPC names "Peasant_Man"...). So .brb = the world graph; .bsb = one node of it.
Exception: RandomDungeon.brb (1 MB) has a different header (+4 = 4, +8 = 48): a block table, not decoded.

## 3. .mcb  (per-motion collision / hit data)   rating: ROUTINE (framing verified; box fields not interpreted)
Count 307 (84 character, 13 player, 2 effect, 208 stage). Paired by name with an .mbs (Goblin00.mcb with Goblin00.mbs).
**Corrections (formatter, asm-checked in FUN_0005d0b0):** index slots start at **+8** (`[buf + id*4 + 8]`), u16@0 =
slot count (ids < A); u32@4 unknown; records at 8+4*A. u16@2 (B) can overstate the record count: T_MagesGuild00,
T_Result00, T_Shop00 end on a record boundary before B records (loader walks blindly) - reader reports `missing`.
Post-load fixup FUN_0005d0b0 (called from FUN_000544e0, which stores the resource at owner+0x3f8):
- u16 A @0 = highest motion id; u16 B @2 = number of records
- index table @4: (A+1) x u32, **filled in at load time** with each record's byte offset, keyed by the record's id
  (the file stores zeros there; the table is empty on disk)
- records begin at 8 + 4*A and are contiguous:
  - u16 id (= motion index of the paired .mbs)
  - u8 n1 @+2, u8 n3 @+3, u8 n2 @+4 (+5..7: flags/ids; byte +6 = 0x9d in the dragon sample)
  - then n1 x 0x20-byte entries, then (n3 + n2) x 0x28-byte entries
  Chain-walking reaches exactly end-of-file for all 307 files, so the framing is certain.
- 0x20-byte entry sample: 4 head bytes, then s16 values (look like a rectangle/box: x, y, w, h-ish) plus small ints.
  0x28-byte entries not inspected. Meaning of n1 vs n2/n3 (hit boxes vs hurt boxes vs attack info) = unverified.
Use: gameplay hit detection per animation frame/motion; no rendering.

## 4. .otb  (global bank "OTB ")   rating: CAREFUL
Single file `other/DraCro.otb` (274032 B), loaded as a resource (RTTI "13FnResourceOTB", FUN_00588b30/00588e70 are its
debug-viewer methods). Header: `'OTB '`, u32 size-2000 (= 272032), u32 0x7d0 (offset/size of the index part), u32 237
(number of entries), then u16s. Body: 237 sub-blocks tagged **'OTBM'** (u32 size, u32 0x50, ..., a 32-byte NUL-padded
name such as `AMAZON00.MBS`, then more tables). Lookup: FUN_00052a30/00053170 -> FUN_00586730(obj, "OTHER/DRACRO.OTB", mbsName)
-> FUN_0056e520 (find chunk by name in the resource at +0x158); result stored at obj+0x28. So OTB carries extra
per-.mbs data (probably precomputed mesh/draw tables that avoid runtime work; unverified) addressed by mbs name.
Not needed for a first renderer if mbs.md's own tables suffice.

## 5. .bin  (boss shot tables)   rating: ROUTINE
`other/DcAncientDragon_ShotTable.bin` (1356 B = 6 x 226) and `...ShotTable2.bin` (678 B = 3 x 226). Loaded by FUN_0010f6a0
through the generic file loader FUN_0056d7f0(obj, path, 0, destBuf, 0xe20) into a 0xe20-byte buffer; count = size / 0xe2
(= 226-byte records, no header; records are arrays of s16 pairs/ints). Per-phase shot geometry for the Ancient Dragon boss.

## 6. .h  (CRI ADX2 headers)   rating: ROUTINE (not game data to parse)
219 files under `snd/` (bgm.h, dc_sound_acf.h, narration_*.h ...): plain-text C headers generated by CRI Atom Craft,
`#define CRI_<ACB>_<CUE> (id)` cue-id lists, CRLF. The game never reads them at run time (compile-time ids); only useful as
cue name -> id tables for the audio work (ADX2 .acb files are the real data).

## Function table
| addr | name (proposed) | role | conf |
|---|---|---|---|
| 5d120 | BsbRes_ReadHeader | file-table copy, bounds, stream start | med |
| 5d430 | BsbRes_BuildStage | bounds grid + parse root item | med |
| 5d5f0 | BsgItem_ParseRecord | recursive item-stream parser, 0x28-way type switch | high (structure) |
| 44f340 / 44ef10 | BSGItem ctor (generic) | | high |
| 464100,457690,462240,458da0,459950,451bc0,46e150,459b70 | per-type BSGItem ctors | | med |
| 479710 / 47c300 / 47c070 | stage-object state handlers using the resource | | med |
| 5d0b0 | McbRes_BuildIndex | writes record offsets into the index table | high |
| 544e0 | Mcb_Attach(owner, path) | lookup resource by name, store at +0x3f8, build index | high |
| 57c9c0 | ResMgr_FindByName | | high |
| 586730 / 56e520 | Otb_FindChunk | | med |
| 4aab0 | RoundName_ToIndex | | high |
| 10f6a0 | AncientDragon_LoadShotTables | | high |

## Open questions
- Exact file-offset framing of the .bsb header vs the loader's buffer base (see note in sec. 1); record payloads per type.
- MCB 0x20/0x28 entry semantics; OTBM sub-block contents; RandomDungeon.brb structure.
- A walker/dumper for .bsb (type histogram across all 384 files) would settle the payload layouts cheaply.
