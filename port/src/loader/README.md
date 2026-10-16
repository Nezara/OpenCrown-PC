# Stage 3 loader prototype (layer 1)

`dcp_run <eboot.bin> [--dry]` maps the user's decrypted eboot into the process, relocates it, binds every import,
and (without `--dry`) calls the entry point until the first fault. The goal is to find out where it stops, not to run the game.

## Pipeline
1. `unwrap_self` turns a fake-signed SELF into a flat ELF (`0x800` data entries only; encrypted/compressed input is refused).
2. `parse_elf` reads the PT_LOAD and PT_SCE_RELRO (0x61000010) segments, then the SCE dynamic tags, whose offsets are
   relative to PT_SCE_DYNLIBDATA. The tags used are strtab 0x61000035/37, symtab 0x61000039/3F, rela 0x6100002F/31,
   jmprel 0x61000029/2D and DT_NEEDED.
3. `map_image` reserves the image at 0x400000 (the PS4 default; the image is PIE, so any base works as a fallback),
   copies the segments, and leaves bss zeroed (bss is fresh committed memory). `relocate` applies RELATIVE, 64,
   GLOB_DAT and JUMP_SLOT, and counts any other type. `protect_image` then sets each segment to r-x, r-- or rw-.
4. `Binder` resolves each import (`nid#lib#mod`) in this order: a registered HLE shim (`register_hle(lib, name, fn)`),
   then zeroed storage for STT_OBJECT imports (vtables, `Need_sceLibc`, `__stack_chk_guard`, which gets a nonzero
   value), and otherwise a generated 28-byte thunk. The thunk writes the import index to a global and jumps to
   `unimpl_handler`, which logs `UNIMPL lib::name(rdi..r9)` once per import, keeps a 16-entry ring for crash reports,
   and returns 0. The NID to name table is built at startup from `sce_names.inc` (SDK API names only) using our own
   SHA-1.

## ABI
Game code is SysV x86-64: arguments go in rdi, rsi, rdx, rcx, r8, r9; only rbx, rbp and r12-r15 are callee-saved;
there is no shadow space. Our host code is Win64. Every function the game calls (shims, stubs, exit callbacks) is
declared `DCP_SYSV` (`__attribute__((sysv_abi))`), so GCC generates the correct prologue and the register saves
needed when calling back into Win64 code. Every pointer to a game function we call (the entry point, and later thread
entries and CRI callbacks) has to be cast to a `DCP_SYSV` function type.

MinGW gotcha: GCC 16 cannot emit SEH unwind data for a `sysv_abi` function that needs EH cleanups. It fails with
"`.seh_handlerdata` used outside of `.seh_proc`". To avoid this, keep every SysV entry a cleanup-free one-liner that
calls a `noexcept`, `noinline` Win64 implementation (see `unimpl_handler` and `hle_exit`).

## Entry environment (guesses marked)
- The entry (`eboot+0x54c690`) is an ordinary function. It reads `argc = *(int*)rdi`, `argv = rdi+8`, and an exit
  callback from `rsi` (passed to `atexit`). It then calls `_init_env`, `atexit` twice, `DT_INIT` (0x20) and
  `main(argc, argv, 0)` (0x496f0), followed by `catchReturnFromMain` and `exit`.
- We pass `{argc=1, pad, argv={"eboot.bin", NULL}}` (guess: the PS4 also supplies argv[0]). The exit callback is a SysV logger.
- The game runs on its own thread with a 16 MiB reserved stack (guess; the PS4 main-thread size is not confirmed).
  There is no auxv and no proc-param pointer; nothing seen so far reads them.
- A vectored exception handler prints the fault code, RIP as `eboot+off`, 16 instruction bytes, the registers, an
  rbp-chain backtrace and the last stub calls, then terminates the process.

## Hazard survey (capstone, 29,819 eh_frame functions in .text 0..0x8b9ec0)
| item | count |
|---|---|
| `fs:` / `gs:` memory operands | 0 / 0 |
| `syscall` / `sysenter` / `int` / `int3` | 0 |
| `rdtsc` / `cpuid` / `rd/wrfsbase` / `xgetbv` | 0 |
| `ud2` | 525 (compiler traps after noreturn calls, e.g. after `exit` in the entry) |
| VEX-encoded xmm ops / ymm ops | 122,270 / 14,816 (AVX1; Jaguar has no AVX2. 112 `vp*` ymm ops are AVX1 permutes) |
| BMI1 / LZCNT (`bextr` 477, `andn` 377, `lzcnt` 27) | 881 |

The stack canary is a data import (`__stack_chk_guard`, bound to real storage), not `fs:0x28`. Jaguar-class
features (AVX, BMI1, LZCNT, F16C) are required of the host CPU, which every x64 CPU from Haswell / Zen onward has.

### fs: proposal (not needed for this eboot)
No game code touches `fs:` and PT_TLS is empty, so nothing needs doing now. If a later module or shim does need
PS4 TLS, the preferred fix is to patch at load time. Each `mov reg, fs:[disp]` (9-10 bytes) becomes a `call` to a
per-site thunk that computes the value from a per-thread block (located through Win64 `gs:` / TlsGetValue) and
jumps back. Sites too short for a 5-byte call get an `int3` patch and are handled by the vectored exception handler
(slow, but rare). Setting the real fs base on Windows is not possible in user mode, so we do not take that route.

