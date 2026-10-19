# Version check: fingerprint for the patched 1.03 eboot ("isolated damage number" option)

Addresses are ELF vaddrs (image base 0, as in q.py / dcp_run mapping). Evidence = capstone asm (q.py dis). No 1.00 eboot
was available, so "1.03-specific" is inferred from the code shape and the string-id layout, not diffed (marked below).

## 1. Summary

The feature is the **"Damage Value"** row on options page "Game Options 1" (SystemMsg id 0x1a7). It is a 3-state row:
`Display controlled players only` / `Display all` / `Hide all`. The old 2-state form would have used the shared
`Display`/`Hide` labels (0x1a8/0x1a9, still used by the HP Gauge row); the 3-state labels live in the later-appended id
block 0x1e3..0x1e5 (the same block that holds the cross-play / NPC-join / player-circle strings), which supports "added by a
patch" (unverified; no 1.00 sheet to compare).

Rating of the fingerprint: routine. All sites are leaf code or fixed instruction sequences inside the eboot.

### Label and message ids (SystemMsg_US.fms, id n = n-th string; dump in notes/raw/msg/system.txt)
| id | text (US) | role |
|---|---|---|
| 0x1a5 | Game Options 1 | page title |
| 0x1a7 | Damage Value | row label |
| 0x1e3 / 0x1e4 / 0x1e5 | Display controlled players only / Display all / Hide all | the 3 values (value v -> id 0x1e3+v) |
| 0x209 | Show damage values. | help text for this row (set in FUN_00504ef0 at 0x505262) |
| 0x1ec / 0x1ed / 0x1ee, help 0x20a | Damage Display ON/OFF, "total damage of combination attacks" | separate ON/OFF row (combo total), not the isolated one |
| 0x1f4 / 0x1f5..0x1f7 | Damage Effects: self only / all / hide | separate 3-state row (hit effects), also possibly new, see 5 |

The strings are only in the CPK sheet (`MSGSHEET/SYSTEMMSG_US.FMS`, other languages have their own sheet); the eboot holds
only the numeric ids. A text-based check would have to read the CPK and is language dependent, so the code check is better.

## 2. Function table
| address | proposed name | role | conf |
|---|---|---|---|
| 0x8c500 | `Opt_GetDamageValueMode(settings*)` | reads byte[0] of the settings block: bit1 set -> 1 (all); else bit7 set -> 0 (controlled only); else 2 (hide) | high (asm) |
| 0x8c520 | `Opt_SetDamageValueMode(settings*, v)` | v=0: `b = (b & 0x7d) \| 0x80`; v=1: `(b & 0x7d) \| 0x02`; v=2: `b & 0x7d` | high |
| 0x2a0fe0 | `Actor_ShouldShowDamageValue(actor)` | gameplay gate: mode 2 -> false; mode 1 -> true; mode 0 -> true only if the actor's owner is a local controlled player (vcall +0xdd8 on the owner/target, then FUN_002e8670/FUN_000271a0 chain). This is the "is this the local player's character?" check | high for structure, medium for what it gates (it is the only gameplay reader of the getter; reached through two vtable-only wrappers FUN_00148180 and FUN_001e98c0) |
| 0x148180, 0x1e98c0 | wrappers of 2a0fe0 (no direct callers, vtable slots) | per-actor-class variants; skip for some actor kinds (0x1e98c0 skips when `[x+0x2980]==0x5a`) | medium |
| 0x503710 | `OptPage1_RenderRow(this, widget, row)` | renders one row of "Game Options 1"; 8 rows, `cmp ebx,7` at 0x503740, row-kind table at 0x759d40, row jump table at 0x759af8 | high |
| 0x503972..0x50399d | row 4 of page 1 | `mov esi,0x1a7` (label), calls 8c500, `lea esi,[rax+0x1e3]` at 0x503997 (value label) | high |
| 0x504ef0 | `OptPage1_OnInput` | cycling logic; row 4 sets help 0x209, calls getter at 0x505291 and setter 0x8c520 at 0x505524 | high |
| 0x8c680 / 0x8c550 | getters of the other two damage rows | Damage Display (byte[2] bit4, ON/OFF); Damage Effects (byte[6] bit1 -> 0, else bit2+1) | high |
| 0xd2fa0 | hit-effect spawn, uses 8c550 at 0xd3032 | gate "self only" for Damage Effects (same style of owner check) | medium |
| 0x4b1000 | combo damage total draw, uses 8c680 | not the isolated-number gate | medium |
| 0xc35ac..0xc3626 / 0xc3c9b..0xc3d11 | save block write / read | the settings block is serialized byte by byte (+0, +1, +2, u16 +4, +6, u16 +8 ...) | high |

## 3. Storage
Settings block = `save + 0xaf20c` (save = `FUN_00081810(...)`, same block as in online_room_attrs.md section 5.1). Relative to the block:

