# Character creation, tutorial offer and per-character story progress

Scope: how a hero is created, where the "skip the tutorial" and "skip the beginning" prompts live, and where the
story state of a hero is stored. All addresses are eboot vaddrs (asm-verified unless marked). Anything not read in
asm is marked (unverified). Save offsets are *payload* offsets of `SAVE0.DAT` (see `input_save.md` 3.2) and were
cross-checked with the user's snapshots in `Work/story/` (`snap_01_checkpoint` = Edwin at the checkpoint,
`snap_02_skip_tutorial` = + Ulysses created, tutorial declined, `snap_04_tutorial_done` = + Connelly, tutorial played).

## 1. Summary

* **A "slot" (0xaf8d4 bytes, 3 per payload at 0x50 + i*0xaf8d4) is one save file ("DATA 1..3")**, not one character.
  Each file holds a hero array of 74 records (stride 0x21d0, slot+0xa480): indices 0,1,2 are the playable characters
  (Edwin, Ulysses, Connelly at payload 0xa4d0 + k*0x21d0), the rest are allies/"bone" characters generated at file
  creation (class + level + name, hero+0x27 = 0xff means empty). `input_save.md` 3.2 has been corrected.
* **Story progress is one byte per character: `hero+0x6f`** (payload 0xa4d0 + 0x6f for character 0). It is a linear
  chapter counter. Written by `FUN_000869e0` (2 callers: the story advance `FUN_00476490` and the creation-time skip),
  read through `FUN_00083ad0` (21 callers) and ~40 one-line predicates `FUN_00086d50..FUN_000870e0`
  (comparisons against the counter). Stable, town tier, which of the 9 areas are passed, second-half loop: all
  derived from this byte by comparison; there is no separate stable flag.
* **The game already has a "story skip" at character creation**: message id 0x3cc of `SystemMsg_US.fms` (offer to
  start the story at the talisman collecting, player at LV15). Yes branch = state 9 of `FUN_003adbf0`. It sets
  `hero+0x6f = 0x2d`, level 14 (displayed 15), 15 skill points, 3 class-specific accessories + 1 more item, and three
  groups of "1" flags (9 + 4 + 9 bytes). 0x2d is exactly the value the user's Edwin has at the checkpoint, so
  "checkpoint" == game's own skip target (first half done, nine talismans stage).
* It is only offered when `menu+0xf8da != 0` (new hero, or editing a hero whose counter is <= 2) **and** at least one
  of six per-class bytes `slot+0x20..0x25` is >= 1. Those bytes are set (via `FUN_004784a0`) when the ending/credits is
  seen with that class (`0x4c19f6`). Fresh saves have them 0, so nobody sees the prompt. Patching two branches in
  `FUN_003adbf0` (0x3aed91, 0x3aeda9) makes it appear for every new hero including the first.
* The separate "tutorial skip" the user knows is the dialog "Begin the tutorial?" (id 0x5d8) in `FUN_003a69e0`
  state 3. "No" writes **one byte: `hero+0x103 = 1`** and goes to town; "Yes" starts `round/Tutorial.brb`. The first
  character of a file gets no dialog (offline path starts the tutorial after ~3 s). The tutorial itself leaves no other
  trace: Ulysses (declined) and Connelly (tutorial played) are byte-identical outside names and class.
* Rating: **careful**. The creation flow and the skip are fully read; the semantics of the 9+9 flag groups, rune
  ownership and orbs are not (see section 6). Reusing the game's own skip branch avoids needing them.

## 2. Flows

### 2.1 File creation (title menu), `FUN_0053ce80` -> `FUN_0053d3d0` (slot select)
State machine on the title-menu object. Slot chosen = `this+0x2948` (index 0..2). Empty file = `FUN_0008bfa0(slot)`
(u32 at slot+0xaf234 == 0). New file: dialog (messages 0x1b8.. "Delete DATA n and save", `FUN_004ec630` arg 0x1b8) then
`FUN_0008beb0(slot, app, idx)` = **NewFile**: header defaults (`FUN_000259e0`), item box clear (`FUN_000461a0`), hero
array build (`FUN_00088ae0`: 74 records cleared with `FUN_00082bd0`, 28 random allies via `FUN_00089210` +
`FUN_0008a1c0`, uses libc rand), per-hero record table clear, `slot+0xaf234 = 100`, `+0xaf240/41`, "initialised"
struct at +0xaf20c (`FUN_0008c290`: u16 = 6, four {u8,u16 id 0x35}). `FUN_00081ac0` initialises all three slots
(`FUN_0008c170` each); `FUN_00081b00` = one slot. The same function family also contains (unrelated to creation)
character export/import to a "DCSD" container: `FUN_0053da70` (export, header built by `FUN_00081a30`) and
`FUN_0053e7c0` (import).

### 2.2 Hero selection and creation (in town/title scene), `FUN_003a69e0` (menu object, states 0..9)
* state 4: player picked an **empty** hero slot: gets the creation sub-object `FUN_003a6560(this, player)`, stores
  the hero slot in `+0xc0`, **sets `+0xf8da = 1`**, goes to the creation screen.
* state 5: player picked an existing hero for editing: `+0x1f1b = 1`; `+0xf8da = 1` only if `FUN_000870d0(hero)` is
  false, i.e. `hero+0x6f <= 2`.
* state 3: after the hero is chosen: runs the tutorial offer (2.4).
* state 9: start tutorial (2.4). State 8: leave the menu to the town (`vtable+0xba8(0xa4)`).

