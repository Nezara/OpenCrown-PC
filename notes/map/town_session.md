# Town session: the online party around TOWN (return to town, join in town, guest pull-in)

Mapper pass 2026-10-19. Addresses are eboot-relative (as in `q.py`). Tags: **(asm)** = read in capstone disassembly (or
Ghidra pseudo-C cross-checked against asm), **(inferred)** = deduced from structure/usage, **(unverified)** = guess.
Baseline: `net.md` 3/4/10/12, `friends.md`, `online_room_attrs.md`, `bones.md`. Nothing in `port/src` changed. Raw
decompiles are in `notes/raw/town/` (ignored). Runtime evidence used: the user's `last_run.log` of a real two-PC Join
Friends into a host standing in town (symptom B), `A-host.log` / `B-guest.log` (mid-quest join that works).

## 1. Summary

**Rating: needs high effort.** The party session is not one subsystem but three layers that all have to agree: the
Matching2 / Rudp link (`DcNetMatching`, done), a NetCtrl "connect check" task per peer (sys-level, `9f550` / `9f1e0`),
and the scene state machine `Act` (`42ddc0`, `428ef0`, `42b470`, `4296a0`, ...) that decides when to leave the room. The
asm is clear enough to name every leave site and the follow/pull-in trigger; what is **not** decided by static reading is
(B) which check kills the host's session in our build. That needs the run-time instrumentation of section 9.

Findings, in order of importance:

1. **The one function that leaves the room on a scene change is `FUN_0009d9e0` (call it `Net::DissolveSession`), not
   the title-menu "go offline" entry.** (asm) It reads the 4 player slots, calls DcNetIf slot `+0x110` (= thunk `b4d80`
   -> `9bb70`: sets DcNet+0xedc and queues Matching command 7 = LeaveRoom if signed in), then `a06c0` re-maps the
   remote player slots to AI. `9c6c0` (slot `+0x188`) is only called from the title scenes (`53ce80` and its sub-states
   `53da70` / `53e7c0`, scene 14 `50da90`); it is **not** on the quest-end path. 9d9e0 has ~19 callers.
2. **The original return-to-town has two different branches.** (asm for the code, inferred for the labels) The round-end
   state machine `Act` mode 5 (`42ddc0`) shows a 2-option result window (window id 10, class `519d70`, answer in
   window+0x9108, copied to Act+0x9314). Option value **0** (first row) keeps the session: fade, then the shared
   **world-map window** (state 0xb: every cursor move is broadcast as scene command 0x7e6), then, if "Town" is picked
   (command 0x7e6 handler `435d12` sets Act+0x110 = 1), state 0xd requests round `Town00.brb` and state 0xe enters
   `SetupScene` (mode 4) - **no `9d9e0` on this path**. Option value **1** (second row) calls host-bit-off + `9d9e0`
   (leaves the room, remote slots become AI) before fading. So "party returns to town together" is the world-map
   branch; "one player leaves, the rest continue with that player as an AI copy" is the second row. (The labels of the
   two rows are not readable from the eboot, text is external: unverified which row is which in the UI.)
3. **Guests are pulled into the host's next round by one scene command: SETUP_SCENE (id 0x7dc).** (asm) Receiving it
   (`4355f0` case 0x7dc) copies the round name, sets Act+0x108 = round index and ORs bits into Act+0xec; the normal play
   mode `428ef0` (Act mode 2, runs in town too) tests that bit every frame and calls `SetMode(4)` = `SetupScene`
   (`42b470`, barrier-synchronised STEP_SETUP_SYNC_STEP0..3). No new join is needed for a guest already in the room.
   The separate mid-quest-join path is Act mode 3 (`4296a0`, `AC_STEP_*`).
4. **The original does support joining a host that stands in town, so our friend list showing town rooms is by design,
   not a bug.** (asm) The accept predicate `c1af0` rejects a town room (round 9) only for sub-ids 2, 8, 10; `43bc60`
   keeps the host bit **on** in town for sub-id 1 or 9; `BuildRoomInfo` marks such a room "openness 1 = advertised but
   not randomly joinable" (friends only). The failure in B is therefore in the connect/AC handshake, not in the list.
5. **There are no network commands for town gameplay.** (asm, string inventory) Command families: `DC_SCENEMANCMD_*`,
   `DC_GAMESERVERCMD_*`, `DC_NETCTRLCMD_*`, `DC_CAMPCMD_*`, `DC_BUFFCMD_*`, `DC_AICMD_*`. There is **no** `DC_TOWNCMD`.
   Camps (`Camp_A/B/C`, rounds 11-13) have their own synchronised cursor / cooking commands; town shops do not. A shared
   town is therefore the generic object replication (SetupScene dumps all objects, then per-object `Serialize`), the same
   as a quest. Per-player town / camera / shop entry would be new code, not a switch (section 8).

## 2. Function table

