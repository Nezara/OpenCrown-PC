# .mbs ("FMBS") sprite / animation bank

Mapper notes. Addresses are eboot-relative (as in q.py). "(unverified)" = inferred, not read in asm.
Companion: `ftx.md` (the paired .ftx texture bank). Samples used (not copied here): Barrel00 (character), OnmemoryEffect00
(effect), Interface_US (UI), Amazon00_arms00_lost / Amazon00_select (small player files). Every statement about
file layout was checked against these; field-range checks (max values vs. table counts) are what pin the index
relationships below.

## 1. Summary
An .mbs is a **2D sprite-mesh animation bank**: a table of textured quads ("parts") grouped into "cells" (one cell =
one drawable pose/sprite), a table of transform keyframes, and a motion -> track -> frame hierarchy that sequences
cells over time. It contains **no pixels**; sprites reference textures of the paired .ftx **by child index**
(byte in each part record) with **normalized float UVs** stored in the mbs (not pixel rects). Vertex positions are
float in sprite-local logical units; typical texels-per-unit ratio measured in the samples is ~4.0 (texture is 4x the
on-screen size; effects differ because they are scaled).

The loader does nothing but (optionally byte-swap and) relocate: the 14 section offsets in the header are turned into
absolute pointers in place (FUN_0057a520), and the vertex arrays are later bound to the GPU **directly from the
file buffer** (colour set, position set and UV set arrays are used as vertex streams by FUN_005a7260). So the
on-disk record layouts are the runtime layouts.

Rating: **routine** for the on-disk layout and texture/UV referencing (all fields below that matter for rendering
are cross-checked); **careful** for the animation player (flag bits of frames, interpolation modes, secondary-bank
override, shadow/3D helper tables c8/d8 are only partly decoded).

