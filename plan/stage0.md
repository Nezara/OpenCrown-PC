# Stage 0 - shadPS4 as behaviour oracle (user steps)

Purpose: a running reference to compare our port against (boot sequence, file access order, screenshots, saves),
not a product. Everything captured stays local (never in the repo).

## 1. Make the dump bootable
- `Game/Personal File Dump/sce_sys/` lacks **param.sfo** (and icon0.png etc.). Re-copy the full `sce_sys/` from your
  own console dump; shadPS4 reads the title ID / version from param.sfo.
- Game folder layout shadPS4 expects: `eboot.bin`, `sce_module/`, `sce_sys/param.sfo`, plus the data files
  (`DRACRO.CPK`, `DRACRO_US.CPK`, `sce_discmap.plt`) at the root, i.e. what `/app0/` is on the console. If a patch
  (update PKG) exists in your dump, note its version - our eboot is whatever is in this folder.

## 2. Boot test (shadPS4 at Documents\ShadPS4 is 0.13.0; a 0.19.0 SDL build is in Downloads - prefer the newer one)
Record, for each version tried:
- reaches: black screen / Vanillaware logo / title / character select / first stage / crash (where)
- the log file (shadPS4 `log/` folder) - keep the full log of one run that gets furthest
- rough fps in a stage, obvious graphics / audio errors

## 3. If it reaches gameplay, capture
- screenshots of: title, character select, the first stage's start, one menu - same spots each time (our later
  comparison set)
- one save file after the tutorial (shadPS4 `home/.../savedata/<title id>/`), copied somewhere under `Work/oracle/`
- the shadPS4 log with file-access logging on if the build has it (CPK names + offsets read during boot)

## 4. Report back
Which version, how far it got, and the paths to the log / captures. Claude takes it from there (log parsing, boot
order map). If it does not boot at all, that is fine - Stage 0 is a convenience, not a gate for Stages 1-3.
