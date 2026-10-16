# Audio output (CRI NcVoice/ASR over sceAudioOut) and pad consumers

Rating: audio = careful (HLE is small, but pacing and 8-port Outputs batching matter); pad = routine (extends input_save.md).

## 1. Summary

### AudioOut
- sceAudioOut is used only inside the CRI middleware ("CRI NcVoice Pseudo/PS4", `CriNcvPS4`), never by game code. The game
  runs CRI Atom/AtomEx 2.14 + CRI ASR; ASR mixes into "native voices" which this backend submits.
- The five imports have exactly one thin logging wrapper each (the wrapper prints an `E20xxxxxxx` string on error and returns
  the SDK result unchanged), reached only through a function-pointer table at **0xbc17a8** (pointed to by the slot at 0xc15fe0):
  +0x08 Init (FUN_005f4580 -> sceAudioOutInit), +0x10 stub returning 0 (FUN_005f3690), +0x18 Open (FUN_005f45c0), +0x20 Close
  (FUN_005f45f0), +0x28 Output (FUN_005f4620), +0x30 Outputs (FUN_005f4650). Other slots (+0x38.. strings) are unused/unknown.
- Backend init FUN_005f36b0 (CriNcvPS4 init): calls table+8 Init (accepts rc 0 or 0x8026000e = already initialised), then creates
  the mixer thread "CRI Audio Output" (FUN_00630820 = thread create; entry **FUN_005f3e90**; stack 0x10000; priority/affinity
  taken from the CRI config at 0xc15fe8 (=0x12c) / 0xc15ff0 (=0x3f)). Max **0x20 (32) native voices** (FUN_005f39c0 checks
  count at list+0x10 >= 0x20).

### Open arguments (FUN_005f3be0 = voice open, calls table+0x18)
Asm 5f3c20..5f3c85: sceAudioOutOpen(userId=edi, type=esi, index=edx=0, len=ecx=**0x100** samples, freq=r8d=**0xbb80 = 48000**, param=r9d).
- userId/type come from an optional config struct (rcx: u32 user, u32 type, u32 attr). With NULL config: user **0xff** (= SYSTEM
  user), type **0** (MAIN); the struct is also copied to voice +0x34/+0x38/+0x3c. Voice +0x24 = (user == 0xff).
- param (format) = **float**: channels 1 -> 3 (FLOAT_MONO), 2 -> 4 (FLOAT_STEREO), other (>2) -> 5 (FLOAT_8CH);
  OR'd with `attr & 0xf0000` (param attribute bits, e.g. restricted/ALLOW_BGM-like flags; shim can ignore).
- Port type 4 (PADSPK) is accepted only for 1 channel (error string E2016090100 otherwise). So pad-speaker output is a code path
  in the library; whether the game's CRI config ever selects it is (unverified, probably not). The shim should accept
  type 0 (MAIN) and ideally also 1/3/4, returning a handle > 0.
- Granularity is 256 frames (5.33 ms at 48 kHz). Stored result handle at voice +0x30 (<=0 -> voice marked unopened).
- Number of ports: one per CRI voice that has output enabled (voice +0x28 flag); in practice CRI/ASR creates few (typically one
  stereo MAIN voice, plus possibly BGM/other; exact count (unverified) - the thread supports up to **8 ports per batch**).

### Mixer thread FUN_005f3e90 (the loop)
Per iteration (5f3f40..5f4550, outer loop until `[ctx+0x18] != 0`):
1. Lock list mutex (0xebef78); walk the voice list (head at [0xebef70]); for each voice with +0x28 (active) set, +0x2c = 1 (busy) and
   +0x18 (running) nonzero: lock its ring mutex (+0x48), pull up to 256 frames from the voice's queue (+0x40) of source buffers
   (FUN_0062dad0 = pop; each source has per-channel float planes at +0x28..), interleaving planar floats into the voice's
   double-buffer (stride 0x2000 bytes, buffer select +0x20, base +0x50; interleave for mono=memcpy, stereo, and 8ch). Zero-fill on underrun.
2. Fills an array of up to 8 entries of 16 bytes {u32 handle (voice+0x30), pad, void* buffer} (at rbp-0x2270; entry = handle,buffer) and
   **one batched `Outputs` call** through table+0x30 with count = number of active voices (`sceAudioOutOutputs(param, num)`).
   Single `Output` (table+0x28) is used only on voice destroy (FUN_005f3ad0, called with NULL buffer to flush before Close, then Close table+0x20).
