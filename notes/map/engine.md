# Engine core: boot, frame loop, threads, render submission, imports, CRI boundary

Mapper notes (survey depth). Addresses are eboot-relative (as in q.py). "(unverified)" = inferred, not read in asm.
Raw per-import tables, call-site lists and scan scripts live in `notes/raw/engine/` (ignored by git).
Anchors: `entry` 0x54c690, main = FUN_000496f0, platform init = FUN_00598290, frame = FUN_005a1f60.

## 1. Summary
The eboot is a **single-threaded game loop** (main thread: input -> update tree -> hand frame to render thread -> vblank
wait) plus a **render-submission pipeline on dedicated threads** (1 "RenderDLThread" + 12 "RenderDLThreadLocal[n]"
workers) that turns the frame's game-level command lists into GNM command buffers and submits+flips them. The engine
is a custom C++ framework (task classes tagged `FCRT`, "Fn*" managers, a `DcCore` game class); **libc malloc / operator
new are NOT imported** - the game has its own heap (FUN_0056efc0 family) built on `sceKernelAllocateDirectMemory`.
GNM is a **statically linked Sony GfxContext-style wrapper** (about 0x55e000-0x564800 and 0x637cb0-0x639fe0, ~214
functions); only 19 of 97 imported libSceGnmDriver symbols are used. CRI FS / Atom / HCA run as ordinary x86-64
code inside the eboot and talk to the OS through the plain libkernel file I/O + sceAudioOut imports, so Stage 3 can
leave CRI untouched and shim the OS layer below it.

Ratings: boot / loop / thread structure **routine**; render submission boundary **routine** (game-level list is clean:
polymorphic command objects); GNM layer replacement **careful** (draw-cmd Execute bodies use GNM state through the
embedded wrapper); Stage 3 shim worklist **careful** only for direct memory, event flags, pthread attrs and the
stack-guard data import (section 5).

## 2. Boot sequence (all steps read in asm / pseudo-C)
1. `entry` 0x54c690 (SysV, rdi = args block {u32 argc; argv at +8}, rsi = exit hook): `libc::_init_env`, `atexit(rsi)`,
   `atexit(0x63a0f0)`, FUN_00000020 (static-constructor run loop over a table 0xc16658..[0xbc2380], (unverified) that it is
   .init_array), then `FUN_000496f0(argc, argv, 0)`, `catchReturnFromMain`, `exit`.
   No `fs:`/`gs:` accesses, no `syscall`/`cpuid`/`rdtsc` anywhere in 0x0-0x63a000 (scanned): game code uses no TLS.
2. **main = FUN_000496f0** (570 B):
   - decodes a 256-char hex string literal into a 128-byte blob and stores an NP title-id string + that blob into the
     platform singleton (FUN_0059b7e0 -> fields +0x12480/+0x12488). NP credential material: not recorded here.
   - `FUN_00568360(...)` (heap/engine bootstrap, 12 size args: e.g. 0x200000, 0x4000000, 0x100, 0x1f000000 (496 MB),
     0x2000, 0x240000, 0x240000, 0x4000000, 0x800000, 0x800000, 0x2c00000; meaning of each unverified): creates the
     **engine singleton** (0xe78708, 0x4d0 B; getter FUN_00568800) and the heap objects (fourcc tags HMAN HTSK HMAS HFLE
     HFNC HTRD are written at 0xe7c560 / 0xe87420 / 0xe78b20 / 0xe839e0 / 0xe8ae60 / 0xe8e8d8), then calls
     **FUN_00598290 = platform init** (below) on the **platform singleton** 0xe63cb8 (getter FUN_00568510; ~0x14a38 B).
   - `FUN_0059b7f0` -> `sceNpTrophyCreateContext`.
   - `FUN_00568520(engine, 0xaf)` = engine init: criErr callback (FUN_0062bed0), CRI FS init FUN_005aaf10 (criFs_Initialize,
     error string names it; config struct copied from 0x761ea0), 2 x FUN_005ac4c0 (criFsBinder_Create, engine +0xc0/+0xc8),
     task tree: allocs + FUN_0058d610 for `FnCoreRootTask`, `FnCoreSystemRootTask`, `FnCoreUserRootTask`; managers:
     `FnResourceMan` (FUN_0057c120), pool setup FUN_0056f440, **render DL system FUN_005a1c60** (creates the render
     threads, section 3), FUN_0057ae70, FUN_00568dc0 (coredump handler), **`FnSoundMan` FUN_00584340**, FUN_0059b820
     (NP / trophy glue), FUN_00580020.
   - FUN_0005cda0 (engine+0x48 -> +0x80 = callback FUN_0005cdc0); alloc 0x138 B and construct **`DcCore`** (FUN_00016b30,
     fourcc 'DCOR', vtable 0x8bf9b8, global DAT_00c17758, stored at engine+0x88); frame-divisor table lookup
     (FUN_00018240(0) from table 0x644ef0, 875 callers: a settings array) -> frame-runner+8.
   - **FUN_00017d80 = mount**: binder bind of `DRACRO.cpk` (FUN_005689e0) and `/app0/DRACRO_US.cpk` (FUN_00568870 +
     FUN_005689b0); FUN_00568920 (create/reset binder) twice first.
   - FUN_005689d0 -> **FUN_00584e30** (FnSoundMan start: criAtomEx init via FUN_005cd210, ACF registration with
     `SND/DC_SOUND.ACF`, FUN_00583060 etc.).
   - clears bit0 of engine+0x48+0x70 (go flag) and enters `for(;;) FUN_005687c0(engine)`.