| addr | proposed name | role | conf |
|---|---|---|---|
| 0x9d9e0 | `Net::DissolveSession(sys)` | records which of the 4 slots are connected (iface slot 0x100 x4), calls iface 0x110 (leave), then `a06c0` | asm |
| 0xb4d80 -> 0x9bb70 | iface slot 0x110 `DcNet::LeaveIfSignedIn` | DcNet+0xedc = 1; if `b5520` (= `bece0` IsSignedIn) queue Matching cmd 7 (`baba0`) | asm |
| 0x9c6c0 (iface 0x188, thunk b4e60) | `DcNet::GoOffline` | edc = 1, leave (cmd 7) if signed in, then cmd 2 (`ba620`). Title scenes only | asm |
| 0x9c5b0 / 0x9c5d0 (iface 0x158 / 0x160) | `DcNet::SetHostBit(on/off)` | DcNet+0x210 bit 4 (`0xc15b30`... entry +0x10 of table 0xc15b20) set / clear, `+0xee4 = 0` | asm |
| 0xa06c0 | `Sys::ApplyConnectedSlots(sys, localSlot, bytes[4])` | rewrites the player slot table sys+0x16918 (stride 0x1c, 4 entries; value = DcNet peer index or 0xff) and flag word sys+0x16908; this is what turns remote players into AI after a dissolve | asm (partly inferred) |
| 0xa2ce0 / 0xa2cc0 | `Sys::KickSlot(i)` / `IsSlotAssigned(i)` | iface 0x118 (`9d1d0` -> `9b8f0(dcnet, idx, 0)`) then `a06c0`; assigned = entry != 0xff | asm |
| 0x9e260 | `Sys::AnyHumanPeerLost(&slot)` | true if an assigned remote slot is no longer connected; used by Act mode 2 (-> mode 8) and mode 8 (-> kick) | inferred |
| 0x9f550 | `NetCtrl::ConnectCheckTask` (client side, sys task mode 3) | 6-step per-peer handshake, sends cmds 0x3e9 / 0x3e8 / 0x3eb; error codes 5..0xd, section 6.2 | asm |
| 0x9f1e0 | `NetCtrl::ConnectCheckTask2` (peer/host side) | 2-step; error codes 1..4 | asm |
| 0x9ffd0 | `NetCtrl::PeerStateQuery(task, n, slot, &out)` | helper of the two tasks above (cmd 0x3ec) | inferred |
| 0x9e980 | `NetCtrl::ReceiveCommand` | parses the 0x14-byte command header (id, dest mask, src slot, object id, scene tag round/sub-id), drops commands whose tag mismatches unless the link state is 2..5, dispatches via a1710 / Act vtable +0x138 | asm (partly) |
| 0xa1710 | `NetCtrl::NewCommand(id)` | factory for ids 0x3e8..0x3f8 | asm |
| 0x43f5b0 | `Act::OnNetError()` | reads sys+0x16a38 (error code); codes {14,15,16,19..22} -> `Act::ChangeMode(9)` (ErrorRecover); 0x12 -> mode 7 (ErrorReturnTown); 0x11 -> `9d9e0`; others (1..13) do nothing here | asm |
| 0x43c6b0 | `Act::ChangeMode(mode)` | if mode differs: host-bit off, `9d9e0`, clear flags, set pending mode byte Act+0x94dc | asm |
| 0x43f570 | `Act::EnterAcceptConnect()` | if Act mode is 5 or 2: `SetMode(3)` (AC), returns true | asm |
| 0xa0200 | `Sys::EnterAcceptConnectIfGuest()` | if iface 0x128 (guest has a live link to the owner, `9c060`) and Act mode != 3: `SetMode(3)`. Callers: `42ddc0` (0x431155), `3ba080`, `514380` | asm |
| 0x428690 | `Act::Update(mode)` | mode jump table 0x753c80: 0 idle, 1 `428970` (start / town load), 2 `428ef0` (play loop), 3 `4296a0` (AC, mid-join), 4 `42b470` (SetupScene), 5 `42ddc0` (round-end / transition), 6 `4312d0`, 7 `431910` (ErrorReturnTown), 8 `432000` (drop a lost player), 9 `4320f0` (ErrorRecover) | asm |
| 0x428ef0 | `Act::Mode2_Play` | per-frame play loop (dungeon **and** town); `9e260` -> mode 8; Act+0xec bit `c15b2c` (SETUP_SCENE received) -> mode 4; game-over check (all humans in char state 0xd0 -> guest: `9d9e0`, host: fade) | asm |
| 0x42ddc0 | `Act::Mode5_RoundEnd` | 0x1a-state machine (Act+0x5c): result window, world map, town re-entry, leave. Table 6.3 | asm |
| 0x42b470 | `Act::Mode4_SetupScene` | STEP_SETUP_* 0..0x15 with SYNC barriers (SYNC_STEP0..3, RESPAWN), handles Town00 / Tutorial / Arena by name | asm |
| 0x4296a0 | `Act::Mode3_AcceptConnect` | AC_STEP_FADEOUT .. AC_STEP_START (joiner side of a mid-round join); sends EXCHANGE_BONEINFO 0x7e4 at START (bones.md) | asm |
| 0x4355f0 | `SceneMan::OnCommand` | jump table over ids 0x7d0..0x7ed; 0x7dc SETUP_SCENE, 0x7e3 transition, 0x7e4 bones, 0x7e6 world-map select | asm |
| 0x43cd50 | `Act::BroadcastTransition(kind, next, ?, ?)` | sends cmd 0x7e3 {kind, next, ...} (once; guard bit `c15b38`); receivers `SetMode(5)` with that kind | asm |
| 0x43f200 | `Act::SendSetupScene(mask, name, ...)` | builds cmd 0x7dc from the Act's embedded SETUP_SCENE object (+0x9228); callers `28330`, `2b9d0` | asm |
| 0x2b9d0 | `GameServer::ExecCommand` | executor of `DC_GAMESERVERCMD_*` (ids 0x5dc..); ROUNDEND (0x5f0) -> `43cd50(kind)`, GAMEOVER_NOTIFY (0x5f2) -> `43cd50(3)` | asm |
| 0x28330 / 0x27ff0 / 0x288f0 | `GameServer::ConnectTask` (host side per joining peer) / idle task / mask broadcast | "Connect Start", "Connect Timeout %d", per-peer steps 0..4, sends 0x7dc/0x7df/0x7e0; times out after `DAT_006a5b64` seconds | asm |
| 0x43bc60 | `Act::PublishRoomInfo()` | `438d00` -> iface 0x310 SetRoomInfo -> iface 0x1c8; then host bit: online-unlocked (`477c60`) and mode != 1 only; see 5.1 | asm |
| 0x438d00 | `Act::BuildRoomInfo` | online_room_attrs.md | asm |
| 0x9a8c0 | `DcNet::RoomOpenCloseStep` | host bit on + free slots 0 -> close (cmd 9) / leave (cmd 7); host bit off + in room + not closed -> close (cmd 9); host bit on + not in room -> create (cmd 3) | asm |
| 0x9ad10 | `DcNet::OnPeerDisconnected(slot)` | sets per-peer flag `c15b30`; if no other member: DcNet+0x210 |= `c15b2c` | asm |
| 0x9bbb0 | `DcNet::OnPeerConnected(slot)` | resets the peer record and its send templates (net.md 4.2) | asm |
| 0x4d3e80 / 0x43c250 / 0x3ba080 | quest-search builders | `43c250(this, round)` builds the 5-condition SearchRoom (iface 0x168) used by Act state 0xc and by the RandomDungeon flow | asm |
| 0xc1af0 | `Signin::IsJoinablePresence` | friends.md 4.3; town rejection only for sub-id 2/8/10 (mask 0x504 at 0xc1c06) | asm |
| 0x4d32f0 | `FriendRow::Eligibility` | returns reason 5 first if sys+0x16908 bit0 is set (this player is already a guest) | asm |
| 0x445bd0 / 0x445c50 | scene-end helpers calling `9bb70` | no static reference found (not in a vtable, not lea'd); possibly dead | asm / unverified |

## 3. Data structures (offsets as used by the code)

### 3.1 The "sys" objects (all reached from singletons)
- `FUN_00016b10()` = platform singleton: `+0xb0` DcNet, `+0xb8` DcNetSignin, `+0xc8` DcNetMatching, `+0xc0` ? (asm).
- `FUN_0007cf50(0)` = game sys: `+0x78` platform object (its `+0x16910` is the DcNetIf, `+0x16908` flag word,
  `+0x168f0..` local slot bytes, `+0x16918 + 0x1c*i` player slot table, `+0x16a38` net error code, `+0x16a40` ?),
  `+0x80` PlayerMgr (4 players, stride 0x23b0), `+0x88` **Act scene**, `+0x90` object manager, `+0xa8` window manager
  (`4b01e0(mgr, id)` window lookup), `+0xc0` camera object, `+0xd8` fader, `+0xe8` save, `+0x110` input/party, `+0x120`
  settings, `+0x130` UI helper, `+0xb0` stage-data manager (`479b50`, `+0x5b8` = a flag tested by `43bc60`) (asm).
- Flag bit table at `0xc15b20`: entry n = `1 << n`; named use: `+0` (bit 0) in sys+0x16908 = "I am a guest" (a06c0 sets it
  from iface 0xf0 = my room slot != 0; `4d32f0` and `428ef0` test it) (inferred), `+0x10` = host bit in DcNet+0x210.

### 3.2 Act scene fields used here
`+0x5b` mode (table 2.), `+0x5c` sub-state, `+0x5d` sub-sub-state, `+0xe8` **kind** (0..0x13, see 6.3), `+0xec` flag word,
`+0xf0` scene sub-id (town: 1 or 9 normally; also copied into the room info `+0x5c`), `+0x104`, `+0x108` round index
(table of 18 in online_room_attrs.md 3; Town00 = 9), `+0x110` byte "town chosen in the world map" (asm), `+0x114`
next round index, `+0x94dc` pending mode, `+0x9314` result choice (0/1), `+0x9228` embedded SETUP_SCENE command.

### 3.3 Net error codes (sys+0x16a38), from `9f1e0` / `9f550` / `9d8a0`
1 first-step failure (9f1e0 step 0), 2 link not valid (iface 0x128 false), 3 slot ids 0xff, 4 member count changed,
5 step 0 timeout, 6 step 1 bad state, 7 step 2 retry limit (200), 8 / 9 step 3 timeouts, 0xa step 4 timeout (Act never
entered AC), 0xb step 5 timeout (AC did not finish), 0xc slot ids 0xff, 0xd member count mismatch, 0xe / 0xf peer
disconnected (`9d8a0`). Timeout length `60 * DAT_006e4a74` ticks (the constant is a float, value not read). (asm for
the assignments; the human-readable meaning is inferred.)

### 3.4 Scene commands seen (ids as handled by `4355f0`, `434890` factory)
0x7dc SETUP_SCENE (round name, next stage, stage data; sets Act+0xec bits `c15b2c`, `c15b90`), 0x7dd (sets `c15b30`),
0x7de (byte -> Act+0x9311, sets `c15b40`), 0x7df (`c15b44`, +0x124/+0x126), 0x7e0 (`c15b48`), 0x7e1 (`c15b4c`),
0x7e2 (frees a task), 0x7e3 transition (Act+0xe8 = kind, Act+0x104 = next, `SetMode(5)`), 0x7e4 EXCHANGE_BONEINFO,
0x7e6 world-map cursor / selection, 0x7e5 / 0x7e7..0x7ed object-level. Names of the other ids were guessed by string
order in an earlier pass and are **not** reliable (the SETUP_SCENE id was wrong by 0xc); only the ids in this paragraph
are verified from handler bodies. Game-server ids: 0x5e3 UPDATE_CONNECTEDINFO (`288f0`), 0x5f0 ROUNDEND_REQUEST
(inferred), 0x5f1 / 0x5f2 GAMEOVER_REQUEST / NOTIFY (inferred from request/notify pairing). NetCtrl ids 0x3e8..0x3f8
(`a1710`); exact name per id unknown, the connect-check family is 0x3e8 / 0x3e9 / 0x3eb / 0x3ec.

## 4. Answer A - return to town disconnects

### 4.1 What fires, per branch (asm unless stated)
All sites are in `Act::Mode5_RoundEnd` (`42ddc0`) unless noted. "Leave" = host-bit-off (`call [iface+0x160]`) + `9d9e0`
(= LeaveRoom queued + remote slots -> AI).

| site (call addr) | state / condition | leaves? |
|---|---|---|
| 0x430e4a / 0x430e54 | state 7 (result window answered), answer == 1 ("second row") | **yes**, then fade (state 8) |
| 0x42f264 / 0x42f26d | state 0xf, first frame (reached from state 10 for kinds 4, 8, 9, 10, 0xb, from state 7 when the answer is 2, and for answer 1 via state 10) | **yes** |
| 0x430984 | state 10, kind 3 (game over / forced) | **yes** |
| 0x42f968 | state 0x19 first frame ("Enter" marker, `FUN_00030c70(.., "round/Town00.brb", .., "Enter")`, then `SetMode(2)`) | **yes**, unconditional |
| 0x42fedc, 0x430efe | state 0xc timeouts (waiting for the next-round room search, `43c250`) | yes (give-up) |
| 0x43bd7c / 0x43bd85 (`43bc60`) | after publishing room info: if StageMgr+0x5b8 == 1 and not (round 9 and sub-id in {1, 9}) | yes |
| `43c6b0`, `431910`, `4326e0`, `439db0`, `43cac0`, `43f5b0`, `428ef0` (guest, all humans dead) | mode changes / error handling | yes |
| 0x53cfee / 0x53cffe (`53ce80`) | title scene state 1 only | go-offline (9c6c0) |

The keep-together branch (answer 0): 7 -> 8 (fade) -> 9 -> 10 -> 0xb (sub 1 world-map window, sub 2 fade) -> 0xd (Town00 if
Act+0x110 == 1, else the chosen round; 0xc first if a dungeon was chosen and this player is not a guest) -> 0xe
(`SetMode(4)`, SetupScene). **None of these states call `9d9e0` or the host-bit-off.** (asm for the states, inferred for
the labels.)

### 4.2 Which one hits the user's "return to town"
Not decidable statically: it depends on which result row was chosen and, if it was the keep-together row, on whether
`43bc60` reaches its host-off branch on arrival (rule: StageMgr+0x5b8 == 1 and not town-with-sub-id-1/9) or a net error
occurs (Act::OnNetError codes, 3.3). Because every leave goes through one function, a single log line in a hook on
`0x9d9e0` with the return address (the address is in the table above) answers it in one run (section 9).

### 4.3 What is torn down by `9d9e0` (host and guest alike)
(asm) LeaveRoom is queued (Matching cmd 7 -> mode 7 `b7480` -> `b7fb0` closes all four Rudp slots, then LeaveRoom, then
ContextStop / DestroyContext in `b8280`), DcNet+0xedc = 1 (cleared later by `9a390`), the sys player slot table is rebuilt
so remote players become AI, sys+0x16908 bit 0 is recomputed, and the 4 "connected" bytes captured before the leave are
passed to `a06c0`. Host bit (DcNet+0x210 bit 4) is cleared by the preceding `call [iface+0x160]`, which also makes
`9a8c0` close the room (cmd 9) if it is still open. A fresh room is created later by `9a8c0` when the host bit comes back
(town: `43bc60`), which is the "every online player owns a room" behaviour of net.md 12.1.

### 4.4 Split choice
There is no separate "split" code: the choice is per player in state 7 (answer 0 vs 1). A player choosing 1 leaves via
`9d9e0` and the others carry on; the remaining side keeps a human slot only for connected peers, so the leaver shows up
as an AI copy (the `a06c0` remap). (asm for the mechanism, inferred for the user-visible description.)

### 4.5 Minimal patch to keep the link when "return to town" is taken (see section 7 for bytes)
Skip the leave on the branches that go to town: sites 0x430e4a/54, 0x42f264/6d, 0x42f968 (and the host-bit-off calls in
front of them so the host bit stays on). State left inconsistent: (1) `a06c0` is not run, so the remote slots stay human
(correct for a kept link); (2) Act+0x9314 / 0x114 / 0x110 are still set as for a leave, so SetupScene for Town00 has to
be reached by the world-map branch (0x110 = 1), not by state 0x19 which never calls `SetMode(4)`: **state 0x19 only
sets the entry marker and `SetMode(2)`**, so skipping its `9d9e0` keeps the old scene running with the party still
connected (no town load). The robust patch is therefore to force the answer-0 branch (world map -> Town) for online
sessions, not to NOP `9d9e0` in state 0x19 (unverified: needs the run in section 9).

## 5. Answer B - joining a host that is in town

### 5.1 Does the original support it? Yes (asm)
- `c1af0` accepts round 9 unless sub-id is 2, 8 or 10 (mask 0x504 at `c1c06`).
- `43bc60`: host bit **on** when round == 9 and (sub-id | 8) == 9 (sub-id 1 or 9) and StageMgr+0x5b8 == 1; otherwise
  off + `9d9e0`. With StageMgr+0x5b8 != 1 the host bit is on unless the round is 0xe (Tutorial).
- `BuildRoomInfo` (online_room_attrs.md 4): in town with sub-id 1/9 openness = 1.
- `Act::EnterAcceptConnect` (`43f570`) is valid in Act mode 2 (play, incl. town) and 5; AC does not look at the round.
What PS4 Join Friends shows: the row passes `c1af0` (flag word bit 0x400, version 0x22, name, world id, filter) and
`4d32f0` (reason 0 when free slots suffice and `info@0x54` matches); the label comes from `4d3660`, which has texts only
for rounds 0..8 and 0xf, so a town row has **no round name** (blank or generic, unverified which).
Gate to stop our list offering town rooms (if wanted): reject in the Toolkit2 GetPresence shim any blob whose u16 at
wire +0x3a is 9 (no eboot patch), or patch `c1c04` / `c1c0e` (7.4).

### 5.2 Observed symptom, mapped (last_run.log, joiner side)
Join -> signaling -> Rudp connected -> first messages 173 B (host) / 44 B (joiner) -> 11 thread creations (the joiner's
SetupScene load, it got far enough to start loading Town00) -> 33 messages / 79.7 KB in, 26 / 40.9 KB out -> Rudp
"closed (reason 1)" -> `owner 0x10 -> 0x20`, `member 0x10 left` -> the joiner leaves too. The close precedes the member
left, which is the order of `b7fb0` (close Rudp slots, then LeaveRoom): a **game-initiated leave on the host**
(`9bb70` / `baba0` or `9a8c0`), i.e. the host's own code decided to end the session. (inferred from the order)

### 5.3 Handshake (asm)
1. DcNet message layer: Rudp connect -> `9bbb0` resets the peer; type-0 packets carry NetCtrl commands (net.md 4.2).
2. Both sides run the connect-check tasks: the joiner's `9f550` steps 0..5, the host's `9f1e0` and `28330` per peer.
   `9f550` step 0 sends 0x3e9 with {member count from iface 0x150, local slot, **round (Act+0x108) and sub-id (Act+0xf0)**
   as the scene tag, local player index}; step 1 queries peer state (`9ffd0`); step 2 retries up to 200 times;
   step 3 sends cmd 0x3e8; step 4 waits for sys flag `c15b28` and then calls `43f570` (`SetMode(3)`); step 5 waits for AC
   to finish and sends 0x3eb.
3. The AC mode (`4296a0`): FADEOUT, ACCEPT_CONNECT, WORD_FILTER_CHECK, PLAYER_BACKUP, CLEANUP, CLEANUP_WAIT,
   CONNECTEDINFO_REQ, REQUEST_SCENE, SETUP_ROUND, SETUP_STAGE, SETUP_CAMERA, RECV, SHARE_ROUND_STAGE, OBJ_SHARE_WAIT, WAIT,
   START (bones exchange), FADEIN, END; each has a "Time out" message and falls to `ErrorReturnTown` (mode 7).
4. Host side: `28330` per peer: step 0 `43c1b0` precondition, step 1 sends 0x7dc (`43f200`) + 0x7df, step 2 waits for the
   joiner's child scene to reach SetupScene mode 4 then sends 0x7e0, step 4 handles a peer dropping (kill 0x7e1).
   `Connect Timeout` = peer removed from the queue after `DAT_006a5b64` seconds or when `a2cc0` is false.

### 5.4 Why would the host end the session? Ranked candidates (all unverified at run time)
**Runtime (section 11): (1) is ruled out (no tag drops in any join); (2) does not fit a host that stood still in the
portal area (user, 2026-10-20). Root cause open; 11.4 lists the host-side trace lines that decide it.**
1. **Scene-tag mismatch dropping the connect-check commands.** `9e980` only executes a command when its round / sub-id
   tag equals the receiver's (`Act+0x108` and `Act+0xf0`), or when iface 0xf0 state is outside 2..5. Town's `Act+0xf0`
   changes while loading (online_room_attrs.md 9: 9 -> 1 with openness staying 1), so the host's and joiner's tags can
   differ for several seconds. A dropped 0x3e9 / 0x3e8 makes `9f550` time out (codes 5 / 8 / 9 / 0xa) and the task's
   cleanup runs `a06c0` (remote -> AI) and sends 0x3eb: **exactly the "each side only has an AI copy of the other"
   symptom.** Matches the ~3 s delay (timeout `60 * DAT_006e4a74` ticks).
2. `43bc60` host-off branch: if StageMgr+0x5b8 == 1 and the town's sub-id is not 1 or 9 at the moment of the republish,
   the host executes host-off + `9d9e0` (immediate leave).
3. `9a8c0`: free-slot count (room info `+0x40` = attr 0x4c) reaching 0 with the host bit on makes the host close or leave.
   With a 4-slot room and 2 players this should not happen unless the info is rebuilt wrongly (the join changes member
   count; `438d00` counts present players via `29320`).
4. `9e260` / mode 8 (`432000`): a peer that is "assigned but not connected" is kicked (`a2ce0`) - this removes the peer,
   it does not itself leave the room.
5. Host game-over rule (`428ef0`): all humans in char state 0xd0 -> a *guest* leaves (`9d9e0`), a host fades. Not
   applicable to a host standing in town.
No code was found that sends a CONNECT_NG or ends the session merely because the host is in town.

### 5.5 Ally / NPC copies
(asm) A remote player's character exists as a player object created by CREATE_PLAYER commands during SetupScene (ids
0x7d4, handler `436fb0`) with the sender's 0x21d0-byte record (net.md 4.3). When a remote slot is not connected
(`a06c0` slot table entry 0xff) the player manager fills the slot with an AI copy taken from the player's selected ally
(`player+0x2288 + k*0x23b0`, bones.md 4.1/4.4). The exchange of ally records between peers is the 0x7e4 command, sent by
each peer at AC_STEP_START and in SetupScene's start step; it only runs if the link survives that long.