3. If no active voices that pass: FUN_00630880(5) = sleep 5 ms (idle). FUN_00630880(1) = 1 ms spin while closing a voice.
4. After Outputs, a debug-only branch (flag 0xebef68 == 1) sums voice float planes into a meter (256-sample add loop, vmovss/vaddss) - metering, not game audio.

### Pacing
Pacing relies on **sceAudioOutOutputs blocking** until the 256-frame period of all ports has been consumed (the only throttle in the
loop; the 5 ms/1 ms sleeps are only for idle/teardown). So the HLE Outputs must block ~5.33 ms per call (or until the host audio device
has consumed that many samples), and must return `>= 0` (the wrapper logs when `<0`; Output logs when `<0` too; Open logs when `<= 0`).
If Outputs returns instantly the thread spins at 100% and the mixer runs ahead (audio fast-forwards / ring starvation).
Because several ports are submitted in one Outputs call, the shim should treat each entry {handle, ptr} as one 256-frame period for that port.

### Volume
No software gain in this layer: the interleave/copy has no multiply; sceAudioOutSetVolume etc. are not imported. Volume is done
inside CRI ASR/AtomEx (bus/category volume, in float) before the native voice. The shim needs no volume code. Format on the wire is
float (non-clipped values may exceed +-1.0; clip on the host).

## 2. Function table (audio)
| addr | name | role | conf |
|---|---|---|---|
| 5f4580 | NcvOut_Init | sceAudioOutInit wrapper (logs E2012111500) | high |
| 5f45c0 | NcvOut_Open | sceAudioOutOpen wrapper (E2012111503; logs if rc<=0) | high |
| 5f45f0 | NcvOut_Close | sceAudioOutClose wrapper | high |
| 5f4620 | NcvOut_Output | sceAudioOutOutput wrapper | high |
| 5f4650 | NcvOut_Outputs | sceAudioOutOutputs wrapper | high |
| 5f36b0 | CriNcvPS4_Init | creates globals, calls Init, creates thread "CRI Audio Output" | high |
| 5f39c0 | NcvVoice_Create | allocate native voice (max 32), channels at +0x10/+0x14 | med |
| 5f3be0 | NcvVoice_Open | maps channels->float format, calls Open(len 256, 48000) | high |
| 5f3ad0 | NcvVoice_Destroy | wait idle, unlink, Output(NULL) flush, Close | med |
| 5f3e90 | NcvMixerThread | interleave + batched Outputs loop | high |
| 5f22e0 / 5f27b0 | Atom voice setup (ASR output voice) | upstream CRI, not needed for HLE | low |

## 3. Pad: verified ScePadData reads (FUN_005a2970 Pad_Update, ScePadData at rbp-0xa8)
Verified against asm (extends input_save.md section 2):
- ReadState(handle, rbp-0xa8): `buttons` u32 at +0x00; left stick x/y +4/+5, right x/y +6/+7 (u8); L2 +8, R2 +9 (u8, movzx at rbp-0xa0/-0x9f);
  `touchData.touchNum` u8 at +0x34 (rbp-0x74); touch[0].x u16 at +0x3c (rbp-0x6c), touch[1].x at +0x44 (rbp-0x64); `connected` u8 at +0x4c (rbp-0x5c).
  The game does not read orientation/gyro/accel (+0x0c..+0x33), timestamp, or connectedCount.
- GetControllerInformation(handle, rbp-0xc8): touchpad resX u16 at +4 (rbp-0xc4), deadzone pair u16 at +8 (L low, R high; rbp-0xc0), connected byte at +0xc (rbp-0xbc).
  pixelDensity (+0) and resY (+6) not read in the visible path (resY read likely via the same dword at +4: unverified).
- Buttons used: dpad, face, L1/R1/L3/R3, OPTIONS, TOUCHPAD click, L2/R2 digital (see input_save.md mask table). SHARE/PS button are not used.
- Vibration: `scePadSetVibration(handle, {u8 large, u8 small})` built at rbp-0xd0/-0xcf, when padSys slot flag +0x1b8 set. Request helpers: FUN_005a2f20(padSys, slot) = clear small/large? (sets +0x1b9=0, +0x1ba=0, +0x1b8=1 = stop), FUN_005a2f50(padSys, slot, large) = set +0x1ba (large motor) + request. Slot index must be <= 3.
- Light bar: SetLightBar via FUN_005a3100 only; ResetLightBar on logout. Open: `scePadOpen(userId, 0, 0, NULL)` per slot (up to 4 slots / 4 users).