3. **FUN_00598290 = platform init** (6463 B, 12 args, order read from the call list):
   FUN_005981c0 = 19 x `sceSysmoduleLoadModule` with ids 7, 6, 0xce, 0xad, 0x81, 0xa4, 0xac, 0xa0, 0x84, 0x98, 0x99,
   0x96, 0xa8, 0x9d, 0x1c, 0x19, 0xe4, 0x21, 0x102 (+ one more id 0x2c later, guarded by a loaded-bitmask at
   platform+0x12474) -> `sceVideoOutOpen(0xff)` + `GetResolutionStatus` -> `sceKernelIsNeoMode` check (FUN_00563fe0:
   1080p vs 4K sizes 0x780x0x438 / 0xf00x0x870) -> **11 x (GetDirectMemorySize, AllocateDirectMemory, MapDirectMemory)**
   (every block: align 0x200000, prot 0x33, type 0; FUN_00599be0 / FUN_00599e10 add more, max 0x100 blocks tracked at
   platform+0x12b20) -> 11 x FUN_005712d0 (heap/pool carve-ups) -> `UserServiceInitialize`, `scePadInit`,
   `UserServiceGetInitialUser` + `GetUserName`, `CommonDialogInitialize`, `SaveDataInitialize3`, `sceNpSetNpTitleId`,
   `sceNpSetContentRestriction`, sysmodule 0x2c, FUN_00599d00 (Net/Ssl/Http init: sceNetInit, sceNetPoolCreate, sceSslInit,
   sceHttpInit), `GameLiveStreamingInitialize`, FUN_0059d870, FUN_00571090 -> **GNM contexts**: 3 x {'CB HEAP',
   'CB DCB', 'CB CCB'} via FUN_00599be0 + FUN_00638b10 (3 gfx contexts, one per frame in flight; context stride 0xe888),
   FUN_0059c9e0, FUN_0054e4e0 x3, buffers: 'RenderBuffer' (FUN_00599e10), 'MAIN BUFFER[%d]', 'DepthBuffer', then
   `sceKernelCreateEqueue("Initialize")` + `sceVideoOutAddFlipEvent`, `SetBufferAttribute` + `RegisterBuffers`,
   `sceKernelCreateEqueue("vblankeq")` + `sceVideoOutAddVblankEvent`, `sceSystemServiceGetDisplaySafeAreaInfo`,
   `sceSystemServiceHideSplashScreen`.
   Not done here: audio (AudioOut is opened later by CRI AtomAsr), Np managers (done lazily by FUN_00599d00 / FUN_0059b160).

## 3. Main loop, per-frame order, threads
**Loop**: `for(;;) FUN_005687c0(engine)`: if `(engine+0x4cc & 3) == 1` -> set busy bit, call **FUN_005a1f60(engine+0x40)**,
clear busy, `++engine[0xa0]` (frame counter). (FUN_005687c0 = EngineStep; engine+0x40 = frame runner "Core".)

**FUN_005a1f60 (frame runner; `this`=engine+0x40)**, order read from pseudo-C:
1. FUN_00599f60 / FUN_00599f50 / FUN_0056fae0 / FUN_0056fad0 (empty hooks, `ret`).
2. **FUN_0056fb80 = update**: loop `this+0x244` times (1..3 logic ticks per displayed frame = frame-skip catch-up):
   FUN_0059b090 (`sceUserServiceGetEvent` poll, pad (re)open/close), **FUN_005a2970(engine+0x68) = pad**
   (`scePadReadState`, `scePadGetControllerInformation`, `scePadSetVibration`), **FUN_0058def0(engine+0x50 task root, 1,1,1,1,1)**
   = run the whole task tree in 6 phases (FUN_0058ddd0 / 58df90 / 58e020 / 58e0c0 / 58e150 / 58e250); everything gameplay,
   mbs layer eval (-> Layer_BuildDraw -> FUN_00597830 enqueue), CRI per-frame server calls (via FnSoundMan / FnResourceMan
   tasks, virtual - not visible in the static call graph) happen inside the tree.
3. **Frame hand-off**: `FUN_0059d6d0(render DL sys = platform+0x12538)` = poll render thread idle (`sceKernelPollEventFlag`).
   Idle -> rotate triple buffer `this+0x38 = (+1) % 3`, copy per-layer state (`memcpy`) and `FUN_005977f0` (clear the 3rd buffer's
   buckets), then **FUN_0059d730** stores the list pointer (`this+0x18`) in the render system (+0x540) and `sceKernelSetEventFlag`
   (wakes RenderDLThread). Busy -> frame dropped from rendering (`this+0x238 = 1`, only reset buffers).
4. Pace: FUN_0056faf0 (computes next tick count 1..3 from last frame time vs `this+0x54` target fps), **`sceVideoOutWaitVblank`**,
   spin on `sceKernelGetProcessTime` + `scePthreadYield` until one frame period elapsed (fps float at `this+0x54`; only when tick
   count < 2), frame counter `this+0x10++` if not dropped.