## 6. Answer C - pulling a guest who is already in the room into the host's quest

### 6.1 Trigger (asm)
Host side: the round-start request reaches the game server (`2b9d0`) and `43f200` builds SETUP_SCENE (0x7dc) from the
Act's embedded command (+0x9228) for all members. Guest side: `4355f0` case 0x7dc stores the round name, sets Act+0x108
to the round index (`4aab0(name)`), stores NextStageIndex and stage data, and ORs the bits `c15b2c | c15b90` into
Act+0xec. `Act::Mode2_Play` (`428ef0`) tests `Act+0xec & c15b2c` on every frame and calls `SetMode(4)` = SetupScene, whose
SYNC steps (a barrier over all connected peers) then load the same round on both sides. **The trigger is therefore the
SETUP_SCENE command from the host, not a Matching2 event and not the room-data change**; a SetRoomDataExternal update
(round attr 0x4d: 9 -> 0..8) is only a consequence. (The host side of "who sends 0x7dc when the portal is used" is the
GameServer WORLDMAP_START request, `0x377c0` creates it; the town portal -> request link was not traced: unverified.)
Also seen: the transition broadcast 0x7e3 (`43cd50`) moves every member to Act mode 5 with the host's kind; used for
round end, not for town departure.

### 6.2 Mid-quest-join path (for a guest that is *not* yet synchronised)
AC (Act mode 3, `4296a0`), entered through `43f570` (from `9f550` step 4) or `a0200` (from the UI flows `42ddc0`
state 0xc, `3ba080`, `514380`). For a guest whose Act is in mode 2 / 5 this is the way to be taken into the host's *current* scene.

