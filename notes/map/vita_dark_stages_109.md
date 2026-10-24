# Vita 1.09: black layers in Ghost Ship Cove B / Kraken / lava stages, and the stuck black screen

Target `Work/vita/update109.elf` (1.09, Thumb-2). Tooling: `notes/raw/vita_net_109/v9.py` (1.09 import stubs, `fn`, `r`,
`callers`, `xr`). Tags: **(asm)** read in the disassembly, **(run)** seen in a `mobile_run` trace, **(inferred)**,
**(unverified)**. Our code: `android-port/src/hle/gxm.cpp`, `android-port/src/gpu/vulkan.cpp`.

## 0. Summary
- **Root cause of the missing layers (asm + code):** the game has **no hardware scissor on Vita. It builds a scissor
  rectangle in the stencil buffer** (f813bc350): a full-screen quad drawn with stencil func **NEVER**, fail op REPLACE,
  ref 1 (writes stencil 1 everywhere, **no colour**), then the window rectangle the same way with ref 0, then every later draw
  runs with stencil **EQUAL ref 0**. These are the only two `sceGxmSetFrontStencilFunc` calls in the game.
  **We ignore stencil**, so those two "stencil-only" quads are drawn as ordinary colour draws. Their program is the game's
  clear program (no blend, colour uniform = 0,0,0,0), so **each scissor change paints the whole target black (0,0,0,0)**.
  Everything drawn earlier in that scene disappears and everything drawn later stays.
- The scissor cache is reset every frame, so **every frame** has one such setup at the first layer (harmless: nothing
  is drawn yet). This was confirmed in a Windows run. The bug shows only when the rectangle **changes mid-frame**. That
  happens with X3 "window" commands, which are clip windows (mode 2) pushed around an actor's draw, then RESET. The base
  actor draw (f811ab17e) does this for actors that have a surface object (+0x3a8, inferred: a water / lava surface, which
  clips the actor at the waterline). So it happens in **water and lava areas** (Kraken caves, the Kraken fight with its
  tentacles, the lava stage). It is **intermittent** because it only happens while such an actor is on screen. That
  depends on what is in play, not on any renderer state.
- `DCP_LOG_GPU=1` cannot show this: the draws are valid, so nothing gets logged. That matches "no warnings in log.txt".
- **Fix:** make draws with stencil func NEVER write no colour. Do it at once (small and safe), together with a correct
  default stencil state. Then emulate the scissor, either as a Vulkan scissor or with a real stencil attachment (s4).
- **The new "whole screen black, Start ignored, a constant 718 draws" report is a different problem** (s5). The game is
  stuck in a state it is waiting in. The black is most likely the game's own fade or transition overlay. The flip path
  cannot produce it: every flip base is exactly one of our two display targets (asm).

## 1. Render targets, scenes and flips (asm)
- Display buffers (f813b6528): two colour surfaces `core+0x56a0 + i*0x30` (core = 0x81bb60f8, from f813be42c).
  Format 0 (A8B8G8R8), LINEAR, **960x544, stride 1024 px**. data = `[core+0x5690+i*4]` (ColorSurfaceInit 813b6b54).
  Depth/stencil: `core+0x5750`, **S8D24** (0x01266000), TILED (0x11000), stride 960 (813b6c2e). So the stencil test is
  real on hardware.
  Each display buffer also has a texture `core+0x5708+i*16`: TextureInitLinear (813b6ba6), U8U8U8U8_ABGR, **1024x1024**,
  same base address. It is used as the backdrop in the sprite offscreen groups.
- Frame (f813f23ca, render thread): scissor cache `X+0x78..+0x84 = 0` (813f2408, X = `[[0x8183c604]+0x40]`), then
  BeginScene on the current display buffer (f813b725e, flags 0, depth `core+0x5750`), then the layer loop f813f5ba0.
  Layer flags +0x194: bit0 = viewport + scissor from +0x184..+0x190; bit3 = clear f813b74ba with the colour at +0x180.
  Then f813b7344: EndScene, sceCommonDialogUpdate (f813b72aa), AddEntry with data = `[core+0x5690+idx*4]`, idx flips 0/1.
- Flip (display callback f813b7aa2): `sceDisplaySetFrameBuf({0x18, base = *data, pitch 0x400, fmt 0, 960, 544}, 1)`.
  **The base is always one of the two display colour surface addresses; the size is always 960x544. No lower-resolution
  or offset buffer is ever flipped** (asm). Every frame begins a scene on that address, so in `present()` the base is
  always a target in `shown_` once a frame has been flushed. The guest-memory fallback should never be the source in a
  stage.
- Offscreen targets (f813bc9aa, kind 1): colour surface A8B8G8R8 LINEAR w x h. Stride = next power of two of w, plus
  a linear texture (pow2 w x pow2 h, same base). Users: the sprite effect groups (f813f3806 at 813f3e8e / 813f4fb8, via
  the scene switch f813b7684, flags 1, same depth/stencil surface) and the X4 snapshot blit (f813f57c8). Both are sampled
  with the base address equal to the surface data, the layout `target_texture()` supports. **No texture at an offset
  inside a surface, no swizzled or tiled render texture** (asm on these creators). These are not the cause. The device
  data agree ("9 + 0 rt").
