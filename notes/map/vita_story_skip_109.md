# Story skip, tutorial skip and the no-skip warning on the 1.09 eboot

Target: `Work/vita/update109.elf` (fixed-address Thumb-2, text 0x81000000..0x81772664). Implementation:
`android-port/src/hle/skip_offers.{h,cpp}`, installed by `vita_run` right after the image is mapped. Same behaviour as the
PC port (`port/src/hle/skip_offers.cpp`, semantics in `story_progress.md` 2.3, 2.4, 3, 7, 9). All addresses below were
read with capstone (`Work/vita/dis_thumb.py`-style disassembly of update109.elf); the save layout (slot / hero offsets)
is the same as on the PS4.

## 1. Switches
Always on. Each is turned off by its environment variable set to `0` (Android: also
`adb shell setprop debug.opencrown.env "DCP_STORY_SKIP=0"`, the property now passes these names besides `VITA_*`).
No launcher option, like the PC launcher.

| variable | effect |
|---|---|
| `DCP_STORY_SKIP` | the game's "skip the beginning" question (msg 0x3cc) for every new character |
| `DCP_TUTORIAL_SKIP` | "Begin the tutorial?" (msg 0x5d8) for the first character of a file too |
| `DCP_SKIP_WARNING` | (with the story skip) the first "no" asks again with a warning about online |
| `DCP_SKIP_DUMP=1` | logs the stub and site bytes (for a capstone check) |

Log lines start with `[skip]`. Every site is compared with its expected bytes first; a mismatch leaves it unpatched.

## 2. Functions (1.09 -> PS4 Pro equivalent)
| 1.09 | Pro | role |
|---|---|---|
| 0x812ae7f6 | FUN_003adbf0 | creation screen states 0..9 (tbh at 0x812ae8a6; state 1 0x812ae9a4, state 9 0x812af2ba) |
| 0x812ab56c | FUN_003a69e0 | hero menu (hero select, tutorial offer) |
| 0x812a81e0 | FUN_003a6560 | per-player creation object (0xea70 bytes, ctor 0x812a7e1a, vtable 0x8174b898) |
| 0x812f664e | FUN_00478470 | header "class k cleared the game" `ldrsb [slot+0x20+k]` |
| 0x812f6750 | FUN_00477f80 | tutorial marked: (current hero && hero+0x6f > 0) or slot+0x48 |
| 0x812a999c | (0x103 test) | 1 when no hero or hero+0x103 != 0 (menu skips the tutorial offer) |
| 0x812a99dc | (0x3a90dc) | tutorial "no": hero+0x103 = 1 |
| 0x8137e45c | FUN_000869e0 | set story counter hero+0x6f (+0xa4 at >= 0x20, +0x9e/+0xa3 at >= 0x2d) |
| 0x8137e844 | FUN_000870d0 | hero+0x6f >= 3 |
| 0x8137e6a2 / 0x812f6472 | FUN_00086f00 / FUN_00477c60 | online gate hero+0x6f >= 0x2d / its wrapper (13 callers; 0x812856d0 is followed by msg 0x599 "Network Menu is now available") |
| 0x813799fc | 0x4b0aa0 | YN dialog builder (factory 0x81377f84 type 0x2a, new object each call); result at dlg+0x4d4 (2 yes, 0/3 no, 1 pending) |
| 0x81325c8c -> 0x8105d46e | 0x4ec630 -> 0x6b850 | dialog text by message id (varargs); resolver call at 0x8105d528 |
| 0x813ea3a8 | FUN_00591450 | resolver: id < 0 or >= count -> 0, else `[[msgobj+0x24]+0xa8] + 0x20 + id*4` |
| 0x813e9e14 | FUN_005911b0 | vsnprintf (sheet string is the format) + set text |
| 0x813e8550 | FUN_0058da30 | SetState(this, kind, s): state[kind] at +0x3c+kind, +0x40+4k = 0, raises bit 0x800<<kind in +0x30 (always), resets deeper levels via vt+0x44 |
| 0x813e89ca | (vt+0xb0) | vt+0x4c ChangeState -> vt+0x44 SetState (creation object vtable 0x8174b898: +0x44 = 0x813e8551, +0x4c = 0x813e89cb) |
| 0x813e8936 | FUN_0058dc40 | first-frame test and clear of bit 0x800<<kind (kind 1..3) |

Message sheet (`SystemMsg_US.fms` in `1.09 patch/DRACRO_PATCH_US.CPK`, 1611 entries, 4-byte slots): id 0x3cc = "Would you
like to skip the beginning and start the story from the talisman collecting? (Player starts at LV15)", id 0x5d8 =
"Begin the tutorial?" (same ids as the PS4).