### 6.3 Act mode 5 states (`42ddc0`, Act+0x5c), compressed
0 start (kind 3 waits then -> 5) | 1 stats / party refresh | 2 choose next round index per kind (writes Act+0x114) |
3 wait | 4 random pick | 5 clear flags | 6 open window 10 (result) | 7 wait answer (Act+0x9314) | 8 fade | 9 apply |
10 branch by kind / answer | 0xb world-map window (0x7e6 broadcast) | 0xc online next-round search (`43c250`) |
0xd load request (`30c70`: round name, entry 9 or 1, marker) | 0xe `SetMode(4)` | 0xf leave + stats | 0x10..0x14 wrap-up |
0x15..0x18 dialogs / errors | 0x19 common exit (leave, "Enter", `SetMode(2)`). Kinds (Act+0xe8): 0..2 story round, 3 game
over / forced, 4, 8..0xb special (auto-answer 1), 5 / 6 / 7 world-map style, 0xc, 0xe, 0xf Arena, 0x10..0x13 random
dungeon / error returns (labels inferred).

## 7. Patch points (all bytes read from the eboot; `rel32` calls shown as `e8 ..`)

The port already has a byte-patch facility that verifies expected bytes (`port/src/hle/skip_offers.cpp`); use the same.

### 7.1 Keep the link on the "return to town" branches
| addr | original | replace | effect |
|---|---|---|---|
| 0x430e4a | `ff 90 60 01 00 00` (host-off) | 6 x nop (`66 0f 1f 44 00 00` is a 6-byte nop) | state 7 answer 1: host bit stays |
| 0x430e54 | `e8 87 cb c6 ff` (9d9e0) | `0f 1f 44 00 00` | state 7 answer 1: no dissolve |
| 0x42f264 | `ff 90 60 01 00 00` | 6-byte nop | state 0xf |
| 0x42f26d | `e8 6e e7 c6 ff` | 5-byte nop | state 0xf |
| 0x42f968 | `e8 73 e0 c6 ff` | 5-byte nop | state 0x19 first frame (see 4.5 caveat) |
| 0x43bd7c / 0x43bd85 | `ff 90 60 01 00 00` / `e8 56 1c c6 ff` | nops | `43bc60` never leaves because of the town sub-id rule |
Conditional version (preferred): hook `0x9d9e0` and `0x9bb70` in an HLE trampoline that returns immediately when a
"keep party" flag is set and the caller is one of the addresses above; log otherwise. This avoids editing the many
other callers (errors still dissolve the session, which is right).