### 2.3 Creation screen, `FUN_003ad160` (wrapper, mode 1/2) -> `FUN_003adbf0` (states 0..9)
`FUN_003ad160` mode 2: new hero is built into a local record with `FUN_00082990` (blank hero) then `FUN_00089c20(heroes,
app, fileIdx, classIdx, out)` (class defaults, new uid taken from `slot+0xaf1e0`, short name at +0x116..), copied into
the menu's working hero at `menu+0x1ba8` (`FUN_003a0bd0`), item box (`slot+0x70`, 0xa410 bytes) backed up to
`menu+0x51c0`. `FUN_003adbf0` return: 1 = confirmed -> `FUN_003a1e80(menu+0xf60, heroSlot)` commits the working hero
into the file (new: `FUN_00088d20`; existing uid: field-wise copy), 0 = discarded -> item box restored from the backup.
States of `FUN_003adbf0` (`this+0x?` offsets are into the menu object):
| state | role |
|---|---|
| 0 | fade-in |
| 1 | main screen: class/appearance editing; `FUN_003a1c70` returns the selected command: 0 -> name (2), 1/2 -> appearance steps (4, 5), 3..6 -> part editors (3), 7 -> confirm. On confirm: `if (this[0xf8da]==0 || all six slot[0x20+k] <= 0) -> state 8 else -> state 9` (asm 0x3aed0c..0x3aedc4) |
| 2 | name entry (system keyboard dialog via `FUN_004b0bb0`, name set with `FUN_000851c0/85250/851f0`) |
| 3 | per-part editor (`FUN_00084c40/84b90/84ae0/84a30` build the part, the `FUN_00084f00..84cf0` variants fill the 4 saved messages) |
| 4, 5 | cycle `hero+0x28` (0..4) and `hero+0x29` (0..1) |
| 6 | test-play actor |
| 7 | "discard changes?" (id 0x3a0): yes -> `this[0xf8d9]=1` |
| 8 | fade out, returns `this[0xf8d9] ^ 1` |
| **9** | **the skip offer**, message 0x3cc (dialog result at `[dialog+0x634]`: 2 = yes, 0/3 = no). On yes see 3.2 |

### 2.4 Tutorial offer (`FUN_003a69e0`, case 3, asm 0x3a8ac9..0x3a90e0)
Applies to the hero of player 0 (uid at `slot+0xaf1e4`) when `hero+0x103 == 0`:
* `FUN_00477f80(app+0xf0)` false (this is the first character and the file has no tutorial mark: `hero+0x6f == 0`
  and `slot+0x48 == 0`): wait until `FUN_000687e0` or 180/fps frames, then state 9 (start the tutorial, no question).
* true: dialog id 0x5d8, built at 0x3a8b1d..0x3a8b39. Result 2 (yes) -> state 9; result 3 (no, **= skip**) ->
  `mov byte [hero+0x103], 1` at **0x3a90dc**, state 8 (town).
* State 9 (`0x3a9770` spawn actor from hero 0, then `FUN_00032250(roundMgr, "round/Tutorial.brb", -1, 0, 0)`).
`hero+0x103` is also set to 1 by `FUN_003a58e0` (scene-enter hook, 0x3a59c8) whenever a player party exists, together
with `slot+0x48 = 1` (`FUN_00477fd0`), so any entry into the post-round scene counts as "tutorial resolved".

## 3. Skip branches: exact writes

### 3.1 Tutorial skip ("Begin the tutorial?" = no)
Only `hero+0x103 = 1` (payload `0xa4d0 + k*0x21d0 + 0x103`). Verified: Ulysses (snap_02) has `0x103 = 1`, `0x6f = 0`,
level 0; same bytes as Connelly after the tutorial.

### 3.2 Story skip (SystemMsg 0x3cc = yes), `FUN_003adbf0` state 9, asm 0x3ae348..0x3ae5f0 (decomp lines match)
`H` = working hero `menu+0x1ba8` (same layout as a save hero record, 0x21d0 bytes).
| write | field |
|---|---|
| `FUN_000869e0(H, 0x2d)` | `H+0x6f = 0x2d`; because 0x2d >= 0x20 also `H+0xa4 = 1`; because 0x2d > 0x2c also `H+0xa3 = 1`, `H+0x9e = 1` |
| u64 `0x0101010101010101` at `H+0xb5`, byte 1 at `H+0xbd` | 9 flags group A |
| u16 `0x0101` at `H+0xf2`, u16 `0x0101` at `H+0xf5` | group C (flags 0xf2,0xf3,0xf5,0xf6) |
| u64 `0x0101010101010101` at `H+0xf8`, byte 1 at `H+0x100` | 9 flags group B |
| `FUN_00081fb0(H, 14)` | `H+0x26 = 14` (displayed level 15) |
| `FUN_00081fe0(H)` (class base id table 0x6e4250) -> `FUN_00082000(H, id, 0, 14, 0)` | level-14 stat block: `H+0xc/+0x10` (max/cur HP), `+0x14/+0x16`, `+0x18..+0x23` from the class tables |
| u16 `0xf` at `H+0x24` | skill points = 15 |
| equipment set 0 (`H+0x178`, items stride 0x54): item 0 -> `+0x50=0x12, +0x51=0x24, +0x4e=0xe`; item 1 -> `+0x50=0x10, +0x51=0x10, +0x4e=0xe` | existing starting weapon/armor raised |
| items 2,3,4 created with `FUN_0001bd90` + `FUN_0001cf00(item, id, 0x10, 0x10)`, ids from a table at 0x74dea0 (3 x u32 per class, 6 classes), `+0x43 |= 0x80`, `+0x4e = 0xe`, stored with `FUN_000229a0` + `FUN_00022dd0` | three class accessories (names resolved from ItemList_US: Hide Gauntlet / Leather Belt / Oracle Pendant for class 0, ... Leather Glove, Wood Glasses etc. for the others) |
| item 5 via `FUN_0001f820(item, 0x197, 3, 100)`, `+0x43 |= 0x80` | Silver Arm (id 407) |
Not written by the branch: `hero+0x103` (so the tutorial dialog still appears afterwards), `slot+0x48`, anything in the
item box, the 50-bit set at `+0x21b4`, `+0x21a7`.
Then control continues to state 8 and the normal commit (`FUN_003a1e80`).