5. FUN_0056f7d0: release/recycle queue of finished resources (ring of refcounted handles).

**Threads** (every `scePthreadCreate` site; 4 sites in eboot code, libs may create more):
| creator | entry | name / role |
|---|---|---|
| FUN_0059ce20 (called from FUN_005a1c60 <- FUN_00568520) | FUN_0059cfa0 | `RenderDLThread` (+ `RenderDLEventFlag`): master render thread. Waits event flag, splits the frame's layers into 4 groups of ~equal command count, signals 4 workers (3 frames x 4 = 12 workers created by 12 x FUN_0059c3b0), waits on label fences (`FUN_00561480` = PM4 write-data/EOP packet, spin + `scePthreadYield`), submits worker DCBs (FUN_00638bb0 -> FUN_00638bc0 -> `sceGnmSubmitCommandBuffers`), then FUN_0059a1e0 |
| FUN_0059c3b0 | FUN_0059c4e0 | `RenderDLThreadLocal[%d]` workers: wait flag, for each layer in [start,end): FUN_005a3a00 (build GNM commands for that layer's sorted list), set flag |
| FUN_005a1bb0 (generic `Thread::Start`, entry FUN_005a1930 -> virtual slot +0x10 `Run`, then `scePthreadExit`) | FUN_005a1930 | generic thread class (vtable of subclass set before Start). Starters: FUN_0008cbf0 / FUN_0008d880 / FUN_0008e010 (save-data worker; strings SAVEDATA00 / ICON0.PNG), FUN_000bed30 / FUN_000bf2e0 (NpTus-related, unverified), FUN_005953a0 / FUN_00595b50 (vtable 0xbc0a68, unverified), FUN_005a6dd0 (vtable 0xbc0e48, unverified, probably async load) |
| FUN_006305f0 (CRI `criThread_Create`-equivalent, string `CriThread`; no direct callers - used through a function pointer) | FUN_006308e0 | CRI server threads (FS/Atom/ASR); uses AttrSetstacksize / inheritsched / schedparam / **affinity**, `scePthreadSetprio` (FUN_006308a0) |
| (join/cancel: FUN_0059d4d0 render shutdown, FUN_005a1980 / 005a1a90 generic thread dtor, FUN_00630830 CRI) | | |

Each frame the RenderDLThread tail FUN_0059a1e0 -> FUN_0059a2f0 also runs `sceNpCheckCallback`, writes an EOP fence, loops
`FUN_00638fd0` (retry on busy code -0x7f2eef7f with `sceKernelUsleep`) and **FUN_00639020 -> `sceGnmSubmitAndFlipCommandBuffers`**
(= present/flip; `sceVideoOutSubmitFlip` is not imported), `sceGnmSubmitDone`, `sceSystemServiceGetStatus`, then advances the
3-deep context ring (platform+0x123b0, stride 0x1b8 per frame slot, ctx stride 0xe888).

## 4. Render submission and the interception boundary
**Pipeline**: producer (any game code, mostly during task-tree update) -> allocate command object from a bump pool
(FUN_0057ae20(pool,size): ring allocator; sprite pool 0xe8e8a0) -> construct (vtable) -> fill -> **FUN_00597830(layerBuf, cmd, bucket)**
(19 callers) -> vector push into `layerBuf+0x2b0[bucket]`; per-frame at render time FUN_005a3a00 sorts and executes.

**FUN_00597830(layerBuf, cmd, bucket)**: drops the command unless the logic tick is the last one of the frame
(`core+0x244 - 1 <= core+0x248`); grows the bucket vector array (FUN_00597960 / FUN_00597ca0) if `bucket` >= count; writes
`cmd+0x50 = index within bucket` (stable-sort tiebreak) and appends the pointer. `layerBuf = &layer[idx].buf[curBuf]` with
`layer = FUN_0056f6a0(core, layerId)` = `core+0x20 + layerId*0xb30` (bounds-checked against vector end, element 0xb30);
`layerBuf = layer + 0x2b0 + layer[+0xb20] * 0x2d0` (3 buffers of 0x2d0 B; byte +0xb20 = current buffer index).

**Layer buffer (0x2d0 B)** as read in FUN_005a3a00 / 59d (partly unverified):
| off | meaning |
|---|---|
| 0x00..0x7f | 4 x 32-byte matrix/state blocks A..D, (re)loaded from staging copies at 0x80.. (+0x80 pairs) when flags +0x294 bit1 (0x20..0x27 -> 0x40) / bit2 (0x28..0x2f -> 0) set; also 0x80-0xbf pair for blocks 0x38.. -> 0x10.., 0x40.. -> 0x18.. (view/projection style; exact roles unverified) |
| 0x284 / 0x288 / 0x28c / 0x290 | viewport rect floats (x0, y0?, x1, y1) -> converted to ints for `FUN_00639610(.., ctx, x0, y0, x1, y1)` = SetViewport/Scissor/GuardBand (flags bit0) |
| 0x294 | u32 dirty flags: 1 viewport, 2/4 matrix copies, 8 = set per-layer render state (FUN_0059a420: DepthStencil/blend defaults + program constants at ctx+0x28/+0x88) |
| 0x29c | i32 worker/context index -> GNM context = `platform[+0x12aa0 + idx*8]->+8` |
| 0x2a4 | sort mode: 1 = FUN_005a3be0 (introsort by key `cmd+0x54`, then `cmd+0x50`), 2 = FUN_005a4570 (other direction, unverified), else none |
| 0x2b0 / 0x2b8 | begin / end of an array of bucket records (0x20 B each: +8 vec.begin, +0x10 vec.end, +0x18 vec.cap of `cmd*`) |

**Execution**: for each bucket (in order): sort, then for each cmd call **virtual slot +0x20 `Execute(cmd, layerBuf)`**.
So the whole draw list is: `for layer in 0..n: for bucket in layer: sorted cmds -> cmd->Execute`. Command objects are never turned
into a flat byte stream; each class' Execute emits GNM through the embedded wrapper.

**Command base class** (vtable 0x8bfbf0, object stores vtable+0x10): `+0x00` vptr, `+0x10..+0x4f` 64-byte matrix (default copied
from 0xe92410 = identity, 16 floats), `+0x50` u32 bucket index (written by FUN_00597830), `+0x54` i32 sort key (zeroed by ctors;
sprite code overrides it from owner+0xb8 if flag 0x20, see mbs.md), `+0x58` u32 (0 by ctors). Slots: [0,1] dtors (ret), [2] FUN_000251b0,
[3] FUN_000251d0, **[4] Execute**. Known concrete classes (ctor -> vtable object -> Execute):
| class (proposed) | ctor | vtable obj | Execute | size / note |
|---|---|---|---|---|
| SpriteDrawCmd (mbs part list) | FUN_005a6820 | 0xbc0e88 | FUN_005a7260 (9663 B) | 0x280 B; fully described in mbs.md; texture-override slots at +0x90.. |
| class X1 | FUN_00568f30 | 0xbc0aa8 | FUN_0059f7b0 (2910 B) | unread (1 producer: FUN_00592e90 / FUN_005811c0 family, unverified) |
| class X2 | FUN_00569010 | 0xbc0af8 | FUN_005a0330 (3691 B) | size >= 0xd8; copies screen size (FUN_0056fc20); used by 5811c0 / 581db0 / 19f8c0 (unverified role: textured quad / effect) |
| class X3 (rect / fullscreen) | FUN_00569240 | 0xbc0b58 | FUN_005a12e0 (725 B) | fields +0x5c mode, +0x60..+0x6c rect (setter FUN_00569300 sets mode 2 + 4 floats); defaults to screen size FUN_005a1c40/50 |
| class X4 | FUN_00569320 | 0xbc0ba8 | FUN_005a15e0 (804 B) | small flags at +0x60..+0x6e |
| (two more ctors reference the base) | FUN_000250c0, FUN_00025120 | ? | ? | unresolved |
Producers of commands (callers of FUN_00597830): FUN_00050ae0, 52770, 6bb90, 6e360, 19f8c0, 1e1c10, 295c10, 2978c0, 30af90, 33a570,
443960, 453a60, 480ba0, 5098f0, 509e10, 573bb0 (mbs Layer_BuildDraw), 5811c0, 581db0, 592e90. All obtain their layer via FUN_0056f6a0.

**Where to intercept for a native renderer** (best first):
1. **`cmd->Execute` slots** (5 vtable objects above; hook `Execute(cmd, layerBuf)`; layerBuf gives viewport/matrices/target). Native replacements
   read the same command struct the game already filled; no GNM parsing. This is the Stage 4 target. Needs all classes covered: ~5 known + 2 unresolved.
2. **FUN_005a3a00(layerBuf)** (per-layer replay) or **the worker FUN_0059c4e0 / master FUN_0059cfa0**: replace the whole GNM build+submit and run
   `sort -> Execute` single-threaded on a native thread, bypassing the 12 workers, event-flag/label fences, and `FUN_0059a2f0/FUN_00639020` flip.
   Then the frame's present is our swap; `sceVideoOutWaitVblank` (FUN_005a1f60) maps to our vsync.
3. Fallback: emulate GNM below FUN_005644xx / FUN_005600a0 wrappers (19 GNM imports; PM4 packets are built by the embedded wrapper, e.g. FUN_00561480
   writes packet header 0xc0044700 = EVENT_WRITE_EOP). Heavy: do not.
The GNM-library entry surface used by game code: 152 functions in 0x55e000-0x564800 / 0x637000-0x63a000 called from outside it (`notes/raw/engine/gnm_boundary.tsv`),
top: FUN_0055e1f0 (43 callers), FUN_0055e6f0 / FUN_00560e00 (10-11), FUN_00564030 (bind texture), FUN_0055e660 (vertex-buffer descriptors), FUN_00564b30 (draw).
Textures reach the draw as the 32-byte T# copied by FtxBank_GetTextureDesc (mbs.md); Execute binds them via the wrapper - a native version needs FTX index -> GL/D3D texture.
Frame-level sizes: base 1080p, `FUN_00563fe0` = IsNeoMode switches targets to 3840x2160-class sizes (0xf00 x 0x870).

## 5. Import usage (Stage 3 shim worklist)
Source: scan of all code (direct calls to PLT stubs + rip-relative GOT refs) -> `notes/raw/engine/imports_usage2.tsv` (per import:
call sites, #functions, first functions). 476 imports; 375 are referenced by code, 101 are not (97 libSceGnmDriver minus 19 used = 78,
plus 23 libc/kernel/misc). Totals: libkernel 56 used / 3508 sites (3246 are `__stack_chk_fail`), libc 52 used / 2817 sites.
Per-library (imports / used / sites) and timing (F = runs every frame or in worker threads; I = init/shutdown; E = event/async/on demand):
| library | imp | used | sites | timing | notes |
|---|---|---|---|---|---|
| libkernel | 62 | 56 | 3508 | mixed | see below |
| libc | 58 | 52 | 2817 | F | see below |
| libSceGnmDriver | 97 | 19 | 19 | F | SubmitCommandBuffers(+ForWorkload), SubmitAndFlipCommandBuffers(+ForWorkload), SubmitDone, DingDong, Set/Update{Vs,Ps,Hs}Shader, SetLsShader, SetCsShaderWithModifier, DrawIndex, DispatchDirect, DispatchInitDefaultHardwareState, DrawInitDefaultHardwareState350, InsertWaitFlipDone. **If the renderer is intercepted at Execute these are never called**; otherwise stub = hard (PM4 + shader binaries). |
| libSceVideoOut | 7 | 7 | 7 | I (+WaitVblank F) | Open, GetResolutionStatus, SetBufferAttribute, RegisterBuffers, AddFlipEvent, AddVblankEvent init-only; **WaitVblank in FUN_005a1f60 per frame**. |
| libScePad | 8 | 8 | 8 | F | ReadState / GetControllerInformation / SetVibration per tick; Open/Close/Set/ResetLightBar on user events. |
| libSceUserService | 5 | 4 | 7 | I + F(GetEvent) | |
| libSceSystemService | 4 | 4 | 5 | I + F(GetStatus per frame) | ParamGetInt (language/age), DisplaySafeArea, HideSplashScreen. |
| libSceSysmodule | 2 | 1 | 20 | I | 19 + 1 LoadModule calls; ids in section 2 (stub returning 0 is enough). |
| libSceAudioOut | 5 | 5 | 5 | CRI thread | Init/Open/Output/Outputs/Close, all inside CRI AtomAsr output (FUN_005f4580..5f4650): the only audio sink. |
| libSceSaveData / SaveDataDialog | 8 / 5 | 8 / 5 | 14 / 7 | E | worker thread; Mount2/Umount/DirNameSearch/SetParam/SaveIcon/Delete/GetMountInfo; dialog flow. Our shim: host dir. |
| libSceCommonDialog, MsgDialog, ErrorDialog, ImeDialog, SigninDialog | 2/6/5/6/4 | 2/6/5/5/4 | small | E | UI dialogs; stub status machine. |
| libSceFont, libSceFontFt | 20 / 2 | 18 / 2 | 25 / 2 | E | system font load + glyph render to CPU surface (FUN_00080bb0 init, 80d70 open/bind, 810d0 render glyph). Needs a FreeType-backed shim: **medium-hard**. |
| libSceNet, NetCtl, Http, Ssl | 9/5/2/2 | 9/5/2/2 | 18/11/3/3 | E | init via FUN_00599d00 / FUN_0059b160, term FUN_0059b360; Net only through Rudp (FUN_000bd4c0 UDP socket). |
| libSceNpManager | 25 | 25 | 54 | E | stub local-only (rule 3). |
| libSceNpMatching2 / NpToolkit2 (52 used, private C++ NIDs nid_*) / NpTus / NpTrophy / NpCommerce / NpUtility | 20/55/9/6/5/6 | 20/52/9/6/5/6 | 27/131/18/10/7/6 | E | multiplayer (later); stub: return not-signed-in. Trophy: Create/Register/Unlock via FUN_0059f020..0059f0f0. |
| libSceRudp | 17 | 17 | 56 | E | multiplayer netcode (FUN_000bd.. / 000bda..); later -> UDP/ENet shim. |
| libSceRtc, Coredump, GameLiveStreaming | 5/2/2 | 3/2/1 | 3/3/1 | I | trivial stubs. |
**libkernel used (calls)**: pthread: MutexInit 13 / Lock 27 / Unlock 25 / Destroy 26, MutexattrInit/Destroy/SetProtocol/SetType (FUN_0062bd40),
CondInit/Wait/Broadcast/Destroy (CRI, FUN_00630400..), Create 4, Join 16, Cancel 15, Exit 2, Yield 5, Self, SetPrio, Attr{Init,Destroy,Setdetachstate,Setstacksize,Setschedpolicy,Setschedparam,Setinheritsched,Setaffinity};
event flags: Create 5 / Set 6 / Wait 6 / Poll 2 / Clear / Delete (render sync, FUN_0059d4d0..0059d730, 0059cfa0); equeue: CreateEqueue 2 (flip, vblank; no WaitEqueue seen, so only registered); direct memory 13 x (GetDirectMemorySize, Allocate, Map); files: Open 4, Close 5, Stat 5, Fstat, Read 2, Write 2, Lseek 2, Fsync, Ftruncate, Mkdir, Rename, Unlink, CheckReachability (CRI I/O 0x5bbd90..5bc4c0 and save code); time: GetProcessTime 2, GetProcessTimeCounter(+Frequency), Usleep 4; IsNeoMode.
**Not imported**: sceKernelMmap/MapFlexibleMemory/ReleaseDirectMemory/Sema*/Mutex-kernel/dlsym/Gettimeofday/Nanosleep/ReadTsc, `malloc`/`free`/`operator new`/`delete`, `__cxa_throw`/`__gxx_personality`/`__cxa_pure_virtual`-use (no C++ exceptions), no `scePthreadSetaffinity/Rename/GetName`.
**libc used**: str/mem (strcasecmp 552, memcpy 417, memmove 204, memset, strlen, strcmp, memcmp, strncpy, strncmp, memchr, strstr, strncat, strncasecmp), `__cxa_guard_acquire/release` 192 (function-local statics, **thread-safety needed in shim**), `__cxa_atexit`, `atexit`, math (powf, atan2f, exp2f, acosf, asinf, tan, expf, modf, fmodf, rand/srand), printf family (snprintf, vsnprintf, sprintf, printf), stdio (fopen 3, fread, fwrite, fclose, remove), qsort, atof, `_Locksyslock/_Unlocksyslock` (31 each: Dinkumware internal locks - map to a mutex), `_FSin`, `_FLog`, `_Stoul`, `_Getpctype`, `_Getptoupper` (ctype tables: **shim must provide the PS4 libc's ctype table layout** - used in main FUN_000496f0 via `_Getpctype`; bit 2 = isxdigit-class test), `_ZSt..._Xlength_error/_Xbad_alloc/_Xout_of_range` (noreturn).
**Hard / careful items for the loader (Stage 3)**
1. **Stack protector**: 3246 `__stack_chk_fail` sites; the guard is read as `*(*(0xbc2278))` (global pointer slot at 0xbc2278, pointing to the guard) - a **data import** (`__stack_chk_guard` or similar, not in imports.tsv) the loader must define at that slot and must keep constant across threads (no fs: TLS used by game code).
2. **Direct memory** (13 sites in FUN_00598290 / 599be0 / 599e10): `AllocateDirectMemory(0, DMsize, len, align 0x200000, type 0)` + `MapDirectMemory(&addr, len, prot 0x33 (CPU RW|GPU RW), flags 0, off, align 0x200000)`; lengths rounded to 16 KB; up to 256 blocks. Shim: reserve/commit host memory at any address (code does not need fixed addresses; unverified: no code assumes the PS4 0x4xxxxxxxx range). Total request is ~0.5-1 GB+ (496 MB main heap + framebuffers + 'CB HEAP/DCB/CCB' per context).
3. **Event flags** with the semantics used (clear-then-wait in FUN_0059cfa0: `ClearEventFlag` + `WaitEventFlag` with mode/pattern read at asm level before shimming; `PollEventFlag` in FUN_0059d6d0). **PthreadAttr**: affinity masks (`(core+1)&0xffffffffffffff03` style for render workers), sched policy/params, inheritsched, stacksize: accept and ignore (host scheduler) but keep the **thread names** and entry ABI (SysV, entry takes void* in rdi).
4. SysV ABI on Windows: all callbacks (thread entries FUN_0059cfa0 / FUN_0059c4e0 / FUN_005a1930 / FUN_006308e0, error callback FUN_00568820, CRI malloc callbacks FUN_00568830/850, DcCore callbacks) are called by shims with SysV convention; shims called from eboot are SysV, extra args on stack (FUN_00568360 has 12 args).
5. Event/async APIs (Np*, SaveData, Dialog) return status codes polled each frame (`sceNpCheckCallback`, `sceSystemServiceGetStatus`, `*DialogUpdateStatus`); stubs must produce a consistent state machine (e.g. dialog status INITIALIZED -> FINISHED with result CANCEL).
6. AVX: eboot uses VEX instructions in game code (e.g. `vxorps ymm`) - host CPU must have AVX/AVX2 for the in-process phase.
7. Fonts: libSceFont is an actual rasterizer used for text (not statically linked in the eboot): needs a real implementation.

## 6. CRI runtime boundary
CRI FS 2.75.05 / AtomEx 2.14.10 (strings `CRI AtomEx/PS4 Ver.2.14.10`, `CRI Error/PS4 Ver.1.04.01`, `CRI UTF Retriever/PS4 Ver.1.00.02`) are statically linked:
code ranges (approx): **CRI FS 0x5aa..0x5b9 (criFs_*, Binder/Loader/Group loader)**, **CRI Atom/AtomEx 0x5c0..0x5ee** (+ASR/HCA/DSP/D-BAS up to ~0x5f5xx; AudioOut calls at 0x5f4580..0x5f4650),
**criErr / criThread / UTF / mem 0x62b..0x631, 0x635..0x636**, **CRI I/O glue to the PS4 OS** 0x5ee490..0x5efb80 + 0x639a20..0x639df0 and OS wrappers 0x5bbd90..0x5bc4c0
(`sceKernelOpen/Close/Stat/Fstat/Read/Lseek/Write/...`). 97 CRI functions are called from outside the CRI ranges (`notes/raw/engine/cri_boundary.tsv`); callers are only the
game wrappers: init/mount (FUN_00568520, FUN_00017d80 -> FUN_00568870/568920/5689b0/5689e0/568b60), resource loader (FUN_0056dda0, FUN_0057bb00 -> FUN_0056dcc0,
FUN_0056d8xx..0056d9f0), sound manager (FUN_00582xxx..0058a790 and game UI callers 0x68090..0x68ae0 / 0x69040), Atom init (FUN_00584e30), draw path (not CRI).
Proposed names for the entry functions (error-string anchored where "string" is noted; others by role, unverified):
| addr | proposed API | basis |
|---|---|---|
| 0x5aaf10 | criFs_Initialize(config) | string "criFs_Initialize function has been called twice" |
| 0x5ab3b0 | criFs_FinalizeLibrary | string (internal callee of finalize) |
| 0x5ab4d0 / 0x5ab4f0 | criFs_SetSelectIoCallback / criFs_SetUserMalloc-style callback setters (args FUN_00568830 / FUN_00568850) | called from FUN_00568520, 24 B each (unverified which) |
| 0x62bed0 | criErr_Initialize / SetCallback (arg FUN_00568820) | string "CRI Error/PS4 Ver" |
| 0x5ac4c0 | criFsBinder_Create | string "...created by criFsBinder_Create" in 5adf90 |
| 0x5adb40, 0x5adf90, 0x5ae2e0, 0x5ae750, 0x5aec90, 0x5ac260 | criFsBinder_BindCpk / BindFile / Unbind / SetPriority / GetStatus ... | E-code strings; FUN_00568870/5689e0 are the game wrappers |
| 0x5b7820, 0x5b7960, 0x5b79b0, 0x5b7a40, 0x5b86b0, 0x5b8740, 0x5b8800, 0x5b8540, 0x5b8940 | criFsLoader_* (Create/Load/Stop/Status/GetStatus) | strings (Can not load over 2GB, criFsLoader_Create ...) |
| 0x5b4c30 | criFsGroupLoader_LimitNumPreparingFiles (internal) | string |
| 0x5cd210 / 0x5cd420 | criAtomEx_Initialize / criAtomEx_Finalize | strings "initialized twice / finalized before" |
| 0x5c1870 | criAtomEx_UnregisterAcf | strings "ACF is not registered", "Unload all ACB before unregistering ACF" |
| 0x5c7410, 0x5c7520, 0x5ccf40, 0x5cd060, 0x5cd470 | criAtomEx_RegisterAcfData / RegisterAcfFile / GetWorkSize / ... (called from FUN_00584e30 in this order) | unverified |
| 0x5c0ef0..0x5c1390 | criAtomExAcb_LoadAcbFile / LoadAcbData / Release / GetCueInfo ... | E-codes only |
| 0x5c8e60, 0x5c9330, 0x5c9ce0 | criAtomExPlayer_Create (string "Failed to create CriAtomExRngHn"), _Start (string "Can not find specified cue ID", "Failed to create cue"), cue/Set functions | strings |
| 0x5c58e0, 0x5c5f10, 0x5c5f70 | criAtomExPlayer_AttachFader / criAtomExFader_* | strings |
| 0x5ca960..0x5cbb10 (~14 small fns) | criAtomExPlayer_Set* (volume, pitch, pan, aisac, category ...) | E-codes, small size |
| 0x5ecbf0, 0x5ecea0, 0x5ed110 | criAtomDbas_Create / criAtom_Initialize helpers | string "D-BAS buffer size" |
Other CRI used by the game: criFsLoader, criAtomExPlayer, criAtomExAcb (ACB/AWB loaded via criFs), criAtomEx_ExecuteMain (not identified, probably one of the 24-byte fns 0x5ceda0 / 0x5cedf0 called from the sound manager).
**What a CRI shim replaces**: nothing is needed for a first boot. CRI FS opens CPKs via `sceKernelOpen/Stat/Read/Lseek` (host files, `/app0/...` path mapping) and Atom outputs via
`sceAudioOutOpen/Output(s)` (host mixer, 48 kHz S16/float: format arg to verify) running on `CriThread` threads; HCA decode, CPK parsing and ACB logic are original x86-64 code.
If we later replace CRI (Ship-of-Theseus), the replaceable seams in order of payoff: (a) the game wrappers FUN_00568870/568920/5689b0/5689e0 (mount) and FUN_00584e30 + sound-man 0x582..0x586 (cue play API: our dcp_audio + HCA decoder), (b) the 97 boundary entries above, (c) OS glue 0x5bbd90..0x5bc4c0 (file I/O) for the CPK reader (our dcp_cpk).

## 7. Function table
| addr | proposed name | role | conf |
|---|---|---|---|
| 54c690 | entry | libc init, atexit, init-array runner (FUN_00000020), main | high |
| 496f0 | main | see section 2 | high |
| 568360 | Engine_Bootstrap | creates engine + heaps (tags HMAN/HTSK/...), calls platform init | med |
| 568510 / 568800 | GetPlatform / GetEngine | return singleton ptrs 0xe63cb8 / 0xe78708 (53 / 382 callers) | high |
| 598290 | Platform_Init | sysmodules, VideoOut, direct memory, pad/user/save/Np init, GNM contexts, flip/vblank eqs | high |
| 5981c0 | LoadSysModules | 19 sceSysmoduleLoadModule | high |
| 599be0 / 599e10 | DirectMemoryBlock_Alloc | Get/Allocate/MapDirectMemory + registry (max 0x100) | high |
| 568520 | Engine_Init | CRI FS init, task tree roots, managers, render threads | med |
| 17d80 | MountCpks | DRACRO.cpk + /app0/DRACRO_US.cpk | high |
| 584e30 | SoundMan_Start | criAtomEx init + ACF registration (SND/DC_SOUND.ACF) | med |
| 16b30 | DcCore_ctor | game core class ('DCOR', vtable 0x8bf9b8) | med |
| 5687c0 | Engine_Step | one iteration of the endless main loop | high |
| 5a1f60 | Frame_Run | update + frame hand-off + vblank + pacing | high |
| 56fb80 | Update_Ticks | pad + user events + task tree, 1..3 ticks | high |
| 58def0 | TaskTree_Run | 6 phase task update | med |
| 5a2970 | Pad_Update | scePadReadState / vibration | high |
| 59b090 | UserEvents_Poll | sceUserServiceGetEvent + pad open/close | med |
| 59d6d0 / 59d730 | RenderThread_IsIdle / Submit | event flag poll / set | high |
| 5a1c60 / 59ce20 / 59c3b0 | RenderDL_Init / Create / CreateWorker | thread creation, event flag | high |
| 59cfa0 | RenderDLThread_Main | master render thread | high |
| 59c4e0 | RenderDLWorker_Main | worker thread loop | high |
| 5a3a00 | LayerBuf_Execute | viewport/state + sort + Execute per command | high |
| 5a3be0 / 5a4570 | CmdSort_Asc / CmdSort_Other | introsort by +0x54 then +0x50 | med |
| 597830 | DrawList_Enqueue | push cmd ptr into bucket vector | high |
| 56f6a0 | GetLayer | core+0x20 + id*0xb30 | high |
| 57ae20 | Pool_Alloc | ring/bump allocator for command objects | med |
| 5977f0 | LayerBuf_ClearBuckets | vec.end = vec.begin for all buckets | high |
| 59a420 / 639610 | LayerState_Set / SetViewport | per-layer GNM state, viewport+scissor | med |
| 599f70 | GfxFrame_Begin | context reset, default HW state, clear viewport | med |
| 59a2f0 / 59a1e0 | GfxFrame_Submit / FrameEnd | EOP fence, submit+flip, SubmitDone, ring advance | high |
| 638fd0 / 639020 | Gfx_SubmitRetry / Gfx_SubmitAndFlip | -> sceGnmSubmitAndFlipCommandBuffers | high |
| 638bb0 / 638bc0 | Gfx_Submit | -> sceGnmSubmitCommandBuffers | high |
| 561480 | Dcb_WriteEopLabel | PM4 EVENT_WRITE_EOP packet | med |
| 55fef0 / 560710 | Dcb_InitHwState / Dcb_DrawIndex | wrappers (reserve-callback pattern at ctx+0x18) | high |
| 5a1bb0 / 5a1930 | Thread_Start / Thread_Entry | generic thread class | high |
| 6305f0 / 6308e0 | criThread_Create / CriThread_Entry | CRI threading | med |
| 5aaf10 / 5ac4c0 / 5cd210 | criFs_Initialize / criFsBinder_Create / criAtomEx_Initialize | CRI | high / med / high |
| 56efc0 (56f340 / 56f350 / 56f360) | Heap_Alloc (alloc / alloc-fallback / free) | game heap; 359 / 30 / 1445 callers | med |

## 8. Open questions
- Meaning of the 12 args to FUN_00568360 and of the 11 FUN_005712d0 pool carve-ups (heap partition layout).
- Command classes X1/X2/X4 and the two ctors FUN_000250c0/FUN_00025120: roles (UI quad, fill, post-process?) - read their Execute bodies before declaring the Stage 4 list complete; check for classes that derive indirectly (not set via base vtable 0x8bfbf0 in the ctor).
- Does `Execute` ever touch state shared between layers (render target switch, depth, blend defaults set by FUN_0059a420)? Needed to decide per-layer independence.
- `core+0x244` tick count semantics and whether skipped ticks run the whole update or only part (FUN_00597830 drops commands except on the last tick).
- Exact event-flag modes used (wait pattern/mode args) and `sceKernelCreateEventFlag` attributes; whether the loader can rely on a plain host event implementation.
- Data import at 0xbc2278 (stack guard) and which other GLOB_DAT data imports exist (imports.tsv lists functions only).
- Which per-frame CRI calls happen (criAtomEx_ExecuteMain, criFs_ExecuteMain): found inside virtual task methods, not located yet.
- Threads created by libraries (Rudp I/O thread via sceRudpEnableInternalIOThread, NpToolkit) - external to eboot code.
- libc `_Getpctype` ctype table format expected by the eboot.