### 7.2 Joiner stays in its own town while in the room (no AC)
Hook `0x43f570` (`Act::EnterAcceptConnect`) and `0xa0200`: when Act+0x108 == 9, return 1 without `SetMode(3)`. Effect:
`9f550` step 4 advances, step 5 sees mode != 3, ends the task cleanly (sends 0x3eb), no scene load is requested. Also
make the host's `28330` not send SETUP_SCENE to this peer: its step 1 (`43f200` call) is the load trigger; skipping that
call for peers whose tag round is 9 is the host-side half (address of the call: inside `28330` step 1, not captured, to be
read when needed). Without the host half the host still sends 0x7dc and the joiner's mode 2 switches to mode 4 anyway
(6.1) - i.e. both halves are required.

### 7.3 Pull-in on host departure
Cheapest correct trigger is the native one: do not suppress the 0x7dc that the host sends when it leaves town; on the
guest, `428ef0` then does `SetMode(4)` and SetupScene's barrier loads the host's round. The risk is that SetupScene was
designed for peers that did the full AC (CLEANUP / PLAYER_BACKUP / CONNECTEDINFO) first. If the native follow misbehaves,
fall back to the AC path: on a room-data update showing the host's round attr 0x4d in 0..8 while the local Act round is 9,
call `0x43f570` from the game thread (Act pointer: `FUN_0007cf50(0) + 0x88`).

### 7.4 Optional: stop the friend list offering town rooms
`0xc1c04` `77 15` and `0xc1c0e` `73 0b` -> `90 90` each: every round-9 row is rejected (falls into `xor ebx,ebx`).

## 8. Shared town feature (per-player camera / shop entry): state of knowledge

Not mapped, and the asm suggests it is new code rather than a flag:
- The town is the normal `Town00` round; remote players are ordinary player objects replicated by SetupScene + `Serialize`.
- Camera: sys+0xc0 (position floats at +0x70 / +0x74 are read in `28330` when a peer is added; speed at +0xc4);
  who follows whom (all four players, single camera) was not traced. Shop doors / stable / portal are town objects
  (`DcSysObj_Town*`, `DcAI_SysObj_Town`, `DcUi_TownMenu`, `Temple::Update 3c3130`) driven by the local player object;
  no network command exists for them, so shop entry is serialised only by the replicated player state and the
  single-UI assumption, not by a lock.
- Risks: object ownership ("CtrlId", `DC_NETCTRLCMD_CHANGE_HOST_CTRLID`) means town-object state is host-owned; a guest's
  local shop UI would edit the host's shop objects; save-data writes (gold, inventory) are local per player but UI
  windows are created on the Act's window manager (sys+0xa8) that exists once per instance, so concurrent shops are
  fine per instance but the camera/fader (`sys+0xc0` / `+0xd8`) are shared by SetupScene's fade steps.
- A per-player camera needs: the camera follow target (read `FUN_00010310` / `000103d0`, `FUN_00443af0` fader), a
  patch making it follow the local player only, and a proof that the replicated remote player does not pull the camera.
  Shop entry needs the town-object event (`EventRevive` family strings at 0x3bf530 / 0x3c0dd0) to be taken from the
  local player only. Both require a separate pass.

## 9. Open questions and what the two-instance run must confirm

Instrumentation to add to the run (all addresses are hook points, none needs the eboot patched):
1. Log every call of `0x9d9e0`, `0x9bb70`, `0xbaba0`, `0x9c6c0` with the return address and the Act mode byte (`Act+0x5b`)
   and sub-state (`+0x5c`), host and guest. This decides section 4.2 and 5.4 in one run.
2. Log writes of sys+0x16a38 (error code, `0x43f5b0` entry reads it) and `Act::SetMode` (vtable `+0xa8`, `0xa0`) calls with
   the new mode; log Act+0x108 / +0xf0 at the moment `9f550` step 0 sends 0x3e9 and at `9e980` for each dropped command.
3. Log the first bytes of the 173 B and 44 B messages (command id at header +8, dest mask, scene tag) to confirm the tag
   theory of 5.4 (1).

Scenario scripts (`Work/stage5`, `DCP_NET_TEST=friend`, `two_instances.sh` with `DCP_TWO_MODE=friends`):
- friends-in-town (host idle in town, joiner via friend list) with instrumentation 1-3: expect the leave site and error
  code. Run twice, once with the host standing at the stable (sub-id may differ).
- host leaves town through the portal with a guest present and kept link (patch 7.2 on): confirm the 0x7dc reaches the guest
  and `428ef0` switches to mode 4 (log 2).
- quest end with two players, answer 0 then "Town" in the world map: confirm no `9d9e0`, SetupScene Town00 on both, and
  that `43bc60` keeps the host bit (log 1). Then answer 1 on one side: confirm the leaver's `9d9e0` and the AI copy.

Still unknown: which result row is which; StageMgr+0x5b8 meaning; the portal -> GameServer request link; the float
timeout constants (`DAT_006a5b64`, `DAT_006e4a74`); the exact names of NetCtrl ids 0x3e8..0x3f8; the host-half call address
in `28330` for 7.2; whether state 0x19 can load Town00 by itself; the camera owner; `445bd0` / `445c50` reachability.

## 10. Dependencies
Imports: none new (Matching2 leave/create/join, Rudp, Np presence as in net.md / friends.md). Subsystems: net.md 3/4/10,
friends.md 4 (list predicate), online_room_attrs.md 2-4 (room info), bones.md 4 (ally exchange, AI copies),
story_progress.md (online unlock counter used by `477c60`).

## 11. Runtime results (2026-10-19, `DCP_NET_TRACE=1`)

Tooling (diagnostics only, off unless `DCP_NET_TRACE=1`): `port/src/hle/net_trace.*` hooks (int3 + relay with the
displaced instruction, bytes checked) every site of section 9: 9d9e0, 9bb70, baba0, 9c6c0, 9c5b0 / 9c5d0 (host bit),
bad20 (close room), 9b8f0 / a2ce0 (drop / kick peer), a06c0, 43c6b0, 43f5b0, 43f570 / a0200, the tasks 9f550 / 9f1e0 /
9d8a0 / 28330 (state changes), the 9e980 receive decision at 0x9eaf6 / 0x9ec87 (header, both tags, drop reason), 9d5e0
sends, the game's own debug text helpers 28770 (GameServer) and 4364d0 (Act: `AC_STEP_*`, `STEP_SETUP_*`,
`Act %d -> %d`), plus a 2 ms watcher of Act / platform-sys / DcNet / matching state; `rudp.cpp` dumps the first 40
messages per direction (48 bytes). `DCP_NET_DELAY_MS` adds receive latency. Lines start with `NETTRACE`. Platform sys =
game sys `[0xc1dc40]+0x78` (confirmed: it is the `rdi` of 9d9e0 / 9f550), Act = `+0x88`.

