# Friends, presence and "Join Friends Room" (DcNetSignin + NpToolkit2 + DcNetMatching mode 6)

Mapper pass 2026-10-17. Addresses are eboot-relative (as in `q.py`). Tags: **(asm)** = read in capstone disassembly,
**(inferred)** = deduced from structure/usage, **(unverified)** = guess. Baseline: `net.md` 2.6, 3.2, 3.8, 12;
`online_room_attrs.md` (RoomInfo struct); `lobby_protocol.md` 4.2. Nothing in `port/src` was changed.

## 1. Summary

**Rating: careful.** The shim work itself is routine (three Toolkit2 calls, fixed struct layouts, below), but the
flow crosses UI scenes, two command queues and Toolkit2, and three points are only partly pinned (section 7).

Findings, in order of importance:

1. **The game has no friend-list read at all. Friends exist only as the Toolkit2 *presence* result.** "Join Friends
   Room" = open a list window -> queue DcNetSignin command type 4 -> `GetPresence` (Toolkit2 NID `7zsee5IHLis`) ->
   a room is listed only if the friend's presence carries the room-info blob (game data) -> user picks a row ->
   DcNetMatching mode 6 -> `JoinRoomA(roomId taken from the blob)`. No SearchRoom, no search by member id.
2. **Correction to net.md 2.6 / 3.2 / 12.3: the "friend vector" at DcNetSignin+0x98 is a BLOCK LIST, not friends.**
   (asm, high confidence) It is only ever (a) copied into the `blockedUser`-style account array of
   CreateJoinRoomA (req+0x50/+0x58) and JoinRoomA (req+0x48/+0x50), max 2000, and (b) used by the search-result
   handler FUN_000b56d0 (`b5936..b5985`) to **drop rooms whose owner account is in it**; no UI reads it (the
   interface accessors 0x298/0x2a0 have no UI caller). It is refreshed before every create / search / join. So
   hosting does **not** need friend data; an empty block list is correct and the Toolkit2 call (`AleDTt_SyDo` /
   `7Te1eeupXxQ`) can keep returning "empty".
3. **Why the port issues nothing today:** no check fails. The game does call `GetPresence`; the shim answers
   instantly with count 0 (`[container+0x10] == 0`, c7b6a), FUN_000c7a50 returns 1 with an empty list, the list
   window shows zero rows, and the selection call (interface slot 0x178 = matching command 6) only exists once a
   row is picked. The Toolkit2 shims are silent, which is why the logs show nothing.
4. **What the game publishes about itself:** `SetPresence` (FUN_000bea20, Toolkit2 `xCg_igDBo3Q`) with game data =
   the 0x78-byte wire RoomInfo blob `DcNet+0xe58` (the same bytes as room bin attrs 0x55/0x57), whose `+0x66/+0x68/
   +0x70` fields were stamped by FUN_0009c3a0 from DcNetMatching (`+0x120` u16, `+0x124` u32 world id, `+0x130` u64
   **room id**). The friend's room id therefore travels *inside the presence blob*, not as a separate field.
5. **A friend row is accepted only if FUN_000c1af0 says so** (version 0x22, flag bit, non-empty host name, world id
   != 0, round filter) and the row's state is 2 (`entry+0x2c8 == 1`). Details and the exact fields to fake: 5, 6.

## 2. Function table

