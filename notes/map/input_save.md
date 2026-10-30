# Input, save data, settings/system (Stage 5 groundwork)

Mapper notes, survey depth. Addresses are eboot-relative (as q.py). "(unverified)" = inferred, not read in asm.
Raw Ghidra dumps: `notes/raw/input_save/` (ignored). Builds on `engine.md` (do not repeat boot/loop).
Platform singleton = FUN_00568510 (0xe63cb8), engine = FUN_00568800; **pad system = engine+0x68**.

## 1. Summary
- **Input** is a tiny, clean layer. One function, **FUN_005a2970 `Pad_Update(padSys)`**, runs once per logic tick (inside
  Update_Ticks, before the task tree), polls `scePadReadState` + `scePadGetControllerInformation` for up to **4 pads**, and converts
  the raw `ScePadData` into the game's own per-pad struct (stride 0x1d0, 4 entries, 0x740 bytes + 2 flag bytes). Everything else
  in the game reads that struct (never the SDK again). A PC shim only has to fake those 3 imports (ReadState, GetControllerInformation,
  Open/Close/SetVibration/SetLightBar) **or** write the struct directly. Rating **routine**.
- **Save data** is one class, `DcSaveLoad` (name string, ctor FUN_0008c800, vtable obj 0x8c8838), one **SAVEn.DAT file per local
  player slot** inside the SaveData dir `SAVEDATA00`. The payload is a **flat in-memory struct memcpy'd as one blob (0x20eb18 =
  2 157 336 bytes), little-endian, CRC-32 (standard zlib CRC-32, poly 0xEDB88320, init/xorout 0xffffffff) in the first 4 bytes over
  bytes [4, size)**. Written/read with plain `sceKernelOpen/Write/Read` on the path returned by `sceSaveDataGetMountInfo`, so a
  directory-backed Mount2 shim gives byte-for-byte compatibility. Rating **routine** for the shim, **careful** for any later
  Ship-of-Theseus replacement of the struct (not decoded below the section level).
- **Settings/system** is minimal: only `sceSystemServiceParamGetInt(LANG=1)` -> game language enum at DcCore+0x130. No region/age/time
  use. Trophies are 3 trivial vtable methods; stub list in section 5.

## 2. Input

### 2.1 Call sequence
| step | function | detail |
|---|---|---|
| init | FUN_00598290 (platform init) | `sceUserServiceInitialize`, `scePadInit`, `sceUserServiceGetInitialUser`, `GetUserName` (once) |
| per tick | **FUN_0059b090 `UserEvents_Poll`** | drains `sceUserServiceGetEvent` (struct: u32 eventType, u32 userId). type 0 = LOGIN -> `FUN_005a2f70` picks a free pad slot, `FUN_005a3060` = `scePadOpen(userId, 0, 0, NULL)`. type 1 = LOGOUT -> `FUN_005a3030` finds the slot with that user, `FUN_005a30a0` = `scePadResetLightBar` + `scePadClose`, then sets padSys+0x741 = 1 (changed flag, (unverified) consumer = co-op join/leave UI). |
| per tick | **FUN_005a2970 `Pad_Update`** | `scePadReadState(handle, &ScePadData)` for each of 4 slots whose handle (+0x44) >= 0; sets valid byte (+0x00) only when ReadState==0 and `connected`; then `scePadGetControllerInformation`. Early-out if platform singleton missing or `FUN_0059b150` (always returns 0; looks like a "suspended" hook). Ends with **FUN_0057b220 `Pad_Edges`**. |
| on demand | FUN_005a3100 (caller FUN_00398fe0) | `scePadSetLightBar(handle, {r,g,b})` - player-colour light bar |
| on demand | inside Pad_Update | if byte +0x1b8 set: `scePadSetVibration(handle, {large=+0x1ba, small=+0x1b9})`, then clear +0x1b8 (one-shot request) |
Touchpad: only the pad's own touch points are read (no touch-driven UI found): see struct. Motion sensor, `scePadSetMotionSensorState`,
`scePadSetTiltCorrection`, `scePadSetLightBarBaseBrightness` etc. are not imported (8 pad imports total).

