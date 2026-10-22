# Vita 1.09 join handshake (guest connect check)
Tags (asm) / (inferred) / (runtime). Base: vita_net_109.md, vita_cmds_109.md, town_session.md 11.1.
Status 2026-10-22: B stalls before sys mode 3 (no 0x3e9; exactly one 0x3ec in 139 s / 81 s) (runtime); root cause
narrowed, not proven - deciding probe run in "Status".

## Functions
| 1.09 | Pro | role |
|---|---|---|
| 0x81391de0 | a1710 | NetCtrl system-command handler, ids 0x3e8..0x3f8 (tbh at 0x81391e2a) |
| 0x81392d7a | 9e980 | command receive filter; non-owner, session not established: only 0x3e9..0x3ed accepted |
| 0x81394ace | - | per-frame receive (sys vt+0x20); counters +0xb5c0 |
| 0x81394d20 | 9e620-ish | sys update (vt+0x38); enters mode 2 when owner link valid, session not established, not owner, mode 0 |
| 0x81394caa | - | sys task dispatcher (vt+0x28) |
| 0x81393910 | 9ffd0 | PeerStateQuery: send 0x3ec, wait flag 0x40, timeout (60/n)*60 |
| 0x81393a64 | 9f1e0 | sys mode 2 (guest): errors 1..4 |
| 0x81393d34 | 9f550 | sys mode 3: 0x3e9.., errors 0xc/0xd.. |
| 0x81393884 | a0200 | SetMode(3) if owner link valid; callers 0x812b4430, 0x812cb47e, 0x813393e8 |
| 0x81395ae6 | - | predicate: owner link valid && mode 2 && step 1 |
| 0x8133916a / 0x813393e8 | 514380-ish | join-menu wait / enter-AC states (9 menu callers) |
| 0x81393472 | a06c0 | slot-table recompute |
| 0x81395b30 | - | error code -> text id f81041e26 |
| 0x8102ff12 / 0x8102ffea / 0x8102fe48 / 0x8102fc88 | - | 0x3ed body write / read / acceptance check / owner state |
| 0x8138e7d6 | 9c060-ish | DcNetIf vt+0x88 "owner link valid" |

DcNetIf vtable fn array 0x81770da8 (thunks to DcNet+0x54): +0x60 IsOwner, +0x68 my id, +0x6c session established
(DcNet+0x17c bit 2), +0x74 peer connected, +0x98 owner slot, +0x9c my slot, +0x108 request done, +0x10c request result.
Sys (= [[0x817eeaa4]+0x58]): +0x3c mode, +0x3d step, +0xb4a8 flags, +0xb4b0 iface, +0xb4b4 player table (4 x 0x1c),
+0xb490 my index, +0xb491 leader index, +0xb492/+0xb494 owner index/peer from 0x3ed, +0xb534 check result, +0xb58c error code.
DcNetMatching = [[0x8177e01c]+0x7c]; modes 0..0x15 dispatch table 0x813a0f84 (3 create, 4 search, 5 join-from-search:
steps online check, StartFlow, ServerFlow, f8139ef68, JoinRoom, WaitPeersConnected 0x8139f200 (14 s, error -5), StopFlow, done).

## Commands (asm)
Type-0 stream record = u32 record length + 0x14-byte header (cmd+4..cmd+0x17; ctor 0x8139173a, write 0x813917ae, read
0x81391840) + payload. Header: +0 u32 0, +4 u32 -1, +8 u16 id, +0xa u16 (0 = system), +0xc dest mask, +0xd 0, +0xe round
(Act+0xcc), +0xf stage (Act+0xb4), +0x10 sender's player-table peer value, +0x11 sender +0xb491 (or 0xff), +0x12 0xff,
+0x13 sender +0xb490.

**0x3ec guest -> owner, "connect query"** (vptr 0x8177085c+8, serializer 0x814f61a0 / reader 0x814f61b0): header round 9,
stage 9, mask 0xff; payload u32 = guest's own slot (vt[0x9c]; runtime 1). Owner handler 0x813924e4 answers 0x3ed to that slot.