| addr | proposed name | role | conf |
|---|---|---|---|
| 0xbf8a0 | `DcNetSignin::RunMode(mode)` | jump table 0x6e5020: 0 bfc80 idle/dispatch, 1 bfe10 sign-in, 2 c00c0 sign-out, 3 c0200 SetPresence, 4 c0290 **presence fetch**, 5 c81c0 block-list fetch, 6 c84a0 (stub, returns 1), 7 c8620 word filter, 8 bed30 / 9 bf2e0 Tus, 10 c0370 invite | asm |
| 0xbfc80 | `Signin::Mode0_Idle` | decrements timers; if presence-dirty (`+0x1428`) and signed in -> SetMode(3); else pops the command queue (+0x240) into `+0x258` and SetMode(`byte +0x260`) | asm |
| 0xc10d0 | `Signin::QueuePresenceRefresh(filter)` | queues command type **4** (0x8e8-B entry, filter u32 at entry+0x83c -> `Signin+0xa94`); returns id | asm |
| 0xc11f0 | `Signin::QueueBlockListRefresh` | queues type **5** | asm |
| 0xc1050 | `Signin::MarkPresenceDirty` | sets `+0x1428` = 1 (needs signed in, `+0x157c`) | asm |
| 0xc1a20 / 0xc1a70 | `Signin::CmdDone(id)` / `CmdResult(id)` | id -1 or not in queue and not current = done; result = dword at cmd+4 (`+0x25c`) | asm |
| 0xc0290 | `Signin::Mode4_Presence` | step 0: if `NpHelper+0x100 == 0` call c7a50; step 1: c0520; error flag -> `+0x25c = -1` | asm |
| 0xc7a50 | `Signin::FetchPresence` | **GetPresence** request, parse into vector P (`+0x78`), see 4.2 | asm |
| 0xc0520 | `Signin::ProbeAndFilterPresence` | waits ~1 s of frames (`60 / FUN_00018240`), sends InGameMessage probes to flagged rows (`+0x17c`), then **re-filters vector P with c1af0** and rebuilds it | asm |
| 0xc1af0 | `Signin::IsJoinablePresence(rec)` | the accept predicate, see 4.3 | asm |
| 0xc81c0 | `Signin::FetchBlockList` | Toolkit2 request (`AleDTt_SyDo` / `7Te1eeupXxQ`, req `+0x18 = 1`), fills vector L (`+0x98`) | asm |
| 0xbea20 | `Signin::PublishPresence` | gates, CRC of the 0x144-B object, Toolkit2 SetPresence (`H8cQuFAzgU4` ctor / `xCg_igDBo3Q` / `HeDIXZsguvA` dtor) | asm |
| 0xc0200 | `Signin::Mode3_SetPresence` | bea20 then be7c0 (Tus SetDataA, slot 4, 0x78 B - the `sceNpTusSetDataAAsync` UNIMPL in the logs) | asm |
| 0xb8ed0 | `Matching::PublishPresenceStep` | calls c1050; used after create (b6b30), join (b94b0), mode 11 (b7970) | asm |
| 0xc7990 / 0xc00c0 | `Signin::ClearPresence` (mode 2) | Toolkit2 `hzgYdvXB_IU` / `0fPn7oELg8E` / `vQhIcDhwZvg` | inferred |
| 0xc84b0 / 0xc0370 | `Signin::SendInvite` (mode 10) | Toolkit2 `1bNH47yLJRs` / `OylO3B2fCE0`; account list `+0xaa0`, count `+0xb38` | inferred |
| 0xc6760 / 0xc1c70 | InGameMessage receive cb / send | queue at `Signin+0xf0` (0x158-B entries) | inferred |
| 0xc6850 / 0xc6860 / 0xc69f0 | Toolkit2 event cb | releases result elements of the futures at `+0x15e0`, `+0x1638`, `+0x1688`, `+0x16b0`; events never fire in our shim (fine) | asm |
| 0x9c3a0 | `DcNet::SetRoomInfo(info)` | ORs `info+0x5f \|= 0x80`, stamps `+0x68` (u32 = matching+0x124), `+0x66` (u16 = +0x120), `+0x70` (u64 = +0x130 room id), serializes to `DcNet+0xe58`, 0x144-B object at `DcNet+0xd98` (len dword `+0x140` = 0x78) | asm |
| 0xb5b10 | create/join response parser | `matching+0x120 <- RDI+0xa` (u16), `+0x124 <- RDI+0xc` (u32 world), `+0x130 <- RDI+0x18` (room id) | asm |
| 0xb5170 -> 0x9c3a0 | iface slot 0x310 `SetRoomInfo` | | asm |
| interface | `DcNetIf` vtable base 0x8cd950 (ctor b4710), reached as `[platform+0x16910]` | slots used here: 0x1b0 b4eb0->9c890->bade0 `QueueRefreshFriendRooms(filter)` (cmd 0xa), 0x178 b4e40->9c660->baae0 `QueueJoinFriendRoom(index)` (cmd 6), 0x170 `JoinBrowseEntry` (cmd 5), 0x168 `Search` (cmd 4), 0x228 b4fa0->9c910->bc620 `CmdFinished(id)`, 0x230 `CmdResult(id)`, 0x248 `IsSignedIn` (bece0), 0x288 b5060->9c120 `FriendRoomCount` (vector P), 0x290 b5070->9c1c0 `GetFriendRoom(i, out)`, 0x298/0x2a0 block-list count/get (no UI user), 0x2c8 stub (0), 0x278/0x280 browse-list count/get | asm |
| 0xb6680 | `Matching::Mode0_PopCommand` | 0x78-B command entries at `+0x5a0`; arg of entry+0x6c lands in `matching+0x62c`, index arg of cmd 5/6 in `+0x5cc` | asm |
| 0xb78b0 | `Matching::Mode10_RefreshFriendRooms` | step 0 signed-in check, step 1 `b9de0`, done | asm |
| 0xb9de0 | `Matching::RefreshFriendRoomsStep` | `c10d0(signin, matching+0x62c)`, polls `c1a20/c1a70`; errors only go to `DcNet::SetError`, always returns 1 | asm |
| 0xb7030 | `Matching::Mode6_JoinFriendRoom` | steps: 0 sign-in, 1 `b8720` (init), 2 `b87c0(null,null)`, 3 **`b9630`**, 5 re-run `b87c0(world of friend)`, 6 `b9400`, 7 `b9890` **JoinRoomA**, 8 `b94b0` post-join, 9 `b8280` error teardown, 10 `b8560` finish | asm |
| 0xb9630 | `Matching::BuildFriendJoinTarget` | takes vector-P row `matching+0x5cc`, deserializes its game data, builds a 0x50-B target (+0x24 u16 = info@0x66, +0x28 u32 = info@0x68, +0x38 u64 = info@0x70) in the vector at `matching+0x428` | asm |
| 0xb9400 | `Matching::LeaveAndRefreshBlockList` | leaves the current (self-hosted) room via b7fb0, then b8930 | asm |
| 0xb8930 | `Matching::RefreshBlockListStep` | queues type 5; called by create (b6b30), search (b6cb0), b9400 | asm |
| 0xb87c0 / 0xbb4f0 | `Matching::GetServerId`/`PickWorld(worldIdPtr)` | with a non-null world pointer the friend's world id must be in the GetWorldInfoList result (`matching+0xa0`, stride 0x28, id at +8), else error 0x80550d11 | asm |
| 0xb8a10 | CreateJoinRoomA | block list copied at b8c61..b8cfa (max 0x7d0), req+0x50/+0x58 | asm |
| 0xb9890 | JoinRoomA | `req{roomId @0, flag byte @0x28, blocked ptr @0x48, n @0x50}`, list copied b992b..b99a8 | asm |
| 0xb56d0 | search-result handler | skips own room (`room id == matching+0x130`) and rooms whose owner account is in vector L | asm |
| 0x4b41c0 | `OnlineSceneA::Update` (scene id 0x40, vtable 0xbb0a10, ctor 4b3b20, accessor 4b0db0) | state byte `+0xab`: 2 menu (4b4370), 3 4b48f0, 4 4b5080, **5 4b5630 (friends)**, 6 4b5990, 7 4b5e40, 9 4b5f30 | asm |
| 0x50da90 | `OnlineSceneB::Update` (scene id 14, ctor 50ca40, accessor 4b0640) | states 2 50e2d0 menu, 3 50f2e0 **Join Random Room action**, 4 50f620, **5 50fc90 (friends)** | asm |
| 0x4b4370 | scene A menu | items built by 4b3f00: online = 5 items (type 0 text 0x48f, **type 1 text 0x4b2 = Join Friends Room**, 2 0x45f, 3 0x460, 4 0x461), offline = types 2..4; jump table 0x757450: type 0 -> state 4, **type 1 -> state 5**, 2 -> 6, 3 -> 7, 4 -> back | asm |
| 0x4b5630 / 0x50fc90 | `FriendsRoomState` | state 0 creates list window 0x33 (`4af4b0`), sets its round to 0xf (A) / `[platform+0xadf0]` (B); when the window is done and not cancelled (`+0xfa1 == 0`) calls iface **0x178(selected index)**; then polls via 513fe0 | asm |
| 0x4af4b0 | `WindowFactory(mgr, id)` | ids 0x31..0x34 build the list window (ctor 4d3ba0, tab = id-0x31) | asm |
| 0x4d3ba0 / 0x4d3e80 | list window ctor / Update (tab at `+0xd48`) | tab 0 stub, 1 browse results (0x278), **2 friends rooms filtered by `+0xf90`**, 3 friends rooms unfiltered (-1); state 0 queues the refresh (tab 2/3: iface 0x1b0), list count 0x288 | asm |
| 0x4d3660 | `ListWindow::SetRound(r)` | writes `+0xf90` (and the label) | asm |
| 0x4d32f0 | `FriendRow::Eligibility` | returns a reason code per row (0 = joinable; 1 = row state < 2; 2/3/6/7/8/9 = mismatch/closed/version), see 4.4 | asm |
| 0x513fe0 | cmd poll helper | iface 0x228 / 0x230 of the stored id (`+0xfc`), maps results to UI messages | asm |

