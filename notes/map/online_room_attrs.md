# Online room attributes: why CreateJoinRoom and the random SearchRoom disagree

**Status: resolved and confirmed end to end** (§9.2) - two instances matched, joined and played over our own lobby +
Rudp on 2026-10-16. No HLE or lobby change was required; the host must be inside one of the nine dungeon rounds.

Mapper notes, 2026-10-16. Addresses are eboot-relative (as in `q.py`). Tags: **(asm)** = read in capstone disassembly,
**(inferred)** = deduced from structure/usage, **(unverified)** = guess. Ground truth: the five labelled HLE run logs in
`%USERPROFILE%\Documents\Dragons Crown Pro\logs\` (2026-10-16) - in particular `Instance Run Online.log`, which goes
online in town and then enters a dungeon, and **confirms the whole chain at runtime (§9)**; plus the user's save at
`%USERPROFILE%\Documents\Dragons Crown Pro\saves\SAVEDATA00\SAVE0.DAT`. Baseline: `notes/map/net.md` sections 3.2-3.4.
Read-only survey; nothing in `port/src` was changed.

**Save file note:** `SAVE0.DAT` (0x20eb18 B, magic `DCSD` at +4) maps **1:1 onto the offsets the game uses**, so
`save+0xaf1e4` / `save+0xaf20c` can be read straight out of the file. Verified: `+0xaf204..0xaf207` = `00 00 00 01`
(sum 1 = the local player count the filters used) and `+0xaf20e` = `0x00` (both matchmaking flag bits clear).

## 0. Answer in one paragraph

**The disagreement is genuine game state, not an HLE marshalling bug.** Searchable int attr `0x4d` (77) is the index of
the **round (stage) the host is currently in**, looked up by name in an 18-entry table; `9` = `round/Town00.brb`.
"Join Random Room" filters `0x4d <= 8`, i.e. **only the nine real dungeon rounds** - a room hosted from town is
deliberately invisible to random matchmaking. The same thing shows up in the packed attr `0x53`: its bits [1:0] are a
"room is open to strangers" state that the builder forces to `1` while the host sits in town, whereas the search asks for
`== 2`. Attr `0x4c` (76, free player slots) is fine (host published 3, filter wanted `>= 1`). So our HLE and lobby server
are reading and writing the right fields; what is missing is that the host must actually be **inside a quest** for a
random search to see it.

## 1. Functions

| addr | name (mine) | role |
|---|---|---|
| `FUN_00438d00` | `Act::BuildRoomInfo(this, RoomInfo* out)` | **the room-info builder**: fills the 0x78-byte RoomInfo from scene + player state (asm). 5 callers: `4296a0`, `42ddc0`, `4355f0`, `438390`, `43bc60` |
| `FUN_0042ddc0` | town/act scene update | calls `438d00` then `SetRoomInfo` at `42e0d7..42e0f7` (asm) |
| `FUN_00098e20` | `RoomInfo::Reset(this)` | ctor/reset: `0x40..0x5b` = `0xff` filler, `+0x56` = 3*savebyte, `+0x58` = 0x22 (version), `+0x5a` = 2*savebyte (asm) |
| `FUN_00098da0` / `FUN_00099110` / `FUN_00098fb0` | RoomInfo stream ctor / read / write | 0x78 B wire form |
| `FUN_0009c3a0` | `DcNet::SetRoomInfo(info)` | stamps `+0x5f |= 0x80`, `+0x66`, `+0x68`, `+0x70` from DcNetMatching, serializes into `DcNet+0xe58`, keeps the struct at `DcNet+0xd98` (asm) |
| `FUN_000b5170` | `DcNetMatching::SetRoomInfo` thunk | vtable slot **+0x310** of the matching interface (vtable base `0x8cd950`), reached as `[X+0x16910]` |
| `FUN_000bba60` | `BuildRoomAttrs(matching, intAttrs[8], binExt, binInt, ...)` | **create-side attr builder**: deserializes `DcNet+0xe58` and emits int attrs `0x4c..0x53` + bin attrs `0x57`/`0x55` (asm) |
| `FUN_000b8a10` | room create | `sceNpMatching2CreateJoinRoomA`, maxSlot 4, flagAttr `0x86000000` |
| `FUN_000b8fd0` | room search | **search-side attr builder**: turns the queued condition vector into filters + the packed `0x53` (asm) |
| `FUN_000ba6e0` | `DcNetMatching::RequestSearch(conds[], n)` | enqueues request type **4** with up to 10 caller conditions, then appends its own slot-11 (`98f00`) and slot-12 (`98f50`) conditions, which `b8fd0` ignores (asm). Entry via `FUN_0009c600` -> matching vtable slot **+0x168** |
| `FUN_0050f2e0` | **"Join Random Room" menu action** | builds the five search conditions on the stack at `50f381..50f445` (asm) |
| `FUN_0004aab0` | `RoundNameToIndex(const char* name)` | linear search of the 18-entry round table at `0xbe0f90`, stride `0xd0`, first field `const char*`; returns index or -1 (asm) |
| `FUN_004d3660` | room-list row label | switches the round index 0..0x11 onto text ids; **only 0..8 have a label** (`0x8b..0x93`), 0xf gets `0x48f` + arena params (asm) |
| `FUN_0008be40` | `LocalPlayerCount(save+0xaf1e4)` | sum of 4 bytes at `+0x20..0x23` = number of local (couch) players (asm) |
| `FUN_00098f00` / `FUN_00098f50` / `FUN_00098f60` | save-derived packing inputs | `3 * byte(save+0xaf20c)` (default 3) / constant `0x22` = protocol version / `2 * byte(save+0xaf20c)` (default 2) (asm) |

## 2. The RoomInfo struct (corrects net.md 3.4)

In memory the 8 member entries are **7 bytes** each (`0x08..0x40`); the serializer writes only 6 of each, so the **wire
blob offsets are the struct offsets minus 8** from `0x38` on. net.md 3.4 quotes struct offsets; the log blob is the wire
form. Verified both ways against `"Pressed - Join Random Room.log"` line 260.

| struct off | wire off | observed | meaning | source in `FUN_00438d00` |
|---|---|---|---|---|
| `0x00` | `0x00` | `"Edwin"` | host character name (8 B) | `[r13-0x20a0]` |
| `0x08..0x40` | `0x08..0x38` | 8 x `00 00 0a 0a 00 00` | 8 member entries (7 B in memory, 6 B on the wire) | player loop |
| **`0x40`** | `0x38` | **3** | **attr `0x4c` = free player slots** | `word [r15+0x40] = r14w`, `4391d8`; r14 counted over the 4 slots via `FUN_00029320` |
| **`0x42`** | `0x3a` | **9** | **attr `0x4d` = round index** (see §3) | `word [r15+0x42] = ax`, `4391ce`, `ax = (u16)[scene+0x108]` |
| `0x44` | `0x3c` | 0 | attr `0x4e` - arena/random-dungeon param (set only when round == 0x11 or 0xf) | `439425`, `43948f` |
| `0x46` | `0x3e` | 0 | attr `0x4f` - ditto | `43942a`, `43945f` |
| `0x48` | `0x40` | `0xffff` | attr `0x50` (unset -> `0xffffffff` after sign-extend) | only written when round == 0xf |
| `0x4a` | `0x42` | 0 | attr `0x51` | `439496` (0) / `439477` |
| `0x4c` | `0x44` | `0xffff` | attr `0x52` (unset) | only when round == 0xf |
| `0x4e` | `0x46` | `0xffff` | not published as an attr | - |
| **`0x50`** | `0x48` | **1** | **packed `0x53` bits [1:0]: room openness** (see §4) | `word [r15+0x50] = r14w`, `4393d3` |
| `0x52` | `0x4a` | 1 | packed bits [20:19] = `1 + (scene+0x10c == 2)` | `4391e5` |
| `0x54` | `0x4c` | 0 | packed bits [4:2] (99 -> `5`); from `[[scene->vslot0xd0()+0x120]+0x84]` | `4393f6` |
| `0x56` | `0x4e` | 0 | packed bits [6:5]; `3 * byte(save+0xaf20c)` (`FUN_00098e20`) | reset only |
| `0x58` | `0x50` | `0x22` | packed bits [13:7] = **protocol/game version 34** | reset only |
| `0x5a` | `0x52` | 0 | packed bits [18:14]; `2 * byte(save+0xaf20c)` | reset only |
| `0x5c` | `0x54` | 9 | `word [scene+0xf0]` - scene/quest sub-id, also used in the `0x50` logic | `439069` |
| `0x5e` | `0x56` | `0x8620` | flag word: per-player bits 0x20/0x40/0x80/0x100 (slots 0..3 present), bits 0..3 from `FUN_00029320(slot)`, bit 0x200 from `FUN_00026220`, bit 0x1000 from matching vslot `+0x258` | `438ec9..439058` |
| `0x60` | `0x58` | `0xffffff05` | 4 member level/class bytes (`+0x60..0x63`) | `438e9f` |
| `0x64` | `0x5c` | 5 | host class/level byte | `438e52` |
| `0x65` | `0x5d` | `0x0b` | `byte [player+0xca]` | `438e36` |
| `0x66`,`0x68`,`0x70` | `0x62`,`0x5e`,`0x64` | 0 | stamped later by `FUN_0009c3a0` from DcNetMatching (`+0x120`, `+0x124`, `+0x130`). Wire order differs: the serializer FUN_00098fb0 writes u32 `+0x68` (world id) at wire 0x5e, u16 `+0x66` at 0x62, u64 `+0x70` (room id) at 0x64 (990dc..990ff; confirmed by presence blobs carrying room 4096 / 4097 at wire 0x64, 2026-10-17) | - |

Packed attr `0x53` construction (create side, `FUN_000bba60` `bbb15..bbbb9`) - **note net.md 3.4 had three of the six
source fields transposed**:

| bits | source (struct off) | create-side code |
|---|---|---|
| [1:0] | `0x50` | `edi = xmm1[0] & 3` |
| [4:2] | `0x54` (99 -> 5) | `edx = (xmm1[2] << 2) & 0x1c`, `cmp 0x63 -> 0x14` |
| [6:5] | `0x56` | `(xmm1[3] << 5) & 0x60` |
| [13:7] | `0x58` | `(word[0x58] & 0x7f) << 7` |
| [18:14] | `0x5a` | `(word[0x5a] & 0x1f) << 14` |
| [20:19] | `0x52` | `(xmm1[1] & 3) << 19` |

Decoding the two observed values with that table (and they agree field-for-field with the blob):

```
create 528641 = 0x81101 : [1:0]=1 [4:2]=0 [6:5]=0 [13:7]=34 [18:14]=0 [20:19]=1
search 528642 = 0x81102 : [1:0]=2 [4:2]=0 [6:5]=0 [13:7]=34 [18:14]=0 [20:19]=1
                           ^ the only difference
