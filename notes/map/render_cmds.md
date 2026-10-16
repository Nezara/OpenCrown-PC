# Render command layer (Stage 4 intercept point)

Builds on `engine.md` s4 (render boundary) and `mbs.md` (Layer_BuildDraw -> sprite DrawCmd). All addresses = eboot VAs.
Evidence: asm (q.py dis) for blend/state/vtables, Ghidra pseudo-C as a hint for the Execute bodies. "(unverified)" = inferred.

## 1. Summary
- The frame is `for layer in layers: for bucket in layer: sort(cmds); for cmd: cmd->Execute(cmd, layerBuf)` (FUN_005a3a00).
  There are exactly **8 command classes** (found by scanning all data for the base slots `251b0,251d0`): the abstract base, **6 concrete
  classes with a draw body** (sprite, X1 quad-list/particles, X2 generic primitive list, X4 snapshot/blit, A, B) and **1 state-only class** (X3 viewport/scissor).
  So Stage 4 needs 7 Execute replacements (6 draws + viewport); everything else (layers, sorting, sizes) is plain data.
- Every Execute is a thin GNM emitter: set raster/depth/blend regs, pick a VS+PS from two global pools of **embedded GCN shader binaries**
  (60 PS + 25 VS + 2 CS, all in rodata 0x787da0..0x79f230), bind constant buffers built from the cmd fields in a per-frame ring pool,
  bind T#/S#, bind vertex streams that point **straight into the .mbs sections**, draw. Nothing is parsed from PM4; replace at Execute level.
- **Correction vs. the viewer's guesses / the task's premise**: the table at 0x79f854 is **not** a blend table, it is a **vertex-shader index table**
  (see s5.2). The real blend factors come from `FUN_005aa470(mode)` (a jump table at 0x79f810) writing CB_BLEND0_CONTROL. Decoded in s4.
- Rating: **careful**. Draw state is simple and fully readable; the effort is in (a) the shader pool (GCN binaries cannot be reused, 85 shaders must be
  re-authored from role + constant layouts below), (b) the sprite constant/permutation logic, (c) offscreen-target effects (modes 1-5).

## 2. Command base class and lifetime
Object layout (vptr = vtable+0x10, base vtable symbol address 0x8bfbe0, vptr 0x8bfbf0 = sprite/X* inherit by overwriting):
| off | meaning |
|---|---|
| 0x00 | vptr |
| 0x10..0x4f | 4x4 float matrix, ctor copies identity from 0xe92410 (0x40 B). Used as the local transform (rows read as 4 xmm at +0x10/+0x20/+0x30/+0x40) |
| 0x50 | u32 index within its bucket (written by FUN_00597830) = stable-sort tiebreak |
| 0x54 | i32 sort key (ctor 0; producers set it; sprite: from owner +0xb8 when flag 0x20, mbs.md) |
| 0x58 | float depth = cmd+0x48 * layerBuf[0x28] + layerBuf[0x38], computed by vtable slot [2] FUN_000251b0 (slot [3] FUN_000251d0 is a nop; caller of slot [2] not located: unverified) |
Vtable slots at vptr: [0] dtor (class specific, trivial), [1] deleting dtor, [2] 251b0 (depth), [3] 251d0 (nop), **[4] Execute(cmd, layerBuf)** at vptr+0x20.
No links / intrusive lists: the bucket holds raw `cmd*` in a vector (bucket record 0x20 B: +8 begin, +0x10 end, +0x18 cap).

**Lifetime**: commands are never freed. Producers allocate with `FUN_0057ae20(pool, size)` = **ring allocator** (pool: +8 base, +0x10 size, +0x1c align,
+0x20 cursor, +0x30 GPU-consumed limit; wraps to base when the tail does not fit; returns 0 if it would overrun the consumed point). Sprite commands use pool 0xe8e8a0; all
transient GPU data written during Execute (constant buffers, vertex data, matrices) use `FUN_0057ae20(PTR_DAT_00bc2378 + layerBuf[+0x14]*0x38, size)` =
**one pool per layer-buffer index** (0xbc2378 array, stride 0x38). Everything is reclaimed by the ring moving on, the bucket vectors are reset per frame (not read). Dtor slots do nothing.
Native port: allocate cmds from a per-frame arena (reset each frame); the object can be a plain struct, no refcounts.