### 11.1 The headless join was not the menu path (fixed in the driver)
The old `DCP_NET_TEST=friend` driver joined the room but never started the game's connect check, so the joiner idled
(the host sent only pings). The menu's `FriendsRoomState` (`50fc90` -> `514380`) calls **`a0200(platform sys)`** after
the join; the driver now does the same (`DCP_NET_TEST_AC=0` = old behaviour), **but only once `sys+0x168f4 != 0xff`**:
- On Rudp connect the game itself sets sys mode 2 = `9f1e0` (joiner side, not host side as guessed in 2.): it sends
  0x3ec, the owner answers 0x3ed (the 173-B first message of the real log), which stores the owner slot in
  `sys+0x168f4`; then it waits in step 1 for sys flag bit 7.
- `a0200` -> sys mode 3 = `9f550`. Called before the 0x3ed answer (first try, 2 frames after connect), `9f550` fails at
  entry with **error 0xc** (`+0x168f4 == 0xff`) and leaves at once (`43f5b0` code 12, then 9bb70 / a06c0 from 9f550).
- After the answer: `9f550` 0x3e9 -> 0x3ea, 0x3ec -> 0x3ed, 0x3e8 -> host 0x3f0 / 0x3ef / 0x3ee, step 4 -> `43f570` ->
  Act 2 -> 3 (`AC_STEP_FADEOUT` .. `AC_STEP_END`, ~140 frames), Act 3 -> 2, joiner sends 0x3eb, both sys modes back to
  0. Host side: sys mode 1 while the peer checks in, GameServer `28330` "CmdConnectPush" / "Connect Start" steps 0..4
  -> "Idle Start". Slot tables end `0 1 255 255` on both. NetCtrl ids seen: 0x3e8..0x3f8 (0x3f7 / 0x3f8 = local pair
  every 30 frames, 0x3f5 periodic per peer, 0x3f1 / 0x3ef on slot changes, 0x3f2 / 0x3f4 / 0x3f3 cleanup / connected info).