## 2. Function table
| addr | name (proposed) | role | conf |
|---|---|---|---|
| 5727f0 | FmbsHeader_Read | memcpy 0x120-byte header into the object, check 'FMBS', byte-swap fields if BE flag, require u16@0x14 == 0x72; on failure sets obj+0xa0 |= 0x10 | high |
| 572a80 | FmbsHeader_ByteSwapInPlace | swaps the header fields of a BE file in the file buffer (u16@0x14, counts 0x50..0x70, 14 pointer slots, date stamps) | high |
| 572c30 | MbsRes_OnLoaded (first vtable slot of the MBS resource class, vtable at 0xbbdb10) | obj+0x158 = file buffer; FmbsHeader_Read(buf, flags&1); then FUN_0057a520 | high |
| 57a520 | Fmbs_RelocateAndSwap | if byte@0x17 bit2 clear: swap whole body (if bit0) and convert the 14 u64 section offsets @0xb0..0x118 to absolute pointers (0 stays 0); sets bit2 | high |
| 57abc0 | Fmbs_SwapSection110Record | swaps one 0x60-byte record of section 0x110 (u16 fields only) | high |
| 57b850 | MbsRes_LoadStep (vtable +0x138 of 0xbbdb10 class) | resource state machine: case1 FUN_0057bb00, case3 raw copy of buffer, case4 sized decode (FUN_005a5040/5a5070, container header at +0x70/+0x78), case5 release/finish | med |
| 579070 | MbsPlayer_Update(dt, obj) | per-frame: virtual +0x48 current motion id, +0x50, FUN_00578000 (select/refresh), then for each of the obj's layers (stride 0x120, obj+0x178 begin, u16 count obj+0x180... see sec. 5) calls Layer_Eval(layer,0), then virtual +0x58 | med |
| 578000 | MbsPlayer_SetMotion(dt, obj, motionId) | bounds-checks id < u16@0x64 of obj+0xc0->+0x158 header; motion flag byte@+0x2f bit0 = disabled; allocates layer array (0x120 B each, obj+0x178, count = motion byte@+0x2a, max of primary/secondary bank) and 0x68-byte "child state" array (obj+0x1a0, count = hdr byte@0x69 * layers); calls Layer_Init per track; optional dt catch-up via FUN_00578460 | high |
| 572ca0 | Layer_Init(layer, trackIdx, owner) | stores owner at +0xa8, track index +0xbe; copies track bytes +0x13 (start frame, signed) / +0x14; sets frame countdown +0xb0 = frame.duration*1000; fires frame event if frame flag 0x80 | high |
| 578460 | MbsPlayer_Tick | fades 3 RGBA tints (owner +0x98/+0xa4, counters +0xb0 s16[3]), then converts dt via engine timer (FUN_00568800 +0x40 -> +8) and advances every layer with Layer_Advance; accumulates motion position floats (+0x160..+0x16c) using track +4 duration | med |
| 5755c0 | Layer_Advance(layer, dt) | consumes duration (u16@+6, scaled x1000), handles frame flags (stop / loop-goto / jump with count / event) and advances index +0xc2 | high (structure), med (flag meanings) |
| 5753e0 | Layer_Eval(layer, flag) | picks previous / current / next / next-next frames (clamped inside the track), computes the eased time t (+0xf mode) cached at layer+0xd0..0xe0, calls Layer_BuildDraw | high |
| 573bb0 | Layer_BuildDraw(layer, f_prev, f_cur, f_next, f_next2, flag) | interpolates the transform keyframes (section e8) between frames (linear if frame byte +0x10 == 1, else 4-point spline (t, t^2, t^3 cached)), builds matrix, multiplies tints (owner colour x frame colour / 255^3), then allocates a 0x280-byte DrawCmd (pool 0xe8e8a0, ctor FUN_005a6820, vtable 0xbc0e98) filled with matrices, colour, cell pointers, texture bank (owner+0xd8) and mbs resource (owner+0xc0); submits with FUN_00597830 (layer = owner short +0x154 + frame byte@+0x13) | high (flow), med (details) |
| 5a7260 | DrawCmd_Execute (vtable +0x20 of 0xbc0e98) | the renderer: for each part of the current cell (and of prev/next/next2 cells when morph mode 1/2) binds texture (FtxBank get-texture by part byte +7), sets blend from part byte +6, binds vertex streams from the file arrays (positions, colours, UVs) and issues one draw of 6 vertices per part | high |
| 70970 | FtxBank_GetTextureDesc(idx) | returns `&rec[idx] + 0x24` (the 32-byte T# copy) or 0 while loading (FUN_00594c70); the draw path's "get texture by index" (virtual on FTX classes, slot offset unverified) | high (body), med (slot) |
| 579290 | Transform_BuildMatrix(obj, out) | builds a 4x4 from owner scale / flip flags (obj+0x17 bits 0x20/0x40/0x200) and Euler angles (+0x44, +0x40, +0x84 floats) | med |
| 572dd0 / 572fb0 / 5732c0 / 573480 / 573790 | Layer_*Helper | called from Layer_BuildDraw when cell byte @+0x18 != 0 and frame byte +0xe in {0,1,2}: use sections c8/d8 (and the transform); 572dd0 builds a 4-point list from layer +0x80..+0x8c. Decompiled, NOT read | low |
| 575e90, 5769a0, 576ab0, 57acf0, 5758f0, 575a90, 575360, 578830 | misc | decompiled / identified as neighbours (container/vector helpers, owner setters), NOT read | low |
| 5a6820 | DrawCmd_ctor | read in asm: sets vtable 0xbc0e98 (+0x10), copies a 0x40-byte default matrix from 0xe92410, zero-inits 10 {ptr,0xff} texture-override slots at +0x90.. | med |

Object classes, as far as they appear (unverified naming): the **effect/animated object** (0x860 bytes, created
e.g. by FUN_0000f220 -> FUN_00090df0) has: +0xc0 primary MBS resource (header ptr = res+0x158), +0xc8 optional secondary
MBS resource, +0xd8 the FTX resource, +0x148/+0x150 current motion id (u16, -1 = none) (`+0x150` used by
MbsPlayer_Tick), +0x178 layer array (0x120 B each, u16 count at +0x180), +0x1a0 0x68-byte child states, +0x1b8/+0x1c0 a
u16 vector of per-layer cell-index offsets (added to a frame's cell index), +0x110.. 4 texture-override {FTX object,
index} slots (draw-cmd flag 0x10).

## 3. On-disk layout (little endian PC/PS4; a BE variant exists via byte@0x17 bit0)

Chunk convention identical to FTEX: `+4` u32 = file size - 0x120 (header) - 0x10 (footer); `+8` u32 = 0x120 header
size; file ends with a 16-byte footer "FEOC", 0, 0x10, 0. Sections are 16-byte aligned (pad with zeros).

### Header (0x120 bytes)
| off | size | meaning |
|---|---|---|
| 0x00 | 4 | "FMBS" |
| 0x04 | u32 | payload size (see above) |
| 0x08 | u32 | header size = 0x120 |
| 0x0c | u32 | 0 |
| 0x10 | 4 bytes | 01 00 22 04 in all samples (unread) |
| 0x14 | u16 | version, **must be 0x72** |
| 0x16 | u8 | 0x30 in samples (unread) |
| 0x17 | u8 | flags: bit0 = big-endian file (loader swaps), bit2 = "already relocated" (set by the loader in memory); samples 0x02 |
| 0x18 | u32 | 0x2000 in samples (unread) |
| 0x20, 0x30, 0x40 | 16 each | three stamps: u32 (id/size/hash, unknown) at +0, u16 year at +4, then month, day, hour, minute, second bytes (build dates, same convention as FTEX); unread by the runtime |
| 0x50 | u32 | count of section 0xd0 (parts) |
| 0x54..0x6e | u16 each | counts, see table below (0x68 is a u8 + u8: +0x68 unknown, **+0x69 = extra child slots per layer**, read by MbsPlayer_SetMotion) |
| 0x70 | u16 | swapped by the BE path, 0 in samples |
| 0x80 | 0x20 | stem name string, e.g. "Barrel00.mbs" (not used by the loader) |
| 0xa0 | 16 | swapped as a vector by the BE path, zero in samples |
| 0xb0..0x11f | 14 x u64 | **section offsets** (file offsets; 0 = section absent), relocated to pointers at load. 0xb0 = 0x120 in every sample (first section starts right after the header) |

### Sections (offset slot, record size, count source, my name, content)
Sections are stored in this file order (not slot order): b0, b8, c0, c8, e0, e8, f0, d0, d8, f8, 100, 108, (110, 118).
Every size/count pair was verified by `count * stride == distance to next section` in the samples.

| slot | stride | count at | name | record layout |
|---|---|---|---|---|
| 0xb0 | 24 | u16@0x54 | ColorSet | 6 x RGBA8 vertex colours (one per fan vertex); used as a vertex stream |
| 0xb8 | 48 | u16@0x56 | UvSet | 6 vertices x (f32 u, f32 v), **normalized 0..1**; vertex 0 is the centre, 1..4 the corners, 5 repeats 1 (fan; unverified prim type 2 in FUN_00564b30) |
| 0xc0 | 48 | u16@0x58 | PosSet | 6 vertices x (f32 x, f32 y), sprite-local units, same fan arrangement; used as a vertex stream |
| 0xc8 | 80 | u16@0x5a | (unknown, "Quad3D"?) | 8 f32 (4 xy corners of a rect) + 12 f32 (look like 2x2 orientation/rotation entries). Consumed with d8 by the Layer_*Helper functions; 0 entries in the effect file |
| 0xd0 | 16 | u32@0x50 | Part | +0 u16 id (unknown role, 0..205 in Barrel), +2 u16 0, +4 u8 ?, **+5 u8 flags**: bit0 = sampler (point/linear, changes a render-state word 0x1000000 vs 0x1500000), bit1 = hidden, bit2 = **untextured** (no texture/UV), bit3, bit4 (use fixed tint), bit5 (alt colour); **+6 u8 blend/shader mode** (0..6; 6 = special, modes 1,2,4,5 use the second colour group); **+7 u8 = FTX child texture index**; +8 u16 visibility mask (tested against the owner's mask at +0x1dc, part skipped if overlap); **+0xa u16 = index into UvSet (b8)**; **+0xc u16 = index into ColorSet (b0)**; **+0xe u16 = index into PosSet (c0)** |
| 0xd8 | 8 | u16@0x5c | (unknown) | u16 value (0..10 in Interface, 0/1 in Barrel), u16 0..3, 4 bytes 0. Passed with c8 to the helpers; values are in range of FTX child count in the samples but the part path never reads this table, so NOT the texture reference (unverified role) |
| 0xe0 | 28 | u16@0x62 | Cell | f32 x4 rect (x0,y0,x1,y1 bounding box, local units), **+0x10 u32 = first Part index**, +0x14 u16 (0..2 / 0..17, unknown), **+0x16 u16 = part count**, +0x18 u8 (nonzero => cell uses c8/d8 helpers), +0x19 u8 flags (bit3 = ignore part colours, bit4 = ?), +0x1a u16 0 |
| 0xe8 | 36 | u16@0x5e | Transform | 8 x f32 (position x,y,z; rotation x,y,z (radians); scale x,y -- identified from the matrix builder and sample values: e.g. (0,-10,0, 0,0,0.349, 1,1)) + u32 at +0x20 (packed RGBA tint, 0xffffffff = none; not interpolated as float) |
| 0xf0 | 32 | u16@0x60 | Frame (keyframe) | **+0 u16 = Cell index** (added to the owner's per-layer offset), +2 u16 0, **+4 u16 = Transform index**, **+6 u16 = duration in ticks** (runtime multiplies by 1000), +8 u32 flags (0x40 = skip/inactive, 0x20, 0x100, 0x04 = jump, 0x08 = stop/hold, 0x1800 = end variants, bit 0x80 of byte@+8 = fire event; +9 bit5 -> owner flag; meanings partly unverified), +0xc u16 jump target (frame index in track), +0xe u8 (0/1/2 = which c8/d8 helper), +0xf u8 time-curve (1 = linear, else smooth t^2/t^3), +0x10 u8 transform interpolation (1 = linear, else 4-point), +0x11 u8 **cell-vertex morph order (0 none, 1 = 2 cells, 2 = 4 cells; stored into draw cmd)**, +0x12.. , event args: +0x14 u32, +0x18 u16, +0x1a u16, +0x1c u32 (passed to owner virtual +0x28) |
| 0xf8 | 48 | u16@0x64 | Motion | f32 x4 rect (bounding box), **name[24] at +0x10** (ASCII, e.g. "IDLE", "MOVE_1", "DIE_A"), **+0x28 u16 = first Track index**, +0x2a u8 = track (layer) count, +0x2b u8 = index of the "main" track (duration), +0x2c u16 ?, +0x2e u8 ?, +0x2f u8 flags (bit0 = disabled) |
| 0x100 | 24 | u16@0x66 | Track | **+0 u16 = first Frame index** (into 0xf0), **+2 u16 = frame count**, +4 u32 total duration (ticks), +8 u32 / +0xc u32 loop range or 0xffffffff, +0x10 u16 (0/1), +0x12 u8, **+0x13 s8 start offset in track**, +0x14 u8 (0..23, layer/channel id copied to layer+0xc0) |
| 0x108 | 20 | u16@0x6a | (unknown) | u32, u16, u32, u16+u16, u32; 2 records in Barrel, 13 in the effect file, 0 in others (values like 116, 36..41, 9). Possibly named events/markers (unverified) |
| 0x110 | 96 | u16@0x6c | (unknown) | 16-bit-field records (swap pattern: 0x1e u16 fields in 4-word groups); **absent (0) in all 6 samples** |
| 0x118 | 120 | u16@0x6e | (unknown) | 32-bit mixed records; **absent in all 6 samples** |

### How a sprite is drawn (chain)
Motion (by id / name) -> its Tracks -> each Track runs a list of Frames (timeline) -> a Frame names a **Cell**
(sprite) and a **Transform** (position/rotation/scale/tint) -> a Cell names a run of **Parts** (u32 start + u16 count in
the Part table) -> each Part = one textured fan of 6 vertices: positions from PosSet[+0xe], colours from ColorSet[+0xc],
UVs from UvSet[+0xa], texture = **FTX child #(byte +7)**, blend mode byte +6. The owner object positions/scales
the whole thing (FUN_00579290 matrix) and layer order comes from the owner short +0x154 plus the frame's signed byte
+0x13 (draw-queue layer key; unverified which byte).

### Texture referencing (answer to the main question)
- Texture = **index** of an FTX0 child, taken from **byte +7 of the 16-byte Part record**; the FTEX name table is not
  involved. The ftx is the owner's FTX resource (owner+0xd8, loaded from the same stem) and the lookup is
  `FtxBank_GetTextureDesc(idx)` -> the 32-byte T# copy built by the FTX loader (see ftx.md).
- UVs are **normalized floats (0..1)** in the UvSet (6 per quad); vertex positions are **float local units**, not
  pixels. Measured: UV extent x texture size / position extent ~= 4.0 for most parts of Barrel and Interface (the
  atlas is authored at 4x the on-screen size); the effect file shows 1.0-1.8 (scaled sprites). No pixel rect is stored.
- Parts with flag bit2 set have no texture/UV (solid-colour quads).
- An owner can override the texture per index (4 slots of {FTX object, child idx} at owner+0x110, enabled by draw-cmd
  flag 0x10); the draw command stores the owner's FTX at +0x68.
- Observed child counts in the samples: Barrel 2 (parts use idx 0..1), Interface 11, OnmemoryEffect00 3; Part byte +7
  never exceeded the FTX child count there.

### Animation summary
- Time unit: frame `duration` ticks (u16) x 1000 internal units, decremented by an engine-timer-scaled dt; a track's
  frames are consumed in order, with per-frame flags for stop / jump (+0xc) / loop / event.
- Interpolation: **between consecutive Transform keyframes** per layer: time curve selected by frame byte +0xf
  (1 = linear; otherwise eased with t^2/t^3 terms), transform interpolation by byte +0x10 (1 = linear, else 4-point spline
  using prev/cur/next/next2 frames). Cell vertex morphing between the part fans of adjacent cells is selected by
  byte +0x11 (0/1/2 -> 1, 2 or 4 cells whose positions/colours are blended in the renderer).
- There are no bones: "layers" are independent tracks, each with its own cell sequence; hierarchy is flat (parent
  attachment is only via the owner object / secondary bank).
- Secondary bank: a second MBS (owner+0xc8) can supply cells/transform data for layers whose bit is set in the
  owner mask at +0xd0 (equipment / variants; unverified).

## 4. Dependencies / open questions
- Dependencies: FTX resource (ftx.md) for textures; engine timer FUN_00568800 (+0x40 -> tick length); render queue
  FUN_00597830 / draw-cmd pool 0xe8e8a0; GNM wrappers in FUN_005a7260 (FUN_00564030 bind texture, FUN_0055e660 vertex
  buffer descriptors, FUN_00564b30 draw). The PC port must reimplement these; no eboot conversion of mbs data exists.
- Open: (1) role of Part +0 u16, Cell +0x14 u16/+0x18/+0x19 bits, tables c8 and d8 (helper functions 572dd0, 572fb0, 5732c0,
  573480, 573790 not read - probably shadow / lit 3D-ish quads), section 0x108, sections 0x110/0x118 (not found
  non-empty; check the other ~400 mbs files); (2) exact frame flag bits (0x100, 0x1800, 0x20) and the jump/loop
  count semantics in Layer_Advance; (3) full semantic of the vertex-morph modes inside FUN_005a7260 (5a9830 /
  5a9eb0 / 5aa5b0 not read); (4) blend/shader mode byte (+6) -> GPU state tables (0x79f854.. in the eboot);
  (5) header bytes 0x10..0x1f, the 0x20/0x30/0x40 id fields, byte +0x68; (6) how paths pair .mbs with .ftx for
  stage/UI users (ftx.md covers the effect object); (7) the .mcb sibling file (OnmemoryEffect00.mcb, 39 KB, no
  magic seen in the first bytes; not mapped); (8) MbsRes_LoadStep case 4 (container/compression of the on-disk
  buffer) not read; the cpk tool already yields the plain FMBS.
- Functions read in this unit: 5727f0, 572a80, 57a520, 57abc0, 572c30, 57b850, 579070, 578000, 572ca0, 578460,
  5755c0, 5753e0, 573bb0, 5a7260, 579290 (partly), 572dd0 (head), 5a6820 + 70970 (asm). Decompiled but unread: 572fb0,
  5732c0, 573480, 573790, 575e90, 5769a0, 576ab0, 57acf0, 5758f0, 575a90, 575360, 578830.
- Raw dumps: notes/raw/mbs/ (c/a.c, b.c, c.c, d.c Ghidra output; sample extracts; list.txt). No game data copied here.
