# Stage 4 renderer, step 1: capture + GL replay (`src/render`)

`dcp_run ... --render | --window | --shot out.png | --shots N=a.png,M=b.png` draws the game with OpenGL 3.3 (SDL3).
`dcp_viewer` shares `gl.*` and `png.*`.

## How it works
- **capture.cpp** (linked into `dcp_hle`, game threads) puts 5-byte `jmp` hooks on 19 eboot functions
  (render_cmds.md s3 and s8). Each relay calls an observer with the original argument registers, runs the displaced
  prologue and jumps back, so the game's own code still runs unchanged. The hooked functions are:
  - the 7 command-class `Execute` bodies plus `Layer_Replay`. These only tag the current thread with the command
    class (sprite, X1..X4, A, B).
  - the GNM-wrapper setters: VS/PS program (mapped to an index in the game's VS/PS pools), constant buffers, T#/S#,
    vertex buffers, blend, primitive type, index size, screen scissor and render target.
  The `sceGnmDrawIndex*` shim snapshots that state, copies the referenced vertex, index and constant bytes into a
  `DrawRec`, and overwrites the draw packet with a marker NOP. Markers are collected in submit order when the DCB is
  submitted. A flip closes the frame.
- **backend.cpp** (GL thread) replays each frame. Every draw is decoded on the CPU into one vertex format (pos,
  colour, uv) and expanded to triangles. One re-authored program stands in for the game's shader permutations.
  - Each colour target address gets its own FBO. Rows are stored top first: clip y is negated.
  - Textures come from the T#: game memory is detiled with our `formats/gnm_detile` (BC7 tile 13, the same reader as
    `dcp_tex`) and uploaded once per T#. Invalidation: `texwatch` write-protects the uploaded pages; a game write faults
    once (vectored handler logs the page, restores access) and the file shim calls `host_write()` before ReadFile, so a
    rewritten texture is re-uploaded in place. Memory that is not plain RW falls back to a 512-byte head hash.
- Display: the target named by the flip's buffer index (`sceVideoOutRegisterBuffers`), otherwise the busiest target.

## Confirmed (asm or captured data)
- Sprite constant slots (FUN_005a6fb0): VS cb0 is 0x40, 0x44 or 0x50 B depending on the VS index (bitmasks 0x5840,
  0x780 and 0x3c, i.e. morph 0/1/2); cb5 is 8 B uv scale (textured); cb6 is the mask rect; cb7 is the colour.
  PS cb1 is the tint (RGBA8/255, FUN_005aa2f0), cb2 the colour matrix 0x30 B, cb3 the factor 4 B, cb4 the gradient
  0x100 B.
- PS table 0x79f890 (perm -> PS) and VS table 0x79f854 were read from the eboot at runtime (no game bytes in the repo).
- S# word0 bit 15 (force_unnormalized) is honoured: uv / texture size. X1 glyph quads use it (S# 00008000 00fff000
  01500000 0, uv in texels over the 4096x4096 BC7 font atlas); without it all text was noise.
- Matrices in VS cb0 are row-major with the translation in elements 3/7. Captured: x/640 - 1/960 on a 1280x720 base.

## Pixel shaders (confirmed from the game's own GCN binaries)
- Sprite PS (perm bits -> 0x79f890): colour = vertex colour (RGBA8/255 x VS cb7) x texture (0x10/0x20) x mask texture
  (0x20, second texture at the mask uv); then, in this order: **factor** (4) `c += f * (avg(rgb) - c)` (desaturate),
  **colour matrix** (2) `c.r = dot(row0.xyz, c) * row0.w` etc. (3 rows of 4 floats), **gradient** (8) two 32-float
  tone-curve stages `t1 = sat((x-lo1)/s1), t2 = sat((x-lo2)/s2), k = base + w1 t1 + w2 t2, x += (k - x) * mix * amount`
  per channel (s = 0 handled as GCN does: rcp = inf, NaN clamps to 0), **tint** (1) `c.rgb = lerp(c.rgb, tint.rgb,
  tint.a)` (a flash colour, not a multiply). Alpha is only touched by the texture / mask multiply.
- Sprite VS: uv = uv / cb5 ("RATE"), mask uv = sat((clip.xy - MASKXYWH.xy) / MASKXYWH.zw) on the clip position before
  the divide. VS rows "untextured + mask" (6, 7, 2) put the mask uv in the only uv (the mask is texture slot 0).
  Morph 1 = lerp(cell0, cell1, cb0[16]) for position and colour; morph 2 = sum of w[k] = cb0[16..19] (colour clamped).
- X1: VS0/PS0 = texture x mask texture (VS cb1 mask rect, unclamped), VS1/PS1 = texture x colour.
- X2: textured VS17 (mask) / VS19, PS53/55 = rgb from texture, alpha from vertex (flag 0x20), PS54/57 = rgba; untextured
  VS15 (mask in slot 0) / VS16. S# (asm 5a08c5..): clamp x/y from table 0x79f52c by +0x64/+0x68 (0 clamp, 1 wrap, 2
  mirror), flag 1 bilinear, flag 8 normalized uv (else force_unnormalized, uv in texels). Applied from the captured S#.
- PS50 (group composite) rgb from the offscreen RT, alpha from the vertex; PS51 constant; PS58 rgb with alpha 1.
- VS20 / VS22 pass position through as clip xy (the VS cb0 / cb5 / cb7 still bound are stale; VS22 stream 1 is uv), VS21 / VS24 are vertex-id rects (bound streams are stale and ignored),
  VS23 maps pixels: (2 sat(x/W) - 1, 1 - 2 sat(y/H)) with cb0 = (W, H).
- Overlay A/B (FUN_0007e330): VS19 + PS55 (rgb only) or PS57, point sampler with force_unnormalized.

## Guesses / not yet seen in game
- Render-target size from descriptor dword 18 (= RT struct +0x70 packed (w-1)|(h-1)<<14 per render_cmds.md s6; every
  target so far is 1920x1080, not checked against asm). GCN clamp modes 3-7 (mirror-once / border) map to GL
  approximations; the game's tables only use 0-2.
- Destination-alpha blend rows 3-5 and sprite effect modes 2-5: decoded from the CB_BLEND word and drawn with the GL
  equivalent (DST_ALPHA factors, reverse-subtract); targets get their alpha from the game's own clears (PS51 rect / layer
  pass). Seen in game: composite modes 0 and 1 only (opening, town). Rows 3-5, matrix, factor, X4 and A/B were not
  reached by the scripted runs (first stage, ~2 min of combat): untested.
- Not compared with shadPS4 on a matching frame (no oracle capture of the same frames).

## Debug
`DCP_RENDER_DUMP=<vblank>` prints every draw of the first frame at or after that vblank.
`DCP_RENDER_PICK=x,y` (with DUMP) lists the draws whose bbox covers that pixel (top-left origin).
`DCP_RENDER_TEXDUMP=<hex T# base>[,out.png]` writes that texture, decoded, once. The dump also prints the first two
vertices of every stream.
`DCP_RENDER_NO_TRANSPOSE` restores the old matrix upload, for checks.
`DCP_RENDER_AB=<uFlags mask>,<vblank>,<out.png>[,<ps>]` replays the first frame at/after vblank (that has a draw with
that PS) twice: with those shader features off -> out.png, then normally -> out_on.png (same-frame before/after).
Every first (class, VS, PS, blend) combination is logged with its vblank (`render: first ...`): a coverage census.
Input scripts accept `LS_LEFT/LS_RIGHT/LS_UP/LS_DOWN` (left stick at an edge), e.g. to walk the town into a stage.