| block off | save off | bits | meaning |
|---|---|---|---|
| +0 | 0xaf20c | bit7 / bit1 | **Damage Value mode**: bit1=1 -> all; else bit7=1 -> controlled players only; neither -> hide. Setter 0x8c520 always clears both then sets one or none (mask 0x7d) |
| +0 | 0xaf20c | bits 3,4 | player marker 3-state (FUN_0008c460) |
| +0 | 0xaf20c | bits 5,6 | player circle 3-state (FUN_0008c4b0) |
| +0 | 0xaf20c | bit0, bit2 | HP gauge display (bit2 read at 0x2a1173 is `&1` of byte[0], i.e. bit0, used by a neighbour gate; bit2 is the subtitle toggle, see 0x503ab4 `and cl,4`) (partly unverified) |
| +2 | 0xaf20e | bit4 | Damage Display ON/OFF (0x8c680) |
| +6 | 0xaf212 | bit1, bit2 | Damage Effects (0x8c550) |

Defaults / reset values were not read (not needed for the fingerprint). The save blob serializes bytes +0..+0x13 of this block
in fixed order (c35ac..c3626), so it is part of the save format; whether the 1.00 save wrote the same bytes is unknown.

## 4. Proposed fingerprint (dcp_run load-time check; all offsets are vaddrs, compare in the loaded image)
Prefer A+B (leaf code, no relative operands, exist only for the 3-state option); C and D are additional.

| id | address | bytes | why 1.03-specific |
|---|---|---|---|
| A | 0x8c500 | `8a 07 31 c9 84 c0 0f 99 c1 01 c9 a8 02 b8 01 00` (22-byte function, 6 more: `00 00 0f 44 c1 c3`) | the 3-state getter: tests bit7 (`setns`) and bit1 (`test al,2`). A 2-state 1.00 getter cannot contain the bit1 "all" test, so this function either did not exist or was different |
| B | 0x8c520 | `83 fe 02 74 12 83 fe 01 74 11 85 f6 75 0c 8a 07` (next: `24 7d 0c 80 88 07 c3 80 27 7d c3 8a 07 24 7d 0c 02 88 07 c3`) | the 3-way setter with the clear-mask 0x7d (both bits 7 and 1) and the `or 0x80` / `or 0x02` values |
| C | 0x503972 | `49 8d bc 24 80 03 00 00 be a7 01 00 00` then at 0x503997: `8d b0 e3 01 00 00` | the row label load (0x1a7) and the value label base 0x1e3 (`lea esi,[rax+0x1e3]`): the code that draws the three "controlled only / all / hide" labels |
| D | 0x2a1041 | `48 05 0c f2 0a 00 48 89 c7` and at 0x2a104f: `83 f8 02 0f 84 b1 00 00 00 83 f8 01` | the gameplay check of the mode (2 -> never, 1 -> always, else owner test). Contains a rel32 at 0x2a104a/0x2a1051 so only valid for this exact build, which is the point |
| E (optional) | 0x503740 | `83 fb 07` and the 32 bytes at 0x759d40: `00 00 00 00 01 00 00 00 03 00 00 00 03 00 00 00 01 00 00 00 03 00 00 00 01 00 00 00 05 00 00 00` | page 1 has 8 rows (kinds 0,1,3,3,1,3,1,5) |

Weaker sites (do not use alone): 0x8c550 (Damage Effects getter, `0f b6 4f 06 31 c0 f6 c1 02 75 0c b8 02 01 00 00`) and
0x8c680 (`8a 47 02 24 10 c0 e8 04 c3`): they belong to two further rows that may or may not be 1.03 additions.

### Table entry count
Options page 1 has **8 rows** (indices 0..7: title, narrator, subtitles, HP gauge, Damage Value, Damage Display, Damage
Effects, End edit; `cmp ebx,7` at 0x503740, 0x503826; row table 0x759d40, 8 entries). Page 3 (FUN_00503fc0) has 7 rows, page 2
(FUN_00503bf0) 9 rows. Whether 1.00 had fewer rows on page 1 is unknown: the Damage Display / Damage Effects rows may be
1.03 additions as well, in which case the count differs; if only the third state was added the count is identical.
Do not base a check on the row count alone.

## 5. Confidence and open questions
* High: label ids, getter/setter semantics, storage bits, the gameplay gate (all read in asm).
* Medium: that this row is the feature named in the patch notes. "Display controlled players only" matches "isolated damage
  number" best, but Damage Effects has a "self only" state too (getter 0x8c550) and could be the 1.03 item; the Damage Value
  row is chosen because its labels come from the later-appended id block and its getter has the retrofit shape (extra bit
  checked first, old bit kept as the "controlled only" state).
* Unverified: that 1.00 lacks A-D. The shape (the "all" state being an extra bit tested before the original bit7) is
  consistent with a retrofit, but no 1.00 binary was available. Safest policy for dcp_run: require A and B and C (all three);
  a mismatch on any means "not the expected 1.03 build" (could also be a different region/build, so word the message
  accordingly).
* The text lookup route (FMS id 0x1e3) needs the CPK and language-specific sheets; 0x1e3 exists in the US sheet (index
  verified in notes/raw/msg/system.txt), other languages not checked.
* Gameplay reader attribution: 2a0fe0 is reached only via vtable wrappers (0x148180, 0x1e98c0); which draw call finally uses
  it (damage number sprite spawn) was not traced.