- Depth: sprites run with depth write off and func ALWAYS (813f3d0e / 813f3d18). X2 primitive lists switch to LESS_EQUAL
  when their flag 4 is set (813f5172). That is a minor separate gap, not related here. Cull and polygon modes are set by
  X2 and ignored by us (minor).

## 2. The stencil scissor, f813bc350(X, x, y, w, h) (asm)
```
if (X.cache == (x,y,w,h)) return;            // 813bc374..813bc398, cache at X+0x78..+0x84
X.cache = (x,y,w,h); save viewport; f813b73fa(core, 0,0,960,544)       // full-screen viewport
SetFrontStencilFunc(ctx, NEVER, fail=REPLACE, zfail=KEEP, pass=KEEP, cmask=0, wmask=1)   // 813bc402
programs: vertex = table[1] (GXP 0x8183f0dc), fragment = table[0] (GXP 0x8183eff8, CreateFragmentProgram blend=NULL, 813ba51c)
fragment uniform = RGBA8 {0,0,0,0} -> floats (f813b9768)
SetFrontStencilRef(1); draw TRIANGLE_FAN 4: (-4,1)(1,1)(1,-4)(-4,-4) in clip space = whole target  // 813bc49c
SetFrontStencilRef(0); draw TRIANGLE_FAN 4: the rect (x..x+w, y..y+h) in clip space               // 813bc55c
SetFrontStencilFunc(ctx, EQUAL, KEEP, KEEP, KEEP, cmask=1, wmask=1)                                  // 813bc572
restore viewport (f813b73fa)
```
- Clear fragment program 0x8183eff8 (hash 8975924ce5511572): `vpck.f16.f32 pa0 = sa0..` = output the colour uniform,
  **no SOP2**. Vertex 0x8183f0dc (bee8e8358c844767): `o0.xy = pa0 * c1.y` (asm via `usse_render.py`). The same pair
  draws the real layer clears (f813b74ba, a 3-vertex triangle with the layer's clear colour). On hardware the NEVER func
  stops any colour write; in our renderer it becomes an opaque write (vulkan.cpp `pipeline()`: no shader blend and no
  blend unit, so blendEnable is off and the write mask is 0xf).
- Callers of f813bc350: the layer replay f813f5ba0 (layer viewport, flag bit0), the layer clear f813b74ba (sets full
  screen, then restores through f813bc5ce), the X3 "window" command Execute f813bf44a (vtable 0x81771a44+8, slot 4;
  debug strings `RESET` / `SCREEN` / `BEFORE WINDOW` at 0x815cc230..). f813bc5ce re-applies the cache after a scene
  switch, which is a no-op because of the cache check.
- On hardware, BeginScene starts each scene with stencil = background (0) because the surface is not force-loaded
  (inferred from the GXM rules). So after any scene switch the scissor is effectively full-screen again.
- **Run (Windows, `mobile_run` headless, title/boot, `--trace-only sceGxmSetFrontStencil,sceDisplaySetFrameBuf`):**
  2434 flips with exactly 4 stencil calls (NEVER func, ref 1, ref 0, EQUAL func), 19 with 8 (two setups), 19 with 0.
  Every call came from 813bc406 / 813bc456 / 813bc4a8 / 813bc576. So the per-frame setup is real, and the extra setups
  are the mid-frame changes.

## 3. Who changes the rectangle mid-frame (asm, purpose inferred)
- X3 command (ctor f813bf1c4, size 0x80, ring 0x81cdd80c): mode 0 = RESET to the layer viewport, mode 1 = absolute
  rect (f813bf7b2, **no callers**), mode 2 = layer-relative rect (f813bf7c8). All windows in 1.09 are mode 2, set by:
  f8104b870 (3x), f8105f856, **f811ab17e** (base actor draw, 3 windows into buckets 3/4/...), f812e217a (one actor
  class, vtable 0x817585b0, ctor f812e0e5e), f8133857e, f81366b5c (a method in many vtables), f813eb134.
  The matching RESETs come from f811abf0a (buckets 6/3/4) and the producers themselves.
- f811ab17e pushes its window only when `[actor+0x3a8]` is set. Its +0x68 virtual returns a level (sentinel 10000000).
  It then clips the actor's draw to the area above that level (inferred: a water or lava surface; the screenshot shows
  ripple rings at the characters' feet). Each window plus its RESET is two rectangle changes, so it wipes the target
  twice in our renderer.
- That explains both screenshots. Kraken caves: the last change comes after the characters and before the foreground
  rocks, light glows and HUD, so only those survive. Lava stage: it comes after the far background and before the rest.
  1.09 also has `pALavaHand` / `AI_LavaHand` actors next to the Kraken names (0x815a85f0..), which is the lava case.
- Not explained by this: no code change between 10-21 and 10-22 affects these draws, so the "caves fixed" report is
  play variance (nothing in water or lava on screen), not a fix.

## 4. Fix (do not apply here; for the implementer)
1. **gxm.cpp:** start the context with the GXM defaults: front stencil func **ALWAYS (0x0E000000)**, ops KEEP, masks
   0xff. Today `gpu::StencilState{}` has func 0, which is **NEVER** in the GXM encoding. The real GXM default is
   unverified: Vita3K's state header is not in Work/. In practice the game sets the func before its first scene draw
   (run, s2).
2. **Immediate fix, vulkan.cpp `draw()` (or gxm.cpp `Draw`):** drop draws with `front_stencil.func == 0` (NEVER). On
   hardware they never write colour, and they are only ever the two stencil quads (asm: the only SetFrontStencilFunc
   callers). This removes all the black wipes. What is left is that windowed actors are not clipped, e.g. legs show below
   the waterline. That is cosmetic.
3. **Correct clipping, choose one:**
   a. *Scissor emulation* (small, game-specific): on a NEVER + REPLACE draw, compute the screen bounding box of its 4
      vertices (stream 0 is float2 clip space, viewport is full screen; f813bc350 sets that) and remember
      `rect[value] = bbox`. On a draw with func EQUAL, apply `vkCmdSetScissor(rect[ref & cmask])`. On ALWAYS, and at
      `begin_scene` / `begin_pass` (the stencil is cleared per scene), use the full target. Only the game's rect pattern
      is supported.
   b. *Real stencil* (general): one D24S8 attachment per target (Mali-G52 supports D24_UNORM_S8_UINT as an attachment).
      Stencil loadOp CLEAR(0) in `begin_scene` and a LOAD variant for `begin_pass` after `target_texture()` breaks the
      pass. The stencil state (func, 3 ops, masks) goes into the `pipelines_` key **and into the `memo_` check in
      `draw()`**, otherwise one pipeline is reused across stencil states. Reference via
      VK_DYNAMIC_STATE_STENCIL_REFERENCE. Back faces use the front state: the game never imports SetTwoSidedEnable or
      the back stencil calls.
4. **Check on Windows:** `--trace-only sceGxmSetFrontStencil,sceDisplaySetFrameBuf` and count the calls between flips.
   More than 4 means a mid-frame rectangle change, which wipes the frame before the fix. With fix 2 those frames must
   keep their background. A Ghost Ship Cove B run is practical with `--window` and keyboard play from a save that has
   both routes, such as `Work/vita/save_cmp/ps4_real.DAT` (Edwin LV 51, talisman phase; loads on 1.09). An `--input`
   script that walks town -> stables -> stage -> route-B door is possible, but fragile: timing drifts by a few hundred
   flips per run.

## 5. Whole screen black, Start ignored, a constant 718 draws with 9 textures (coordinator data, 2026-10-24)
- **Not the flip path** (s1, asm): the base is always a display target address of fixed size, so `present()` always finds
  it. If it showed guest memory instead, the HUD drawn by our renderer would not be visible either way, but the
  scene count says the frame is rendered.
- **Not the s2 wipe alone:** a wipe after the HUD can black out the whole frame. It cannot stop Start from opening the
  pause menu. A constant 718 draws for minutes, with no reaction to input, means the game logic is in a state that
  draws an unchanging scene and ignores Start (inferred).
- 718 draws with 9 textures (1 CPU-written, e.g. the font, + 8 BC art atlases) fits a normal stage frame: a background
  atlas, the 4 characters, HUD and effects, one draw per sprite part. A static picture with a full-screen black overlay on
  top fits too. It does not fit an empty loading screen. **Most likely the game is in a room or stage transition with
  its fade at full black, waiting for something that never completes** (inferred). Candidates:
  (1) a resource load: the resource manager `[0x8183c604]+0x44`, requests 0x813d7398 / find 0x813d718a, nodes in
  state 2 = pending, a file read that never completes or fails without an error the game handles;
  (2) **online play**: the room-transition sync between players (SETUP_SCENE / AC exchange, still unchecked, see the
  README online section) waiting on a peer;
  (3) a guest thread blocked on one of our sync objects.
  **First question for the reporter: was it online, and which transition (end of a room, boss door, route-B door)?**
- **Data that would settle it** (Android, release APK). (a) `adb shell setprop debug.opencrown.env "DCP_GUEST_PROF=1
  DCP_IMPORT_CALLERS=sceIoRead,sceIoOpen,sceKernelWaitSema,sceKernelWaitEventFlag,sceKernelWaitEventFlagCB"`, then
  `log.txt`: which guest thread is waiting, where, and on which import or LR. (b) The `[timing]` per-thread CPU lines:
  a main thread idle in a wait points to (1) or (3); a busy loop points to a poll on a network or state flag. (c) Any
  `[io]` open failure for a `GC_1_st*` / `MR_*` / `KRAKEN*` file in log.txt, which would be a missing or renamed file in
  the user's dump. On Windows the same state can be reproduced with `--trace-only sceIo,sceKernelWait` once the stage is
  reached.

## 6. Open
- The class behind f812e217a and the exact meaning of `actor+0x3a8` (water or lava surface: inferred).
- Which producer runs last in the fully black frames, if any: needs the per-flip count from s4.4 on the device.
- The cause of the stuck state (s5): needs the runtime data listed there.