### 2.2 Pad slots / users
Up to **4** slots (padSys+i*0x1d0). Slot allocation `FUN_005a2f70(padSys, userId)`: if padSys+0x740 == 0, first slot with user id (+0x28) == -1 (sequential fill; returns 4 = full); if +0x740 != 0, slot whose
*previous* user (+0x2c, set on close) equals this user is preferred (rejoin keeps the slot), else first never-used/empty slot. Close keeps the user id in +0x2c only when +0x740 != 0.
Handle at +0x44 (-1 = closed), user id at +0x28 (-1 none). Slot 0 is the primary player; the slot index doubles as the **player index** for local co-op (up to 4; matches the 4 `DcVirtualMachine` objects, FUN_0007c590, which register in 0xc1dc40[4]). Join/leave is purely the user-service login/logout event + pad open/close (no separate "press START to join" found in this layer, (unverified) it lives in the task tree reading the slot-valid byte).

### 2.3 Game pad struct (engine+0x68), 0x1d0 per pad
| off | type | meaning |
|---|---|---|
| +0x00 | u8 | valid this tick (ReadState ok + connected) |
| +0x04/+0x08 | f32 x2 | touch point 1 x,y (raw / (pad resolution) * const DAT_0079f5c8, normalised) |
| +0x0c/+0x10 | f32 x2 | touch point 2 x,y |
| +0x14/+0x18, +0x1c/+0x20 | f32 x2 | per-point delta vs last tick (sentinel value DAT_00e93850/54 when not touching both ticks) |
| +0x24 / +0x25 | u8 | touch count now / previous |
| +0x28 / +0x2c | i32 | user id / previous user id (-1 none) |
| +0x44 | i32 | scePad handle (-1 closed) |
| +0x48 | u64 | **button mask** (current), layout below |
| +0x50 / +0x54 | f32 | left stick X / Y in [-1,1] (0 inside deadzone) |
| +0x58 / +0x5c | f32 | right stick X / Y |
| +0x60 / +0x64 | f32 | L2 / R2 analog (0..1) |
| +0x68..+0x6e | u16 x4 | autorepeat config (+0x6a first-repeat delay frames, +0x6c repeat start, +0x6e interval; exact roles unverified) |
| +0x70 | u64 | **pressed edge** = cur & ~prev (Pad_Edges) |
| +0x78 | u64 | **released edge** = prev & ~cur |
| +0x80 | u64 | **repeat mask** (bit set on press and on each autorepeat tick) |
| +0x88 | u64 | previous button mask |
| +0x98.. | i32[24] | per-bit hold counters; +0xf8.. per-bit release counters; +0x158.. per-bit decay counters (+13 on a press edge) |
| +0x1b8..+0x1ba | u8 | vibration request flag, small motor, large motor |
| +0x1bc/+0x1c0, +0x1c4/+0x1c8 | f32 | stick-as-digital thresholds: Y axis (neg stored positive, pos), X axis (neg, pos) |
Padding to 0x1d0; padSys+0x740 / +0x741 = slot-mode flag / "pad set changed" flag.
Whole 0x1d0 block is zeroed by nothing but the first 4 x 32-byte clears at +0x48, +0x218, +0x3e8, +0x5b8 (the four button/stick heads) each tick, so the **button mask is rebuilt every tick**.

**Button mask layout** (game bit <- `ScePadData.buttons` PS4 bit):
0x1 UP (0x10), 0x2 DOWN (0x40), 0x4 LEFT (0x80), 0x8 RIGHT (0x20), 0x10 TRIANGLE (0x1000), 0x20 CIRCLE (0x2000), 0x40 CROSS (0x4000), 0x80 SQUARE (0x8000),
0x100 OPTIONS (0x8), 0x200 TOUCHPAD click (0x100000), 0x400 L1, 0x800 R1, 0x1000 L3, 0x2000 R3, 0x400000 L2 digital (0x100), 0x800000 R2 digital (0x200).
Synthetic stick-direction bits (analog -> digital when stick beyond the threshold): 0x4000 / 0x8000 = left stick Y -/+, 0x10000 / 0x20000 = left X -/+,
0x40000 / 0x80000 = right Y -/+, 0x100000 / 0x200000 = right X -/+ (in the code the Y(neg) = 0x4000 pairing is read from the asm; which direction is "up" depends on the stick-Y sign convention, check before use).
(0x400/0x800/0x1000/0x2000 mapping read from the bit-twiddling in pseudo-C: L1/R1 pass through, L3/R3 shifted; treat as "high confidence" for the group, verify each bit against a shim test.)