### Touchpad IS consumed
Pad_Update writes normalised touch points into the game pad struct (+0x04..+0x24, see input_save.md 2.3). They are not unused:
- A per-frame object (FUN_00045530 on the input/pad manager, driven from FUN_0007cfb0) builds **4 per-player touch records** via
  FUN_00070a60: record size 0x34, base manager+0x248c (accessor **FUN_00045850(mgr, playerIdx<=3)** returns &record, else NULL).
  Record layout: +0 u8 valid (touchNum>=1), +0x1c/+0x20 i32 sentinels (-1 / 0), then per point (0x24 stride from +0x20): flag byte, raw x,y,
  screen-scaled x,y (scaled by engine+0x40 +0x3c/+0x40 = screen size), scaled delta x,y, point index. Up to 2 points.
  FUN_00070bc0 (4 records at manager+0x2560.., stride 0x28) post-processes (gesture/swipe, (unverified)).
- Consumers of FUN_00045850: FUN_0039a1e0 (called from FUN_0039ad80, sits next to the `DcAI_SysObj_Cursor` code FUN_003999a0; reads
  record+0 valid and +0xc pos, picks the player's pad via the per-player 0x23b0 stride table at +0x2284 = pad slot id) - **this is the
  touchpad-as-absolute-pointer path for the cursor/fairy (role of the object (unverified))**, FUN_002f25e0 (6.6 KB, UI/menu touch handling,
  via FUN_002eb220 <- FUN_002ea7e0), and FUN_00070bc0. The mouse-attacks/fairy pointer design can therefore use either:
  (a) populate ScePadData touchData (1 point, absolute within 1920x943 coordinates, id) from the mouse, valid only while "R" held, or
  (b) the right stick (already read: normalised +0x58/+0x5c; game derives digital R-stick direction bits 0x40000..0x200000). Both are real paths;
  the touch path is only used when touchNum>0, so supplying touchNum=0 falls back to stick-only behaviour.
- Whether the original fairy cursor is driven by right stick or touch in gameplay was not traced to the consumer in the task tree (unverified).

### Players / per-frame path
Up to 4 pads: login event (sceUserServiceGetEvent) -> FUN_005a2f70 slot -> scePadOpen(userId,0,0,NULL). Each tick (inside Update_Ticks) FUN_005a2970
runs ReadState + GetControllerInformation per open slot, then Pad_Edges FUN_0057b220. Game-side readers: button getters FUN_00043f30 /
FUN_00043cf0 (pressed/released/repeat by bit per player via the 0x1d0 slot array), consumers of slot user id (+0x28) in FUN_00080400, FUN_000c6b60,
FUN_003ac250, FUN_00438d00 etc.

## 4. Shim state (port/src/hle/pad.cpp, input.h) and gaps
Current shim: scePadInit ok; scePadOpen accepts only (kUserId, type 0, index 0) and returns a single constant handle (1), others -> 0x80920001;
scePadGetHandle same; ReadState fills buttons, sticks, L2/R2, quaternion w=1, connected=s.connected, timestamp (QPC us at +0x50), connectedCount (+0x68)=1,
everything else (touchData incl.) zero; GetControllerInformation: density 44.86, 1920x943, deadzones 0x0d/0x0d, conn type local, connectedCount 1,
connected 1; SetVibration/SetLightBar forwarded to InputSource; Reset* ok. Sources: Idle (all zero) and ScriptSource (pad 0 only).
Gaps vs the game:
1. Single pad only: players 2-4 (4-player local co-op) impossible: Open rejects other users. Needs extra user ids (the UserService shim must emit
   LOGIN events and the initial-user path; see input_save.md 2.2) and per-handle state (read pad index = handle - 1).
2. touchData always zero -> touch path never active; needs touchNum=1/2 + points at +0x34.. (touchNum +0x34, touch[i] at +0x3c + 8*i: x u16, y u16, id u8) to use the mouse as touch pointer.
3. Stick Y sign, deadzone values (0x0d) fine; shim buttons use PS4 bit values (matches mask table).
4. ReadState is called in the logic tick, not on a device thread - no issue.

## 5. Open questions
- Which CRI config the game passes to voice open (user id / port types / pad-speaker); count of voices that actually reach Outputs per frame.
- Role of FUN_0039a1e0/FUN_0039ad80 object (cursor vs fairy vs menu) and whether gameplay fairy control uses the touch record.
- Meaning of the 0x28-stride records at manager+0x2560 (FUN_00070bc0).
- Outputs blocking contract: the exact return-code the library needs (the wrapper only checks `<0` for error).