Sanity check against the user's save (Edwin, natural progress): `0x6f = 45`, `0x9e/0xa3/0xa4 = 1`, group A all 9 set,
group C 0xf2..0xf5 set, group B 0xf8..0xff set (0x100 unset), level byte 13. So Edwin matches the skip state except
0x100 and the extra natural flags `0x95,0x96,0xa5,0xec,0xed,0xee,0xf4`.

## 4. Per-character record (hero), 0x21d0 bytes, base = slot + 0xa480 + k*0x21d0 = payload 0xa4d0 + k*0x21d0
Offsets relative to the record. "acc" = accessor functions found.
| off | size | meaning | conf |
|---|---|---|---|
| 0x00 | 0x30 | stats: +0x08 u32 (Edwin 0x16c0: probably EXP, unverified), +0x0c/+0x10 u32 HP max/cur (new: 300), +0x14/+0x16 u16, +0x18..0x23 six u16 attribute values (set by FUN_00082000) | med |
| 0x24 | u16 | skill points (FUN_00081fb0 neighbour; set to 15 by the skip) | high |
| 0x26 | u8 | level, 0-based (display +1), capped 0x62 | high |
| 0x27 | s8 | class 0..5 (Fighter, Amazon, Dwarf, Sorceress, Wizard, Elf order of the UI is 0,1,3(?),..: unverified), 0xff = empty record (`FUN_00081fd0`) | high |
| 0x28 / 0x29 | u8 / u8 | appearance indices cycled in creation states 4/5 (0..4, 0..1) | med |
| 0x6f | u8 | **story progress counter** (3.5) | high |
| 0x70 | u8 | small value, acc 86d30/86d40, 1 caller (unverified purpose) | low |
| 0x95,0x96,0xa5 | u8 | flags set naturally; 0xa5 is set once by `FUN_00079b60` (unverified purpose) | low |
| 0x9e,0xa3,0xa4 | u8 | flags set whenever the counter reaches 0x2d / 0x20 (`FUN_000869e0`, `FUN_000871b0`); gate for later content | med |
| 0xb5..0xbd | 9 x u8 | group A, all 1 at the checkpoint | med (meaning unverified) |
| 0xeb..0xf1 | 7 x u8 | town-event flags, get/set pairs `FUN_00086cb0/cc0` (0xeb), `86a10/a20` (0xec), `86c70/80` (0xed), `86c90/a0` (0xee), `86cd0/e0` (0xef), `86cf0/d00` (0xf0), `86d10/d20` (0xf1); users are the town event scripts `FUN_00397450`, `FUN_003cafe0`, `FUN_003c1770`, `FUN_004c3ea0`, `FUN_0054a810` | med |
| 0xf2..0xf6 | 5 x u8 | group C | low |
| 0xf8..0x100 | 9 x u8 | group B (8 of 9 set in Edwin) | low |
| 0x101 | u8 | nonzero makes `FUN_00083ae0` return 99 (acc), set late (unverified) | low |
| 0x103 | u8 | **tutorial offer resolved** | high |
| 0x104..0x109 | | per-hero random ids/seeds (differ per hero, not story) | med |
| 0x110 | u32 | uid copy (also at 0x1f6c); lookup key for the party list | high |
| 0x116..0x135 | | cc padding | high |
| 0x138 / 0x140 | 8 B / 8 B | display name and alternate name (NUL padded; `FUN_000851c0` picks one by `0x21a8` bit 7) | high |
| 0x14a.. | 8 x 6 B | appearance colour entries (u16 0x0a0a ...) | low |
| 0x178 + j*0x354 | 9 x 0x354 | equipment sets, each up to 10 items of 0x54 bytes, item count byte at +0x352 (`FUN_00022960` = item j, `FUN_00022dd0`) | high |
| 0x1f6c | u32 | uid | high |
| 0x1f70 + m*0x78 | 4 x 0x78 | the four saved player messages (dying, resurrection, selection, greeting; setters `FUN_00084f00..84cf0`) | high |
| 0x2150 | 0x56 x u8 | skill levels (acc `FUN_00082f40`) | high |
| 0x21a7 | u8 | loop / difficulty index (0 or 1 = first run, up to 9; acc 82ef0/82f10, used by 86f10/86f30) | med |
| 0x21a8 | u8 | flags (bit0..4 message set flags, bit7 alt name) | med |
| 0x21b4 | 50 bits | **completed challenge requests** (50 entries, table `FUN_00015930` stride 0x2c at 0xc16eb0, reward skill points in field +8, folded into the skill budget by `FUN_0008aa00`); 0 in Edwin | med |
| 0x21c4..0x21cb | | party/slot bytes (acc 86780/867c0), 0xff = none | low |
| 0x21c9 | u8 | 3 flag bits (acc 86bb0..86c20) | low |
| 0x21ca | u16 | last visited stage id, 0xffff none (acc 83b10/83b30) | med |
Note: items inside the equipment sets carry uninitialised padding that changes from save to save (floats vs zeros,
the `0x125cd7 / 0x13aff3 / 0x1545b3 / 0x7867b` single-byte diffs are in unused records of files 2 and 3 or in ally
item padding) - ignore them.