## Status
On the user's eboot, `dcp_run --dry` prints: base 0x400000, 3 segments, image 0xec4000, relocs 64=97, JUMP_SLOT=476
and RELATIVE=378779 (no other types), 475 stubs + 1 hle + 5 data, 55 unknown NIDs (unnamed libSceNpToolkit2
C++ API). The real run reaches `main` (0x496f0) and faults at `eboot+0x49785` (`test byte [rax+r13*2], 2`). The cause
is that the stubbed `libc::_Getpctype` returns NULL in place of a ctype table. The first shims to write are therefore
libc (memset, strncpy, _Getpctype, ...), followed by the kernel memory calls.

# Layer 2: libc.prx as a module, kernel + service shims (`src/hle/`)

**libc path: LLE.** The user's `sce_module/libc.prx` is a decrypted fake-signed SELF like the eboot (ET_SCE_DYNAMIC,
all segments `0x800`-plain), so `dcp_run` loads it as a second module (anywhere in memory), registers its global/weak
`nid#lib#mod` exports, and binds imports in this order: HLE shim (by NID, any library) > module export > zeroed data
block > logging stub. All 63 eboot libc imports resolve to libc.prx exports. libc.prx has PT_TLS (0x490 B) accessed
through `__tls_get_addr` + 13 DTPMOD64 relocs (no `fs:`), served by per-thread blocks in `hle/kernel/misc.cpp`.
Its `module_start` (= e_entry 0: preinit/init arrays) runs on the game thread before the eboot entry. Its imports are
89 libkernel / libSceFios2 / libSceLibcInternal functions; the ones reached are shimmed (heap: MapNamedFlexibleMemory
+ `sceLibcHeapGetTraceInfo`). libSceFios2.prx / libSceNpToolkit2.prx are not loaded.

**Stub thunks** now pass the import index in `eax` to an asm trampoline (7th SysV arg), fixing the layer-1 global race.

**Stacks** of game threads are committed up front: PS4 code has no stack probes, so a frame > 4 KiB skips the
Windows guard page (seen: a 0x7250-byte frame on the main thread).

**Shims** (`src/hle`, built with `-fno-exceptions`, which removes the SysV SEH problem for direct shims):
kernel/memory (direct + flexible memory, mmap, 2 MiB-aligned VirtualAlloc), kernel/thread (scePthread*, mutex =
CRITICAL_SECTION with lazy static init, cond, keys, once, event flags, equeue create), kernel/file (read-only `/app0/`
-> dump folder; other mounts ENOENT, writes EACCES; fd 1/2 -> host), kernel/misc (errno, TLS, time, sleep, proc param,
signals inert, stack-chk / debug traps), services (sysmodule load ok, user service = user 1, libc heap trace info).

**Status:** `dcp_run` runs libc init and main, creates the CRI / render threads, opens `/app0/DRACRO.cpk`, reaches
the frame runner (`sceVideoOutWaitVblank` in FUN_005a1f60) and the first `sceGnmSubmitCommandBuffers`, and keeps
running (no fault in 60 s). Visible state: save mount stubbed -> `/SAVE0.DAT` ENOENT -> error dialog (stubbed) loop.

# Layer 3: save data, dialogs, pad, frame pacing (`src/hle/`)

`dcp_run <eboot.bin> [--input script.txt] [--frames N]`; `DCP_SAVE_ROOT` overrides the save root (tests must set it),
`DCP_PROBE=68840*,42b470,...` arms int3 probes at eboot vaddrs (one-shot, or every hit with `*`; logs frame + args).

- **SaveData** (`savedata.cpp`): `<root>/SAVEDATA00/` (root = `%USERPROFILE%\Documents\Dragons Crown Pro\saves`, PLAN
  rule 0). Mount2 -> `/savedataN`, which the kernel file shims open read-write (create/trunc/append, ftruncate, mkdir,
  unlink, rename); `/app0` stays read-only. SAVEn.DAT bytes are the game's own; sidecars `sce_sys/param.bin` (raw
  0x530 B param) and `sce_sys/icon0.png`. Error codes as the SDK (NOT_FOUND 0x809f0008 on a missing read mount).
- **Dialogs** (`dialogs.cpp`): Error/Msg/SaveData/Signin share NONE -> INITIALIZED -> RUNNING -> FINISHED (first poll)
  machine; result = OK/YES (buttonId 1); the message / error code is logged.
- **Pad / users** (`pad.cpp`, `input.h`, `services.cpp`): user 1000, one LOGIN event, pad handle 1 (= shadPS4).
  `InputSource` interface; the scripted source reads `frame buttons` lines (names `CROSS+UP` or a hex mask), frame =
  vblank count. An SDL3 source can replace it via `set_input_source`.
- **VideoOut / GNM** (`video.cpp`, `gnm.cpp`): WaitVblank paced to 60 Hz on QPC. GNM packet builders emit PM4 NOPs;
  submits execute only EOP / RELEASE_MEM / WRITE_DATA label writes so the RenderDLThread fences complete. Direct
  memory is mapped below 1 TiB because those packets carry 40-bit addresses. A `FRAME` line every 60 flips.

**Status:** with the shadPS4 oracle save in a scratch root: save read at vblank 51, OPENING attract loop (scenes
01..08), Options + Cross -> TITLE (vblank 668) -> INN (1553) -> TOWN (1919), 5600 frames, 1 flip per vblank, no fault.
With an empty root the game creates SAVEDATA00 and writes a valid SAVE0.DAT (0x20eb18 B, CRC ok). Still stubbed:
libSceFont (no glyphs), AudioOut, NpTrophy, Net/Http, 55 NpToolkit2 NIDs; nothing is drawn.