**Raw -> float normalisation**: sticks are u8 with centre 0x80. Deadzone per stick = `ScePadControllerInformation.stickInfo` bytes (left in low byte, right in high; used only if the controller reports connected, else 0); if |v-0x80| <= dz the value is 0, else `v*2/255 - 1`. Triggers `v/255`. Touch positions scale by the controller's reported touchpad resolution (u16 x, y at +4 of ControllerInformation).

**What a PC shim must supply**: `ScePadData` (buttons u32, left/right stick u8 x4, L2/R2 u8, touch count u8 + 2 points {x u16, y u16, id}, `connected` byte) and `ScePadControllerInformation` (touchpad resolution u16 x/y, deadzones u8 x2 (recommend 0x0d-ish or 0), `connected` byte), plus a `ScePadOpen` that returns a handle >= 0 per user. Simplest intercept = the 3 SDK imports; alternative = replace FUN_005a2970 and write +0x00..+0x88 directly (must still call Pad_Edges FUN_0057b220 for edges / repeat). Fake one login event for the initial user (UserService GetEvent returns "none" otherwise; engine already open pad via GetInitialUser path? (unverified) - ensure slot 0 opens on first event).

## 3. Save data

### 3.1 Class and call sequence
`DcSaveLoad` (state-machine task, vtable obj 0x8c8838, ctor FUN_0008c800, vptr slot dispatch FUN_0008cbc0 by mode: **1 = FUN_0008cbf0 (create/initialise)**, **2 = FUN_0008d880 (save)**, **3 = FUN_0008e010 (load)**; mode meanings inferred from which worker thread each starts (unverified naming), owner = `DcVirtualMachine` (param+0x20): +0x74 = player slot index 0..3, +0xe8 = save buffer, +0xa8 = message/UI manager, +0xd8 = UI sub-objects). Request flags at task+0x50: bits 0x1000 / 0x2000 / 0x4000 (= requests for modes 1/2/3, polled+cleared by FUN_0058dc40).
Each mode runs a state machine (`task+0x5c`), calling a **generic Thread object (Thread_Start FUN_005a1bb0, `Run` vtable slot +0x10)** so the actual filesystem work is off-thread; the main task polls `FUN_005a1b90` (thread done) and a timer state (FUN_00018240 divisor: waits ~60 frames x 1.? scale x retry up to 10 ("case 4" in 8cbf0), then goes to error state 7 -> dialog).
Worker `Run` bodies (vtable objects with Run at slot +0x10):
| vtable obj | Run | role |
|---|---|---|
| 0x8c8960 | **FUN_0008f270** | **write**: `snprintf(dirname, 0x20, "%s", "SAVEDATA00")`; `sceSaveDataDirNameSearch` (user id = FUN_00568cc0 = platform+0x14a38, max 0x400 entries, key = dirname) to see if the directory exists; `sceSaveDataMount2` (user, dirName, blocks, mode) with **mode 2 (RDWR) if existing else 0x16 (RDWR\|CREATE\|COPY_ICON)**; blocks = max(0x60, ((size*1.1 + 0x190000) >> 15)) (32 KiB blocks); `sceSaveDataGetMountInfo` -> mountPoint; path `"%s/%s"` mountPoint + `SAVEn.DAT`; `sceKernelOpen(path, 0x602 = WRONLY\|CREAT\|TRUNC, 0777)`; `sceKernelWrite(fd, buf->ptr, buf->size)`; close; build `SceSaveDataParam` (0x530 B: title 0x80 / subTitle 0x80 / detail 0x400 from the three strings of the task) -> `sceSaveDataSetParam(mount, PARAM, &p, 0x530)`; `sceSaveDataSaveIcon` (buffer + size from the task, the icon is `other/SAVEDATA/ICON0.PNG` loaded from the CPK); `sceSaveDataUmount`. Errors are stored at task+0x38->+0x10. |
| 0x8c8988 | **FUN_0008f670** | **read**: same dirname/Mount2 (read mode), `sceKernelStat` + `sceKernelOpen` + `sceKernelRead` of SAVEn.DAT into the buffer, close, Umount |
| 0x8c89d8 | FUN_0008f940 | **delete**: `sceSaveDataDelete(user, dirname)` (FUN_0008f890 is an identical sibling) |
Dialog (FUN_0008fa60, a per-task poll function called while an error/ask state is active, + FUN_0008fd30): `sceCommonDialogIsUsed`, `sceSaveDataDialogInitialize/Open/UpdateStatus/GetResult/Terminate`; two uses: a save-data dialog with a user/system message (struct 0x30 + 0x98 B, `FUN_0008fa60` case 4; UI result -> task+0x864) and an error-code dialog (FUN_0008fd30, error code from task+0x860). Dialog status poll expects `UpdateStatus` == 3 (FINISHED), then GetResult (field 8 == 1 means "OK/Yes", stored to +0x864).
Error mapping FUN_0008fa00 (SDK error -> game result 0..6): 0x809f0008 -> 3, 0x809f000a -> 2, 0x809f000f -> 4, 0x809f0011 -> 5, other 0x809f00xx -> 6; kernel 0x8002xxxx -> mostly 0 (ignored) except a short list -> 6. (Pairing to SDK names such as NO_SPACE / BROKEN / NOT_FOUND: unverified.) `FUN_0008f9f0` = has-error (task+0x88 != 0).
File name table at 0x8c8910: `SAVE0.DAT`..`SAVE3.DAT` indexed by the owner's player index (+0x74). Dir name `SAVEDATA00` is the same for all. Extra: `FUN_0008e610` builds the 3 title strings (localised: English, German, Spanish, Italian variants; the game-language is picked by DcCore+0x130, French uses English fallback? (unverified)) and a "TOTAL " string used in the detail field; it reads u32 at buf+0x20eb10 (likely a total counter) into the detail text.