## 3. Story skip offer, creation state 1 command 7 (confirm), 0x812aeb90..0x812aebda
```
812aeb90  r6 = [fp+0x2d8] (= 0, stored at 0x812ae9ae) ; loop k = 0..5: if 0x812f664e(hdr, k) > 0: r6 = 1
812aebb2  ldrb.w r0, [r8, #0xa66]       ; obj+0xea66: new hero (set to 1 at 0x812abfe2 for "Create new";
                                         ; for an edit only if hero+0x6f < 3, 0x812ac520..0x812ac52e)
812aebba  cbz r0, 0x812aebce            ; -> state 8
812aebbc  cbz r6, 0x812aebce            ; -> state 8      <- PATCH 3e b1 -> 00 bf (nop)
812aebbe  ... movs r2, #9 ; blx [vt+0x4c]   ; state 9 = the offer
```
The new-hero test stays, so editing an advanced hero never gets the offer (it would reset it to the skip state).

State 9 (0x812af2ba): first frame builds the YN dialog and sets text 0x3cc (0x812af2e2/0x812af2e6). "Yes" (result 2,
0x812af306..0x812af47a) is the game's own skip: `0x8137e45c(H, 0x2d)`, the three flag groups (byte loops at
obj+0x16dd.., obj+0x171a.., obj+0x1720..), level 14 (0x8137ac44), level-14 stats (0x8137b17a / 0x8137b1b8), SP 15,
equipment raised, three class accessories (table 0x815be738) and item 0x197 (Silver Arm), then SetState(kind, 8).
"No" (result 0 or 3) = 0x812af47c: SetState(kind, 8).

## 4. Tutorial question, hero menu 0x812abd76..0x812abe8c
```
812abd78  bl 0x8138463a ; 812abd86 bl 0x812a999c   ; either true -> state 8 (no offer: hero+0x103 already set)
812abda2  bl 0x812f6750 ; cmp r0, #0
812abda8  bne 0x812abdf6                   <- PATCH 25 d1 -> 25 e0 (b 0x812abdf6)
812abdaa  auto-start: wait for 0x8105b4de or 0xb4/fps frames, then state 9 (round/Tutorial.brb)
812abdf6  dialog: 0x813799fc, text 0x5d8 (0x813ea4b2 at 0x812abe2e); result 2 -> state 9 (tutorial),
          3 -> 0x812a99dc (hero+0x103 = 1) and state 8 (inn)
```

## 5. No-skip warning (stubs)
Stubs in the zero tail of the text segment's last page (0x81772700.., text ends 0x81772664; checked zero and mapped
before use), flags + text in a page from `Memory::alloc` (flags at +0, [0] text_due, [1] in_second; text at +0x10).
| site | original | patched |
|---|---|---|
| 0x8105d528 | `bl 0x813ea3a8` (8c f3 3e ff) | `bl L` (0x81772700) |
| 0x812af47c | `ldr r1,[r7] ; adds r0,r7,#0` (39 68 38 1c) | `b.w B` (0x81772780) |
| 0x812af306 | `str.w r7,[fp,#0x2f8]` (cb f8 f8 72) | `b.w Y` (0x81772800) |

* L: `if (r1 == 0x3cc && text_due) { text_due = 0; return text; }` else `b.w 0x813ea3a8`. Uses r0 (result), r2, r3
  (caller-saved at a call; the caller reloads r2 from r6 at 0x8105d534).
* B: `if (in_second) { clear both; r2 = 8 } else { set both; r2 = 9 }`, then the original `ldr r1,[r7] ; adds r0,r7,#0 ;
  ldr r3,[r1,#0x4c] ; mov r1,sb ; blx r3` and `b.w 0x812af488` (return -1). SetState(kind, 9) while in state 9 re-arms
  the first frame (0x813e8550 always raises the bit), so the next frame builds a fresh dialog whose 0x3cc text is ours.
* Y: clear both, `str.w r7,[fp,#0x2f8]`, `b.w 0x812af30a`.
Text (ASCII, `\n`, no `%`): "Without the skip, this character can't play online / until the first half of the story is
finished. / Skip to the talisman collecting? (Player starts at LV15)". Stub bytes printed with `DCP_SKIP_DUMP=1` and
disassembled with capstone: every literal and branch target matches.

## 6. Verified (Windows dev build, fresh save root, `--input` scripts, screenshots)
* Story offer appears on the first character of a new file (Fighter); with `DCP_STORY_SKIP=0 DCP_TUTORIAL_SKIP=0` it
  does not, and choosing the character starts the tutorial by itself (baseline).
* "No" -> the warning dialog (3 lines fit the box) -> "no" -> LV1 character; flags 0 -> 0x101 -> 0x100 -> 0 (`--watch`).
* "Yes" -> LV15 character with Hide Gauntlet / Leather Belt / Oracle Pendant, HP 349.
* "No" -> warning -> "yes" -> LV15, flags cleared by Y.
* "Begin the tutorial?" appears for the first character; "no" -> back at the inn with the character, no tutorial.
* A hero that took the story skip (LV15) is still asked "Begin the tutorial?" when chosen (the two stay independent).
* Note for scripts: the YN dialog ignores CROSS on the preselected YES until the cursor has moved (RIGHT, LEFT, CROSS
  works); unrelated to the patches (same with the warning off).

Not verified: on an Android device (same code, ARM64 host; not built into an APK in this session), the second character
of a file, editing an existing hero, the tutorial "yes" after the patch, two players creating at once
(the flags are global, as on the PC).
