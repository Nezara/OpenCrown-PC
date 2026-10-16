# Fairy / pointer cursor: touchpad and stick -> screen position

Rating: careful (all formulas read from asm; the aim-assist "magnet" state machine and the scroll compensation are only skimmed).

## 1. Summary

The cursor object is the DcAI_SysObj_Cursor-family task whose per-frame update is FUN_0039ad80 (rdi = cursor, vfunc +0x1c8 = player type id, 11..14 = players 0..3).
Each frame it picks one of two input paths:

- touch record of that player has its active flag (record+0) set -> FUN_0039a1e0 (touchpad path);
- otherwise -> FUN_00399d00 (stick path). Both only produce a *request* (cursor+0xabc/0xac0 absolute target, or cursor+0xac4/0xac8 per-frame delta);
  FUN_0039a610 then applies it (position += delta) when cursor+0xb1c != 0, and 39ad80 clamps the result.

The cursor position is stored as two floats in the cursor object: **+0xabc = x, +0xac0 = y, in screen pixels of the 1920x1080 base screen, origin at the screen CENTRE** (clamp is +-0.5*W, +-0.5*H, see 3).
Touch Y grows downward, and the absolute modes keep that sign, so +y is down (the stick path also adds stick-y directly, unverified sign of the stick axis).

Touch is NOT always absolute. A per-player option (4 modes + off) selects:

| mode (option nibble) | meaning | formula (p = touch_screen_pos - screen/2, i.e. centred touch in 1920x1080 px) |
|---|---|---|
| 4 | touchpad cursor disabled | function returns, no update |
| 0 | RELATIVE swipe | delta = touch delta (screen px) * k, k = 0.2 * sens (sens 1..10, default 3 -> 0.6; sens 5 -> 1.0, sens 10 -> 2.0). Added to the position (after a scroll compensation and aim-assist step). |
| 1 | ABSOLUTE full pad | cursor.x = 1.5 * p.x ; cursor.y = 1.5 * p.y - 0.1 * H (const 0x74d26c = 1.5, 0x74d270 = -0.5 and 0x74d274 = 0.2 multiply H) |
| 2 | absolute, LEFT half only | only applied if p.x < 0: cursor.x = 2.5 * p.x + 0.65 * W ; cursor.y = 2.0 * p.y - 0.1 * H (consts 0x74d130 = (2.5, 2.0), 0x74d140 = (1.3, 0.2) times screen/2) |
| 3 | absolute, RIGHT half only | only applied if p.x > 0: cursor.x = 2.5 * p.x - 0.65 * W ; cursor.y = same as mode 2 |

(Finger lifted / record flag 0 -> falls to the stick path; modes 1-3 do not "stick" the cursor, it simply stops being driven.)
The absolute modes therefore have gain 1.5 / 2.5 and an offset, so a window-pos -> touch-pos mapping with gain 1 can never match: this is the cause of the reported offset that grows with distance. Mode 0 has gain 0.2*sens (default 0.6): same cause for the relative mapping.

## 2. Answers

### 2.1 Absolute or delta? Exact data path

Raw -> record (FUN_005a2970 at 5a2a58..5a2b6e, then FUN_00070a60 at 70a60):
- pad manager block stride 0x1d0 (engine+0x68 + player*0x1d0): +4/+8 f32 touch0 (x,y) = raw_u16 * (DAT_0079f5c8 = 1.0 / resX) and raw_y * (1.0 / resY), resX/resY = u16 at ControllerInformation+4/+6 (read every frame via scePadGetControllerInformation). So the normalised value is raw/res, **range [0,1) with the reported resolution (we report 1920x943, raw y 0..942 -> up to 0.9989)**. Second point at +0xc/+0x10.
- +0x14/+0x18 delta of point 0 = norm_now - norm_prev, **only if touch count now != 0 AND count previous tick != 0** (bytes +0x24 / +0x25). Otherwise the delta is the sentinel at 0xe93850 (bss, never written by any code I found -> (0,0) at runtime, unverified). There is NO finger-id check, so a finger swap or a teleporting virtual finger with count staying 1 produces a full-size jump delta. First frame of a new touch (count 0 -> 1) gives delta 0.
- FUN_00070a60 (called per player from FUN_00045530) builds the 0x34-B record: +0 active flag (count != 0 for point 0, 1 for each point < count), +4/+8 copy of norm x,y, **+0xc/+0x10 = norm * (engine+0x40)+0x3c / +0x40 = screen W / H** (1920x1080 base per notes/map/render_cmds.md), +0x14/+0x18 = pad delta * W / H, +0x20 point index. Two points: second at record +0x24.. stride 0x24 (record [0x20] first point index, loop writes up to count points).
- The mode functions use only point 0 (record+0xc, +0x14).

