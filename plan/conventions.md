# Conventions and tools

## Layout
- `port/` - our C++ (CMake + Ninja, MinGW g++ 16, `-static`). `src/formats/` readers, `tools/` CLIs, `tests/`
  (synthetic data only - tests must never need game files). Build: `cmake -S port -B port/build -G Ninja && cmake --build port/build`.
- `Work/` - RE workspace. Versioned: `Work/tools/`, `Work/ghidra_scripts/`. Everything else in Work/ (eboot.elf,
  Ghidra DB, index/, logs/, nid tables) is derived from the dump and stays local.
- `notes/map/` - subsystem maps (prose + addresses; never raw Ghidra output or long asm - the repo is public).
  Raw dumps go to `notes/raw/` (ignored).
- User dump: `Game/Personal File Dump/` (never versioned).

## Lookup tools (cheap -> expensive; use the cheapest that answers)
1. `python Work/tools/q.py fn|callers|callees|str|strs|dis ...` - instant, no Ghidra. `dis` = capstone, the truth.
2. `Work/tools/ghidra.ps1 decomp <out.c> <fn...>` - pseudo-C hint (~30 s startup; batch all functions in one call).
   Only one Ghidra run at a time on the project (read-only, but not concurrently with a writer / the GUI).
3. `Work/tools/ghidra.ps1 index` after any change that renames / adds functions in the DB.

## Code
- C++20, `namespace dcp::...`, no exceptions across the future shim / ABI boundary (formats may throw for now).
- Big-endian CRI fields via explicit helpers; 64-bit file offsets everywhere (CPKs > 4 GB).
- Paths from archives are untrusted: reject absolute / `..` before writing.
- Comments: why, not what; one line where possible.

## Checks
- Every tool prints one summary line (`N entries, M failures`) and details only on failure.
- Format readers: unit test on synthetic bytes + `verify` over the user's real dump (0 failures).
- Chain independent checks into one shell call.
