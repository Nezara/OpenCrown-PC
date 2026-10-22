# SceTouch on the 1.09 eboot: how the game reads touch, and why a tap in town opens the Town Map

Target: `Work/vita/update109.elf` (Thumb-2, text 0x81000000..). Addresses read with capstone (`Work/vita/dis_thumb.py`,
`Work/vita/calls.py`); 1.09 import stubs from the module's import table. Runtime checks: `mobile_run` on Windows with a
temporary (not committed) touch injector in `hle/touch.cpp`, save `Work/online_test/A` (Edwin, town).

**Answer to the Android report ("touching the screen opens the map"): this is the game's own Vita behaviour, not an HLE
bug.** In town, a front-panel touch moves the fairy pointer to the finger and the town menu object (`DcUi_TownMenu`)
handles that touch in its idle state by opening the Town Map, exactly like SELECT. With the map open, releasing the
finger over a destination warps there. The rear panel is read but never used. Nothing changed in the port.

## 1. Imports and call sites

| stub | NID | function | called from |
|---|---|---|---|
| 0x81504d6c | 0x1b9c5d14 | sceTouchSetSamplingState(port, 1) | 0x8107554c (init, ports 0 and 1) |
| 0x81504d7c | 0xb18370c2 | sceTouchEnableTouchForce(port) | 0x81075552 (init, both ports) |
| 0x81504d4c | 0x10a2ca25 | sceTouchGetPanelInfo(port, &info) | 0x8107555a (init, both ports) |
| 0x81504d5c | 0x169a1d58 | sceTouchRead(port, &data, 1) | 0x810755e4 (per frame, both ports) |

sceTouchPeek is not imported. Only these 4 SceTouch functions are used.

## 2. Touch manager (input manager + 0x7c8, 2 ports x 0xf0)

Input manager (per-frame update 0x8103e6f0..0x8103e746: pad update 0x8103dad2, then touch 0x810755c2, then touch regions
0x810756b2). Touch manager = input manager + 0x7c8, one 0xf0 block per port (0 front, 1 rear):

| port block offset | content |
|---|---|
| +0x00..+0x8f | SceTouchData of the last sceTouchRead (1 buffer) |
| +0x90..+0xbf | SceTouchPanelInfo |
| +0xc0 | touch record (0x24 bytes): u8 active, f32 nx, ny (normalised), f32 sx, sy (screen px), ?, ?, f32 force (+0x1c, normalised), s32 id (+0x20; -1 = none) |
| +0xe4/+0xe8/+0xec | 1/(maxDispX-minDispX), 1/(maxDispY-minDispY), 1/maxForce (from GetPanelInfo at init, 0x8107555e..0x810755aa) |

Init 0x810754cc. Per frame 0x810755c2: `sceTouchRead(port, block, 1)`; record cleared (active 0, force 0, id -1);
`n = reportNum (+0xc)`, **only the first report is used** (`n > 1` -> 1). From report 0 it reads: **x (+2), y (+4),
force (+1), id (+0)**. `nx = x * 1/(maxDispX-minDispX)`, `sx = nx * screen width` (render info `[0x813be24c()+0x40]+0x24`),
same for y with +0x28. **Not read: timestamp, status, report `info`/reserved, minAa*/maxAa*, minDisp*, minForce.**

Accessor `0x8103e67a(mgr, i)` returns the touch manager only for i == 0, so only the front panel and only the local
player whose pad port is 0 reach gameplay code. The rear record (port 1) has no reader (no other access to the
records was found): the rear panel is sampled but unused. Runtime: a rear tap in town does nothing (checked).

## 3. Touch regions -> virtual action 0x2a

List at input manager + 0x9b0, 0x30-byte entries {+0 port, +4 tracked id, +8 previous id, +0xc action slot,
+0x10..+0x1c rect x0,y0,x1,y1 in screen px, +0x20 held, +0x21 released edge, +0x22 pressed edge, +0x24 hold frames,
+0x28 release frames, +0x2c repeat counter}. Updater 0x810756b2: a report inside the rect whose id equals the tracked id
(or any id when none is tracked) keeps it held; no such report -> released, id -1. Add region 0x8107582a; its only
caller 0x81075c24 (from the input manager ctor 0x8103e668) registers **one region: port 0, whole screen (0,0)-(W,H),
action slot 0x2a**. So action 0x2a = "front screen touched anywhere".