Mode 0 (39a3b9..39a4b6 and 39a59c..39a5f5): sets cursor+0xb1c=1 (request flag), +0xab0=1, writes delta to +0xac4/+0xac8 (|delta| to +0xad8), then multiplies it by k: sens = FUN_0008c260(options for the player), c = min(sens,10); if sens <= 0 k = 0.2 (0x74d27c) else k = (c-5)*0.2 + 1.0 (0x74d274, 0x74d278) = 0.2*c. Sens is the nibble bits 4..7 of the u16 at option_entry+2, value 0 -> 3 (FUN_0008c260), so the default k = 0.6.
Then FUN_0039a610 (state at +0xae8, default 0 = free) does, for free state: optional aim-assist snap (see 2.4), a scroll compensation (subtracts camera-scroll-derived amount from +0xac4/+0xac8 when the controlled actor +0x1bb is set; uses owner(+0xc0)+0x80/+0x84 and FUN_000148f0, constant 812.0 at 0x74d2ac), then at 39ac42: **x += ac4, y += ac8** (no smoothing/lerp in the free state, no acceleration curve, no per-frame delta cap, no dead zone for touch).
The ae8 = 1/2/3 states are aim-assist magnetism toward a target (target pos at +0xaec, id +0xaf4): they blend toward the target position (via FUN_005714e0, camera-space transform) and are only entered when the target exists and finger speed is low (thresholds 0x74d28c = 0.5, 0x74d298 = 20, 0x74d29c = 30 px, constants around 39a960-39ac27). A port that reads the cursor position back (closed loop) can ignore them, but they will pull the cursor off a purely mouse-driven target near enemies/items.

Modes 1-3: written directly to +0xabc/+0xac0 (39a573..39a59a), +0xab0 = 1, +0xb1c = 1, no smoothing; FUN_0039a610 then sees ac4/ac8 unchanged from the last frame (they are not cleared by this path, unverified effect) and `abc += ac4` still happens in the free state only if the touch path left ac4 non-zero; in practice for modes 1-3 verify by reading back.

### 2.2 Resolution constants

resX and resY from GetControllerInformation ARE used (1/resX, 1/resY at 5a2a58..5a2aa4, DAT_0079f5c8 = 1.0f is the numerator). Not fixed constants. With 1920x943 and raw y 0..942, y_norm max = 942/943. Consistent mouse mapping: raw_x = mx/W_client*1920, raw_y = my/H_client*943 (norm then covers 0..1 almost exactly, and the screen mapping uses norm*1080 for y).

### 2.3 Where the position lives

- Cursor object: base class with vtable; offsets: +0xabc f32 x, +0xac0 f32 y (centred 1920x1080 px, clamp +-960 / +-540: 39b19d..39b220, 0x74d2b0 = 0.5 times screen W/H); previous-frame copy +0xab4/+0xab8 (written at 39b12a before updating); +0xac4/+0xac8 f32 per-frame delta; +0xad8 f32 |delta|; +0xab0 u8 "moved by input"; +0xb1c u8 request flag; +0xae8 u32 snap state; +0xaec/+0xaf4 snap target pos/id; +0xe0? n/a; +0x1e0 ptr to the controlled actor (fairy); +0xb1b u8 "button held" latch.
- After the frame: FUN_0039a610 end (39acbd..39ad27) transforms (+0xabc) from camera 0x29 space to camera 0x28 space with FUN_005714e0 and stores to actor(+0x1e0)+0x38fc/+0x3900 and +0x3f0/+0x3f4 (floats).
- To convert to the screen pixel: px = x + 960, py = y + 540 (assuming y down, as the touch sign; verify with the first run).
- Finding the instance: no global found. Easiest: hook FUN_0039ad80 (rdi = cursor) and remember rdi per player (player index = (int16)vfunc[+0x1c8](rdi) - 11, range 0..3, same expression the code uses to index the options at (owner+0x80)+idx*0x23b0+0x2284). The player's touch record is FUN_00045850(mgr = [engine-ish +0x110 of FUN_0007cf50(0)], player) (mgr+0x248c + player*0x34).

### 2.4 Right stick (FUN_00399d00, only used when the touch record is not active)