### 11.2 B: friend joins a host idle in town - works headless
Runs (mock lobby, `two_instances.sh` with `DCP_TWO_MODE=friends`, host frame 2400, join 2500): the same save on both
(saves-b), different saves (`DCP_SAVES_JOIN` = the user's current save, the joiner of the real log), and 150 ms one-way
delay on both. **All three join and stay joined** (250 msgs / 197 KB in by v4800, no leave on either side).
- **Scene tags never mismatch** in a normal join: every NetCtrl command carries tag 9/0 = both Acts' round / stage;
  **0 commands refused by tag** (0x9ec87) in any run. The only drops are `DROP(flag)` object / 0x7e5 commands on the
  host during the joiner's AC (generation / sender-player check before the peer is registered); harmless. Candidate
  5.4 (1) is ruled out for this path.
- The joiner's out counters in the real log (26 msgs / 40,907 B) match the headless joiner at `AC_STEP_START` /
  `AC_STEP_FADEIN` (25-26 msgs / 39.9 KB, its 18,072-B player record just sent): the real failure came at the end of
  the joiner's AC.
- The real close is `reason 1` = a FIN from the host's carrier (`sceRudpTerminate` / LeaveRoom `b7fb0`), not a crash
  (a crashing dcp_run is killed by `TerminateProcess`: no FIN, the peer would time out after 10 s with reason 2).

### 11.3 What the trace shows the game does with town stages (Act+0xf0 = stage index inside Town00)
`Act+0xf0` is written only at `0x43aaf7` (`43a5a0`, the stage setup used by SetupScene / AC / round end): it is the
**stage (area) index of the round**, not a scene sub-id. Town00 stages seen: 2 = inn interior, 0 = street after the
tavern door, 1 = the next area to the right (forest path / portal side); the user's real rooms show 9, then 1.
- **Every SetupScene end runs `43bc60`**: in town (round 9, `StageMgr+0x5b8 == 1`) it keeps the host bit only for stage
  1 or 9; any other stage -> host-bit-off (0x43bd7c) + **`9d9e0` Dissolve (0x43bd85)**. Seen in every run at stage 2
  (v1219) and stage 0 (v1530); at stage 1 it turns the host bit on (0x43bd6a, v4775).
- **A host stage change takes the guest along**: host walking 0 -> 1 with a guest present (run 6): host GameServer
  `SETUPSCENE_REQUEST`, scene command 0x7dc `NextStageIndex 1` to the guest, both run `STEP_SETUP_*` with the SYNC
  barriers in lock-step, host bit off at `42bb74` (room closed while loading, `bad20` from 9a8c0), `43bc60` host bit
  on for stage 1, both back in Act mode 2 with the slot table intact. (The test driver re-sets the host bit after a
  scene change, net_test step 5; that hides the close / reopen, not a Dissolve.)
- So a town room only exists while its owner stands in stage 1 or 9, and **the owner walking into any other town area
  dissolves the session** with exactly the real symptom: Rudp slots closed gracefully, then LeaveRoom (guest sees
  `owner 0x10 -> 0x20`, `member 0x10 left`), `a06c0` turns the remote player into an AI copy on both sides.

### 11.4 B: conclusion (revised 2026-10-20) - root cause still open
The user's friend reports that the host stood still in the portal area during the failed join (room 4251), so the
stage-change explanation drafted on 2026-10-19 does not fit. **That exact case works headless** (run p3: the host walks
to the portal area, stage 1, where `43bc60` itself turns the host bit on (0x43bd6a); a friend joins at v4100; AC loads
stage 1; the link holds past v6600 with no leave, `DCP_TOWN_PARTY=0` = original code). What the trace settles for the
coordinator's questions:
- **Does the guest's AC make the host re-run SetupScene / `43a5a0`?** No. In every join (runs 3, 5, p3, L_on, G_on)
  the host Act stays in mode 2 for the whole AC; the only `43a5a0` run is on the joiner (`AC_STEP_SETUP_STAGE`), with
  the host's stage index (0 or 1). Confirmed by trace (watcher + ACTLOG).
- **Is the portal area stage 1 or 9?** Walking from the street: stage 1 (trace, `NextStageIndex 1`, host bit kept).
  Arriving from the world map: entry 9 or 1 (asm of `42ddc0` state 0xd, not run); the user's own rooms show 9. Both
  pass `43bc60`, so a host standing there never dissolves by that rule.
- **Timeouts / SYNC barrier with a slow joiner?** Not on this time scale. The host's GameServer ConnectTask times out
  after `DAT_006a5b64` = **35.0 s**, the connect check after `60 * DAT_006e4a74` = **60 s** of ticks; the real drop came
  ~2 s after connect. The host runs no SYNC barrier during the guest's AC (mode 2 throughout). The `AC_STEP_* Time out`
  paths are on the joiner and end in the joiner's ErrorReturnTown (the joiner would leave, not the host). 150 ms
  latency each way changes nothing (run 5). A slow disk on the joiner was not simulated.
- **Other ways the host sends a FIN:** (a) LeaveRoom (`b7fb0` closes every Rudp slot, then leaves): queued only by
  `baba0` <- `9bb70` (from `9d9e0`, `9f550` cleanup) or `9c6c0` GoOffline; (b) one peer dropped: `9b8f0` / `a2ce0`
  (lost-peer mode 8 `432000`, ConnectTask timeout), FIN to that peer only, the host stays in its room; (c) the online
  menu scene: `50da90` (scene B) calls `9c6c0` and, through `510650` / `510840`, `43bc60`; (d) **the host pressing
  "Join Friends Room" / "Join Random Room" itself**: matching mode 6 / 5 first leaves its own room (`b9400`), which is
  FIN + owner change on our side - plausible if both players were trying to join each other; (e) our shim:
  `sceNetSocketClose` (carrier destructor FINs every link) and the game's `sceRudpTerminate`. A crash is ruled out
  (11.2).
- One observable difference in the user's log between the failed and the successful join: right after connect the
  joiner's own `SetPresence` carried **no name and flags 0x8400** in the failed attempt (line 2535) but `'Edwin'`
  0x8420 in the successful one (line 3249) and in every headless join (0x8460). The player record was empty at that
  moment; not explained yet.

**What decides it: the host's log of a failed join with `DCP_NET_TRACE=1`.** Lines to look for on the host, within the
~3 s before its `RUDP ... closed` / `sceNpMatching2LeaveRoom`:
- `NETTRACE ... Dissolve(9d9e0) ret eboot+0x...` and its `| act mM.S ...` (who dissolved: 0x43bd8a = `43bc60` stage
  rule; 0x43c6xx ChangeMode; 0x43f6xx OnNetError; 0x9f8b6 connect check cleanup; 0x429xxx / 0x431xxx Act modes);
- `QueueLeaveRoom(baba0) ret ...`, `LeaveIfSignedIn(9bb70) ret ...`, `GoOffline(9c6c0) ret ...` (0x53d004 / 0x50xxxx
  = a menu scene), `HostBitOff(9c5d0) ret ...`, `QueueCloseRoom(bad20)`;
- `DcNetDropPeer(9b8f0)` / `KickSlot(a2ce0)` (peer kicked), `Act::OnNetError(43f5b0) ... code N`,
  `Act::ChangeMode(43c6b0) ... new mode N`, `GSLOG 'Connect Timeout %d'`, any `ACTLOG '... Time out'`;
- `HLE sceNpMatching2JoinRoomA` / `SearchRoom` on the host (case (d): the host was itself joining someone);
- `NETTRACE ... RX ... DROP(` / `REFUSED` lines and the `STATE act ...` lines around the drop.
The joiner's log with the trace adds its own `Act %d -> %d`, `AC_STEP_*` and error lines for the same seconds.

The town-stage rule of 11.3 is still real and is what `DCP_TOWN_PARTY` changes (11.6), but it is not shown to be B.

### 11.5 A (quest end -> town): not reached
Not feasible headless: it needs two scripted instances to clear a quest (boss fight) and pick a result-window row; the
driver has no such script. The trace answers it in one real run: `DCP_NET_TRACE=1` on both PCs, then look for
`Dissolve(9d9e0) ret eboot+0x...` and its `act mN.S` (mode / state) in each `last_run.log`. From 11.3: the world-map
return lands in Town00 entry 9 or 1 (state 0xd), which `43bc60` keeps; a dissolve on arrival would therefore come from
the result-window branch (0x430e54 / 0x42f26d / 0x42f968) or from the first walk into another town area (0x43bd85).

### 11.6 `DCP_TOWN_PARTY` verification (2026-10-20)
Patch: `skip_offers.cpp` (port/ and linux-port/), `0x43bd46 75 2a -> 66 90`, on by default, `DCP_TOWN_PARTY=0` = off.
Windows dcp_run built, tests 7/7. Runs with `DCP_NET_TRACE=1`, mock lobby, host saves-b, joiner the user's save.
- **Patch effect confirmed at the instruction level:** with it on, the end-of-SetupScene `43bc60` turns the host bit
  **on** at stage 2 (inn, v1219) and stage 0 (street, v1531) (`HostBitOn ret eboot+0x43bd70`); with it off the same
  frames show `HostBitOff ret 0x43bd82` + `Dissolve ret 0x43bd8a`. Side effect: an online player now keeps (and
  advertises) a room in every town area, the inn included, not only in stages 1 / 9.
- **Not reached: a host move into a stage other than 1 / 9 with a guest present.** The only town transition the pad
  scripts reach is street (0) -> portal area (1) (allowed with or without the patch). Walking left from stage 1 for
  2,900 frames never leaves it (run L_on). Pressing R1 at Morgan's Magic Item Shop door enters the shop (stage 6) when
  alone (ex3, `NextStageIndex 6`), but **with a guest present four R1 presses at the door do nothing** (run f_shop3,
  screenshots: both players at the door, no scene change): the game seems to block shop entry while a guest is in
  the party. The tavern door was not entered either (f_inn). So "no Dissolve at 0x43bd85 on a non-1/9 move with a
  guest" and the inn / shop behaviour with a guest are **unverified**; the control (`DCP_TOWN_PARTY=0`) for that case
  therefore has nothing to compare against, beyond the stage 0 / 2 dissolves before going online shown above.
- **Host stage change with a guest (0 -> 1), patch on:** host `SETUPSCENE_REQUEST`, 0x7dc `NextStageIndex 1` to the
  guest, both run `STEP_SETUP_*` incl. `STEP_CHECK_HOSTCTRLID` / `STEP2_CHECK_HOSTCTRLID` in lock-step, host bit on at
  0x43bd70, both back in Act mode 2; Rudp counts keep rising (run 6 and G_on).
- **Guest changes stage alone (G_on, patch on):** the guest walking right does not load anything by itself: its
  request goes to the host (`GSLOG 'SupSn ign ...'` on the guest, `SETUPSCENE_REQUEST` on the host at v4646) and the
  **host moves the whole party** to stage 1 (both `STEP_SETUP_*`, host bit on, link holds: 1,787 msgs out of the host
  by v6000). No stuck state, no leave.
- Shared town works visually: after the join both players stand together at the shop door in both instances
  (screenshots `s4` / `sj4`).
Tooling added: `two_instances.sh` `DCP_HOST_ARGS` / `DCP_JOIN_ARGS` (extra dcp_run args, e.g. `--render --shots ...`;
note `--shots` ends the process after the last shot, which the peer sees as a leave).

## 12. Return to town (A), 2026-10-24

Debugger pass. Static reading of `42ddc0` states 6-0xb, the result window class `519d70` (via `5199d0`), the world map
window (`547b90`), scene commands (`4355f0`) and the GameServer table (`2b9d0`, jump table 0x6a5cf8); plus three
headless two-instance runs with a scratch build (round end forced on the host, section 12.4). Fix 12.5 is in
`port/src/hle/skip_offers.cpp` and `linux-port/src/hle/skip_offers.cpp` (on by default, `DCP_PARTY_RETURN=0` = off).

**REVERTED in v1.0.5.1 (2026-10-24, user, after v1.0.5 shipped it):** players reported that it skips the spoils screen
(no loot, no EXP) and the party return only reaches the portal area: heading on into town proper drops players out of
the session. Fix 12.5 is removed from both trees; the analysis below stays as the map for a second attempt.

### 12.1 The result window is per player; nothing carries the host's answer (asm)
- Each instance opens its own window 10 (class `519d70`) in Act mode 5 state 6; input is the local pad only
  (`4ae190` = window+0xf4 pad; vtable +0x118 / +0x120 move the cursor to row 1 / row 2, +0x128 confirms). Window state 8
  writes the answer: row 1 -> window+0x9108 = 0, row 2 -> 1; Act state 7 copies it to Act+0x9314 at **0x42e9bf..0x42e9c7**.
- Rows (SystemMsg_US.fms, set at 0x51d281..0x51d2b1): 0x53e "If you wish for a further challenge, you may continue your
  adventure. You may also return to town if you are exhausted.", **0x53f "Continue adventure" = answer 0**, **0x540
  "Return to town" = answer 1**. Arena kind 0xf: 0x550 "Continue battle" / 0x551 "Return to the gate".
- **Online countdown on every player, host included**: window state 7 creates a timer (`4b0c10`, `4c2010`, 16 s =
  60 * DAT_0075a4c0 16.0) when platform sys+0x16908 bit 0 is set; expiry with no cursor move = row 2 = "Return to
  town". Window state 6 also auto-advances after 5 s online. Offline there is no timer (run r1: two solo instances
  sat in m5.7 for 4,000 frames).