Pad actions (0x2b slots, 0x28 bytes at pad + 0xa8, ctor 0x8103d1d0): each has up to 3 physical buttons (+0/+4/+8,
0x1a = none) or a region pointer (+0x24). Physical button id = SceCtrl bit index (0 SELECT, 3 START, 4..7 d-pad,
8 L, 9 R, 0xc..0xf triangle/circle/cross/square, 0x12..0x19 stick directions). Queries: held 0x8103d3a2, released
0x8103d470, pressed 0x8103d512, repeat 0x8103d5a2 (region: +0x20 / +0x21 / +0x22 / +0x2c > 0), pressed-once
wrapper 0x8103e40a. Defaults 0x81073ed0 (excerpt): **action 0x24 = phys 0 (SELECT)**, action 9 = SELECT,
actions 8 / 0x23 = START, 0x29 = L.

Users of action 0x2a: text advance (pressed 0 / 6 / 1 / 0x29 / 0x2a: 0x810533ae, 0x81340ea6, 0x81377ba0), 0x813254f6,
and the fairy pointer (held 0x812a1308 / 0x812a137c, repeat 0x812a17fe; 0x812a221e skips the stick-cursor update while
the screen is touched).

## 4. Fairy pointer (touch path)

0x812a05d4: if the front record is active, pointer position (+0x9e8/+0x9ec) = touch screen pos - screen/2 (absolute,
centred), +0x9dc = 1 (touch-driven). The pointer update 0x812a12a0.. reads held(0x2a) as the "pointer pressed" flag
and sends the pointer state (incl. that flag) through the object's network command (vt+0xa4 at 0x812a181a). The
pointer appears where the finger is (seen in every run).

## 5. Town Map

`DcUi_TownMenu` (name 0x815c9bdc, tag 'TOMN', vtable 0x8176bc0c, ctor 0x8135e2f4, UI factory type 0xf via 0x81379ada,
accessor 0x81379ae4 = ui+0x144) is created by 0x812ceab8 whenever the local player controller is in round 9
(Town00; `+0xcc` = RoundNameToIndex 0x81043086) and none exists: it lives for the whole town visit. State dispatcher
0x8135ed52 (`this+0x80`): state 4 = idle (map closed) 0x8135e83c, state 2 = map open 0x8135eaea.

- Idle (0x8135e83c): for each local player with a pad, **opens the map when pressed-once(action 0x24 = SELECT)
  (0x8135e8ec) or when the touch flag `this+0x609` is set** (0x8135e8f4).
- Touch callback, vtable slot 8 = 0x8135de36(this, i): **state 4 -> `this+0x609+i = 1` (open the map)**; state 2 ->
  hotspot `this+0x18` 0..7 -> `this+0x600+k` (warp target; 0x654 = k), 0xff -> `+0x608`. Slot 9 = 0x8135e00a: same
  state-4 open, gated on a hero flag (+0x2df0).

So the game itself turns a front touch in town into "open the Town Map", and with the map open a touch on a
destination into a warp. Where the pointer press is dispatched to slot 8 was not pinned down (it goes through the
pointer's network command); the runtime result below shows any front-screen tap in town reaches it.

Runtime (Windows, save A, standing at the inn door; touch = one point, id 0, force 128):

| input | result |
|---|---|
| tap 6 flips at the centre (960,544) | pointer at the finger, Town Map open ~20 flips after the touch-down, stays open after release |
| hold 250 flips at the centre | map opens while held (~20 flips); on release the finger is over the Adventurers Guild marker -> warps into the Guild |
| tap 6 flips on the inn door (260,440) | map opens (the door is not entered) |
| rear tap 6 flips | nothing |

## 6. Our HLE vs a real Vita

Nothing the game reads is wrong, so no HLE change:
- Panel info front 0..1919 x 0..1087 (disp = active area), maxForce 128: the game only uses maxDisp-minDisp and
  maxForce, values match the hardware.
- Only report 0's x, y, id, force and reportNum are read; status / info / timestamp are ignored (we write timestamp,
  zero the rest).
- Our ids are the index in the current list (0 for one finger) where the hardware gives an id per new touch. The
  region and pointer code only compare ids while a touch is held and reset to -1 on release, so a new touch with the
  same id 0 behaves like a new id; no difference for one finger. (With two fingers, lifting the first renumbers the
  second to 0; only report 0 is used anyway.)
- `touch_controls.cpp` front_touches(): with the on-screen controls off every finger is a front touch; the letterbox
  mapping to 1920x1088 units is correct (pointer lands under the finger).

If players with a controller should not open the map by touching the screen, that is a design change, not a fix
(e.g. an option to ignore screen touches when the on-screen controls are hidden). The Vita behaviour is kept.