- Axes: FUN_00043cf0(mgr, 4) and (mgr, 5) (normalised right stick, per input_save +0x58/+0x5c); if manager +0x8de byte set, axes 2 and 3 are used instead when non-zero (left stick).
- len = hypot(x, y); len < 1.19e-7 -> no input. Else unit direction u = v/len; angle quantised by FUN_00045890 (int16 angle) only for the turn-rate bookkeeping (+0xacc counter, +0xad0).
- Speed variable +0xae0 (units of a 960-px-wide screen): len <= 0.6 -> constant 4.0 (ramp = max = 4); len > 0.6 -> ramps by `FUN_00018250(0)` = 1.5 per frame up to `FUN_00018250(1)` = 14.0 (table at 0x644f30: 1.5, 14.0, 0.5, 10.0, 0.3, 20.0); after the stick has been held > 14 frames (+0xacc > 14) ramp 0.5, max 13.0. len < 0.2 decays speed by 1.0 per frame. Result velocity = u * speed (39a058..39a0b2).
- Then velocity *= W/960 (= 2 at 1920) (39a0db: 0x74d25c = 960) and *= stick sensitivity k_s: s = FUN_0008c230 = nibble bits 0..3 of the u16 at option_entry+2 (0 -> 1, clamp 10); k_s = 0.4 if s <= 0 else (s-5)*0.15 + 1.0 (0x74d260 = 0.15, 0x74d264 = 0.4). Applied as position += velocity each frame (via 39a610 free state, 39ac42).
- So full-deflection stick at sens 5: up to 14*2 = 28 px/frame (1920 scale), ~4*2 = 8 px/frame below 0.6 deflection.

## 3. Function table

| addr | name | role | conf |
|---|---|---|---|
| 0x39ad80 | Cursor_Update | per-frame: select path (touch active -> 39a1e0 else 399d00), 39a610, clamp +-0.5*screen, events | high |
| 0x39a1e0 | Cursor_TouchInput | mode from options (FUN_0008c250), modes 0..3 formulas above | high |
| 0x399d00 | Cursor_StickInput | right stick velocity with ramp, writes ac4/ac8 | high |
| 0x39a610 | Cursor_Apply | free-state `pos += delta`, aim-assist snap state machine, writes actor +0x38fc/+0x3f0 | medium |
| 0x70a60 | TouchRecord_Build | raw norm -> record with screen scaling | high |
| 0x45530 | PadMgr_Tick | builds 4 per-player touch records (mgr+0x248c + 0x34*i) and 0x28-B gesture records (+0x2560) | high |
| 0x45850 | PadMgr_TouchRecord | accessor, null if player > 3 | high |
| 0x5a2970 | Pad_ReadTick | scePadReadState + norm + delta (5a2a58..5a2b6e) | high |
| 0x8c250 / 0x8c260 / 0x8c230 | Opt_TouchMode / Opt_TouchSens / Opt_StickSens | option getters (entry = FUN_0008c400(base+0xaf20c, slot): +2/+6/+0xa/+0xe) | high |
| 0x5714e0 | Cam_Transform | 2D point through two camera matrices | medium |
| 0x45890 | Atan2_16 | int16 angle from (x,y) | medium |

## 4. Closed-loop recipe for the port

Read cursor+0xabc/+0xac0 each frame (hook FUN_0039ad80), compute error = desired_centred_pos - read pos; for mode 0 (relative) feed a virtual finger delta of err_px / (1920 * k) in x-norm and err_px / (1080 * k) in y-norm (k = 0.2*sens; sens from option nibble or just measure). For absolute mode 1: touch_norm_x = 0.5 + cx / (1.5 * 1920); touch_norm_y = 0.5 + (cy + 0.1*1080) / (1.5 * 1080) (limits |cx| <= 0.75*W etc.; cursor clamp is still +-960/+-540). Which mode the player has: byte [entry+3] & 0xf (default unknown, unverified: check in the run, or read the 4-byte entry at options_base + 0xaf20c + 2 + 4*slot). Easiest for 1:1: force/override the mode to 1 in the options struct is NOT needed if you read back; otherwise write the mode and use the mode 1 inverse above.

## 5. Open questions

- Default value of the mode/sens nibbles (options init) and whether the game's menu exposes them.
- Value of the sentinel at 0xe93850 (assumed zero), and exact y sign of the centred coordinates (assumed down).
- Modes 2/3 semantics (halves, possibly for 2 players sharing one pad) and the effect of unchanged ac4/ac8 when a mode 1-3 request follows a mode 0 frame.
- The scroll compensation block (39a8a1..39a968) and aim-assist magnet thresholds; the cursor object class/vtable and its creation site (no global pointer found).