## 3. Data structures

### 3.1 DcNetSignin (vtable 0x8cdda8, reached as `[FUN_00016b10()+0xb8]`) - offsets used here (asm)
| off | meaning |
|---|---|
| +0x10c | sign-in state, 2 = signed in (also read by bece0) |
| +0x157c | byte, account OK flag (gate for presence) |
| +0x70 | command id counter |
| +0x78 | **vector P** (presence rows, 0x180 B each): begin +0x80, end +0x88, cap +0x90. After mode 4 it holds only rows accepted by c1af0 |
| +0x98 | **vector L = block list** (same 0x180 record): begin +0xa0, end +0xa8, cap +0xb0 |
| +0xb8 | vector I (begin +0xc0, end +0xc8; accessor slot 0x2b0; filler not seen) |
| +0xf0 | InGameMessage receive queue (0x158 B entries), +0x108 u32 count of flagged rows |
| +0x220 | mutex; +0x238 command vector (begin +0x240, end +0x248, 0x8e8-B entries) |
| +0x258 | current command copy (id dword, `+0x25c` result, `+0x260` type byte = mode, `+0xa94` filter dword); `+0xb40` last finished copy |
| +0x1428 u8 | presence dirty; +0x142c u32 timer; +0x1430 0x144-B copy of last published object; +0x1574 u32 last CRC; +0x1578 pending CRC |
| +0x15b8 | Toolkit2 future: GetPresence; +0x15e0 block-list request; +0x1620 SetPresence; +0x1638 / +0x1688 / +0x16b0 event futures |