### 4.1 Record table after the hero array: slot+0xa6aa0 (74 x 0x16a, `FUN_0008bb00`, cleared by `FUN_0008b960`)
One compact record per hero index, end `0x1ff` u16 at +0x168. Contains the **4-character short name at +0x150**
("Edwi", "Ulys", "Conn" - this is what the user saw change from "Bab" to "Ulys" at payload 0xa6daa), a byte at +0xd4 that
differs per hero (97, 7, 57: looks like a per-hero random id, unverified) and 0x160 (5, 7, 8 for the three heroes, unverified).
It is display/matchmaking data, not story: keep it out of the skip.

### 4.2 Slot tail (after the record table)
| off (slot) | meaning |
|---|---|
| 0xaf1e0 | u32 next hero uid (0x44 -> 0x45 -> 0x46 per created hero) |
| 0xaf1e4 | 4 x s32 uid of the hero chosen per player (-1 none); 4 x u8 "active" at 0xaf204.. (`FUN_0008bc30/bcf0/bdd0/bc50/bd10`). Edwin uid 29; after creating Ulysses the list holds 0x45 = Ulysses |
| 0xaf20c | "initialised" struct (`FUN_0008c290`, test `FUN_0008c5f0`); +0xaf20d = default class (`FUN_0008c440`, set by `FUN_005402e0`) |
| 0xaf234 | u32 play time (ms-like, capped 3 600 000 by `FUN_0008c120`, which also is the slot CRC with this field zeroed) ; 0 = file empty |
| 0xaf238 | u32 = 100 after NewFile |