**FUN_00597830(layerBuf, cmd, bucket)** (read, see engine.md): only pushes on the last logic tick of the frame; `bucket<0` -> 0; grows the vector-of-buckets
(FUN_00597960) if needed; `cmd+0x50 = current bucket size`; push_back (FUN_00597ca0 grows).
**FUN_005a3a00(layerBuf)** per layer: select GNM ctx by `layerBuf+0x29c`; flags 0x294: bit0 viewport (FUN_00639610 SetViewport/Scissor from floats +0x284..0x290), bits 1/2/4 copy
staged matrix blocks (+0x80.. -> 0x20.., +0xc0.. etc.), always copies blocks 0x38.. -> 0x80.. and 0x40.. -> 0xc0..; bit3 FUN_0059a420 (per-layer defaults:
reg 0x?? colour-mask/primitive reset via FUN_005613c0/5600a0/55ff50 + `FUN_0059a520` clear-colour setup + `FUN_0059a930` fullscreen pass, see s6); then for each bucket:
`sortMode (+0x2a4)==1` -> FUN_005a3be0 introsort **ascending by +0x54, ties ascending +0x50**; `==2` -> FUN_005a4570 **descending by +0x54, ties ascending +0x50**;
else keep insertion order. Then `Execute` each. Layer matrices relevant to Execute (read as floats): VP-type blocks at layerBuf+0x80/+0xa0/+0x90/+0xb0 (used by X1/X2/A/B), +0x1c0..+0x1ff
and +0x200..+0x23f (sprite). Layer default setup FUN_00596ed0(layer) (called for layers 0,1,2 from the scene function FUN_0058a790): viewport = (0,0,W,H) with W,H the
current screen floats (core+0x40 -> +0x3c/+0x40, 1920x1080 base), builds an orthographic projection (matrix at +0x100..0x13f) and a 2nd/3rd camera matrix pair at +0x180..0x1bf.