**0x3ed owner -> guest, "connect info"** (vptr 0x8177089c+8; serialize 0x81396110 -> body 0x8102ff12; deserialize
0x813961a8 -> body 0x8102ffea -> check 0x8102fe48): header round 7, stage 0. u32 0x81 body length; u32 free slots (3);
s64 owner state (0x8102fc88: 0 = accepting when Act mode 2, 1 busy, 2 other); 108-byte room info (name "Edwin",
8 x `00 00 0a 0a 00 00`, pairs +0x40.. 3/7, 0/0, -1/0, -1/-1, 2/1, 2/0, 0x22/0, +0x5c 0, flags +0x5e 0x4620, +0x60
`05 ff ff ff`, +0x64 5, +0x65 0x32, world 1, +0x66 0, room 0x1004); u8 owner +0xb491; s64 owner peer id.
Guest handler 0x8139257e: +0xb4a8 |= 0x40, +0xb534 = check result, +0xb492 = 0, +0xb494 = 0 (target of mode 3's 0x3e9).
Check 0x8102fe48 (inferred to pass): free slots vs guest's human count, difficulty-like field vs room info +0x54 (2),
two option bits (0), version 0x22.

## State machine after 0x3ed (asm)
Guest: 0x81394d20 enters mode 2 (owner link valid, DcNet+0x17c bit 2 clear, not owner, mode 0).
- Step 0: 0x81393910 sends 0x3ec once, waits flag 0x40. Link drop or (60/n)*60 ticks -> code 1; also code 1 if check
  result 1, +0xb492 = -1 or +0xb494 = 0xff.
- Step 1: no timeout; link invalid -> code 2; sets mode 3 only on flag 0x80, which nothing sets.
- Join menu 0x8133916a, each frame: request not done (vt[0x108]) -> wait; failed -> 2/3; +0xb58c != 0 -> 0x81395b30
  message, return 4; predicate true -> 0x813393e8 calls 0x81393884 -> mode 3; else after 5 s a "waiting" dialog, after
  (60/n)*60 more frames cleanup 0x81393820, message 0x19, return 5.
- Mode 3: 0x3e9 to +0xb494, wait 0x3ea, ... (as Pro: 0x3e9->0x3ea, 0x3ec->0x3ed, 0x3e8->0x3f0/0x3ef/0x3ee, then Act AC).
Owner: nothing more until 0x3e9 arrives; type 2/3/4 not expected yet; both sides send type-1 alives once a second.

## Ruled out
Command layouts / serializers, CRC, receive budget (acks 44 / 173 / 16 consistent), dest mask, receive filter, round /
stage tag (no check on the system path), join error dialog (no sceMsgDialogInit), search attrs, room-info flag 0x4000
(not read here), SceNpId (not used by 0x3ec / 0x3ed).

## Candidates
- **gamma (most likely, inferred):** right after 0x3ed, vt[0x88] "owner link valid" (0x8138e7d6) reads false -> code 2 in
  step 1 (0x81393c66) or code 1 in step 0 -> +0xb58c -> menu shows the message ("unknown error"?), no re-entry (one 0x3ec).
  vt[0x88] inputs fed by HLE: owner / my member id from Matching2 member data (owner memberId +0x3c8, mine +0x32c),
  in-room bit +0x1e4 bit 4, RudpMgr slot 0 state == 2, DcNet peer flags +0x79c bits 0x10/0x20, no leave-type request
  (modes 1/2/7/0xf/0x10/0x12) running or queued.
- alpha: 0x3ed never applied on the guest (no dropping gate found; would need the menu's 65 s to beat the task's 60 s).
- beta: menu never calls 0x81393884 although the predicate is true (contradicts all three callers' asm).

## Deciding run (guest only)
`--probe 0x81393bb8` step-0 failure, `0x81393c66` step-1 link invalid (gamma), `0x81339254` menu saw error code,
`0x81339276` menu saw predicate true, `0x813392fc` menu timeout, `0x813938a6` mode 3 requested. Note which fires and how
long after `RUDP ... first message in (173 bytes)`. None in 3 min -> `--watch 0x8177e01c:0x7c:0x3c` (DcNetMatching
mode/step). If gamma: `--watch 0x817eeaa4:0x58:0x3c` (sys mode/step). One --watch chain per run.

## Error surfaces (1.09)
Runtime 2026-10-22: none of the six "Deciding run" probes fired while B showed "Network Error: unknown Error"; the Rudp
link stayed up (only type-1 alives) until the user dismissed it, and no common dialog was used.
- DcNet error +0x18c (DcNet = [[0x8177e01c]+0x70]): set by 0x8138e7ca from DcNetMatching steps only; cleared at each
  request start (0x813a0f30); read via iface vt+0x110 by the join flows 0x812b4430 (0x812b450e) and 0x812cb47e
  (0x812cc8a0) after matching idle (vt+0x104). The menu 0x8133916a uses the request result vt+0x10c: -100 -> ret 3,
  else 0x8138f42a -> text 0x81041dc0(code): -2..-8 -> 0x38..0x3e, anything else -> 0x37 (generic / "unknown", inferred).
- Sys error +0xb58c: connect-check codes 1..0xd; SetNetError 0x813938b4 codes 0x10..0x15 -> NetErrHandled 0x812d10da
  -> Act mode 9 / 7; text = code + 0x20 (0x81041e26).
- Mode-5 post-connect error: WaitPeersConnected 0x8139f200 -> -5 after (60/n)*14 ticks (site 0x8139f2c2); needs every
  in-use peer record (+0x414 + k*0x9c) connected per 0x8139f0ac (slot is mine, or RudpMgr slot state == 2 and no
  request of type 0x12/0x10/1/7/2/0xf running or queued).
- Flow failures without a code: 0x812cc852 / 0x812b44b2 (idle, zero search results), 0x812cc988 / 0x812b463a (sys flags
  0x8/0x10 after SetMode(3)), owner link lost 0x812b458c / 0x812b4668, timeouts 0x812b45e4 / 0x812b46c0 / 0x812cc9d2.
- Join step 7 (0x8139f3fe) error dialog: out (no sceMsgDialogInit).
- iface slots: +0xac queue join (mode 5), +0xb0 queue join by index, +0x104 matching idle, +0x108 / +0x10c request
  done / result, +0x110 GetError, +0x130 search-result count.
Next run (guest): probes 0x8138e7ca 0x813938b4 0x8138f42a 0x81339248 0x812b4518 0x812b452e 0x812b44b2 0x812cc8a8
0x812cc8bc 0x812cc852 0x812cc988 0x812b463a 0x812b458c 0x812b4668 0x812b45e4 0x812b46c0 0x812cc9d2 (r1 / lr at
0x8138e7ca or 0x813938b4; r0 at 0x8138f42a; lr 0x8139f2c6 = the -5 peers timeout); sanity probe 0x81392580 (0x3ed
guest handler); watches 0x8177e01c:0x70:0x18c (DcNet error), 0x8177e01c:0x7c:0x3c (matching mode/step/substate;
0x00010505 = waiting for peers), 0x817eeaa4:0x58:0xb58c (sys error).

## Root cause of "Network Error: unknown Error" on join (1.09, asm + runtime 2026-10-22)
- netobj (= [[0x8177e01c]+0x74]) request type 5 = a PSN userProfile list: queued by 0x813a617e; handler 0x813a3c80
  (netctl+0x7c must be 1 = PSN online mode, else result -1) -> 0x81076e38 -> web job on netobj+0x14d8 (status -1 busy,
  <0 error) run by the Toolkit::NP executor 0x813ae342 via sceNpWebApiCreateRequest(apiGroup, path, method, content,
  int64* id) / SendRequest2(id, data, size, info) / ReadData loop / JSON parse. Failure -> 0x813a349c(netobj, -1).
  **Runtime (2026-10-22): the request is the BLOCK list**, `GET userProfile /v1/users/<id>/blockList?&limit=100&offset=0`
  (path built at 0x813b4380, string 0x815ca920; parser 0x813b4406: a non-object value clears the list, sets +0x6e0
  done, returns 0; needs result +0x3c non-null). It runs at online start, again on entering a dungeon, and twice
  before JoinRoom. The friendList GET (paths 0x815ca688.., parser 0x813b3b12) was not seen in the run. The earlier
  "friend list" reading was asm-only and wrong.
- Issued by DcNetMatching mode 3 (online start, 0x813a06bc) and mode 5 step 3 (join, 0x8139ef68 -> latch 0x8139f026 /
  DcNet+0x18c 0x8139f032; the step continues). Latches are cleared only when the next request starts (0x813a0c30,
  0x813a0f30), so the join's own -1 survives until the caller reads it after Rudp + 0x3ec/0x3ed: menu 0x8133916a ->
  0x8138f42a -> 0x81041dc0(-1) = text 0x37; join flows 0x812b4430 / 0x812cb47e -> GetError -> fail. B never reaches
  SetMode(3), so no 0x3e9. The host never runs mode 5, so it is unaffected.
- HLE answer (np.cpp WebApiCreateRequest / SendRequest2 / ReadData): blockList and friendList GETs succeed, HTTP 200,
  empty body (stub Json -> parser's non-object path, empty list); every other WebApi call stays offline (presence
  gameData PUT, method 2, is harmless). np.cpp logs every CreateRequest path with "-> empty list" / "-> offline".
  **Verified 2026-10-22 (Windows, two instances, local dcp_lobby):** the guest joins the host's dungeon room with no
  "unknown Error" (JoinRoom -> member 0x20, signaling Established both ways, Rudp up); user-confirmed in game.
  Later: feed the lobby's real block / friend lists here.