### 4.3 File header (slot+0x00, 0x70 bytes, `FUN_000259e0`)
+0x00 u32 (5 in Edwin's file, 0 new), +0x04/+0x08 u32 = 0x1f (constants), +0x0c u32 (0x19ca in Edwin), six u16 at +0x10..0x1b
(2,5,8,3,3,8 in Edwin; per-class counters?, unverified), +0x1e s8 = 0xff, +0x1f = 0x0e.
**+0x20..+0x25: six s8, one per class: "cleared the game with this class"** (`FUN_00478470` get, `FUN_004784a0` set to 1; set at
0x4c19f6 from the credits scene, read by the skip offer and by `FUN_00396e80`). +0x26 u16 = 1.
**+0x29..+0x68: 64 one-byte "seen / done" flags** (37 set in Edwin), with get/set pairs in 0x4770xx..0x4785xx:
0x29 (`FUN_004784e0` / `478510`, first-time marker), 0x2e (`478370/478390`), 0x2f (`4780b0/4780d0`), 0x30 (`478070/478090`),
0x31, 0x32, 0x33 (`4780f0..4781b0`), 0x3a, 0x3b, 0x3f..0x46, **0x48 (`FUN_00477f80` read, `FUN_00477fd0` set: file has
passed the tutorial/first town visit)**, 0x49, 0x4a, 0x4b, 0x4c. +0x68..+0x6d: per-class "unlocked" bytes read by
`FUN_0007d9b0` (class available if the global mask `app+0x154` or this byte is set; `FUN_0007d970` sets the global mask
to all classes).

### 4.4 Item box (slot+0x70, 500 x 0x54, init `FUN_00046330`)
Records start with a u16 type id (index into `ItemList_US.fms`, 0xffff = empty). Edwin's box holds 8 items, none of them
quest items. Quest/carry items in the ItemList: 104 Talisman, 164 Saint Orb, 172/173 keys; they are not in Edwin's box
(unverified whether they are ever stored there).

## 5. Story counter semantics (`hero+0x6f`)
Read from the predicate set (all compare the byte):
* 0 = new character, 1.. = early tutorial/first chapters (`FUN_00087110` >1, `FUN_000870c0` >3 ...).
* Nine area milestones (index i of `FUN_00087000` "==" and `FUN_00087060` ">"): area 3 at 4, area 6 at 0xb, area 4 at 0xf,
  area 1 at 0x15, area 2 at 0x19, area 0 at 0x1c, area 5 at 0x20, area 7 at 0x24, area 8 at 0x29 (area passed when the
  counter is >= that value). `FUN_00086f80` maps the counter to a 0..0x11 town tier (thresholds 0xa,0xe,0x14,0x18,0x1b,
  0x1f,0x23,0x29,0x2d; > 0x2d gives 0x11); callers `FUN_0042ddc0`, `FUN_0054a810` (town/world map). `FUN_00087000`
  callers: `FUN_00477ca0` wrappers used by `FUN_0020e6e0`, `FUN_003b9d60`, `FUN_00466c90/ce0`, `FUN_0051dc50`.
* **Stable: unlocked when the counter is > 0x28** (`FUN_00087160`, wrapper `FUN_00477e40`, 5 callers incl. the town portal
  setup `FUN_003cdf30` and `FUN_0054a810`). The unlock message ids 0x58d/0x58e are raised in `FUN_003ce2f0` the first time the
  predicate is true in a town session (seen-bytes live in the scene, not in the save). 0x2d satisfies it.
* 0x2d = end of first half (`FUN_00086ee0` "< 0x2d" has 18 callers = "first half still running"); > 0x2d (`FUN_00086ff0`)
  enables the stable's random-dungeon entry (msg 0x590).
* Second half: `FUN_000871b0(hero, mode)` sets 0x2e / 0x31 / 0x33 (+ flags 0xa3,0xa4,0x9e), called by `FUN_00478560` from
  the ending scene `FUN_004c3ea0`; `FUN_00086f50` returns the loop number 1..3 from 0x2e..0x30 / 0x31..0x32 / above;
  `FUN_00083ab0` returns 0/2/4 phase codes. The ending scene also sets the per-class cleared byte (4.3).
* Natural advance: `FUN_00476490(app, v)` (17 callers in the town event scripts 0x397450.., 0x39ca90.., 0x3c0090.., 0x1ce7f0)
  edits the **live scene player objects** (4 x 0x23b0 at `scene+0xa8`, hero record at offset 0 of each player object, found
  with `FUN_004763d0`) and propagates the new counter to the other active players that had the same old value; the live
  record is committed to the file by the usual save path (`FUN_00089000` / `FUN_003a1e80`).

## 6. Function table
| address | proposed name | role | conf |
|---|---|---|---|
| 0x53ce80 | TitleMenu_FileFlow | title/file menu state machine (states 0..0xb) | med |
| 0x53d3d0 | TitleMenu_SlotSelect | file select, new file via 0x8beb0 | med |
| 0x53da70 / 0x53e7c0 | Title_CharExport / Import | DCSD export/import of a character | med |
| 0x8beb0 | SaveFile_New | initialise a file (header, allies, defaults) | high |
| 0x8c170 | SaveSlot_Init | validate/normalise a slot | high |
| 0x81ac0 / 0x81b00 | SaveBuf_InitAll / InitOne | | high |
| 0x81810 | Save_CurSlot | current slot pointer via table 0xc1dc60 | high |
| 0x81830 | Save_GetSlot | slot i (< 3) | high |
| 0x88cf0 / 0x89400 / 0x89440 | Heroes_ByIndex / ByUid / ByUidSafe | 74-entry array | high |
| 0x88d20 | Heroes_Insert | commit new hero | med |
| 0x89c20 | Hero_NewFromClass | class defaults + uid | med |
| 0x82990 | Hero_InitBlank | | high |
| 0x88ae0 | Heroes_InitFile | clears heroes, creates 28 allies | high |
| 0x3a69e0 | HeroMenu_Update | hero select states 0..9, tutorial offer | high |
| 0x3a6560 | HeroMenu_GetCreateObj | per-player creation object | med |
| 0x3a9770 | HeroMenu_SpawnPlayer | actor from chosen hero | med |
| 0x3a58e0 | HeroMenu_OnEnter | sets slot+0x48 and hero+0x103 | high |
| 0x3ad160 | CreateScreen_Update | wrapper, backup/restore of item box, commit | high |
| 0x3adbf0 | CreateScreen_States | states 0..9, skip offer in state 9 | high |
| 0x3a1e80 | Hero_CommitFromMenu | working hero -> file | high |
| 0x869e0 | Hero_SetStory | sets 0x6f + 0xa4/0xa3/0x9e | high |
| 0x83ad0 | Hero_GetStory | 21 callers | high |
| 0x871b0 | Hero_StartLoop | 0x2e/0x31/0x33 | high |
| 0x476490 | Story_Advance | live player objects | med |
| 0x4763d0 | Story_CurPlayerObj | | high |
| 0x477e40 / 0x87160 | Story_StableUnlocked | counter > 0x28 | high |
| 0x477f80 | Story_TutorialMarked | slot+0x48 or counter > 0 | high |
| 0x478470 / 0x4784a0 | Header_ClassCleared_Get / Set | slot+0x20+k | high |
| 0x4c19f6 | (call site in ending scene) | sets class cleared | med |
| 0x3ce2f0 | Town_UnlockNotices | one-shot unlock popups by counter | med |
| 0x81fb0 / 0x81fe0 / 0x82000 | Hero_SetLevel / ClassBase / ApplyLevelStats | | high |

## 7. Hooks (implemented: `port/src/hle/skip_offers.cpp`, launcher Game tab)
Story skip and tutorial skip are separate questions (user, 2026-10-16), each the game's own dialog and branch:
1. **Story skip** (`DCP_STORY_SKIP`, on unless 0): NOP only the `jne 0x3aedb2` at **0x3aeda9** (`75 07`, the "some class
   cleared the game" gate). Keep the `je` at 0x3aed91: `+0xf8da` is 0 when editing a hero with counter > 2 (charsel
   0x3a76e9 sets it only via `FUN_000870d0`), so NOPing it would offer the skip on an appearance edit of an advanced hero
   and reset it to the level-15 / counter-0x2d state. "Yes" runs the original writes (3.2); nothing is written by us.
2. **Tutorial skip** (`DCP_TUTORIAL_SKIP`, on unless 0): NOP the `je 0x3a9025` at **0x3a8adb** (`0f 84 44 05 00 00`,
   `FUN_00477f80` false = first character of a file with no tutorial mark -> auto-start). The fall-through builds dialog
   0x5d8 for every hero with `+0x103 == 0`. A hero that took the story skip has counter 0x2d, so `FUN_00477f80` is true
   and it is asked anyway: the two questions stay independent.
3. Not done: a silent skip (force the yes-test at 0x3ae3c5, or hook the commit at 0x3ad3df and call the 3.2 helpers),
   and `slot+0x48 = 1` (`FUN_00477fd0`) on the file.
4. Consistency: the counter derives everything else (stable, town tier, area milestones, stable random dungeon); the 50-bit
   request set at +0x21b4 is independent and the skill budget fold-in (`FUN_0008aa00`) runs at file load - keep SP = 15 and
   skills 0. `slot+0xaf234`/CRC are recomputed on save (`FUN_0008c730`), do not patch the CRC.
5. Do not copy the user's snapshot hero as a template: it contains natural flags (0x95, 0x96, 0xa5, 0xec..0xee, 0xf4) the
   skip does not set, and the display name/uid.

## 8. Dependencies and open questions
* Dependencies: message sheet `SystemMsg_US.fms` (ids: 0x3cc skip offer, 0x5d8 tutorial dialog, 0x3a0 discard, 0x58d..0x590
  unlock notices, 0x50d..0x515 nine talisman pickup lines), `ItemList_US.fms` (item ids), the save layout in `input_save.md`.
* **Runes, orbs, talismans are NOT located.** What is known: nine talisman pickup messages (SystemMsg 0x50d..0x515) and
  group A/B/C flags that the game's own skip sets to "all 1" are the likeliest storage for the nine talismans and
  per-area story items; the results-screen quest items (orb 0x51b, black rune stones 0x51e, ...) are announced by
  `FUN_0051e510`. Rune stone counts / rune-magic knowledge were not found in the hero record outside the groups above;
  no persistent "held orb" field was found (`Saint Orb`, item 164, is a carry item; `DcOrb` is a stage object).
  (unverified) group A = 9 area story items, group B = 9 talismans (Edwin lacks the last).
* Snapshots that would settle it: (1) a character right after picking up one talisman, and one right after a rune stone /
  rune-magic discovery in town (diff within hero 0 only); (2) the same character before/after the Stolen Orb quest hand-in;
  (3) a character created through the patched skip (section 7.1) vs Edwin, to compare group A/B/C, equipment and `+0x95/0x96/0xa5`.
* Which flags the stable random-dungeon feature (counter > 0x2d) needs beyond the counter, and the exact meaning of the six u16
  at header +0x10 and `hero+0x70`, are open.
* Debug menu / mass-flag helper: none found. The only existing mass setters are the skip branch (3.2) and
  `FUN_000871b0` (counter + three flags).

## 9. No-skip warning: online unlock condition, dialog text path, re-ask (asm-verified unless marked)
Goal: on the first "no" to the story-skip question show a warning (no online until a story point), then ask again; the
second answer is final. All addresses are eboot vaddrs; message ids are indices into `SystemMsg_US.fms`.

### 9.1 Online unlock condition (hero+0x6f >= 0x2d, per hero)
* **`FUN_00086f00(hero)` = `cmp byte [hero+0x6f], 0x2c ; seta al`**, i.e. `counter > 0x2c` (>= 0x2d). Its only caller is
  the wrapper **`FUN_00477c60()`** (`FUN_004763d0` = the live hero of the first active player, then 86f00; 0 if none).
  It is the "net menu unlocked" predicate; 13 callers, all network UI: `FUN_00369920` (the "Go online?" confirm, msg 0x57,
  gated at 0x369a06), `FUN_00369ca0` (one-shot notice **msg 0x599** "Network Menu is now available. You can open it with
  [button]", gated at 0x369d0e, once per session through a scene byte `[+0x19d]`), `FUN_0050b2f0` (start-menu entry: the
  "Begin online" 0x4b9 / "End online" 0x4ba line is only added when the predicate is true, 0x50b36a), `FUN_0039ca90`
  (town scene, 0x39cd1f), `FUN_0042ddc0` (0x42f040), `FUN_004da4f0` (0x4dac8f), `FUN_00438d00`, `FUN_0043bc60`,
  `FUN_004326e0`, `FUN_0050b3e0`, `FUN_0050b740`, `FUN_0050c980`, `FUN_0050e2d0`.
  Below the threshold the hero simply has no online entry in the menu (no "not available yet" message exists in the
  sheet; 0x4a4 "You cannot use this facility yet." belongs to the town buildings, unverified).
* The check is on the **hero, not the file**: another hero of the same file past 0x2d can go online; the lock applies
  to the first active player's current hero (`FUN_004763d0` picks player objects 0..3 in order).
* **The game's own story skip (3.2) sets 0x2d, which satisfies the gate**: a hero that accepts the skip can go online
  at once. The warning therefore only concerns the "no" path.
* Player terms: 0x2d is the end of the first half (section 5; the skip text calls the following part "talisman
  collecting"). The counter steps 0x2a/0x2b/0x2c are town cutscene steps in `FUN_00397450` (0x2b -> 0x2c at 0x397685,
  scene index 0xa) and `FUN_003cafe0` (0x2c -> 0x2d at 0x3cb106, after the vtable call `[+0x10d8](this, 0xb)`), reached
  after the ninth area milestone (0x29, section 5). In words: **online unlocks once the first half of the story is
  finished - all nine stages cleared once plus the town story scenes that follow - the point where the game's own skip
  starts you**. Which named stage / boss that is was not read from the sheets (unverified); do not name it in the
  dialog. Suggested wording: "until you finish the first half of the story".

### 9.2 How a dialog gets its text (message id -> string)
* **Sheet format** (`SystemMsg_US.fms`, 63680 bytes): 0x20-byte header (`FMSB`, u32 size-48 at +4, 0x20 at +8,
  **entry count 1611 at +0x14**, 3 at +0x18), then `count` 8-byte slots (zero on disk, the loader fills absolute
  string pointers), then the strings back to back, NUL terminated, in id order: **id n = n-th string, UTF-8** (the
  trademark sign is e2 84 a2). Verified: index 0x3cc = the skip question, 0x5d8 = "Begin the tutorial?".
* **Runtime lookup**: `FUN_00591450(msgobj, id)` (0x28 bytes, leaf): `if id < 0 or [msgobj+0x38]==0 -> 0; tbl =
  [[msgobj+0x38]+0xf8]; if id >= [tbl+0x14] -> 0; return [tbl + id*8 + 0x20]` (char*). `msgobj+0x38` is the current
  sheet, linked from the global at `[0xc1acd0]+0x158..0x170`; which sheet it is was not traced (SystemMsg for these
  dialogs; unverified). Resolver callers: `FUN_0006b640`, `FUN_0006b850` (dialog path), `FUN_0006c4d0`,
  `FUN_004814b0`. The language getter `FUN_00025740` is not involved here (the sheet was chosen at load).
* **Dialog text setter**: `FUN_004ec630(dlg, id, ...)` is printf-style: `FUN_0006b850(dlg+0x3e0, id, va_list)`. That
  calls the resolver, then `FUN_005911b0(msgobj, str, va)`, which does `vsnprintf(buf[0x400], 0x3ff, str, va)` with the
  *sheet string as the format* (a literal percent must be written `%%`; max 1023 bytes), converts with `FUN_000021f0`
  and stores with `FUN_0058f280` (text-box set-text); `msgobj+0x1b4` keeps the id (-1 when none). The other setter
  `FUN_00591730(msgobj, id)` (used for 0x5d8 at 0x3a8b39) indexes the same table inline and calls `FUN_0058f0e0`
  without vsnprintf - a hook on the resolver does **not** cover it, a patch of the table does.
* **String format**: UTF-8, `\n` (0x0a) line break; automatic wrap not proven (existing texts break by hand at about
  50 characters, 2-3 lines; box auto-size not checked - keep to <= 4 lines of <= 50 characters, unverified). Tags in
  the sheet: `##(NAME)` named substitution (COLOR_P_STABLE, FIGHTER, ...), `@c(18)text@/c` colour span, `@#(n)`
  button icon (34/35 = menu buttons), `%d`/`%s` printf args. Plain ASCII plus `\n` (and optionally
  `@c(18)..@/c`) is safe.
* **Dialog classes** (factory `FUN_004af4b0(mgr, type)`, per-type slot at `mgr + type*32 + 0x78`, type table at
  0x757250, one wrapper per type):

  | type | wrapper | class (name string) | note |
  |---|---|---|---|
  | 0x2a | `FUN_004b0aa0(mgr, player)` | `DcUi_YNDialog` (ctor `FUN_004ecee0`, 0x950 bytes) | yes/no; result dword `[dlg+0x634]` is **1 = pending** (ctor stores qword 0x100000001), 2 = yes, 0 or 3 = no; type table value 1: a still-registered old instance is closed with `FUN_0058d990(old+0x50)` and a **new object is built every call**; slot `mgr+0x5b8` is read by `FUN_004b0ae0` |
  | 0x25 | `FUN_004b08e0(mgr, player)` | `DcUi_MsgDialog` (ctor `FUN_004ec9a0`, 0x640 bytes) | message / OK only; result dword starts at -1; type table value 0: an existing instance is reused; slot `mgr+0x518` read by `FUN_004b0920`. Example `FUN_003c1490` 0x3c15c6..0x3c1611: `4b08e0`, `4ec630(dlg,id)`, `[dlg->vt+0x190](dlg,1)`, then each frame `FUN_004b0920(mgr)` non-null = still open, null = closed |

  Both wrappers store `player` in `[dlg+0xf4]` and call `[vt+0xf8]`. Meaning of vt+0x190 (arg 1) not decoded.

### 9.3 Existing wording that could carry the warning
None fits. Closest online strings: 0x57 "Go online? Network games will become available." (YN), 0x599 (net menu now
available), 0x4b9/0x4ba, 0x65 "Player must be signed in to PSN to go online", 0x4a4 "You cannot use this facility yet."
No string says that online is locked behind progress. Custom text is needed.

### 9.4 Custom text plan (host-side hook on the resolver, no sheet edit)
Use an id outside the table (for example 0x7ff0, above the 1611 entries) and return our UTF-8 pointer for it.
**Hook R, `FUN_00591450` entry**: the 6 bytes `31 c0 85 f6 78 21` (xor eax,eax / test esi,esi / js 0x591477) are replaced
by `e9 rel32 ; 90` into a relay stub (same technique as `port/src/hle/volume.cpp`):
```
cmp esi, 0x7ff0 ; jne orig ; lea rax,[rip+text] ; ret        ; text = NUL-terminated UTF-8, '%' written as '%%'
orig: xor eax,eax ; test esi,esi ; js -> 0x591477 (the ret) ; jmp 0x591456 (mov rcx,[rdi+0x38])
```
(0x591456 is an instruction boundary: `48 8b 4f 38`.) It affects only the 4 resolver callers. The id kept in
`msgobj+0x1b4` is our id; readers of that field other than the setters were not checked (unverified). The hook ignores
sheet language; localise only if wanted (language int at DcCore+0x130, getter `FUN_00025740`).
Fallback: overwrite the table slot of an unused in-range id (`[[sheet+0xf8] + 0x20 + id*8]`) with our pointer after the
sheet is loaded; needs the sheet pointer, so the hook is simpler.

### 9.5 Re-ask plan (reuse the YN dialog; state 9 entered twice, no new state)
Facts: the state-9 first-frame test is `FUN_0058dc40(this, kind, clear=1)` at 0x3ae376 (`kind` = second argument of
`FUN_003adbf0`, kept in r12d / `[rbp-0x2c4]`): it tests and clears bit `0x800 << kind` of `[this+0x50]`. The state setter
**`FUN_0058da30(this, kind, newState)`** (vtable +0xa0 of the menu class; vtable +0xb0 = `FUN_0058dcb0` tail-jumps to it;
vtable located through the relocation table, slot offsets 0xa0/0xb0 relative to the slot that holds 0x58da30)
raises that bit, stores `state[kind]` at `this+0x5b+kind`, zeroes `this+0x60+4*kind` and resets deeper levels. So
`SetState(kind, 9)` while in state 9 re-arms the first frame: the next frame runs 0x3ae37f..0x3ae39b again (`4b0aa0`
builds a **fresh** YN dialog with result 1 = pending; the factory closes the old one if still registered), so the second
ask cannot read the old answer (`r14` is the new object, read at 0x3ae3a7). No explicit reset or close call is needed.

The two "no" exits are `je rel32` at **0x3ae3b3** (`0f 84 37 02 00 00`, result 0) and **0x3ae3bc** (`0f 84 2e 02 00 00`,
result 3), both to 0x3ae5f0 (`mov rax,[r13]; mov edx,8; mov rdi,r13; mov esi,r12d; jmp 0x3aecd2`, where 0x3aecd2 is
`call [rax+0xb0]` followed by `mov ecx,-1` = "stay in this frame loop"). 0x3ae5f0 is also the target of the `jmp` at
0x3ae254 (another state), so do not patch it. Registers at the exits: r13 = menu (`this`), r12d = kind.
Patches (two relay stubs and two bytes of state, `pending_second` and `in_second`):
1. **Builder, 0x3ae391** `be cc 03 00 00` (`mov esi,0x3cc`) -> `e8 rel32` to stub A, which only sets esi:
   `if pending_second { pending_second=0; in_second=1; esi=0x7ff0 } else { in_second=0; esi=0x3cc } ; ret`
   (esi is the only live register there; rdi is set after, at 0x3ae398). The else branch also clears a stale
   `in_second` left by a "yes".
2. **No exits, the rel32 operands at 0x3ae3b5 and 0x3ae3be** retargeted to stub B:
   `if in_second { in_second=0; jmp 0x3ae5f0 }` (final: state 8 as before), else `pending_second=1; rax=[r13];
   edx=9; rdi=r13; esi=r12d; jmp 0x3aecd2` (SetState(kind, 9), return -1).
3. The warning is shown inside the second YN dialog as a combined text (warning lines + the skip question), because the
   first dialog is already answered. A separate OK box would be: in stub B `4b08e0(mgr,[this+0xb8])` (mgr =
   `[[this+0xf8d0]+0xa8]`, player = `[this+0xb8]`, as at 0x3ae34f..0x3ae389), `4ec630(dlg, 0x7ff0)`,
   `[dlg->vt+0x190](dlg,1)`, plus a wait that polls `FUN_004b0920(mgr) == 0` before `SetState(kind,9)`; that needs a free
   state number or a counter, more code and `vt+0x190` is undecoded. Not recommended.
4. Cancel (result 0) on the first question counts as "no" through the same exit, so it also triggers the warning; cancel
   on the second question is the final no.
Suggested text (ours, 4 lines): "Without the skip this hero cannot play online\nuntil the first half of the story is
finished.\nSkip the beginning and start from the talisman\ncollecting? (Player starts at LV15)". The yes branch is
unchanged game code (3.2) and unlocks online at once (counter 0x2d).
Not asked, noted only: hooking `FUN_00086f00` to return true would open online for every hero.

### 9.6 Unverified / open
* Which sheet `[msgobj+0x38]` is for dialogs (SystemMsg assumed) and the box auto-sizing for long text.
* Readers of `msgobj+0x1b4` besides the setters; `vt+0x190` on `DcUi_MsgDialog`.
* The named story point of counter 0x2d (stage / boss).
* Whether the answered YN dialog removes itself before the re-ask frame (the factory handles both cases).

### 9.7 Implemented (port/src/hle/skip_offers.cpp, `DCP_SKIP_WARNING`, on with the story skip)
Differs from 9.5: no fake id. The lookup relay at 0x591450 returns our text for the real id 0x3cc while a one-shot
`text_due` byte is set (cleared by that lookup), so no reader ever sees an out-of-range id. A second byte `in_second`
drives the "no" relay (both `je` rel32 at 0x3ae3b3 / 0x3ae3bc retargeted): first no -> set both, SetState(kind, 9);
second -> clear, 0x3ae5f0. The yes path start (0x3ae3cb, 8 bytes) jumps through a relay that clears both and replays
`mov rbx, r15 ; mov esi, 0x2d`. Relay bytes are printed with `DCP_SKIP_DUMP=1` and were disassembled (capstone) from a
real run. Not yet seen in play: the re-armed state-9 build and how the 3-line text fits the box.