```

## 3. Attr `0x4d` (77) = round index - the primary blocker

`Act::BuildRoomInfo` writes `info[0x42] = (u16)scene[0x108]`, and `scene[0x108]` is set in `Act_SetupScene`
(`FUN_0042b470`, `42b64e`) and `FUN_004355f0` (`435f2a`) as `FUN_0004aab0(roundName)` - the index of the current
`round/*.brb` in the table at `0xbe0f90` (stride `0xd0`, name pointer first):

| idx | round | idx | round |
|---|---|---|---|
| 0 | `round/GhostshipCove.brb` | 9 | **`round/Town00.brb`** |
| 1 | `round/UnderCitadel.brb` | 10 | `round/LostZone.brb` |
| 2 | `round/Catacomb.brb` | 11 | `round/Camp_A.brb` |
| 3 | `round/MysticRuins.brb` | 12 | `round/Camp_B.brb` |
| 4 | `round/MoldyDungeon.brb` | 13 | `round/Camp_C.brb` |
| 5 | `round/ForgottenRealms.brb` | 14 | `round/Tutorial.brb` |
| 6 | `round/AncientLabyrinth.brb` | 15 | `round/Arena.brb` |
| 7 | `round/MageTower.brb` | 16 | (empty) |
| 8 | `round/LostWoods.brb` | 17 | `round/RandomDungeon.brb` |

`FUN_004d3660` (the room-list row) confirms the split independently: it has a text id for **exactly** indices 0..8
(`0x8b`..`0x93`, the nine dungeon names), nothing for 9..16, and a special arena path for 0xf.

Our host published **9** (town) because the room was created from the online menu while standing in Town00.

## 4. Packed `0x53` bits [1:0] = "open to strangers"

`FUN_00438d00` `439271..4393d8` (asm), writing `info[0x50]`:

```
if (byte[scene+0x5b] == 5)                                  -> 0
else if (FUN_00479bf0(..) && (byte[that+0x394] & 1))        -> 0        // only consulted when scene[0xf0] != -1
else {
    r = 1;
    if ((u32)scene[0x108] == 9 && ((scene[0xf0] | 8) == 9)) -> 1        // in town with sub-id 1 or 9
    if (scene[0x10c] == 2)                                 -> 1
    else r = (no player has [+0x2980]==1 && [+0x7b8]==3) ? 2 : 0;
}
info[0x50] = r;
```

So `2` = **open/recruiting**, `1` = "advertised but not randomly joinable" (what town always yields), `0` = closed. Our
host hit the town clause: round 9 and `scene[0xf0] == 9` (same value also copied to `info[0x5c]`, observed 9), so
`(9|8)==9` -> `1`. Bits [20:19] gate on the same `scene[0x10c] == 2` (observed `0`, giving `1`, which is what the search
wants), so `scene[0x10c]` must stay != 2; it is written `0` in the normal paths (`42808c`, `42e6ea`) and from
`[r15+0x49c]` in `FUN_0043ac50` (`43aea5`) (**unverified** what sub-mode 2 is).

## 5. Where the search filters come from - three builders

All three build an array of `{u32 op, u32 slot, u32 value}` entries on the stack and call matching vtable slot `+0x168`
(`FUN_0009c600` -> `FUN_000ba6e0`, request type 4). `FUN_000b8fd0` then maps slots 0..7 to attr ids `0x4c + slot` and
folds slots 8/9/10 into the single packed `0x53 EQ` filter, so **5 conditions produce 3 filters**.

| builder | round condition (slot 1) | when it fires |
|---|---|---|
| `FUN_0050f2e0` | `<= 8` (the `== [menu+0xadf0]` branch is dead, see below) | **"Join Random Room"** menu item |
| `FUN_004d3e80` | `== [this+0xf90]` = the **selected** round, or `<= 8` if that is -1; plus `{4, slot 2, floor}` and `{4, slot 3, 4}` when the selected round is `0x11` (RandomDungeon), a separate path for `0xf` (Arena) | **setting off on a quest** - looks for a room already in that quest; when it finds none the game fills the party with an NPC (observed in `Instance Run Online.log`) |
| `FUN_005131f0` | `== 17` hardcoded (template at `0x75a2c0` = `{6,8,2}, {1,1,17}, {1,9,1}`), plus `{1, slot 2, [this+0x11bdc]}` and `{4, slot 3, 4}` | **RandomDungeon / Labyrinth** matchmaking (7 conditions) |

The last two also explain attrs `0x4e`/`0x4f` (78/79), which `FUN_00438d00` only fills when the round is `0x11` or `0xf`
(`4393fb`: `info[0x44] = min(scene[0xa8], 0xc34f)`, `info[0x46] = word[scene+0xb0]`): they are the
**RandomDungeon/Arena floor-depth** parameters, filtered `78 == mine` and `79 <= 4`.

`FUN_0050f2e0` ("Join Random Room") passes five conditions, `edx = 5` (asm, `50f381..50f445`):

| entry | op | slot | value | meaning |
|---|---|---|---|---|
| 0 | 6 (GE) | 0 -> attr `0x4c` | `FUN_0008be40(save+0xaf1e4)` = local player count (1 observed) | "at least as many free slots as I need" |
| 1 | 6 | 8 -> packed bits [1:0] | **2** | "room is open/recruiting" |
| 2 | 4 (LE) or 1 (EQ) | 1 -> attr `0x4d` | **8**, or `[menu+0xadf0]` when that is != -1 | "any of the 9 dungeon rounds", or one chosen round |
| 3 | 1 | 9 -> packed bits [20:19] | 1 | `scene[0x10c] != 2` |
| 4 | 1 | 10 -> packed bits [4:2] | `[[...+0x120]+0x84]` (0 observed) | same source the host writes into `info[0x54]`, so it always agrees |

`[menu+0xadf0]` is only ever initialised to `-1` and never assigned (whole-text scan: `FUN_0050ca40` `50cd35` writes the
qword `0xffffffff00000000` at `+0xadec`; the only readers are `50f3ca` and `FUN_0050f620` `50fb17`, which passes it to
`FUN_004d3660` for display). So **in this builder** the round filter is always `0x4d <= 8` and the `== N` branch is dead
code. The live form of that branch is the identically-shaped `[this+0xf90]` in `FUN_004d3e80` (`4d4122..4d4153`), which
is the selected round and did produce `{77, op 1, value 2}` in `Instance Run Online.log` line 390.

`FUN_000b8fd0` then maps slots 0..7 to attr ids `0x4c + slot` with the operator table at `0x6e4f74` (identity 1..6), and
folds slots 8/9/10 into the single `0x53 EQ packed` filter together with `FUN_00098f00`, `FUN_00098f50` (version 0x22)
and `FUN_00098f60`, which it calls itself - which is why the slot-11/12 conditions `FUN_000ba6e0` appends are silently
dropped. Request `flag_attr` is `0x64000000`, `attr_ids` `[0x4c, 0x54, 0x55]` (= `[76, 84, 85]` in the log).

### 5.1 The two save-derived bits are player *options*, not progress

`FUN_00098f00` and `FUN_00098f60` are not reading a progress counter - they read **two single bits of one byte** (asm):

* `FUN_0008c620(save+0xaf20c)` = `(byte[save+0xaf20e] >> 2) & 1`; `98f00` returns `3 * bit`, so packed bits [6:5] are
  **0 or 3**.
* `FUN_0008c700(save+0xaf20c)` = `byte[save+0xaf20e] >> 7`; `98f60` returns `2 * bit`, so packed bits [18:14] are
  **0 or 2**.

Both bits are **user-toggleable settings**. `FUN_00503bf0(this, widget, rowIndex)` renders one options row per call,
dispatching on `rowIndex` 0..8 through the table at `0x759b48`, so the **screen order is known exactly** (asm) - and
every getter reads the same small block at `save+0xaf20c`:

| row | title id | getter | reads | values |
|---|---|---|---|---|
| 0 | `0x1da` | - | - | label only (header//no value) |
| 1 | `0x1db` | `FUN_0008c460` | `byte[0xaf20c]` bits 3,4 | 3-state `0x1dc + v` |
| 2 | `0x1df` | `FUN_0008c4b0` | `byte[0xaf20c]` bits 5,6 | 3-state `0x1e0 + v` |
| 3 | `0x1ef` | `FUN_0008c6b0` | `byte[0xaf20e]` bits 5,6 | 3-state `0x1f0`/`0x1f1`/`0x1f2` |
| 4 | `0x1ff` | `FUN_0008c5a0` | `byte[0xaf212]` bits 3,4 | 3-state `0x200`/`0x201`/`0x202` |
| 5 | `0x1e9` | `FUN_0008c650` | `byte[0xaf20e]` **bit 3** | On/Off `0x1ed`/`0x1ee` |
| **6** | `0x1e6` | `FUN_0008c620` | `byte[0xaf20e]` **bit 2** | On/Off, **displayed inverted** (`xor eax,1`, `503edb`) -> **packed [6:5]** |
| **7** | `0x1d1` | `FUN_0008c700` | `byte[0xaf20e]` **bit 7** | On/Off `0x1d2 | v` -> **packed [18:14]** |
| 8 | `0x1a4` | - | - | label only |

So of the three On/Off toggles, the **second and third** are the matchmaking-visible ones; row 5 (bit 3) is not in the
key. The strings live outside the eboot, so the two option *names* are still **unresolved** - reading the rows off the
screen in this order names them, and flipping exactly one toggle and diffing `byte[save+0xaf20e]` anchors the mapping.

Consequence: these two bits are part of the matchmaking key on **both** sides (host via `FUN_00098e20`, searcher via
`98f00`/`98f60`), so **two players whose settings differ on either option cannot see each other at all** - four mutually
invisible cohorts. That is plausibly deliberate (a preference filter), but it is worth knowing before blaming the lobby.
In the user's save both bits are clear (`byte[0xaf20e] == 0x00`), matching the `[6:5]=0 [18:14]=0` seen in every logged
request; the 3/2 values in `FUN_00098e20` are the no-save fallbacks, so seeing 0 also confirms the save was loaded.

## 6. Is our HLE at fault? No

`port/src/hle/matching2.cpp` marshals exactly what the asm writes, re-verified here:

* create searchable int attrs: `{u16 id @0, u32 value @4}`, stride 8, array ptr `req+0x70`, count `req+0x78`
  (`FUN_000bba60` `bbbc0..bbc47`) - matches `int_attrs_json`.
* search filters: stride 16, `op` byte at `+0`, `u16 id` at `+8`, `u32 value` at `+0xc`
  (`FUN_000b8fd0` `b91db..b92b7`: `mov word [rsp+r15+0xa8], id` / `mov dword [rsp+r15+0xac], value` /
  `mov byte [rsp+r15+0xa0], op`, with `r15 = idx*16` and the frame base at `rsp+0xa0`) - matches `sceNpMatching2SearchRoom`.
* `lobby_server.cpp` `int_match` ops (1 EQ, 2 NE, 3 LT, 4 LE, 5 GE... ) line up with the game's table at `0x6e4f74`.

The values differ because the two requests describe different game states, which is also why the existing
`DCP_JOIN_CODE` workaround in `sceNpMatching2SearchRoom` (drop all filters when joining by code) was needed and is
correct for that path.

## 7. Recommendation

**No HLE change for the attrs themselves.** For "Join Random Room" to find a room hosted by another copy of our build,
the host must satisfy, at the moment it publishes room data:

1. `attr 0x4d <= 8`: be **inside one of the nine dungeon rounds** (`GhostshipCove`, `UnderCitadel`, `Catacomb`,
   `MysticRuins`, `MoldyDungeon`, `ForgottenRealms`, `AncientLabyrinth`, `MageTower`, `LostWoods`) - i.e. go online in
   town, then actually set off on a quest. Town (9), the camps (11-13), Arena (15), Tutorial (14) and RandomDungeon (17)
   are all excluded by design.
2. `attr 0x4c >= searcher's local player count`: at least one free slot (automatic with fewer than 4 players).
3. packed `0x53` bits [1:0] == 2: `byte[scene+0x5b] != 5`, `scene[0x10c] != 2`, and no player in the `[+0x2980]==1 &&
   [+0x7b8]==3` state - all normal for a party walking into a dungeon.
4. packed bits [4:2], [6:5], [13:7], [18:14], [20:19] must match the searcher's. Bits [6:5] and [18:14] are two
   **player options** (§5.1), so **two players who have set either option differently cannot see each other** even in a
   dungeon - read `byte[save+0xaf20e]` bits 2 and 7 out of both saves before blaming the lobby.

Note the host **re-publishes** room data as the game runs (`FUN_0009a8c0` -> `SetRoomDataInternal` /
`SetRoomDataExternal`, net.md 4.2), so the attrs change when the host leaves town, and the lobby must re-index them.
Both sides already do: `port/src/net/lobby_server.cpp:427` and `server/lobby-worker/src/index.ts:417`
(`set_room_external`) overwrite `int_attrs`. **No lobby change needed** - this item is closed.

Neither implementation excludes the searcher's own room from `search` (`lobby_server.cpp:346..357`,
`index.ts:353..360` filter only on world/code, fullness, flags and int attrs), which is what makes §9's 0-room results
meaningful rather than inconclusive.

**Timing matters, and §9 shows why:** a single instance issues both of its searches *before* it enters the dungeon, so
it is never matchable at the moment anyone looks. The two-instance recipe is therefore ordered: instance A goes online in
town and **sets off on a quest**; once A's log shows `SetRoomDataExternal` with `77` in 0..8 and `83 = 528642`, instance
B presses "Join Random Room" (or picks the same quest, which uses the `== round` builder `FUN_004d3e80`). B searching
first will always return 0 rooms.

If B still finds nothing with A confirmed matchable, compare B's filter line against A's last `SetRoomDataExternal` field
by field - `76`, `77` and all six `0x53` subfields - and check the lobby is re-indexing (below).

## 8. Open / unverified

* `scene[0xf0]` (-> `info[0x5c]`, and the `(x|8)==9` test): some quest/scene sub-id; 9 observed in town. Not traced to
  its writer.
* `scene[0x10c] == 2`: a scene sub-mode that forces both `0x53` state fields to the non-random values. Writer
  `FUN_0043ac50` `43aea5` from `[r15+0x49c]`.
* the per-player `[+0x2980]==1 && [+0x7b8]==3` test that forces openness to 0 (likely "a player is in a state that
  blocks joining"; not resolved).
* `FUN_00479bf0` and its `+0x394` bit 0 (the other hard "closed" condition).
* text ids `0x8b..0x93` are message-table ids; the strings live outside the eboot, so the round-to-label mapping is
  confirmed only by position.
* the friend account-id list `CreateJoinRoomA` passes (net.md 3.2) is independent of these attrs: `FUN_000bba60` never
  touches it and no condition slot references it. "Join Friends Room" does not go through `FUN_0050f2e0`; presence-based
  joining uses the room-info blob from `FUN_000bea20` instead, so friends/presence work is orthogonal to this fix.
* which two options bits 2 and 7 of `byte[save+0xaf20e]` are (title text ids `0x1e6` / `0x1d1`, §5.1).

## 9. Runtime confirmation - `Instance Run Online.log` (2026-10-16)

One instance, go online in town, then set off on a quest to Catacomb. Room 4122. Every prediction in §3 and §4 fired.
Decoded from the `bin_external` blob (wire offsets; struct offset = wire + 8) alongside the published int attrs:

| log line | call | attr 77 (round) | `info[0x50]` openness | attr 83 | `info[0x5c]` = `scene[0xf0]` |
|---|---|---|---|---|---|
| 328 | `CreateJoinRoomA` | 9 (Town00) | 1 | `528641` = `0x81101` | 9 |
| 349 | `SetRoomDataExternal` | 9 | 1 | `528641` | 9 |
| 358 | `SetRoomDataExternal` | 9 | 1 | `528641` | **1** |
| 373 | `SearchRoom` (`FUN_0050f2e0`) | filter `77 <= 8` | filter `83 == 528642` | -> **0 rooms** | |
| 390 | `SearchRoom` (`FUN_004d3e80`) | filter `77 == 2` | filter `83 == 528642` | -> **0 rooms** | |
| 392 | `SetRoomDataExternal` | 9 | **0** | `528640` = `0x81100` | 1 |
| 402 | `SetRoomDataExternal` | **2 (Catacomb)** | **2** | **`528642` = `0x81102`** | 0 |

What this nails down:

1. **Attr 77 is the round index.** It went 9 -> 2 exactly when the party entered Catacomb, and 2 is `round/Catacomb.brb`
   in the table at `0xbe0f90`. The round-table mapping of §3 is confirmed by gameplay, not just by position.
2. **Attr 83 reaches the searched-for value.** `0x81101 -> 0x81100 -> 0x81102`: only bits [1:0] ever moved, through
   `1` (town) -> `0` (closed, during the transition) -> `2` (open). Line 402's room satisfies **all three** filters that
   line 373 asked for: `76` 3 >= 1, `77` 2 <= 8, `83` == 528642. So a host in a dungeon *is* matchable; §0's conclusion
   holds and no HLE change is needed.
3. **The `(scene[0xf0] | 8) == 9` town clause is real.** At line 358 `scene[0xf0]` dropped 9 -> 1 and openness stayed
   `1` - which is only explicable by that `|8` test (both 9 and 1 satisfy it). Independent confirmation of the asm at
   `439298`.
4. **The other packed subfields never moved**: [4:2]=0, [6:5]=0, [13:7]=34, [18:14]=0, [20:19]=1 in every single
   request, host side and search side. Version gating and the two option bits are stable within one save, as §5.1 says.
5. **`FUN_0008be40` = local player count** is confirmed numerically: the save's `+0xaf204..0xaf207` = `00 00 00 01`
   (pad 3 joined) and both searches filtered `76 >= 1`.
6. **Two searches fire per quest start**, from two different builders (§5): the random one (`77 <= 8`) and the
   quest-specific one (`77 == 2`). Both ran **while this instance's own room was still in town**, which is why both
   returned 0 rooms - and then the game filled the empty slots with an NPC. The NPC fill is the game's normal "nobody
   answered" behaviour, not a lobby failure.

7. **The 0-room results were purely the attr mismatch.** Neither lobby implementation excludes the searcher's own room
   (§7), so room 4122 *was* examined by its own searches at lines 373 and 390 and rejected on attrs - which is a
   positive test of the filter logic, not an artefact of self-exclusion.

### 9.2 Cross-instance match - CONFIRMED (2026-10-16, `A-host.log` / `B-guest.log` / `lobby.log`)

Two instances on one PC against a local `dcp_lobby` (§9.1). A hosted room 4096 and walked into Catacomb; its last
`SetRoomDataExternal` published `76 = 3`, `77 = 2`, `83 = 528642`, and the lobby answered `{"err":0}` to six
`bin_external` updates, so the server-side `int_attrs` were the dungeon values, not the create-time town ones. B then
searched from town with the unmodified random filter:

```
B 351: SearchRoom filters [{76 op6 1}, {77 op4 8}, {83 op1 528642}]
B 352: MATCHING2 search -> 1 room(s)          <-- the whole question, answered
B 358: JoinRoomA {"room_id":"4096"}
B 359: joined room 4096 as member 0x20 (owner 0x10, 2 members)
B 366: signaling Established: member 0x10 at 127.0.0.1:3658
B 369: RUDP socket 2 conn 1 connected
B 809: RUDP ctx 1 terminated: in 1661 msgs / 430594 B, out 755 msgs / 122539 B
```

So the **entire chain works**: the attrs match, the room is found, the join succeeds, signaling resolves the peer, Rudp
carries a real session (430 KB in / 122 KB out over ~1700 messages - gameplay, not a handshake). §0's conclusion is
confirmed: nothing in the HLE needed changing, the host simply has to be inside a dungeon round.

Two incidental observations from the same run:

* The same-PC NAT-hairpinning worry in §9.1 **did not materialise**: the lobby saw both peers on loopback, so
  `g_lobby_local` (`matching2.cpp:163`) kept local endpoints and signaling handed out `127.0.0.1:3658` directly. Two
  instances on one machine are therefore a fully valid test of the P2P path too, not just of matchmaking.
* `MATCHING2 owner 0x10 -> 0x20` followed by `member 0x10 left` (B lines 808-810) means **host migration was
  exercised** - net.md 3.7 lists whether a migrated session keeps playing as an open question. This run only shows the
  ownership transfer at the point A left, so it is evidence the transfer happens, not yet that play continues after it.

**Caveat on the log you audit:** `dcp_lobby` truncates its lines at 421 characters, and `op` is the last key in each
request body, so the op name is missing from every long request. Counting ops in `lobby.log` gives a wrong answer;
match on `bin_external` / `bin_internal` / `rid` instead.

### 9.1 Two instances on one PC

No portable/second-machine build is needed - the existing build takes everything from the environment (asm/code read):

| var | default | why it must differ |
|---|---|---|
| `DCP_NP_NAME` | `%USERNAME%` | `np.cpp:33` - `account_id` is an FNV-1a hash of this name, so a different name gives a distinct PSN identity automatically |
| `DCP_NET_PORT` | 3658 | `matching2.cpp:177`, `net.cpp:64` - two processes cannot both bind the same local UDP port |
| `DCP_SAVE_ROOT` (or `DCP_USER_DIR`) | `%USERPROFILE%/...` | `savedata.cpp:259` - separate save dirs, so the two instances do not fight over `SAVE0.DAT` |

`DCP_LOBBY` (`lobby_client.cpp:140`) can point both at a local `lobby_server` build instead of the Worker if an offline
test is wanted. Note both instances should keep **the same `byte[save+0xaf20e]` bits 2 and 7** (§5.1) or they will be in
different cohorts - copying one save into both save roots is the safe way to start.

The launcher already passes `DCP_SAVE_ROOT`, `DCP_NP_NAME` and `DCP_LOBBY` from `launcher.ini`
(`port/tools/launcher/main.cpp:317..320`) but **not** `DCP_NET_PORT`, so the simplest split is: instance A from the
launcher as usual (gets the default port 3658), instance B from a shell with the three vars set, calling
`dcp_run.exe <dump>/eboot.bin --window` directly. Verified working (PowerShell, `--dry`):

```
$env:DCP_NP_NAME='GuestB'; $env:DCP_NET_PORT='3758'; $env:DCP_SAVE_ROOT='<saves-b>'
& '<dist>/dcp_run.exe' '<dump>/eboot.bin' --window
```

**Caveat for same-PC testing:** both instances reach the Worker through the same external IP (different UDP ports), so
the *search* and the room listing should behave exactly as on two machines, but the subsequent P2P connect may fail on
NAT hairpinning. A failed **join** on one PC is therefore not evidence against this map; the thing to read is whether
B's `SearchRoom` returns A's room at all.

## 7. Source of attr 0x53 bits [4:2] (`[[platform+0x120]+0x84]`) - 2026-10-23

**Result: it is not the Normal/Hard/Infernal choice picked in a menu. It is a per-session "stage tier" dword that mirrors the
active player's hero byte `hero+0x21a9`; 99 (0x63) = "no tier / any".** The hypothesis is only partly right: the value
is read through a 0..4 -> {0,0,1,1,2} difficulty-index mapping (FUN_00024e40: `[obj+0x84]`, 99 -> 4, then FUN_0004c730 with
table at 0x6aa4d0), so the game does use it as a three-step difficulty, but the stored raw value is 0..4 / 99.

* Object: `[platform+0x120]` (platform = `FUN_004adce0(scene)` = `[[scene+0x70]+0x20]`; same object via scene vslot 0xd0) is a
  game-session state block. `+0x84` dword = tier, `+0x88` byte = flag from FUN_00086ee0(hero) (first half running), `+0x70+0x19`
  byte = (tier == 99).
* **Writers of `+0x84` (all three are the same pair of stores)**:
  1. `FUN_00026b40(players, value, slot)` at 26b93 (player array = `[scene+0x80]`, slots `+0xa8 + n*0x23b0`, n <= 3, slot must be
     in use): also stores `hero+0x21a9 = value`. Callers: town scene `FUN_003c6a80` (3c6b86, 3c6bd3, 3c6c24, 3c6c75 - one call per
     slot; value = 2 if FUN_00083ab0(hero) <= 2 else 4; FUN_00083ab0 maps the story counter `hero+0x6f`: 0x30/0x31 -> 2, > 0x31 -> 4,
     else 0, i.e. loop 1 and 2 -> 2, loop 3 -> 4) and town portal handler `FUN_003c1770` (3c2490: value = `record+0x14` of the
     selected round record, record = `FUN_0006a840(round)` = 32-byte entries, <= 20, base via data import 0xbc2368).
  2. `FUN_00026820` at 26a2d (slot copy for a new/replaced player; copies `hero+0x21a9` from the source player, caller 436fb0).
  3. `FUN_004296a0` at 42a8c1 (client "AC_STEP_*" connect state machine): copies a per-player byte received from the host into the
     scene copy (`[scene+idx+0x24b9]`, unverified which wire field) and into `+0x84`.
  `hero+0x21a9` is also part of the hero-record field serializers (FUN_000a9a70 / FUN_000aabe0 / FUN_000c45e0 / FUN_000c4d70), so it
  travels with CREATEPLAYER / bones data; whether it is stored in SAVE0.DAT was not checked (unverified).
* Readers: 99 selects "last visited stage" mode (`hero+0x21ca`, FUN_00083b10) instead of the tier-capped value (FUN_000871f0 =
  min(`hero+0x30` u16, cap 2 / 5 / 98 by `hero+0x6f`)); consumers include 42ddc0, 478aa0/478ae0, 513be0, 50e2d0, 48d930, 521ce0.
* **How a player changes it:** no option or difficulty menu writes it. In town it is reset to 2 or 4 from the story loop
  (`hero+0x6f`) by FUN_003c6a80, and picking a portal destination overwrites it with that round's table value (FUN_003c1770).
  So a town searcher has 2 (loop 1/2) or 4 (loop 3); a host that entered a round (value from the round record, 0 observed for
  round 5) publishes that value.
* **Original game:** the searcher passes the same field as condition slot 10 and the host publishes it in `info[0x54]`; the
  friend-row predicate 4d32f0 compares it for equality (reason 2). Random search therefore only matches hosts whose published
  value equals the searcher's current value; a host in a dungeon (0) and a searcher in town (2) mismatch unless the server
  ignores the slot (server side not known). Hosts created from town publish the town value.
* Open: the table behind 0xbc2368 (record +0x14 values per round); whether the server treats slot 10 as exact match.