### 3.2 Presence row (0x180 B, vector P) (asm)
| off | size | meaning |
|---|---|---|
| 0x00 | 0x14 | SceNpOnlineId (16 chars + NUL + 3 pad), from `entry+8` |
| 0x24 | 0xc0 | game status string, from `sub+0x98` |
| 0xe4 | 0x80 | game data (the RoomInfo wire blob), from `sub+0x158` |
| 0x164 | u32 | game data length (low dword of `sub+0x1d8`); 0 = none |
| 0x168 | u32 | state: **2 = online** (`entry+0x2c8 == 1`), else 0. UI needs >= 2 |
| 0x170 | u64 | account id (`entry+0`) |
| 0x178 | u32 | presence type (`entry+0x2cc`, replaced by `sub+4` in the fallback pass) |
| 0x17c | u8 | 1 = "online but not joinable-looking", candidate for the InGameMessage probe; only set when type & ~1 != 2 and no game data |
(`rec+0x24..+0x164` is the same 0x144-B object as `DcNet+0xd98`: 0xc0 status + 0x80 data + dword length.)
Block-list record (vector L): `+0x00` online id (0x14 B copied from element `+8`), `+0x170` account id (from element `+0`); rest zero.

### 3.3 Toolkit2 results as read by the game
Container returned by the "get result" NIDs: `+0x08` pointer to the element array, `+0x10` u64 element count (asm; `+0x00`, `+0x18` unused here).
- Block list (`AUW6at2PxHs(future)`): element stride **0x20**: `u64 accountId @0`, `SceNpOnlineId @8` (0x14 B copied: 16 B + dword at +0x18).
- Presence (`4mdynkqoCFc(future)`): element stride **0x2e0**: `u64 accountId @0`, `SceNpOnlineId @8`, `u32 @0x2c8` (== 1 -> online), `u32 @0x2cc` (presence type), `ptr @0x2d0` sub-entry array, `u64 @0x2d8` sub-entry count. Sub-entry stride **0x1e0**: `u32 @4` (matched against `@0x2cc`), status string `@0x98` (0xc0 B), game data `@0x158` (0x80 B max), `u64 @0x1d8` game data size.
- `mIinAn8TvdY(future)` = isBusy (al); `5_UHpg8s00I` return code is **not** read on these two paths.
- Requests are built on the caller's stack (the game pre-zeroes only after the call, so ctors may be no-ops for us); only `req+0x18` is set by the game: **1** for the block-list request, **2** for GetPresence.
- SetPresence request (offsets from the caller's stores, bea20 `beb54..bec07`; ctor-set defaults unknown): `gameStatus @+0x18` (0xc0 B), `gameData @+0xe8` (0x80 B), `u64 gameDataSize @+0x168` (0x78).

### 3.4 RoomInfo fields the friend path reads (struct offsets, see online_room_attrs.md for the full table) (asm)
`+0x42` u16 round index (filter), `+0x50` u16 (0 -> reason 6), `+0x54` s16 (must equal `[[platform+0x120]+0x84]`, reason 2), `+0x56/+0x5a` (compared with FUN_00098f00 / 98f60, reasons 8 / 7), `+0x58` version (== FUN_00098f50 = 0x22, else reason 9 / reject), `+0x5c` sub-id, `+0x5e` flag word (bit 10 = 0x400 required by c1af0; `(w & 0x20e) == 0x20e` -> reason 3), `+0x66` u16, `+0x68` u32 world id (non-zero required), `+0x70` u64 room id, `+0x00` host name (non-empty required, via FUN_00081ca0).

## 4. Answers

### 4.1 Where the game gets friends (Q1)
Only NpToolkit2 (no libSceNpFriendlist / Presence / Profile imports exist; checked against `imports.tsv`). Two requests:
`GetPresence` (c7a50) -> vector P with the layout of 3.2 - this is the only friend data the UI shows - and a second
request (c81c0) that is the **block list** (2). `NpManager` contributes only the local account id / online id.
Per friend the game keeps exactly the fields of 3.2. DcNetSignin's "friend vector" layout: 3.1 / 3.2.

### 4.2 Presence read path (Q2), c7a50 in order (asm)
1. First call per step (one-shot `FUN_0058dc40`): clear vector P (`+0x88 = +0x80`), `+0x108 = 0`, issue GetPresence (`req+0x18 = 2`, future `+0x15b8`).
2. While `isBusy(future)` return 0 (pending). Then, for each element: zero a record, copy online id and account id, `rec+0x178 = entry+0x2cc`.
3. For each sub-entry with `sub+4 == entry+0x2cc`: copy status (0xc0) and game data, `rec+0x164 = size`, `rec+0x168 = (entry+0x2c8 == 1) ? 2 : 0`.
4. If `(entry+0x2cc & ~1) == 2` and still no game data: second pass over the sub-entries, every sub-entry with `size != 0` overwrites (type from `sub+4`).
5. If `(type & ~1) == 2` or game data present: keep the row only if `c1af0(rec)`; else (no data, other type): row gets `+0x17c = 1`, kept for the probe, `+0x108++`.
6. Mode 4 step 1 (c0520) then waits about one second of frames, sends InGameMessage probes to flagged rows, **re-runs c1af0 on every row and keeps only the accepted ones**. The list the UI sees = accepted rows only.

What it writes about itself: `PublishPresence` (4 above). Gates (asm): `NpHelper+0x100 == 0` (set 0 by the callback registration at sign-in), `Signin+0x10c == 2`, NetCtl online (59bc90), `+0x157c != 0`, and `FUN_0043c1b0(player+0x88)` (a vector at `+0x9390/+0x9398` must be empty - meaning not decoded). It republishes only when the CRC of the 0x144-B object changes. It is marked dirty after room create/join (b8ed0). On sign-out the presence is cleared (mode 2).

### 4.3 c1af0 accept predicate (asm)
All must hold: game data length != 0; deserialized info `u16@0x5e & 0x400`; `u32@0x68 != 0`; `FUN_00081ca0(info)` (host name) non-empty; not (round `@0x42 == 9` and `@0x5c` in {2, 8, 10}); the filter `Signin+0xa94` is -1 or equals `@0x42`; `s16@0x58 == FUN_00098f50()` (0x22).
**Filter (partly open):** it is the argument of command 0xa. Scene A's friends window sets `+0xf90 = 0xf` (Arena, round table index 15), so scene A lists only Arena rooms or nothing else; scene B passes `[platform+0xadf0]`, which is also the value its "Join Random Room" uses and the user's log shows `0x4d <= 8` for that, i.e. **-1 = any round** in the observed runs. The standard online menu is therefore scene B (inferred; both reach the same interface calls).

### 4.4 Join Friends Room call chain (Q3)
1. Menu (scene A 4b4370 jump table 0x757450, or scene B menu 50e2d0 -> state 5): item "Join Friends Room" (text id 0x4b2) -> scene state 5 = `FriendsRoomState` (4b5630 / 50fc90).
2. State 0: creates list window id 0x33 (tab 2), `SetRound`, waits.
3. Window state 0 (4d3e80, tab 2): `iface[0x1b0](filter)` = cmd 0xa queued -> Matching mode 10 (b78b0 -> b9de0) -> `Signin::QueuePresenceRefresh(filter)` -> command type 4 -> Signin mode 4 (c0290): c7a50 (GetPresence) then c0520; then `iface[0x228](id)` true -> window state 1.
4. Window state 1: row count `iface[0x288]` (vector P), rows `iface[0x290](i)`; row label/eligibility 4d2b80 / **4d32f0** (reason code; user may only confirm a row with code 0 - inferred from the code flow, not traced to the confirm gate). Cancel sets `+0xfa1 = 1` and the scene goes back to the menu.
5. State 0 of `FriendsRoomState` (after the window is done and not cancelled): `iface[0x178](selected index)` = command 6, stores the id in `+0xfc`.
6. Matching mode 6 (b7030): sign-in check, init (b8720), b87c0, **b9630** (row -> target {worldId u32, roomId u64, u16}), **b87c0 with the friend's world id** (must exist in our GetWorldInfoList, id 1 today), **b9400** (LeaveRoom of our own self-hosted room + block-list refresh), **b9890 `sceNpMatching2JoinRoomA(ctx, {roomId, flag 0, blocked list}, cb)`**, b94b0 post-join checks (the progression gate of net.md 3.5), b8560 finish.
So the request is **JoinRoomA by room id**, nothing else. LeaveRoom precedes it (already supported).

### 4.5 Hosting carries a list (Q4)
b8a10 (mode 3 create, after a type-5 refresh via b8930): `Signin+0xa0` records, qword at `+0x170` each, first `min(count, 2000)` account ids into a stack array, `req+0x50 = ptr`, `req+0x58 = n` (zero -> both null). It is the **block list** (2). Use: the account array in CreateJoinRoomA / JoinRoomA, and search-result filtering. Not room visibility for friends. An empty list is the right shim answer.

## 5. What the shims must return (Q5)

Today: `np.cpp` registers the 55 Toolkit2 NIDs; the four getters return a shared zero block, everything else returns 0. Needed, per NID (names are the NID prefixes already used in `np.cpp`):

| NID | now | must do |
|---|---|---|
| `7zsee5IHLis(req, future)` GetPresence | returns 0 | build the presence container P (below) from the lobby friend list, mark `future` complete (or pending until the lobby answered); return 0 |
| `4mdynkqoCFc(future)` | shared zero block | return `&P`; use a separate buffer from the block-list container |
| `mIinAn8TvdY(future)` isBusy | 0 | 1 while a lobby request for that future is outstanding, else 0 (the game polls every frame; no blocking needed) |
| `xCg_igDBo3Q(req, future)` SetPresence | returns 0 | read `gameData = req+0xe8`, `size = u64 req+0x168` (0x78), `status = req+0x18` (0xc0 B); send to the lobby as the account's presence blob; return 0 (>= 0) |
| `0fPn7oELg8E(req, future)` clear presence | returns 0 | clear the stored blob |
| `7Te1eeupXxQ` / `AUW6at2PxHs` (block list) | empty | keep empty (count 0); optional later: elements 0x20 B `{u64 account, online id @8}` |
| `5_UHpg8s00I`, ctors/dtors, event NIDs | ok | unchanged |

Presence container P (static buffers, valid until the next request): `P{+0x08 = rows, +0x10 = N}`; for **each friend that is online and hosting a room** one row of 0x2e0 B:
`+0x00 u64 accountId` (the lobby account id, as the friend's own `sceNpGetAccountIdA`), `+0x08` online id (16 chars + NUL, zero padded to 0x14), `+0x2c8 u32 = 1`, `+0x2cc u32 = 2`, `+0x2d0 ptr -> sub[1]`, `+0x2d8 u64 = 1`;
sub[0] (0x1e0 B zero-filled): `+0x04 u32 = 2`, `+0x98` status string (zeros are fine), `+0x158` = the friend's room-info wire blob (0x78 B, rest zero), `+0x1d8 u64 = 0x78`.
Friends online but not in a room: either omit them (simplest) or give `+0x2c8 = 1` and `+0x2d8 = 0` (the game will then drop them after the probe window).

Blob source: prefer the friend's own SetPresence payload (it carries the stamped room id at struct `+0x70`). Fallback: the room's external bin attr 0x55 held by the lobby (also `DcNet+0xe58`); its first copy is sent at create time **before** the room id exists, later `SetRoomDataExternal` updates carry it (unverified timing). The lobby's `friend_list` today returns only `room_id` / `join_code`: it needs an added `game_data` (hex, the presence blob) field, or the shim fetches the room via `search` + `join_code` and reads attr 0x55.

Other facts the join needs from existing shims (all already true): `world_id` in `RoomDataInternal+0xc` of the host's create response equals an id in the joiner's GetWorldInfoList (1); `RoomDataInternal+0x18` = the lobby room id (becomes info@0x70); the lobby accepts `join {room_id}` while the joiner still sits in its own room only after LeaveRoom (the game sends it first); version 34.
Pre-conditions in the room info of a town-hosted room (observed values): flag word `0x8620` (bit 0x400 set, `0x20e` not all set), version 0x22, `@0x50 = 1`, name non-empty - rows pass c1af0 and 4d32f0, assuming the filter is -1 (4.3) and `info@0x54` equals the joiner's local setting (both 0 in the observed runs).

## 6. Dependencies
Imports: Toolkit2 only (ids above); `sceNpGetAccountIdA`, `sceNpGetNpId`, `sceNpInGameMessage*` (probes; with no flagged rows nothing is sent), `sceNpTus*` (slot 4 write after SetPresence - stubs, harmless). Other subsystems: lobby protocol 4.2 (friend list / presence ops), Matching2 shim (JoinRoomA by id), room info builder (online_room_attrs.md).

## 6a. Implemented (2026-10-17) and run-time values
Shims: `port/src/hle/np.cpp` (Toolkit2 `7zsee5IHLis` / `mIinAn8TvdY` / `4mdynkqoCFc` / `xCg+igDBo3Q` / `0fPn7oELg8E`)
over the game's own lobby session in `matching2.cpp` (`set_presence` / `friend_list`, lobby_protocol.md 4.3). Rows =
friends online, in a room, with a blob. Test path: `DCP_NET_TEST=friend` (net_test.cpp) +
`two_instances.sh` with `DCP_TWO_MODE=friends`.
Values from headless two-instance runs (save with a character in town, mock lobby and `wrangler dev`, both passed):
- **SetPresence is called while hosting** (item 3): once, right after CreateJoinRoom (dirty flag from b8ed0);
  `PublishPresence` first rebuilds the info from the live player (FUN_00438390 -> 438d00), so the blob carries host
  name "Edwin", flag word 0x8620, round 9, world 1 and **the lobby room id at wire 0x64** (the serializer's order, see
  online_room_attrs.md 2; struct +0x70). Status field (0xc0 B at req+0x18) was all zero. With no character selected
  (save at "Select player", 0/16) the blob has no name and flags 0x8200 and c1af0 rejects the row (correct behaviour).
- **attr 0x55 at create time does not hold the room id** (wire 0x64 = 0, world 0): the create attrs are serialized
  before the room exists. No SetRoomDataExternal was sent in these headless runs, so whether later 0x55 updates carry
  it stays open; the presence blob makes it unnecessary.
- GetPresence request `req+0x18 = 2` (as mapped). Joiner: row state 2, type 2, 120 B data, c1af0 accepted with
  filter -1; cmd 6 -> LeaveRoom(own room) -> **JoinRoomA {room_id = host's room}** -> joined, signaling Established,
  RUDP connected both ways.
- **Filter Signin+0xa94 from the real menu press: not measured** (the headless driver passes -1 itself); the shim logs
  it on every GetPresence (`HLE Toolkit2 GetPresence(...): Signin+0xa94 filter = N`). Signin+0xa94 reads 0 after the
  command finished (the current-command copy is reused), so only the value logged at GetPresence time counts.

## 7. Open items
1. **Filter value at run time**: log `Signin+0xa94` (or the argument of `FUN_000c10d0`) on the first "Join Friends Room"; if it is not -1, no friend in town is listed (scene A: always 15 = Arena). Which of scene A / B the user's menu uses is inferred, not traced.
2. Scene B's menu (50e2d0) item -> state mapping was not decoded (only its state table: 3 random, 5 friends).
3. `FUN_0043c1b0` gate of SetPresence (vector `+0x9390 == +0x9398` of the player object): meaning not decoded; log whether `xCg_igDBo3Q` is ever called while hosting.
4. Block-list vs friend-list naming of NID `AleDTt_SyDo` / `7Te1eeupXxQ` is inferred from use (strong, 4.5); the SDK request field `+0x18` (1 vs 2) is a selector of unknown meaning. The `blockedUser` role of req+0x50 / +0x48 follows the same evidence, not the SDK header.
5. The confirm gate of a row (code != 0 rows): not traced; if rows with reason codes can still be confirmed the join would run anyway.
6. Invite path (c84b0, 0x823-B blob), InGameMessage receive (c6760) and vector I (+0xb8) not decoded.
7. Whether `Toolkit2` presence of friends not hosting appears in the real UI list (they are dropped by c0520 here).