- **sys+0x16908 bit 0 is "online party" (set from DcNetIf +0xf0 in `a06c0`), not "I am a guest"** (correction to 3.1):
  host 0x1, guest 0x41, alone 0x0 (traces). The game's own host test is `9ffa0`: sys+0x168f0 == sys+0x168f1.
- No command transports the answer: senders of the scene commands are 0x7dc (SetupScene / GameServer), 0x7dd
  (`43f3c0`), **0x7de (`43d190` <- `288f0` UPDATE_CONNECTEDINFO: the barrier mask Act+0x9311 used by `69b70`, not a
  decision)**, 0x7df / 0x7e0 / 0x7e1, 0x7e3 (`43cd50` only: round-end transition, kind), 0x7e6 (`42ddc0` world map
  only), 0x7e7 (GameServer 0x62c <- `33ee0` from state 0xb: next round index), 0x7e8 (GameServer 0x62d <-
  `43f5b0` error 0x11: all to mode 6). The window class sends nothing.
- Answer 1 in state 7: host-bit-off **0x430e4a** + `9d9e0` **0x430e54**, state 8 .. 10 -> state 0xf (second leave
  0x42f264 / 0x42f26d) -> 0x11 ("round/Town00.brb" marker "A_Result", 0x754282) .. 0x19 (0x42f968 leave, "Enter",
  `SetMode(2)`): a solo return to the player's own town. Answer 0: state 8 -> 9 (`439db0`: barrier `69b70` over the
  connected peers) -> 10 -> 0xb world map (or 0xd directly when Act+0x114 is 0xb..0xd).

### 12.2 The game's party-wide return to town is the world map's "Return to town" (asm)
World map window (`547b90`): "@#(35) Return to town" (0x2ac) -> YN "Return to town?" / "(Your stable fee will not be
refunded.)" (0x2fa / 0x2fb) -> yes writes map+0x2d18 = -2 (0x548081). Act state 0xb, host branch (`26220` true), map
state 3: `54b9d0` (+0x2d18 == -2) at **0x431171** -> sends 0x7e6 with +0x39 = 1 (0x43117e..0x4311f9, delivered to all
incl. self). Handler 0x435d12: map+0x2d09 = 1 (map update 0x547c25: word +0x2d08 >= 0x100 -> map state 9 -> `54ba10`)
and Act+0x110 = 1. Then state 0xb sub 2 -> 0xd: the host requests "round/Town00.brb" entry 9 (kind 7) or 1 (`30c70` ->
GameServer SETUPSCENE_REQUEST; guests' own requests are ignored, `SupSn ign`), 0x7dc to all, state 0xe -> `SetMode(4)`,
SetupScene with the SYNC barriers on every member. Guests reach 0xd from 0xb either way (0xc falls through to 0xd
for a party member, observed). No `9d9e0` anywhere on this path.
So in the original code: **"Continue adventure" + world map "Return to town" = party goes home together; result
window "Return to town" (or the 16 s timeout) = that player leaves.** Nothing in the asm makes only the host's result
answer count (the user's memory of PS4 does not match the code; unverified on hardware).

### 12.3 Root cause of the user's symptom
Not an HLE / Matching2 / Rudp gap: each player's own result window (or its 16 s countdown) chose "Return to town" and
that player dissolved at 0x430e54 at its own moment; the Rudp FIN reason 1 and owner migration are the normal
LeaveRoom (4.3). The real logs (guest left, host ~8 s later, last guest became owner and went home alone) and report
21 (three leaves within 4 s, the fourth player continued alone and searched for a room) are this per-player split.

### 12.4 Runtime (scratch build; diagnostic hook `DCP_DBG_ROUNDEND=<vblank>:<kind>` on `428ef0` calls `43cd50(act,
kind, act+0x104+1, 0, act+0x128)` once on the host in a dungeon, i.e. what GameServer ROUNDEND 0x2c557 does)
Setup = Work/npcjoin r1 (friends mode, host `in_stage.txt` cut at 6400, joiner `in_town_to_portal.txt`), round end
kind 0 at v6500, no input afterwards on either side.
- r2 (original code): both m5.3 -> m5.6 -> **m5.7 at v6508** (result window reached in 8 frames). At **v8540 both**
  answer 1 (the countdown), host and guest each log `HostBitOff ret eboot+0x430e50` + `Dissolve ret eboot+0x430e59 |
  act m5.7.0`, then `ret 0x42f26a` / `0x42f272 | act m5.15.0`; guest: `RUDP ... closed (reason 1)`, `owner 0x10 ->
  0x20`, `member 0x10 left`; both continue m5.17 .. m5.22 alone into their own Town00 = the user's symptom.
- r3 (patch 12.5 on): v8540 answer 1 rewritten to 0 on both (host Act+0x110 = 1), m5.8 -> 9 (barrier) -> 10 -> 0xb
  (v8588-9132, host auto-picks town) -> 0xd -> 0xe, **both** `m4.*` SetupScene round 9 stage 1 in lock-step (v9145-9280),
  `HostBitOn ret eboot+0x43bd70` on arrival, both Act mode 2 in Town00 from v9310; **no Dissolve / HostBitOff from mode
  5**, Rudp counters still rising at v12000 (1028 / 3076 msgs). (The trace's RX/TX line cap was hit before the
  round end, so the 0x7e6 / 0x7dc lines themselves are not in the log.)

### 12.5 Fix (two relays in the `skip_offers.cpp` style; `DCP_PARTY_RETURN=0` = original)
- **P at 0x42e9bf** (14 bytes `41 8b 84 24 08 91 00 00 89 83 14 93 00 00` -> `jmp P` + 9-byte nop, back to 0x42e9cd):
  answer = window+0x9108; only if answer == 1, kind (Act+0xe8) in {0, 1, 2, 0xc} (shows the choice and uses state 10's
  default branch; arena 0xf, game over 3, auto-leave kinds 4 / 8..0xb untouched) and sys+0x16908 bit 0 (online
  party): guest -> 0 (follows the host); host (`[sys+0x168f0] == [sys+0x168f1]`) -> 0 and byte Act+0x110 = 1.
- **T at 0x431171** (`e8 5a a8 11 00` call 54b9d0 -> call T): if byte Act+0x110 == 1, map+0x2d18 = -2 (what the map's
  YN yes does), then tail-jump to `54b9d0`. T reads Act from the caller's stack slot [rsp+0x18] (= [rsp+0x20] inside T),
  not rbx: rbx held Act in run r3, but 42ddc0 reuses ebx in nearby blocks (0x430ebc, 0x43103f) and the dispatch into
  0x430c01 was not traced, so the slot is the safe source (inferred). Act+0x110 is cleared in state 2 of every round end and otherwise set only by
  the town 0x7e6, so the original flow is unchanged when the host did not pick "Return to town".
- When Act+0x114 is 0xb..0xd (no world map), every member goes to state 0xd directly and the host's Act+0x110 makes
  it request Town00 (asm; not run).
- Behaviour differences (unverified in play): the host sees the world map for ~0.5-9 s before town; the solo path's
  states 0xf..0x19 (stat tally `272f0(..4..10)`, "A_Result" scene, Act+0x9318 item marks, save+0x1c reset, Act+0xa4 /
  +0xa8..0xb4 reset) are replaced by what the original world-map town pick does; in the labyrinth (round 0x11) answer
  0 also advances the floor counters (same as the original Continue + world-map town). A guest can no longer leave a
  party from the result window.