## 3. Command classes
All Execute bodies share the prologue: `ctx = platform(FUN_00568510) + 0x12aa0 + layerBuf[+0x29c]*8 -> ptr; gnm = ctx[+8]; ctx[+0x14] = pool index; ctx[+0x58] = layer render target`.
Stage numbers in constant/texture binding helpers: **1 = pixel stage, 2 = vertex stage** (FUN_005646d0 = SetConstantBuffer(ctx stage, slot, buf), FUN_00564030 = SetTexture T#,
FUN_005644b0 = SetSampler S#, FUN_00564b30 = SetVertexBuffers(stage 2, first, count, descs)). Vertex/constant "descriptors" are built by FUN_0055e660 (V# from address, format, stride, count) /
FUN_0055e820 (constant buffer from address+size). Vertex formats: 0x22c70b = float2 (stride 8 or 0x20..), 0xfac40a / 0xfac70e = RGBA8 unorm (0xfac70e used for float4-ish colour in dynamic vertices), 0x3ac70d = float3.
| class | ctor | vptr | Execute | producers (callers of the ctor; game feature unverified) |
|---|---|---|---|---|
| Base | - | 0x8bfbf0 | FUN_000251e0 (stub) | - |
| **Sprite** (mbs part list) | FUN_005a6820 | 0xbc0e98 | **FUN_005a7260** (9663 B, 0x280 B object) | FUN_00573bb0 (mbs Layer_BuildDraw): every mbs/animated sprite |
| **X1** quad-list / particle batch | FUN_00568f30 | 0xbc0ab8 | **FUN_0059f7b0** | FUN_0006bb90, 6e360, 592e90 |
| **X2** generic primitive list | FUN_00569010 | 0xbc0b08 | **FUN_005a0330** | FUN_0019f8c0, 1e1c10, 30af90, 33a570, 5811c0, 581db0 |
| **X3** viewport / scissor | FUN_00569240 (+setter 569300) | 0xbc0b68 | **FUN_005a12e0** | FUN_00050ae0, 52770, 6e360, 295c10, 2978c0, 453a60, 480ba0, 5098f0, 509e10, 592e90 |
| **X4** snapshot blit | FUN_00569320 | 0xbc0bb8 | **FUN_005a15e0** | FUN_00443960 |
| **A** screen overlay quad (fade/flash type) | FUN_000250c0 | 0x8c87a8 | **FUN_0007ddb0** | FUN_00050ae0, 295c10 |
| **B** screen overlay quad (params in cmd) | FUN_00025120 | 0x8c87e0 | **FUN_0007eb40** | FUN_00050ae0, 295c10 |

### 3.1 Sprite (FUN_005a7260) - the big one
Command fields read (offsets in the 0x280-byte object; beyond the base): `+0x60` MBS resource ptr (file buffer = `*(+0x60+0x158)`; sections Part table `+0xd0` 16 B entries,
ColorSet `+0xb0` 0x18 B/part (6 u32 RGBA8), UvSet `+0xb8` 0x30 B (6 float2), PosSet `+0xc0` 0x30 B (6 float2)); `+0x68` FTX bank (vtable +0xe8 = get T# by index, mbs.md);
`+0x70` secondary (mask/second texture) slot struct (FUN_0056fc30 bank, 0x56fc40 index, 0x56fc50/60 two uv corners); `+0x198` colour u32 RGBA8; `+0x19b` group-alpha byte (0xff = none);
`+0x19c` second colour; `+0x1a0..+0x1af` screen-space bounds floats (x0,y0,x1,y1); `+0x1b0/1b8/1c0/1c8` four cell records (current, prev, next, next2; `+0x10` first-part index, `+0x16` part count);
`+0x1d0..+0x1d8` morph floats; `+0x1dc` u32 visibility mask (tested against part +8); `+0x1e0/0x1e4/0x1e8` flash colour(s) and intensity; `+0x1ec` u32 morph mode 0/1/2 (1,2 or 4 cells blended);
`+0x1f0` u32 **effect mode** 0..6; `+0x1f4` u16 flags (0x400: use fixed palette bytes from DAT_00761e44); `+0x1f5` flags (0x10 per-part texture overrides at `+0x110 + 0x10*idx` {ptr,byte}
(mbs.md says slots start at +0x90; discrepancy unverified), 0x40 colour transform enabled, 0x20); `+0x1f6` byte ==1 gradient-tint block; `+0x200..+0x22f` 12 corner/gradient colours;
`+0x230..` floats; `+0x232` byte factor (x 1/255 -> 4-byte PS constant); `+0x233` byte colour-transform preset 1..5 (0/6+ = custom coefficients at `+0x240..+0x278`).
**Colour-transform presets** (tables at 0x79f950..0x79fa50, 9 arrays x 5 floats; read as 3 rows of 3 + 3 extra at +0x270): 5 channel-permutation-style 3x3 matrices (recolouring of characters) (unverified exact orientation).
Execute flow:
1. Build the transformed screen rect from the cmd matrix x layer matrices; cull if fully outside `[0,W]x[0,H]` (W,H = core+0x40 floats) or <4 px.
2. Effect mode (+0x1f0) 1..5: **offscreen group draw** - `FUN_005a9eb0`: bind an offscreen RT from `FUN_0059afa0(platform,0)` (4-slot pool at platform+0x144e0, stride 0x1c0, generation-checked),
   clear the rect (rect-list prim 0x13, blend One/Add/Zero, VS23 + PS51), render all parts into it, and at the end `FUN_005aa5b0` composites the RT onto the layer RT with a textured quad
   (VS20 + PS50, vertex colour = white with alpha = +0x19b/255, **blend = effect mode via FUN_005aa470**, i.e. the table of s4). Modes 0/6 with group alpha (+0x19b != 0xff) use
   `FUN_005a9830` (copy the layer RT region into the offscreen RT, VS22 + PS58, blend 4,0,5) as backdrop; mode 0/6 w/o group alpha draw directly.
   Effect mode 1..5 also override the part blend via table 0x79f840 = {1,2,(none),4,5} (u32 indexed by mode-1; value 0 = keep part blend).
3. Per frame state: raster FUN_00561a10/a30/a20/a40 -> PA_SU_SC_MODE_CNTL: no culling, filled; depth: FUN_00561b00/b10/b50 -> DB_DEPTH_CONTROL z-func ALWAYS, write off (depth effectively unused).
4. Per part (loop over the cell's `part count` parts in the Part table; skip hidden bit1 or mask-overlap): flags drive a 6-bit permutation word `local_238`: 1 = tint const (PS cb slot1), 2 = colour matrix (PS cb slot2, 0x30 B),
   4 = factor (slot3, 4 B), 8 = gradient block (slot4, 0x100 B), 0x10 = textured, 0x20 = textured + mask texture. It indexes the **PS index table 0x79f890** (48 x u32 -> PS pool index 2..49; default 44).
   The **VS index** comes from the 4x3 table at 0x79f854: row = (mask & textured) 0x79f854, (textured, no mask) 0x79f860, (untextured & mask) 0x79f86c, (untextured, no mask) 0x79f878; column = morph mode 0/1/2 (default VS 11).
   Blend = FUN_005aa470(mode), mode = part byte +6 (or the effect override). Texture = T# from the FTX bank by part byte +7 (or per-part override); sampler S#: part flag bit0 -> bilinear (0x1500000) else point (0x1000000) (S# word2), wrap/clamp word0=0.
5. VS constant buffer slot0 (stage 2) = per-cell matrices built in the per-frame pool: morph 0 -> 0x40 B (one 4x4 MVP), morph 1 -> 0x44 B (two cells' 4x4 + t), morph 2 -> 0x50 B (+4 spline weights); slot5 = 8 B UV scale; slot6 = mask UV rect 0x10 B; slot7 = 0x10 B colour.
6. Vertex streams (**direct file pointers**, no copy): stream0 = PosSet[(partWord>>16)] as float2 x6 (stride 8), stream1 = ColorSet[(partWord & 0xffff)] RGBA8 x6 (stride 4), stream2 = UvSet[part +0xa] float2 x6 (only if textured);
   morph 1 doubles the streams (cells cur+next: 4 or 5 buffers), morph 2 uses 4 cells (8 or 9). `FUN_005606a0(5)` sets VGT_PRIMITIVE_TYPE = **5 (triangle fan)**, `FUN_00560970(0,2)` index size, `FUN_00560710(ctx, 6, indexbuf = platform+0x123b8, 0)`
   = **DrawIndex, 6 indices per part** with a shared index buffer (contents not read: the 6-vertex fan topology and winding are unverified; likely quad corners + 2 extra vertices).
Note: mbs.md partWord at part +0xa/+0xc/+0xe = indices; in Execute the colour/pos index is the 32-bit word at part +0xc: low16 -> ColorSet index, high16 -> PosSet index; UV index from the u16 at +0xa.

### 3.2 X1 (FUN_0059f7b0) - textured quad batch
Fields: `+0x5c` blend mode 0/1/2, `+0x60` FTX bank, `+0x70` array of 0x48-byte records (count +0x78; each: 4 vertices pos/uv/colour, flag bit1 at +0x45 = skip), `+0x80` colour u32,
`+0x88` optional mask texture slot (same slot struct as sprite +0x70). State: no cull; blend by +0x5c: **0 normal alpha (4,0,5), 1 additive (4,0,1), 2 reverse-subtract (4,4,1)**, alpha eqs as table s4 (separate-alpha enabled for 1 and 2).
Shaders: with mask texture VS0+PS0, without VS1+PS1. VS cb slot0 = 4x4 MVP (0x40 B) = cmd matrix x layer matrix (+0x80/0xa0/0x90/0xb0); mask: PS tex slot1, VS slot1 uv rect.
Per record: colour multiply (cmd colour x record colour /255) with early skip if alpha 0; texture T# = bank `get(index byte at record+0x44)` (rebinding only when index changes), sampler 0x1500000 bilinear; builds a 0x88-byte
temp (4 verts: float2 pos, float4 colour, float2 uv at stride 0x20) in the ring pool; prim type 5 (fan); one draw per record. Used for HUD/particle-style quads (unverified feature).

### 3.3 X2 (FUN_005a0330) - generic primitive list
Fields: `+0x5c` primitive mode 0..7 (1->point list? see switch: 1->2 (line list), 2->3 (line strip), 3->4 (tri list), 4->6 (tri strip), 5->5 (fan), 6->quad list converted to 2 tris (0x13 / FUN_005a11a0), 0 -> 1 (point list)); `+0x60` blend 0/1/2 (as X1, always separate alpha);
`+0x64/+0x68` wrap modes (table 0x79f52c); `+0x6c` flags (1 bilinear, 2 cull front?, 4 depth write, 8 clamp?, 0x20 variant); `+0x70` vertex array (32 B per vertex: float3 pos, RGBA8 colour, float2 uv), `+0x78` count; virtual slot +0x28 returns the T# (0 = untextured);
`+0xc0` mask texture slot. Shaders: untextured VS15/16 + PS 52/57 (mask or not), textured VS17/19 + PS 53..57 (selected by flags 0x20 and mask), VS cb0 = 4x4 MVP (0x40 B) or 0x48 B block for the textured variant (16 floats + 2).
Vertex data is copied into the ring pool when not already in pool 0xe8e8a0 (FUN_0057ae00 test). Draws with FUN_00560710 (DrawIndex, count = vertex count). Generic "2D/3D primitive" path (debug lines, UI shapes, effect polys: unverified).

### 3.4 X3 (FUN_005a12e0) - viewport/scissor, no draw
Fields `+0x5c` mode, `+0x60..+0x6c` rect: mode 1 = absolute rect (clamped by FUN_00569110 to the layer), mode 2 = layer-relative rect transformed by the layer matrix (FUN_005a3770), other = layer's own viewport floats scaled by screen/base ratio.
Result -> `FUN_005a21d0 -> FUN_005612d0` = SetScissor (x0,y0,x1,y1) on the GNM context. Pure state: the native equivalent is a scissor change affecting later commands of the same bucket order.

### 3.5 X4 (FUN_005a15e0) - snapshot blit ("SnapTexture[FTX]")
Fields: `+0x60` resource owner, `+0x68` s16 index (entry stride 0x58 in `owner+0x160`; FUN_00594c70 = invalid-check, FUN_005a3450 = RT of that entry). Flow: barrier on the layer RT (FUN_0059ab40), switch the render target to the snapshot entry's RT (FUN_0059a160),
bind the **layer RT as texture** (T# at `layer+0x68`, point sampler), VS21 + PS58, prim 0x11 (rect list), `FUN_00560710` 1 rect, blend (4,0,5)/(1,0,1) alpha, then restore the layer RT. Captures the current layer image into a named texture slot (the string `SnapTexture[FTX]` is in FUN_0058a790's scene setup).

### 3.6 A (FUN_0007ddb0) and B (FUN_0007eb40)
Both: multiply the cmd matrix by layer matrices at layerBuf+0xc0..+0xff and +0xe0.. -> a screen rect via FUN_0007e020 (clip test, returns visible), then call **FUN_0007e330(c0,c1,c2,c3, layerBuf, rect4, alpha, srcTexture, flagA, flagB)** which draws one **screen-sized quad of four corner colours** (vertex: float3 pos, RGBA8 colour, float2 uv, stride 0x20; prim 6 = tri strip) with VS19/PS(tex) from the pool, texture T# bound at slot0 from `param_8`, blend (4,0,5) normal or (5,0,4) when flagged, constants 0x48 B.
A: tint uses a fixed colour (DAT_006e3fa0) and samples the **layer RT** (`lVar24+0x68`) after switching RT with FUN_0059a160 pair (a post-process style fullscreen tinted copy); B: reads colours/alpha from the cmd (`+0x70` alpha, `+0x74..+0x80` 4 floats) and samples a bank texture `FUN_00016b10()+0xd0 -> +0x160` (FUN_005a3450). Likely screen fades / full-screen overlays (unverified). Ctor constants: +0x68 = 720.0f.

## 4. Blend table (CONFIRMED, replaces the viewer guess)
`FUN_005aa470(out, mode)` builds a CB_BLEND0_CONTROL dword: FUN_00561970 init 0, FUN_00561980(en=1) bit30 enable, FUN_005619f0(1) bit29 separate-alpha, FUN_005619a0(c_src, c_comb, c_dst) -> bits 0-4/5-7/8-12,
FUN_005619c0(a_src, a_comb, a_dst) -> bits 16-20/21-23/24-28. Applied with FUN_0055ffb0(ctx, rt0, word). GCN enums: factors 0 ZERO,1 ONE,4 SRC_ALPHA,5 1-SRC_ALPHA,6 DST_ALPHA,7 1-DST_ALPHA; combiners 0 ADD, 4 REVERSE_SUBTRACT (dst*dstF - src*srcF).
Jump table at 0x79f810 (mode 6 reuses mode 0's code). With S = shader output, D = destination:
| mode (part byte +6) | colour equation | alpha equation | meaning |
|---|---|---|---|
| 0 | S.rgb*S.a + D.rgb*(1-S.a) | S.a*S.a + D.a*(1-S.a) | **normal alpha** (viewer guess correct) |
| 1 | S.rgb*S.a + D.rgb | same as 0 | **additive (alpha-weighted)** |
| 2 | D.rgb - S.rgb*S.a (rev-sub) | D.a*(1-S.a) - S.a*S.a | **subtractive / darken** (viewer guessed additive: wrong) |
| 3 | S.rgb*D.a + D.rgb*(1-D.a) | S.a*D.a + D.a*(1-D.a) | **destination-alpha normal** (paints where the target already has alpha; used inside offscreen groups). Viewer guess "reverse-subtract": wrong |
| 4 | S.rgb*D.a + D.rgb | same as 3 | destination-alpha additive |
| 5 | D.rgb - S.rgb*D.a (rev-sub) | D.a*(1-D.a) - S.a*D.a | destination-alpha subtractive |
| 6 | = mode 0 | = mode 0 | normal alpha; flag-like: `uVar68==6` also toggles the second-colour group / untextured handling (bit set 0x36 = modes 1,2,4,5 use the second colour group, mode 6 is skipped for the backdrop copy) |
Also: X1/X2/A/B blend mode (their own 0/1/2 field) = rows 0,1,2 above (A/B also have (5,0,4) = inverse-alpha variant). Sprite effect modes (+0x1f0 = 1..5) = composite with rows 1..5 above after drawing the group into an offscreen RT.
For a native renderer: rows 0-2 map 1:1 to GL/D3D (SRC_ALPHA, ONE_MINUS_SRC_ALPHA / ONE / REVERSE_SUBTRACT); rows 3-5 need destination alpha (RGBA target cleared to alpha 0, layer composited later) - in a first Stage-4 pass they can be approximated by rows 0-2 inside the same offscreen group.

## 5. Shaders
### 5.1 Pools
- Static initialiser FUN_0059e1c0 (registered atexit) fills 87 objects `{vptr, data ptr, kind}`; the loader `FUN_0059d870` (called from boot FUN_00598290) calls each object's vslot +0x10 to create the GPU program and stores it at object+0x18 / +0x28.
- **PS pool** `0xe97350`, 60 entries x 0x20 B (vptr 0xbc07c8, kind 5) accessed via FUN_0059d810; **VS pool** `0xe97ad0`, 25 entries x 0x30 B (vptr 0xbc0768, kind 1) via FUN_0059d830;
  2 **compute** shaders at 0xe97310 and 0xe97330 (vptr 0xbc0958, kind 6, role unread: probably colour-copy/downsample; unverified).
- Binary blobs: embedded GCN shader binaries in rodata **0x787da0..0x79f230** (compute 0x787da0, 0x788020; PS from 0x7881e0; VS from 0x79a480). Each blob has an 8-byte magic/hash header (`00 04 be 40`...) then a header with the shader type byte at +0xc (0 VS, 1 PS, 2 CS) and code size at +0x10.
  Per-blob table: `notes/raw/render/shaders.json` (address list, local only). They are PS4 ISA: not reusable; **must be re-authored** (HLSL/GLSL) from the role table below.
### 5.2 Role / selection summary (indices into the pools)
| use | VS | PS | inputs |
|---|---|---|---|
| sprite part (FUN_005a7260) | 2..12,14 via the 4x3 table at 0x79f854 (see 3.1), default 11 | 2..49 via the table at 0x79f890 indexed by the 6-bit permutation (tint/colour-matrix/factor/gradient/textured/mask); default 44 | pos float2, colour RGBA8, uv float2; cb as 3.1 |
| X1 quad batch | 0 (mask) / 1 | 0 / 1 | MVP cb |
| X2 primitive list | 15/16 untextured, 17/19 textured | 52/57 untextured, 53..57 textured | pos float3, colour, uv |
| overlay quad A/B (FUN_0007e330) | 19 (index [0x73-?]: unverified exact) | tex quad PS | pos, colour, uv |
| sprite group composite (FUN_005aa5b0) | 20 | 50 | pos float2, colour float4, uv; src = offscreen RT |
| snapshot blit X4 | 21 | 58 | - |
| copy-under / backdrop (FUN_005a9830) | 22 | 58 | 2 pos/uv streams |
| rect clear (FUN_005a9eb0) | 23 | 51 | rect + screen size cb |
| fullscreen rect-list pass (FUN_0059aa90, via FUN_0059a930) | 24 | 51 | none (rect list prim 0x11, VS generates corners) |
(Role names inferred from callers' state, shader code itself not disassembled: unverified.)
Distortion / bloom / post: **no distortion shader or blur chain was found in these classes**. The only full-screen passes are 0x11 rect-list draws: per-layer default FUN_0059a420 (clear via
FUN_0059a520 + FUN_0059a930 pass with PS51/VS24, writes a colour constant from DAT_00787b40) and the snapshot/copy passes above; the 2 compute shaders are the only other candidates (unread).

## 6. Render targets and defaults
- 2 full-screen colour targets created in boot (FUN_00598290 -> FUN_0059ac60 -> handles stored at platform+0x14a2c / +0x14a30), **1920x1080** (0x780 x 0x438), **3840x2160** (0xf00 x 0x870) when FUN_00563fe0==1 (IsNeoMode);
  RT format constant 0xf2e00a (8:8:8:8, unverified channel order/sRGB). Pool of 4 offscreen targets at platform+0x144e0 (stride 0x1c0, slot validity byte at +0x144e4) with the same creation routine; `layer+0x50` / `layer+0x58` = per-layer offscreen / main target pointers.
  RT struct: +0x28 RT descriptor words, +0x68 T# (to sample it), +0x70 packed size ((w-1) | (h-1)<<14), +0x88 depth descriptor. FUN_0059a160 = set RT (FUN_00638870) + depth + mask 0xffff + clear; FUN_0059ab40 = cache flush barrier.
- FTX-owned render targets (FUN_005a33b0, from FUN_00594f50 resource creation) exist for snapshot textures.
- Per-layer defaults FUN_0059a420: blend off for slot0 mask write, depth ALWAYS, `FUN_00560100(DAT_00787b74)` + constants, colour mask 0xffff, then FUN_0059a930 pass.

## 7. Layers
Layer table = `core+0x20`, element 0xb30 (FUN_0056f6a0(core,id), bounds-checked; 3 command-buffer copies of 0x2d0 B inside). Ids are **dynamic**: producers receive the id from their owner objects
(the sprite path = owner short +0x154 + frame byte +0x13, mbs.md); only FUN_00110b0 (ids 0x23, 0x2a), FUN_00016da0 (0xad, 0xae) and the scene function FUN_0058a790 (0,1,2, set up with FUN_00596ed0) use literals, so the
**semantic of ids cannot be derived statically** (candidates: 0-2 = world/scene cameras; 0xad/0xae and 0x23/0x2a in 0x10000..0x17000 = UI/menu code: unverified). Needs a runtime log of (layerId, class) per frame, or a call-site study of the owners.
Buckets inside a layer are free (second argument of FUN_00597830) and sorted by +0x54 key per layer sortMode (+0x2a4).

## 8. Function table
| addr | name | role | conf |
|---|---|---|---|
| 597830 | Layer_Push | bucketed push | high |
| 5a3a00 | Layer_Replay | per-layer state + sort + Execute loop | high |
| 5a3be0 / 5a4570 | Bucket_SortAsc / SortDesc | introsort on +0x54 then +0x50 | high |
| 57ae20 | Ring_Alloc | per-frame ring pool allocator | high |
| 56f6a0 | Core_GetLayer | layer table lookup (0xb30 stride) | high |
| 596ed0 | Layer_InitCamera | viewport + ortho matrices | med |
| 5a7260 | Sprite_Execute | see 3.1 | high |
| 59f7b0 | X1_Execute (quad batch) | 3.2 | med-high |
| 5a0330 | X2_Execute (primitive list) | 3.3 | med |
| 5a12e0 | X3_Execute (scissor) | 3.4 | high |
| 5a15e0 | X4_Execute (snapshot blit) | 3.5 | med |
| 7ddb0 / 7eb40 | OverlayA/B_Execute | 3.6 | med |
| 7e330 | Overlay_DrawQuad | 4-colour screen quad | med |
| 5aa470 | Blend_FromMode | CB_BLEND0 word from mode 0..6 | high |
| 5a6fb0 | Sprite_BindConstants | binds the per-part constant buffers (slots above) | high |
| 5aa2f0 | Sprite_SetTint | writes the 0x10-B tint constant (RGBA8 -> floats, de-duplicated) | high |
| 5a9eb0 / 5a9830 / 5aa5b0 | Sprite_GroupClear / BackdropCopy / Composite | offscreen group passes | med-high |
| 59a160 / 59ab40 | Rt_Set / Rt_Barrier | set target + cache flush | med |
| 59a420 / 59a520 / 59a930 / 59aa90 | Layer_DefaultState + clear + fullscreen rect | per-layer defaults | med |
| 59afa0 | Rt_GetOffscreen(handle) | 4-slot pool lookup | high |
| 59e1c0 / 59d870 | Shader_StaticInit / Shader_LoadAll | 87 embedded shaders | high |
| 59d830 / 59d810 | VsPool / PsPool accessors | 0xe97ad0 / 0xe97350 | high |
| 5606a0 / 560710 / 560970 / 564b30 / 5646d0 / 564030 / 5644b0 / 55e660 / 55e820 | GNM wrappers | SetPrim (reg 0x242), DrawIndex, SetIndexSize, SetVB, SetCB, SetTex, SetSampler, V#, CB | high |
| 561970..561a40, 561b00..561b50 | register builders | CB_BLEND/PA_SU_SC_MODE/DB_DEPTH_CONTROL words | high |

## 9. Open questions
- Index-buffer content at platform+0x123b8 (6-index pattern; prim 5 fan) - read the initialiser to confirm the sprite topology/winding (needed to draw parts correctly; the viewer already guesses 2 triangles).
- The 2 compute shaders (0xe97310/0xe97330) and who dispatches them; any bloom/blur/distortion path (none found in the 8 classes).
- Layer id semantics (needs runtime log or owner studies); who calls vtable slot [2] (depth) and the +0x54 key producers.
- Part blend 3-5 / effect modes 1-5 exact usage in game data (scan the .mbs corpus for part byte +6 and cmd effect mode origin in Layer_BuildDraw).
- Meaning of the sprite 6-bit permutation -> PS table entry for each of the 48 combos (shader semantics) and the exact colour-matrix presets.