### 3.2 Payload layout (as read)
Blob size **0x20eb18** (allocated via heap vtable 0xe7c560 +0x10 in FUN_0008d880, the buffer descriptor is `{ptr, u32 size}` at task+0x?). Little-endian x86 in-memory struct, no per-field serializer: the whole struct is one contiguous object `memcpy`'d to/from the file.
| off | size | meaning |
|---|---|---|
| 0x00 | u32 | CRC-32 of bytes [4, 0x20eb18) (standard CRC-32; table-driven FUN_00572340(data, len, init=0xffffffff, xorout=0xffffffff, table 1 = zlib table at 0x7738e0). Computed **only on save** by FUN_0008c730, which also copies the checked blob to a fresh heap buffer. A second table (0x7734e0) is also available (other polynomial; not used on this path). |
| 0x04..0x4f | 0x4c | header / global section (not decoded) |
| 0x50 + i*0xaf8d4 | 3 x 0xaf8d4 (= 718 036) | **three save files ("DATA 1..3" in the game UI)**, not three characters (FUN_00081830 gives the pointer for i < 3). Each file holds up to 3 playable characters plus ~28 allies in one hero array (74 entries, stride 0x21d0, at slot+0xa480; the user's characters are indices 0,1,2); layout in `story_progress.md`. FUN_0008c170 initialises a slot: item box at slot+0x70 (500 x 0x54, FUN_00046330), hero array at slot+0xa480 (FUN_0008a9d0 / 8acc0 / 88d10 / 8b960), "initialised" struct at slot+0xaf20c (FUN_0008c5f0 / 8c600). A file counts as empty while the u32 at slot+0xaf234 (play time) is 0 (FUN_0008bfa0); FUN_0008beb0 creates it (header, allies, defaults). |
| 0x20eacc..0x20eb0f | ~0x44 | tail section (not decoded) |
| 0x20eacc | 0x39 x u8 | account-wide bool table (collection marks; FUN_00081850 sets an entry via the id map FUN_00070d50 and updates a count with FUN_00072520; all 0 in the checkpoint save) |
| 0x20eb07 | 6 x u8 | settings block (CRC'd by FUN_0008bbb0; checkpoint save: 10,10,10,10,0,1) |
| 0x20eb10 | u32 | counter shown as "TOTAL" in the save-data detail text (grows between saves, 11791 -> 11830; unverified: play time) |
| 0x20eb14 | u8 | current slot index (FUN_00081980) |
| 0x20eb15 | u8 | index into the live-slot pointer table 0xc1dc60 (FUN_00081810 returns the *current slot* pointer from it) |
Characters, inventory and story progress live **inside the 3 x 0xaf8d4 slots**; decoded in `story_progress.md`. Verification of the CRC on **load** was not found in FUN_0008f670 / the load state machine (only generation seen): (unverified) the check may be in FUN_0008e610's caller or absent; keep the bytes identical anyway.
No encryption, no MD5/SHA found on the save path. Icon: PNG from the CPK, passed raw to `sceSaveDataSaveIcon` (the shim can ignore it).

### 3.3 Triggers
Requests are raised by setting bits 0x1000 / 0x2000 / 0x4000 on the DcSaveLoad task (setter vtable methods around 0x58dbd0..0x58dc20 are the *test-and-clear* getters; setters not located). Autosave call sites (which gameplay events request mode 2) **not mapped** (open question). The DcSaveLoad object is created in the FUN_00016da0 setup chain (FUN_0007c590 creates DcVirtualMachine, 8c800 follows).

### 3.4 What a PC save shim must provide
- `sceSaveDataInitialize3` (stub), `sceSaveDataDirNameSearch` (list sub-dirs of `<root>/<userId>/`; entries are 32-byte names; result count at +0), `sceSaveDataMount2` (mount params struct read in 8f270: {userId u32, dirName ptr, blocks u64, mountMode u32}; return mountPoint struct {char path[0x10]} via `GetMountInfo` / in the mount-result struct; map to a host directory and return a path such as `/savedata0`, then make `sceKernelOpen/Stat/Read/Write` resolve that prefix - the CRI file glue also sees `/savedata` (string 0x7a36d2 in FUN_005bbd90), check that routing), `sceSaveDataUmount`, `sceSaveDataSetParam` (store the 0x530 B param, optionally into a sidecar), `sceSaveDataSaveIcon` (ignore or store), `sceSaveDataDelete`, `sceSaveDataGetMountInfo` (blocks free/used: return generous values; the game stores nothing from it as far as read).
- Dialog stubs: `sceCommonDialogIsUsed`=false, `sceSaveDataDialogOpen` etc. returning immediate FINISHED(3) with result OK (field 8 = 1) or a console message.
- Files: `SAVE0.DAT..SAVE3.DAT`, plain, exact bytes, 2 157 336 B each. The user's ps4 saves, if copied over, work as-is only if the endianness/layout is unchanged (it is x86 little-endian, so yes).
- Rtc: only `sceRtcSetTick`/`GetCurrentClockLocalTime`/`GetWin32FileTime` (FUN_00572370/572470/5723e0) convert time into the 8-byte date struct stored with saves/ranking (u16 year, u8 month/day/hour/min, u8 pad...): shim = host clock conversion.

## 4. Settings / system
- **Language**: `sceSystemServiceParamGetInt(1 = LANG)` in FUN_00016da0 (setup, case 0 of the startup state machine) and FUN_00017750. Mapping to DcCore+0x130: PS4 lang 2 or 0x16 (French / French-Canada) -> 3, 3 or 0x14 (Spanish / Latin Spanish) -> 4, 4 (German) -> 5, 5 (Italian) -> 6, everything else (incl. Japanese, English) -> 1. A getter (FUN_00025740, 85 callers) uses `this+0x130 - 2 < 5` to select the message-sheet table (entries per language, e.g. MSGSHEET_SYSTEMMSG_US_FMS default) - these are the per-language resource names; the shim just returns the desired language int. 0x134 = flag set to 1 afterwards. The startup state machine also resolves `SND/...ACB/AWB` names here, so **language must be known before the first scene** (already read at DcCore start).
- No region, age (`sceSystemServiceParamGetInt` is called with id 1 only), time-zone or enter-button queries. The
  confirm/cancel layout is the game's own flag DcCore+0x134 (0 = Circle confirms, JP), set to 1 (Western) at startup:
  see confirm_cancel.md (superseded the earlier "hard-wired" note here; hle/confirm_button.cpp switches it).
- Other per-frame imports already listed in engine.md; nothing new here: `sceSystemServiceGetStatus` per frame (FUN_0059a2f0, return struct with all zero flags is enough), `sceNpCheckCallback`.
- `FUN_0059bf60`: sign-in dialog helper (`sceSigninDialogInitialize/Open`, `GetInitialUser`), stub: return false.

## 5. Trophies (sceNpTrophy)
Platform vtable holds 3 helpers (data refs 0xbc0a08 / 0xbc0a48 / 0xbc0a88): **FUN_0059f020** `Create/Register context` (`sceNpTrophyCreateHandle`, `RegisterContext`, `DestroyHandle`), **FUN_0059f080** `GetTrophyUnlockState`, **FUN_0059f0f0** `UnlockTrophy`. Init: **FUN_0059b7f0** `sceNpTrophyCreateContext(&ctx(platform+0x12498), userId(platform+0x14a38), 0, 0)` once (if ctx == -1). Stub list (all return 0 / no-op, handle/context ints arbitrary): `CreateHandle`, `DestroyHandle`, `CreateContext`, `RegisterContext` (the trophy count is not read here; return 0 means "ok", optionally return ready state), `GetTrophyUnlockState` (flags = all 0, count 0), `UnlockTrophy`. The game never inspects the return of unlock beyond ignoring it (unverified). For a Windows build later map to Steam/GOG achievements or log only.

## 6. Function table
| addr | proposed name | role | conf |
|---|---|---|---|
| 59b090 | UserEvents_Poll | sceUserServiceGetEvent -> pad open/close | high |
| 5a2970 | Pad_Update | ReadState + ControllerInfo -> game pad struct | high |
| 57b220 | Pad_Edges | pressed/released/repeat per button, 4 pads | med |
| 5a2f70 / 5a3030 | PadSlot_FindFree / PadSlot_FindUser | slot allocation | high |
| 5a3060 / 5a30a0 / 5a3100 | Pad_Open / Pad_Close / Pad_SetLightBar | scePadOpen / Close+ResetLightBar / SetLightBar | high |
| 59b150 | Pad_IsSuspended (always 0) | 3-byte stub | med |
| 59bf60 | SigninDialog_Open | | med |
| 8c800 | DcSaveLoad_ctor | class 'DcSaveLoad', vtable 0x8c8838 | high |
| 8cbc0 | DcSaveLoad_Step | dispatch by mode -> 8cbf0 / 8d880 / 8e010 | high |
| 8cbf0 | DcSaveLoad_ModeInit | state machine: icon load, build params, start write thread | med |
| 8d880 | DcSaveLoad_ModeSave | state machine (mode 2) | med |
| 8e010 | DcSaveLoad_ModeLoad | state machine (mode 3) | med |
| 8e610 | SaveParam_Build | title/detail strings by language, 'TOTAL', buf+0x20eb10 | med |
| 8f270 | SaveThread_Write | DirNameSearch, Mount2, write SAVEn.DAT, SetParam, SaveIcon, Umount | high |
| 8f670 | SaveThread_Read | Mount2, Stat, Open, Read, Umount | high |
| 8f890 / 8f940 | SaveThread_Delete | sceSaveDataDelete | high |
| 8fa60 / 8fd30 | SaveDialog_Poll / ErrorDialog_Open | SaveData/Common dialog state machine | med |
| 8fa00 | SaveError_Map | SDK error -> game result 0..6 | high |
| 8c730 | SaveBuf_Finalize | CRC-32 stamp + copy of 0x20eb18 blob | high |
| 572340 | Crc32 | table-driven (tables 0x7734e0, 0x7738e0 = zlib) | high |
| 8c170 | SaveSlot_Init | slot reset (3 substructs) | med |
| 81830 / 81980 | SaveBuf_GetSlot / GetCurIndex | slot i at +0x50 + i*0xaf8d4; byte at +0x20eb14 | high |
| 59b7f0 | Trophy_CreateContext | | high |
| 59f020 / 59f080 / 59f0f0 | Trophy_Register / GetUnlockState / Unlock | vtable helpers | high |
| 16da0 / 17750 | DcCore_Startup / DcCore_ReadLanguage | ParamGetInt(LANG) -> +0x130 | med |
| 572370 / 572470 / 5723e0 | Rtc_* | tick / local time / win32 filetime -> 8-byte date | high |
| 58dc40 | Flag_TestClear | request-flag helper (bits 0x1000/0x2000/0x4000) | high |

## 7. Dependencies, open questions
- Deps: engine.md (task tree phases, thread class FUN_005a1bb0 used for save worker); kernel file I/O shim must route the SaveData mount point; UserService user id (platform+0x14a38, FUN_00568cc0); CommonDialog.
- Open: (1) where requests are raised (autosave triggers: after stage clear? shop? menu) - set 0x1000/0x2000/0x4000 on the DcSaveLoad task, find setters / callers of the DcSaveLoad pointer (owner +0x?); (2) load-time CRC verification (not found); (3) full layout of the 3 slots (0xaf8d4 each) and the 0x4c-byte header; (4) exact owner of repeat-config u16s and how the game maps player index <-> pad slot for co-op beyond slot id; (5) stick Y sign convention; (6) ScePadData/ControllerInformation field offsets taken from SDK memory, confirm with the 3 reads in 5a2970 (offsets of buttons/sticks match, touch fields partly); (7) save dir name format "%s" only (no slot number) - the per-slot separation is by file name.
