# Networking / online multiplayer: Np*, Matching2, Rudp, NpToolkit2, game netcode

Mapper notes. Addresses are eboot-relative (as in q.py). Tags: **(asm)** = read in capstone disassembly, **(inferred)** = deduced from
usage/structure, **(unverified)** = guess. Raw per-function disassembly lives in `notes/raw/net/` (ignored by git). Method: anchors
`DcNet*` class-name strings, import call sites (`notes/raw/engine/imports_usage2.tsv`), then walk callers/callees; no Ghidra run.
Baseline context: `notes/map/engine.md` section 5 (Np/Rudp rows) and PLAN rule 3 (RPCS3/RPCN model).

## 1. Summary
Dragon's Crown Pro online play is **PSN room-based matchmaking (NpMatching2) + P2P signaling -> Rudp over a P2P UDP socket, with the
game's own message layer on top** (class `DcNet`, 4 players max). The engine owns everything above libSceRudp itself; there is no
libSceNet use outside the Rudp socket. Layers (all C++ "Fn" task objects owned by the game singleton `DAT_00c17758`, getter `FUN_00016b10`):

| game singleton field | class (name string) | ctor | role |
|---|---|---|---|
| +0xb0 | `DcNet` (0x99500) | FUN_00099500 | the game message layer: per-peer packet framing, CRC, heartbeat, data-stream buffers, NetCtrl commands |
| +0xb8 | `DcNetSignin` tag 'SIGN' (0xbe1f0) | FUN_000be1f0 | NpToolkit2 session: friend list, presence, invitations, Plus check, sign-in state machine |
| +0xc0 | `DcNetInfoReal` tag 'NINF' (0xb4710) | FUN_000b4710 | trivial info holder |
| +0xc8 | `DcNetMatching` (0xb51e0) | FUN_000b51e0 | **NpMatching2 manager**: context, world, room create/search/join/leave, room attrs, event + signaling callbacks, embeds the **Rudp manager** at +0x440 (ctor FUN_000bd100) |
| (other) | `DcNetTask` (0xc2a90), `DcNetGameServer` (0x276f0), `DcNetSceneMan` (0x427ac0), `DcNetCtrl` (0x9dbd0) | | game glue: scene sync, NetCtrl command objects (`DC_NETCTRLCMD_*`) |

All four construct in FUN_00016da0 (the DcCore-setup function) at 0x17437 (+0xb8 signin... order: be1f0, 99500, b51e0, b4710).

**Rating: careful** (not "needs high effort"). Reasons: the Np surface the game really uses is small and polled/status-code driven
(Matching2: 20 imports; NpManager: 25; Rudp: 17) and every piece of state is observable; the game never implements its own NAT
traversal. Risks: (a) the 55 private NpToolkit2 C++ NIDs (friends/presence/invite) have no public signature - they can be stubbed to
"signed in, no friends" but the DcNetSignin state machine needs testing against the stub; (b) the Rudp semantics (message-oriented,
reliable, ordered, 256 KB, P2P mux) must be reproduced by an ENet/UDP shim; (c) memberId encoding quirk (section 5.3); (d) whether the
gameplay sync is full state replication needs a runtime trace (section 5.5).

**Transport facts (asm):** UDP socket opened with `sceNetSocket(name "RUDP_NetId", AF_INET=2, type 6 = SOCK_DGRAM_P2P, proto 0)`; bound to
**real port 3658 (0xe4a), vport 4649 (0x1229)**; Rudp contexts bound to **Rudp port 20 (0x14), mux mode 1 (P2P)**; peer address and
port come from `sceNpMatching2SignalingGetConnectionStatus` (peer IPv4 + UDP port), connection started with `sceRudpInitiate` to
{peerIP, peer port, vport 4649}. Max **4 peer slots** (Rudp slots 0..3, DcNet per-peer records 0..3).

## 2. Import inventory with game wrappers (confirmed from call sites)
Each row: import -> calling function(s) -> role. Library init chain: FUN_00599d00 (Net/Ssl/Http, from platform init) and `NpHelper`
vtable-ish methods FUN_0059b160 (init step n of 6) / FUN_0059b360 (term step n) / FUN_0059b4c0 (combined init) / FUN_0059b650
(combined term). The `NpHelper` object (class vtable at 0xbc0638 ctor FUN_0059ba10, size 0x108) lives at `platform+0x124a8`
(getter pattern `FUN_00568510()->+0x124a8`).

### 2.1 libSceNet / NetCtl / Ssl / Http
| import | caller (wrapper) | role |
|---|---|---|
| sceNetInit, sceNetPoolCreate("FnPS4_Device", 0x20000) | FUN_00599d00 (step 0 of FUN_0059b160) | init, pool id -> platform+0x123d4 |
| sceNetTerm, sceNetPoolDestroy | FUN_0059b360 | term |
| sceSslInit(0x4c000), sceHttpInit(netpool, sslctx, 0xc000), sceSslTerm, sceHttpTerm | FUN_00599d00, 59b160 steps 2/3, 59b360 | init only, never used for requests (inferred: Toolkit2 needs them) |
| sceNetSocket, sceNetSetsockopt x3 (SOL 0xffff: opt 0x20000, 0x40000, 0x1200 = nonblock; value 1), sceNetHtons, sceNetBind, sceNetSocketClose | **FUN_000bd4c0** `RudpMgr_OpenSocket` | the P2P UDP socket, see section 1 (asm) |
| sceNetSocketClose | FUN_000bd260/bd480/bd730 | close on rudp event-2 / shutdown |
| sceNetCtlGetState | FUN_0059b9c0 (callback), 59ba10, 59bb40, 59bfd0 | stored in NpHelper+0x74; **value 3 (IPOBTAINED) = "online"** (asm: `FUN_0059bc90` returns +0x74 == 3) |
| sceNetCtlGetInfo(code 2) | FUN_0059bb40, 59ba10, 59bfd0 | 6-byte result stored at NpHelper+0x78 (code 2 = ether addr, inferred); tolerates error 0x80412108 |
| sceNetCtlRegisterCallback(FUN_0059b9c0) / Unregister / CheckCallback | FUN_0059be60 / 59bef0 / 59bb40 | callback just sets a "changed" bit in NpHelper+0x104 |

### 2.2 libSceNpManager (25 imports, 54 sites)
| import | caller | role |
|---|---|---|
| sceNpSetNpTitleId, sceNpSetContentRestriction | FUN_00598290 (platform init) | startup (title id blob from main, see engine.md) |
| sceNpCheckCallback | FUN_0059a1e0 (render-thread frame tail) | per-frame callback pump (must dispatch our queued callbacks) |
| sceNpRegisterStateCallbackA / UnregisterStateCallbackA | FUN_0059be60 / 59bef0 | state callback; plus NetCtl callback registered together |
| sceNpCreateAsyncRequest, sceNpPollAsync, sceNpAbortRequest, sceNpDeleteRequest | 59bca0, 59bd20, 59bdb0 (create) / **59bb40 (poll+cleanup)** | async request pattern: result stored NpHelper+0x88 (result code), +0x84 request id (-1 when none) |
| sceNpCheckNpAvailabilityA | FUN_0059bca0 | async check, result in +0x88 |
| sceNpGetParentalControlInfoA | FUN_0059bd20 | async; called from FUN_000c7050 (DcNetSignin) |
| **sceNpCheckPlus** | FUN_0059bdb0 | async; request {size 0x38, userId, **features = 1**}; result **byte at NpHelper+0x90** (1 = has PS Plus). Used by sign-in state machine FUN_000c71f0 (asm) |
| sceNpRegisterPlusEventCallback / Unregister | FUN_000c71f0 / FUN_000c7990 | plus-membership change events (callback FUN_000c6840 = ret) |
| sceNpNotifyPlusFeature | FUN_00428ef0 (<- 428690, a scene) | tells the OS the Plus feature is used; stub |
| sceNpGetAccountIdA | be7c0, c0520, c1c70, c1fa0, c22f0, c75e0 | local account id (u64) |
| sceNpGetNpId | c75e0, c8620 | local NpId |
| sceNpGetAccountCountryA | c75e0 | country code (drives region/word-filter behaviour, unverified) |
| sceNpGetState | c75e0 (DcNetSignin state machine) | signed-in check |
| sceNpInGameMessageInitialize(0x4000) / CreateHandle / DeleteHandle / Terminate | 59b160 step 4 / 59b360 | in-game message channel (invites/join requests) |
| sceNpInGameMessagePrepareA | FUN_0059b7c0 (<- c71f0) | registers receive callback **FUN_000c6760** |
| sceNpInGameMessageSendDataA | FUN_000c0520 (2274 B), FUN_000c1c70 | send invite / room-info message to other players; payload built with the same room-info serializer (98da0/99110) |

### 2.3 libSceNpMatching2 (20 imports, 27 sites) - all inside `DcNetMatching` (0xb4000-0xbc800)
| import | caller | role |
|---|---|---|
| sceNpMatching2Initialize({poolSize 0x60000,...,0x30}) | FUN_0059b160 step 1 / 59b4c0 | |
| sceNpMatching2Terminate | 59b360 / 59b650 | |
| sceNpMatching2CreateContextA(userId-based params, out ctxId u16 -> matching+0x98) | **FUN_000bb2e0** | step 1 of the init machine; then RegisterContextCallback(FUN_000b5540, this) |
| sceNpMatching2RegisterContextCallback | FUN_000bb2e0 | context events (start/stop) |
| sceNpMatching2ContextStart / ContextStop / DestroyContext | **bb3e0** / bb940 / b5350 (dtor), b8280 (shutdown) | |
| sceNpMatching2GetServerId | **FUN_000b87c0** | step 2, id -> matching+0x9a |
| sceNpMatching2GetWorldInfoList | **FUN_000bb4f0** | step 3: picks a world (compares world list entries against matching+0xd0) |
| sceNpMatching2SetUserInfo (user bin attr id **0x5f**, one blob built from FUN_00098f00) | **FUN_000bb770** | step 4 |
| sceNpMatching2CreateJoinRoomA | **FUN_000b8a10** (1213 B) | host creates room, section 3 |
| sceNpMatching2SearchRoom | **FUN_000b8fd0** (1050 B) | section 3 |
| sceNpMatching2JoinRoomA | **FUN_000b9890** (1003 B) | join by roomId |
| sceNpMatching2LeaveRoom | **FUN_000b7fb0** | |
| sceNpMatching2SetRoomDataInternal (bin attr **0x57**, len = serialized info, <= 0xff) | **FUN_000bbda0** | host re-publishes room info |
| sceNpMatching2SetRoomDataExternal | **FUN_000bc020** | host re-publishes searchable attrs (same attr builder FUN_000bba60) |
| sceNpMatching2RegisterRoomEventCallback(FUN_000b5ec0 thunk -> FUN_000b5ee0) | b8a10, b9890, b7fb0 | room events |
| sceNpMatching2RegisterSignalingCallback(FUN_000b6330 thunk -> FUN_000b6350) | b8a10, b9890 | P2P signaling events |
| sceNpMatching2SignalingGetLocalNetInfo(size 0x18) | b8a10, b9890 | stores mapped addr -> matching+0x534, local addr +0x538, natStatus +0x53c; **no reader found in 0xb4000-0xcb000 or 0x99000-0xa5000 -> NAT type does not gate anything there** (asm scan; global scan not done) |
| sceNpMatching2SignalingGetConnectionStatus | **FUN_000b6350** | on Established event: gets peer IP + port |

### 2.4 libSceRudp (17 imports, 56 sites) - all in `RudpMgr` (embedded in DcNetMatching at +0x440; ctor FUN_000bd100)
| import | wrapper | role |
|---|---|---|
| sceRudpInit(pool 0xc22250, 0x100000), sceRudpGetStatus(buf,0xf8), sceRudpEnableInternalIOThread(stack 0x2000, prio 0x1f4), sceRudpSetEventHandler(FUN_000bd240, mgr), sceRudpPollCreate(0x80) | **FUN_000bd600** `RudpMgr_Init` (called from matching state machine FUN_000b8720) | |
| sceRudpEnd, PollDestroy, SetEventHandler(0,0) | **FUN_000bd730** `RudpMgr_End` | |
| sceRudpCreateContext(&ctx), sceRudpBind(ctx, sock, vport **20**, muxmode **1**), sceRudpSetOption(ctx, 2 (send buf), 0x40000), (3 (recv buf), 0x40000), sceRudpInitiate(ctx, sockaddr{len 0x10, fam 2, port 0x210?=see note, addr, vport 4649}, 0x10, 0), sceRudpPollControl(poll, add=1, ctx, events **0xb**) | **FUN_000bd8b0** `RudpMgr_Connect(slot, ipPtr, port)` (<- FUN_000b6350 signaling Established) | note: sockaddr first word stored 0x0210 = len 0x10 + family 2; the 16-bit port field gets the **peer port from signaling** |
| sceRudpPollWait(poll, evbuf, 0x80, 0), Read, Write, GetOption(ctx, 0xe), PollControl(remove=3), Terminate | **FUN_000bdae0** `RudpMgr_Pump` (<- FUN_000b64e0 which wraps it in the mgr mutex; called every game tick from DcNetMatching) | the I/O loop, section 4 |
| sceRudpTerminate, PollControl(3) | FUN_000bd260 (event handler body), bd400 (`CloseSlot(slot)`), bd730 | |
| ctx close notifies `DcNet` via FUN_0009ad10(slot) | | |

### 2.5 Other Np libraries (not needed for MVP)
| library | callers | role / shim |
|---|---|---|
| libSceNpTus (9 imports) | FUN_000be7c0 (SetDataAAsync+PollAsync), c1fa0 (SetDataA), c22f0 (GetDataForCrossSave), bed30/bf2e0 (AbortRequest in worker threads) | title user storage: cross-save / data upload. **stub: return error / "no data"** (no online dependency in gameplay) |
| libSceNpUtility WordFilter (6) | FUN_000c8620 | profanity check for room/player names. stub: return text unchanged, status complete |
| libSceNpCommerce dialog (5), SigninDialog (4) | c71f0 (Plus upsell dialog), 59bf60/59bfd0 (sign-in dialog) | stub: dialog status INITIALIZED -> FINISHED. Not reached if sceNpCheckPlus says "has Plus" and state is signed in |
| libSceNpTrophy (6) | FUN_0059f020..0059f0f0 | see engine.md |

### 2.6 libSceNpToolkit2 - the 55 unresolved private NIDs (inferred roles)
All are the C++ (mangled) API of the Sony NpToolkit2 library; names are unknown. Roles deduced from call patterns. Three are never called
(`7e6mE4oIQnU`, `5H3GkWWQY5A`, `cbJSAoeH_mo`). **Clusters** (type names mine, **inferred**):

| NID(s) (prefix `nid_`) | seen in | proposed role |
|---|---|---|
| `vbh_MWtjMh4` (ctor), `LLdnqVnnNBQ`, `_bAjEe_hdCg` (dtor) | 59b160/59b4c0 (init, with params{callbacks FUN_0059b740 / 59b780 = alloc/free under mutex 0xe97300 into pool 0xe938c0, event cb FUN_00597db0, pool size 0x4000, flags}) | `InitParams` ctor, `Toolkit::Initialize(params)`, dtor |
| `s0u30pOfi9Q`, `IcuECtP3L0E`, `vycUuYWwilI` | 59b360/59b650 | `Terminate`-sequence calls |
| `Ht77qXUqN7U` (ctor), `2w8SdVsst74`, `lWJNqXQ6_mI` (dtor) | 59b880 (register, arg FUN_000c6850 trampoline, this) | `RegisterEventCallback(params{cb,arg})` |
| `AwWC0Y_Shbg`, `XfdgmVGydYU`, `c9kMGltLpaQ` | 59b8f0 | `UnregisterEventCallback` |
| `sZ8FuTrbhUc` (ctor), `K0gxiS_SBho(type, ev*)`, `7IHg_sW3Ez8(type, ev*)`, `BNYc2zKvxyM` (dtor) | 597dc0 (Toolkit event callback), c69f0 | `Event` object + "decode/handle event of type N" helpers |
| `tlpXm5KLgqk` (ctor), `30Zxe35LtBg` (dtor) | NpHelper ctor/dtor (3x), DcNetSignin ctor (4x) | `Future<T>` default constructor / destructor |
| `5_UHpg8s00I` (returns int, 25-12 sites), `kxzokJHZ2nY`, `gEvnF0rDgnA` | c6860, c69f0, 59c090 | `Future::getReturnCode()` (0 = ok; 1 / 0xa treated as known soft errors in 59c090), `Future::release/reset`, `Future::destroy` |
| `mIinAn8TvdY` (bool) | c7a50, c81c0, c84b0 | `Future::isBusy()` / `hasFinished()` |
| `4mdynkqoCFc` (25 sites in c7a50), `aA83TCJI9Kw`, `bxgy_tUd8YA`, `AUW6at2PxHs` | c7a50, c81c0, c69f0, c6860 | typed result accessors (`Future<T>::get()`): vector-like results with element/count at +8/+0x10 |
| `YeQtFtidPNw`, `oyl98yuMwTs` | c6860 | per-element ctor/dtor (0xaa8-byte element: a friend/profile record) |
| `Y4_NqOIedOk`, `UOVPAHM8ZHw`, `SLK65JLYrUE`, `RPVTcq8iqTg` (ctors) / `WCTsqoUEsTc`, `mQf_iu0AG4M`, `VVWPUyolcGQ`, `c6uPCbW4_lQ` (dtors) | DcNetSignin ctor be1f0 / dtor be540 | `Future<T>` instantiations for the result types (friends list, presences, ...) |
| `AleDTt_SyDo` (ctor), `7Te1eeupXxQ(req, future)`, `PRGCDAuvO4U` (dtor) | FUN_000c81c0 | **GetFriendslist** request; result elements copied into the 0x180-byte friend records (vector at DcNetSignin+0xa0, account id at +0x170) |
| `qqkywbrhVtk` (ctor), `7zsee5IHLis(req, future)` (+ `gEoCCr0gCjQ` dtor) | FUN_000c7a50 | **GetPresence(es) of friends** (request field `[+0x38] = 2`); result parsed into friend records, joinable-room info |
| `H8cQuFAzgU4` (ctor), `xCg_igDBo3Q(req, future)`, `HeDIXZsguvA` (dtor) | FUN_000bea20 | **SetPresence**: game data = the serialized room info (DcNet+0xd98, length FUN_0009d550), republished only when its CRC changes (asm) |
| `1bNH47yLJRs` (ctor), `OylO3B2fCE0(req, future)`, `2783_UMEt_o` (dtor) | FUN_000c84b0 | **send invitation** to the selected friends (title id string "CUSA10454" copied into the request, 0x823-byte blob; account-id list at DcNetSignin+0xaa0, count +0xb38) |
| `hzgYdvXB_IU` (ctor), `0fPn7oELg8E(req, future)`, `vQhIcDhwZvg` (dtor) | FUN_000c7990 (sign-out/cleanup) | clear presence / leave (low confidence) |
| `dB9nauAWtgg` | 59c090 | post-processing of a finished future into NpHelper (+0xb8 / +0xd0) |

**Shim consequence (inferred):** `Future::getReturnCode` must return a non-error for the success path; friend/presence/invite requests can
complete instantly with empty results. All Toolkit2 calls are guarded by FUN_0058dc40 (a one-shot task "request" bit), so they only fire
when the UI asks (open the friends/invite screen), not continuously. Initialization (59b160 step 5) must not fail.

## 3. Session flow (what the game asks, in order)
All of DcNetMatching is driven by the task step machine pattern: each method returns **0 = pending, 1 = done, 2 = error** and is polled
each tick via `FUN_0058dca0` (current step) / `FUN_0058dcd0` (advance step) / `FUN_0058dc40(task, which, consume)` (one-shot trigger
bits at task+0x50). State bits for the matching object live in `matching+0x88/0x8c/0x90` using the global bit table at 0xc15b20
(entries are 1<<n, n = 0..23): +0x88 {bit0 Rudp inited, bit1 context created, bit2 context started, bit4 in room (FUN_000b8700
`IsInRoom`)}, +0x8c / +0x90 = request-done / request-issued bits for the create/join/search request. Errors store a code at +0x5c4
(-100 = 0xffffff9c "Matching2 call failed", -6 on join "no such room") and raw Matching2 error at +0x510; the same code is pushed to
`DcNet::SetError` FUN_0009cae0.

### 3.1 Bring-up (asm)
1. platform init: sceNpSetNpTitleId etc. (FUN_00598290); FUN_00599d00 Net/Ssl/Http; NpHelper steps 0..5 (FUN_0059b160): Net, Matching2
   init, Ssl, Http, InGameMessage init+handle, NpToolkit2 init.
2. Sign-in state machine **FUN_000c71f0** (DcNetSignin, 7 states via jump table 0x6e5098): checks NpHelper availability/NetCtl (state 3),
   `sceNpCheckPlus` (FUN_0059bdb0), parental info, opens Plus upsell (NpCommerce dialog) if not Plus; on fail `FUN_000bed20(this, -100)`.
   NpHelper registers NetCtl + Np state callbacks (FUN_0059be60) at FUN_000c74f0. FUN_000c75e0 reads account id / country / NpId / np state.
3. Matching init machine **FUN_000b8720(this, tick)**: step 0 `RudpMgr_Init` (bd600); step 1 `CreateContextA` + callback (bb2e0); step 2
   `ContextStart` (bb3e0) -> FUN_000b87c0 `GetServerId` (step 2) -> `GetWorldInfoList` (bb4f0, step 3) -> `SetUserInfo` (bb770, step 4).
4. Rudp socket is opened at room create/join (`thunk_FUN_000bd4c0` called from b8a10 / b9890 after the request succeeds).

### 3.2 Room create (host) - FUN_000b8a10 (asm)
`sceNpMatching2CreateJoinRoomA(ctx, req, opts)` with: **maxSlot 4** (u16 at req+0x00 = 4, worldId from matching+0xd0), **flagAttr
0x86000000** (symbolic names unverified), 8 searchable int attrs (ids 0x4c..0x53), one bin attr id **0x57** (<= 0xff B) and one bin
attr id **0x55** (<= 0x100 B) both copied from `DcNet+0xe58` (serialized room info), plus an optional list of up to 2000 account ids
copied from the friend vector of DcNetSignin (`+0xa0`, qword at +0x170 per 0x180-byte record) - allowed/blocked-user list
(unverified which). Callback FUN_000b5a90 (response handler, stores `matching+0x80` result, `+0x84` done byte). After success:
`RudpMgr` socket open, Register{Room,Signaling}Callback, `GetLocalNetInfo`, set bit4 in +0x88.

### 3.3 Search - FUN_000b8fd0 (asm)
`sceNpMatching2SearchRoom(ctx, req{startIndex 1, max 0x14 (20), flags 0x64000000, worldId = matching+0xd0, lobbyId 0}, ...)`.
Conditions come from a user-set vector of {int slotId, int op, int value} at `matching+0x5d8` (12-byte entries, max 10 considered):
slotId 0..7 -> int attr id `0x4c + slotId` with operator = op (1..6 mapped via table 0x6e4f74, identity-like); slotId 8/9/10 are
folded into the packed attr 8 (below). The last condition is always attr **0x53 EQUAL packed value** that includes the protocol
version, so rooms of other versions are invisible. Result handler FUN_000b56b0 -> FUN_000b56d0 builds the browsing list.

### 3.4 Packed attr 0x53 (asm, fields named by position only)
bits [1:0] = info u16 @0x50 & 3 (search slot 8), bits [4:2] = info @0x52 (value 99 means "any" -> 5; else (v<<2)&0x1c) (search slot 10),
bits [6:5] = (info @0x54 << 5) & 0x60, bits [13:7] = info @0x58 & 0x7f (**matches the constant 0x22 = 34 returned by FUN_00098f50 in
the search packing -> protocol/game version, inferred**), bits [18:14] = info @0x5a & 0x1f, bits [20:19] = info @0x56 & 3 (search
slot 9). The search side computes bits [6:5] from FUN_00098f00 (3 * a save-data byte, default 3), bits [18:14] from FUN_00098f60
(2 * a save-data byte, default 2). Int attrs 0x4c..0x52 = info u16 fields @0x40,0x42,0x44,0x46,0x48,0x4a,0x4c (sign-extended).
Room info is a **0x78-byte struct** (size checked == 0x78 in the UpdatedRoomDataInternal handler b5ee0): header (0..8), 8 x 6-byte
member entries @0x08..0x40, 15 u16 @0x40..0x5e, u16 @0x5e, 5 bytes @0x60..0x64, byte @0x65, u16 @0x66, 8 B @0x68, 8 B @0x70
(field layout from serializer FUN_00099110 / reader FUN_00098fb0; **semantics of the u16 fields (stage, difficulty, progress...) not
resolved** - they come from FUN_0009c3a0 `BuildInfo`, which reads game state; see open questions).

### 3.5 Join - FUN_000b9890 (asm)
`JoinRoomA(ctx, req{roomId (u64 arg), ..., joinFlag byte arg}, opts)` (same blocked-id list). Post-join check in the response handler
(b9a8f..b9a9a): local value `FUN_0008be40(save+0xaf1e4)` compared with `matching+0x3e8` (= room int attr 0x4c); if local > room value the
join is treated as error and the room is left (meaning: some progression/level gate; unverified). Matching2 errors 0x80550d19,
0x80550d15, 0x80550d0f are specially mapped (full / closed / not found, unverified names); anything else -> generic -100.

### 3.6 Leave - FUN_000b7fb0: closes all 4 Rudp slots (FUN_000bd400 on slots with active flag), re-registers callback, `LeaveRoom`.

### 3.7 Events and host migration
- **Room event callback FUN_000b5ee0** (asm): event ids `0x1103` (-> bit [8] in +0x8c, "kicked out"), `0x1104` (-> bit [4], "room
  destroyed"), `0x1105` (copies the new-owner member record into +0x1aa: **owner changed**), `0x1106` (UpdatedRoomDataInternal: bin
  attr must be 0x78 B; decoded into matching+0x3e8..0x41c ints), plus `0x8101` (**member joined**, calls FUN_000b61e0 to fill the 0x70-byte
  member record at matching+0x220+slot*0x70) and `0x8102` (**member left**, zero the record and `RudpMgr_CloseSlot`).
- **Host = room owner** (the member record flag byte +0x221 holds "is owner"); the lowest-level "who is host" test is the local slot
  `FUN_000bc2d0` vs. NetCtrl host id. **Host migration exists** in the message layer: `DC_NETCTRLCMD_CHANGE_HOST_CTRLID` (FUN_000a4000) and the
  OwnerChanged event, but whether a migrated session keeps playing was not traced (open).
- **Signaling callback FUN_000b6350** (asm): `0x5102` (Established) -> `SignalingGetConnectionStatus` -> `RudpMgr_Connect(slot, peerIP, peerPort)`;
  `0x5101` (Dead) -> clear member record, `RudpMgr_CloseSlot`.
- Context events handled by FUN_000b5540 (state/started/stopped; not decoded).

### 3.8 Invitations / presence (DcNetSignin + Toolkit2)
Presence data = the room-info blob (FUN_000bea20, hash-guarded), friends' presence parsed in FUN_000c7a50 (provides joinable room
info), invitation sent by FUN_000c84b0, in-game messages by FUN_000c0520 / c1c70, received by FUN_000c6760. **Not required for MVP**:
the lobby server browser (SearchRoom/JoinRoom) is enough.

## 4. P2P transport and the game's message layer
### 4.1 Rudp pump (asm, FUN_000bdae0)
Per call, with mgr mutex held: for each slot with active flag increments a tick counter (+0x3c.. per slot); builds poll set;
`sceRudpPollWait(poll, events[0x80], 0x80, 0)`; for each event id (slot looked up by context id):
- READ (ev&1): loop `sceRudpRead(ctx, recvbuf[slot], 0x40000, flags 1, NULL)` until error 0x80770016 (no data) / 0x80770024 (closed);
  every successful read is **one whole game packet** -> `DcNet::OnPacket` **FUN_0009bd80(net, slot, buf, len)**. Recv buffers:
  256 KB per slot at 0xd22250 + slot*0x40000.
- WRITE (ev&2): if slot state==2 and the one-deep send slot is full (byte +0x20), `sceRudpWrite(ctx, ptr(+0x28), len(+0x30), flags 1)`;
  complete when result >= len (then the byte is cleared and the sent length stored, +0x34); error 0x80770007/0x80770016 = retry.
  State 1 (connecting) -> on WRITE the code reads `GetOption(ctx, 0xe)` (error/state; 0 = ok), sets state 2 and calls
  **`DcNet::OnPeerConnected(slot)` FUN_0009bbb0**.
- ev&8 (abort/closed) -> `PollControl remove`, `Terminate`, `DcNet::OnPeerDisconnected(slot)` FUN_0009ad10.
- Rudp slot record (0x28 B, stride from mgr+0x18): +0 ctx id (-1 none), +4 state (0 off, 1 connecting, 2 connected), +8 send-pending
  byte, +0xc send ptr, +0x14 len(+0x30), +0x1c sent len, +0x20 tick counter. API: `RudpMgr_Send(slot, buf, len)` = FUN_000be170
  (fails if not state 2 or previous message still pending; **one outstanding message per peer**), `IsConnected` FUN_000be1b0,
  `IsSendIdle` FUN_000be1d0 (wrapped by FUN_000bc5e0/bc5f0 in DcNetMatching).
- Shim requirements for libSceRudp: message-oriented (preserves boundaries), reliable + ordered, messages up to 256 KB, nonblocking
  Read/Write, `PollWait` returning (ctx, events) with events 1=read 2=write 8=abort, `SetEventHandler` callback with one event id (2 = shutdown
  of everything; the handler FUN_000bd240 forwards to FUN_000bd260), `GetOption(0xe)` = 0 when ok, `Initiate` is called by **both** peers
  (simultaneous open); `Bind(ctx, socket, vport 20, mux 1)`.

### 4.2 Game packet layer (`DcNet`, asm unless marked)
**Header (16 bytes, little endian):** `+0 u32 sendTimeMs` (sceKernelGetProcessTime()/1000, FUN_005714b0), `+4 u32 peer sent-bytes
counter` (DcNet peer record +0x2c4, reset to 0 after send), `+8 u32 CRC` (**table-driven CRC-32 FUN_00572340(buf, len, init -1, final xor
-1)** over the whole packet with this field zeroed, selected table index 1 passed in r8; verified on receive: mismatch -> error -4),
`+0xc u16 (type-specific; for type 0 = payload byte count)`, `+0xe u8 type`. Receive checks (FUN_0009bd80): peer slot <= 3, length
in [0x10, 0x40000), CRC ok, then updates the peer's receive accounting (+0x2c0 -= hdr+4 ...).
**Types (jump table 0x6e4994):**
| type | rx handler | tx template | meaning |
|---|---|---|---|
| 0 | inline (9be82): append payload to double-buffered stream (`DcNet+0x70`/`+0xf0`, buffer index byte +0xd88, per-buffer byte count word @+0xc8/+0x148) via FUN_000a4240 | via FUN_0009b1c0 / 9b300 | **game data stream** (serialized objects; NetCtrl commands) |
| 1 | no-op (bookkeeping only) | template at peer+0x230 (16 B) | alive notify / pong |
| 2 | FUN_0009b560 (>= 0x38 B; 0x28 B payload incl. per-slot byte table; sets "member state" flag bits, writes a 0x20-byte state block into the 4 peer records) | template peer+0x250, 0x38 B (FUN_0009af70) | member / ready table (client -> host?) (unverified direction) |
| 3 | inline (>= 0x38; copies 0x28 B payload to DcNet+0x180..0x1a8, sets flag bit) | template peer+0x288, 0x38 B (FUN_0009b0b0) | member table broadcast (host -> clients?) |
| 4 | FUN_0009ba40(slot, len) | template peer+0x240, 0x10 B | alive request (ping) |
| 5 | FUN_0009baf0(slot, buf, len) | template DcNet+0x1a8 + slot*0x18, 0x18 B (8 B payload; FUN_0009aee0) | small control word |
Templates are (re)initialized per peer at connect by FUN_0009bbb0 (type bytes at +0x23e=1, +0x25e=2, +0x296=3, +0x24e=4, recv buffers
2 x 0x40000 + 2 x 0x2000 cleared). `DcNet::Update` = **FUN_00099e60** (called from a vtable, no static caller) runs `9a020`
(online-mode check), `9a0b0` (send path: alive request every 60 ticks via `game+0x90 % 60`, flagged control packets from bit table
0xc15b20, stream flush), `9a390`, `9a7f0`, `9a8c0` (room-info republish: builds blob via FUN_00098da0/99110, compares CRC, calls
Matching `SetRoomData*` and Signin presence). Flag word per peer at `DcNet + slot*0x2d8 + 0x4fc` (bits: 0 data pending, 1 alive-reply, 2 resync,
3/4 error, ...; exact names unverified). Per-peer record stride **0x2d8**, 4 peers (base `+0x250`).
**Tick rate:** the update runs on the 60 Hz logic tick (the code uses `60 / frameDivisor` conversions at b62e1 and bec28, the frame
runner of engine.md is 60 logic Hz with 1..3 ticks per displayed frame); alive ping = 1 s.

### 4.3 NetCtrl command layer
`DcNetCtrl` (0x9dbd0, vtables 0x8ccc30/0x8ccd88/0x8cd5a8) wraps stream objects (FUN_000a41b0 ctor) and commands with a 0x14-byte header
written by FUN_0009d5e0 (hdr = 16 B copied from the command at +8 and a u32 from +0x18, then a virtual `Serialize(stream)` at vtable[0], then
a commit FUN_000a4380). Named commands (strings): `DC_NETCTRLCMD_CONNECT_CHECK_CLEAR` / `_COUNT`, `_CHECK_ALIVE_REQUEST` / `_NOTIFY`,
`_CHANGE_HOST_CTRLID`; more command classes exist without name strings. Stream primitives: FUN_000a4240 (raw bytes), a47c0 (4 B),
a4b00 (1 B), a4bd0, acb90, a7fb0 (arrays/vectors); mirrors for reading (FUN_000a43c0 etc.). Gameplay classes implement
`Serialize(stream)` virtuals (e.g. FUN_004403e0 writes two 0x40-byte blocks + ~12 fields; FUN_00445f50 / 446390 / 44e890 / 44f1d0 /
451940 / 458020 / 45a6d0 / 479140 / 19ea40 / cbb80 / cbcc0 similar) - **object-level state replication**. Scene setup FUN_0042b470
(Act_SetupScene) -> FUN_0043b9d0 dumps all 4 player objects (raw 0x21d0-byte block each, stride 0x23b0 in the player array at
`scene->vslot(+0xd0)()->+0x80`) plus custom per-player serialization (FUN_002f7ff0) into the stream (initial state sync, inferred).
**Sync model (inferred, needs runtime confirmation):** host-authoritative state replication by serialized objects ("CtrlId"
ownership, host migration command), not deterministic lockstep: the packet layer has no frame/input numbering, only time + byte counters.
Connection checks `CONNECT_CHECK_COUNT` suggest all peers must report the same connected count before the scene proceeds.

## 5. PSN / Plus / NAT / account checks that must be faked
5.1 **Online ready (asm):** NetCtl state == 3 (NpHelper+0x74), NetCtl info code 2 readable (6 B), `sceNpGetState` signed in (value used
by FUN_000c75e0: treat as `SCE_NP_STATE_SIGNED_IN`), `sceNpCheckNpAvailabilityA` completes with 0 (result code at NpHelper+0x88),
`sceNpGetParentalControlInfoA` completes with no restriction (FUN_000c7050 reads chat/UGC flags; exact bits unverified).
5.2 **PS Plus (asm):** `sceNpCheckPlus(req, &result)` must complete and write **1** to the result byte (NpHelper+0x90); otherwise the
sign-in machine opens the NpCommerce upsell dialog and the online features stay blocked. PlusEventCallback may never fire.
5.3 **memberId quirk (asm, b61e0/b6350/bc2d0/b5ee0):** all code maps a Matching2 memberId to a slot with
`slot = (memberId >> 4) - 1`, valid only for memberId in [0x10, 0x50) (anything else -> 0xff = invalid). **Our lobby server must issue
member ids 0x10, 0x20, 0x30, 0x40** (or any value whose >>4 is 1..4) or the game will ignore the members. The same field is the 2nd
arg of the signaling event handler (peerMemberId). The local member id is stored at matching+0x170 (u16), and owner at +0x1e0.
5.4 **NAT type:** `SignalingGetLocalNetInfo` result is stored but not branched on (section 2.3). No `sceNetCtlGetNatInfo`/`NpNatType`
import exists. Provide natStatus = 2 (Type 2) or any value.
5.5 Account/NpId: `sceNpGetAccountIdA` (u64), `sceNpGetNpId` (used as part of the room member data & word filter), country code.
Any stable per-install value works; ids must differ between players (server assigns).
5.6 Title id "CUSA10454" (US) is hard-coded in the invitation request and `sceNpSetNpTitleId` blob in main.

## 6. Recommended minimal shim surface
**Must be real (functional) for an online MVP (lobby browser + P2P):**
- libSceNpMatching2 (20 functions listed in 2.3): Initialize/Terminate, CreateContextA, RegisterContextCallback, ContextStart/Stop,
  DestroyContext, GetServerId, GetWorldInfoList, SetUserInfo, CreateJoinRoomA, JoinRoomA, LeaveRoom, SearchRoom, SetRoomDataInternal/External,
  RegisterRoomEventCallback, RegisterSignalingCallback, SignalingGetLocalNetInfo, SignalingGetConnectionStatus. Responses are delivered by the
  request callback (arg/ctx as given at call) from `sceNpCheckCallback` (called each frame by the render thread tail - the shim must pump a
  callback queue there (resolved in section 10: all Matching2 callbacks are delivered from sceNpCheckCallback).
- libSceRudp (17) + the P2P socket use of libSceNet (Socket/Bind/Setsockopt/Htons/Close) -> UDP/ENet (section 4.1 semantics).
- NpManager: GetState, GetAccountIdA, GetNpId, GetAccountCountryA, CreateAsyncRequest/PollAsync/AbortRequest/DeleteRequest, CheckNpAvailabilityA,
  CheckPlus (result 1), GetParentalControlInfoA, Register/UnregisterStateCallbackA, CheckCallback.
- NetCtl: GetState (3), GetInfo (code 2 -> fixed MAC), Register/Unregister/CheckCallback.

**Can be stubs:** sceNpSetNpTitleId, SetContentRestriction, NotifyPlusFeature, Register/UnregisterPlusEventCallback, Net init/pool, Ssl, Http,
NpTus (all), NpUtility WordFilter (pass-through), NpCommerce / SigninDialog (immediate finished), NpInGameMessage (return success; no messages),
all NpToolkit2 NIDs (init -> 0; futures complete with return code 0 and empty results; presence/invite requests return success; event callback never
fires). Test the DcNetSignin state machine against the stubs (it must reach "signed in").

**Lobby server must store:**
- Worlds: >= 1 `WorldInfo` (id; matching+0xd0 is the world used) and a server id.
- Rooms: roomId u64, worldId, maxSlot 4, flagAttr (0x86000000 on create), owner memberId, member list, 8 searchable int attrs (ids 0x4c..0x53,
  searchable with operators 1..6; attr 0x53 EQUAL on the packed word), bin attrs 0x55 (<= 0x100 B, external) and 0x57 (<= 0xff B, internal - correction, section 10), room
  internal data (0x78 B bin attr, rewritten by SetRoomDataInternal; fan-out `UpdatedRoomDataInternal` 0x1106 to members), allowed/blocked user list.
- Members: memberId (0x10*(slot+1)), npId/account id, user bin attr 0x5f, natStatus, joinDate; member record struct delivered by 0x8101/0x8102
  (fields read by b61e0: memberId word at +0x38, ptr at +0x8 (NpId, copied by FUN_00572370), +0x10 qword, +0x20 16 B, +0x30 dword).
- Events to emit to members: MemberJoined 0x8101, MemberLeft 0x8102, Kickedout 0x1103, RoomDestroyed 0x1104, RoomOwnerChanged 0x1105
  (data: u16 prev owner @0, new owner @+2), UpdatedRoomDataInternal 0x1106, and signaling Established 0x5102 (+ peer address/port for
  `SignalingGetConnectionStatus`) / Dead 0x5101. Do the rendezvous (public ip:port of each member) in the server, like RPCN.
- Gameplay toggles (rule 3 / PLAN): room-info compatibility is an EQUAL match on protocol version (info @0x58, = 34); adding story-skip /
  quest-reshuffle flags can be put in unused int attrs or the 0x55/0x57 bin attrs without changing the 0x53 filter.

## 7. Function table (proposed names)
| addr | proposed name | role | conf |
|---|---|---|---|
| 00016b10 | Game_Get | returns global `DcCore*` (0xc17758) | high |
| 00016da0 | DcCore_SetupSubsystems | constructs DcNetSignin/DcNet/DcNetMatching/DcNetInfoReal among others (+0xb8/+0xb0/+0xc8/+0xc0) | med |
| 00099500 | DcNet_ctor | 'DcNet' | high |
| 00099270 | DcNet_Reset | init per-peer templates, streams | med |
| 00099e60 | DcNet_Update | per-tick driver | high |
| 0009bd80 | DcNet_OnPacket | rx: size/CRC check + type dispatch | high |
| 0009bbb0 | DcNet_OnPeerConnected(slot) | reset peer record + templates | high |
| 0009ad10 | DcNet_OnPeerDisconnected(slot) | sets flag bits, may set error | med |
| 0009a0b0 | DcNet_SendTick | alive/ctrl packets (every 60 ticks) | med |
| 0009ae00 | DcNet_SendPacket(slot, buf, len) | fills header (time, counter, CRC) and queues on Rudp slot via FUN_000bc5f0 | high |
| 0009aee0 / 9af70 / 9b0b0 | DcNet_SendType5 / SendType2 / SendType3 | control packets | med |
| 0009b1c0 / 9b300 | DcNet_SendStream_Client / _Host | stream flush (type 0) | med |
| 0009b560 / 9ba40 / 9baf0 | DcNet_RxType2 / RxType4 / RxType5 | | med |
| 0009b8f0 | DcNet_RxError(slot, code) | | med |
| 0009a8c0 | DcNet_PublishRoomInfo | serialize info, republish | med |
| 0009cae0 | DcNet_SetError | | med |
| 0009d5e0 | NetCtrl_WriteCommand | header + virtual Serialize | med |
| 0009dbd0 | DcNetCtrl_ctor | | high |
| 000a4240 | NetStream_Write(bytes) | | high |
| 000a43c0 | NetStream_Reset/Read | | low |
| 000a41b0 | NetStream_ctor | | med |
| 00098da0 / 99110 / 98fb0 | RoomInfo_Init / Read(reader) / Write(writer) | 0x78-byte info struct | high |
| 00098f00 / 98f50 / 98f60 | RoomInfo_SearchFieldA / ProtocolVersion(=0x22) / SearchFieldB | | med |
| 0009c3a0 | RoomInfo_Build | fills struct from game state | med |
| 000b51e0 | DcNetMatching_ctor | | high |
| 000b5350 | DcNetMatching_dtor | | med |
| 000b5540 | Matching_OnContextEvent | | low |
| 000b5ee0 | Matching_OnRoomEvent | event switch | high |
| 000b6350 | Matching_OnSignalingEvent | | high |
| 000b61e0 | Matching_AddMember(slot, data, isOwner) | | high |
| 000b5a90 / 5d50 | Matching_OnCreateJoinRoom / OnJoinRoom response | | med |
| 000b56b0 | Matching_OnSearchRoom response | builds list | med |
| 000b64e0 | Matching_PumpRudp | locks mgr, calls bdae0 | high |
| 000b8720 | Matching_StepInit | rudp init, ctx create, ctx start | high |
| 000b87c0 | Matching_StepSession | server id, world list, user info | high |
| 000b8a10 | Matching_CreateJoinRoom | | high |
| 000b8fd0 | Matching_SearchRoom | | high |
| 000b9890 | Matching_JoinRoom | | high |
| 000b7fb0 | Matching_LeaveRoom | | high |
| 000b8280 | Matching_Shutdown | | med |
| 000b8700 | Matching_IsInRoom | tests state bit | high |
| 000bc2d0 / bc2a0 | Matching_LocalSlot / OwnerSlot | `(memberId>>4)-1` | high |
| 000bbd40 | Matching_NumConnected | sums FUN_000bc400 over 4 slots | med |
| 000bba60 | Matching_BuildRoomAttrs | int attrs 0x4c-0x53, bins 0x55/0x57 | high |
| 000bb2e0 / bb3e0 / bb940 | Matching_CreateContext / ContextStart / ContextStop | | high |
| 000bb4f0 / bb770 | Matching_GetWorldInfoList / SetUserInfo | | high |
| 000bbda0 / bc020 | Matching_SetRoomDataInternal / External | | high |
| 000bd100 | RudpMgr_ctor | | high |
| 000bd4c0 | RudpMgr_OpenSocket | | high |
| 000bd600 / bd730 | RudpMgr_Init / End | | high |
| 000bd8b0 | RudpMgr_Connect | | high |
| 000bdae0 | RudpMgr_Pump | | high |
| 000bd240 / bd260 / bd400 / bd480 | RudpMgr_EventHandler / OnShutdownEvent / CloseSlot / CloseSocket | | med-high |
| 000be170 / be1b0 / be1d0 | RudpMgr_Send / IsConnected / IsSendIdle | | high |
| 000be1f0 / be540 | DcNetSignin_ctor / dtor | | high |
| 000bea20 | Signin_SetPresence | | med |
| 000c71f0 | Signin_StateMachine | Plus/avail/dialog | med |
| 000c74f0 / c75e0 | Signin_RegisterCallbacks / ReadNpInfo | | med |
| 000c6860 / c69f0 / c6850 | Signin_OnToolkitEvent | event dispatcher | med |
| 000c7a50 / c81c0 / c84b0 | Signin_GetPresences / GetFriendList / SendInvitation | | med |
| 000c0520 / c1c70 / c6760 | Signin_SendInGameMsg / Reply / OnInGameMsg | | med |
| 000be7c0 / c1fa0 / c22f0 | Tus_Set(Async) / Set / GetCrossSave | | med |
| 000c8620 | WordFilter_Check | | med |
| 0059ba10 | NpHelper_ctor | platform+0x124a8 | high |
| 0059b160 / 59b360 | NpHelper_InitStep / TermStep | | high |
| 0059b9c0 / 59bb40 | NpHelper_NetCtlCb / Poll | | high |
| 0059bca0 / bd20 / bdb0 | NpHelper_CheckAvailability / ParentalInfo / CheckPlus | | high |
| 0059be60 / bef0 | NpHelper_RegisterCallbacks / Unregister | | high |
| 0059bf60 / bfd0 | NpHelper_OpenSigninDialog / UpdateSigninDialog | | high |
| 00572340 | Crc32_Update(buf, len, init, xorout, tableIdx) | | high |
| 005714b0 | TimeMs | | high |

## 8. Open questions
1. What the 15 u16 room-info fields mean (stage / difficulty / progress / quest id): trace FUN_0009c3a0 (`RoomInfo_Build`) and the search UI
   writing the vector at matching+0x5d8 (writers not located).
2. Sync model confirmation (state replication vs. per-frame inputs): trace what the type-0 stream carries per tick (callers of FUN_000a4240 in gameplay
   are virtual `Serialize` methods; the dispatcher that picks classes is not located) - needs a live shadPS4/oracle capture or a runtime hook.
3. Exact meaning of DcNet packet types 1..5 direction/fields and the peer flag bits at +0x4fc.
4. Whether anything reads natStatus (+0x53c) outside the two ranges scanned.
5. (resolved, section 10) Matching2 request-callback signature actually used (events 0x0101-class ids not decoded in FUN_000b5a90/5d50/56b0/5540): needed to define the
   server's response format.
6. NpToolkit2 NIDs: only roles inferred; confirm against a Toolkit2 header if ever available.
7. Host migration end-to-end (CHANGE_HOST_CTRLID) and behaviour when the owner leaves.
8. (resolved, section 10) Threading: Matching2/Np callbacks and `sceNpCheckCallback` run on the RenderDLThread tail (engine.md) while `DcNet`/`RudpMgr` use mutexes
   (matching+0x6b0, mgr+0xb8, DcNet+0x208): the shim must keep the same threading contract.

## 9. Transport shim (implemented, step 1 of multiplayer)
Code: `port/src/net/` (host C++, lib `dcp_net`) + SysV entry points in `port/src/hle/net.cpp`; test `port/tests/test_net.cpp`.
- **Implemented:** libSceNet Init/Term/PoolCreate/PoolDestroy, Htons/Ntohs/Htonl/Ntohl, Socket (only AF_INET type 6), Setsockopt (no-op),
  Bind (creates the carrier; host UDP port = game's 3658 unless `DCP_NET_PORT` is set), SocketClose; all 17 libSceRudp imports; and
  `sceNpMatching2SignalingGetConnectionStatus`, which reads `net::get_peer_address(memberId)` (status 2 + addr/port in network order).
  The rest of Matching2/Np stays stubbed.
- **Layers:** `rudp` (contexts, poll sets, sockets; game semantics of 4.1) -> `Carrier` interface (`net/carrier.h`: `connect(peer,
  rudpPort)` (both sides call it), `send(conn, msg)` reliable+ordered whole messages, `queued_bytes`, `state`, `disconnect`;
  `CarrierSink` events on_connected / on_message / on_closed) -> `UdpCarrier` (own framing over one UDP socket, format in
  `udp_carrier.h`: SYN/SYNACK simultaneous open, 1200-byte fragments, cumulative ACK + 32-bit SACK, fast retransmit, RTO with backoff,
  1 s keepalive, 10 s idle / 15 s connect timeout, session nonce for peer restarts). Retransmission (2026-10-25, bug report 26: a
  guest frozen 30-45 s, no disconnect): per-fragment RTOs while ACKs make progress, backoff capped at 2 s; once no ACK has acked
  anything for a base RTO the link is "dark" and one connection timer resends only the oldest 32 fragments per timeout (the old code
  resent the whole 256-fragment window every round into a dead link and backed off to 16 s per try); any datagram from the peer on a
  dark link resends the oldest fragment at once. Wire format unchanged. Log lines `UDPC conn N: send stall` / `inbound gap` (oldest
  unacked fragment or inbound hole older than 2 s, and when it ends) and a per-link summary on close (timeouts, timed / fast / probe
  resends, longest stall, srtt) show transport stalls in bug-report logs. `test_net` `test_carrier_blackout` covers it. Own framing instead of ENet: Rudp needs a symmetric
  simultaneous open (ENet would make two peers per pair and need a tie-break), the carrier must run on a socket a NAT-traversal step
  may pick (`UdpCarrierOptions::adopt_socket`), and no fetch dependency.
- **Mapping:** one carrier per P2P socket; Rudp context <-> carrier ConnId per socket; a connection is (peer ip, peer UDP port, Rudp
  port 20). The Initiate vport (4649) is kept in `PeerAddr::vport` but not used on the wire. The game's 4 slots = 4 contexts.
- **Events:** READ = message queued; WRITE = connected and carrier queue < send buffer (0x40000); ERROR (8) = closed (FIN, timeout, reset,
  socket closed). `GetOption(0xe)` always succeeds with 0 (the game closes a slot only when it succeeds). Write accepts a whole message or
  returns 0x80770016; Read returns one message or 0x80770016 (none) / 0x80770024 (closed and drained).
- **Threading:** each carrier has a host I/O thread (select 5 ms, retransmits, keepalives); sink callbacks run there without the carrier
  lock and take the global rudp mutex. The game thread's PollWait(timeout 0)/Read/Write only touch queues. No guest code runs on host
  threads: the Rudp event handler (FUN_000bd240, event 2 = close+reopen socket) is stored but never called.
- **Hooks for the next steps:** `net::set_peer_address(memberId, ip_be, port_be)` (lobby/signaling fills it before raising the
  0x5102 Established event); `net::set_carrier_factory` (alternate carrier); `net::set_port_map_hook(port, open)` runs after bind with
  the real UDP port (UPnP IGD / NAT-PMP mapping, must not fail the bind) and at close.
- **Boot behaviour (verified, 1500 frames headless):** only sceNetInit + sceNetPoolCreate("FnPS4_Device", 0x20000) are reached; NetCtl
  GetState/GetInfo still unimplemented stubs; no socket opened, no new faults.
- **Open questions (Matching2 step):** real meaning of Rudp option 0xe and of error codes 0x80770001/04 (ours unverified); should the
  carrier report the public (STUN/UPnP) address to `SignalingGetLocalNetInfo`; hole punching must happen before 0x5102 is raised (the game
  Initiates immediately and our connect timeout is 15 s); whether to raise Rudp event 2 on socket failure (needs a guest-callable thread);
  message truncation if a peer ever sends > 0x40000 B (game reads with a 0x40000 buffer).

## 10. Matching2 / NpManager shim and lobby (implemented, step 2 of multiplayer)
Code: `port/src/hle/matching2.cpp` (19 Matching2 imports; the 20th, GetConnectionStatus, stays in hle/net.cpp),
`port/src/hle/np.cpp` (NpManager, NetCtl, 55 Toolkit2 NIDs, Commerce, WordFilter, Tus, Rtc SetTick), lobby link
`port/src/net/lobby_client.*`, mock lobby `port/src/net/lobby_server.*` + `port/tools/dcp_lobby`, protocol
`notes/map/lobby_protocol.md`, test driver `port/src/hle/net_test.cpp`, script `port/tools/dcp_lobby/two_instances.sh`.
- **Callback signatures (asm):** request cb `(u16 ctx, u32 reqId, u16 event, i32 err, void* data, void* arg)` (handlers
  b55b0 / b5660 / b5a90 / b5d50 / b56b0->b56d0 check `reqId == matching+0x74`, store err at +0x80, done byte +0x85; event
  ids ignored); context cb `(ctx, u16 event, u8 cause, i32 err, arg)` (b5540; 0x6f01 marks the context unusable, we send
  0x6f02 after the lobby answers hello); room event cb `(ctx, u64 roomId, u16 event, void* data, arg)` (thunk b5ec0);
  signaling cb `(ctx, u64 roomId, u16 peerMemberId, u16 event, i32 err, arg)` (thunk b6330). Requests are
  `(ctx, req*, opt{cb @0, arg @8}*, u32* reqId)`.
- **Struct layouts** used by the game are listed at the top of matching2.cpp with addresses (CreateJoinRoomA / JoinRoomA
  / SearchRoom / SetRoomData* requests, create/join response {RoomDataInternal*, member list, me, owner}, member record
  (joinDate @8, accountId @0x10, onlineId @0x20, memberId @0x38), search result RoomDataExternal (roomId @0x20, owner
  @0x40/@0x50, 8 int attrs read blind @0x78, external bin attr @0x98), event payloads). **Correction to 3.2/6:** in
  CreateJoinRoomA the 0x57 attr sits at req+0x60 (room *internal* bin attrs; it is also what SetRoomDataInternal sends
  and what the 0x1106 handler requires to be 0x78 B) and 0x55 at req+0x90 (room *external*, returned by search).
- **Threading (resolves 8):** every Matching2 callback takes the matching mutex (+0x6b0) and DcNetMatching issues its
  requests while holding it (Update b64e0 locks it), so no callback may run inside a Matching2 call. All are queued and
  delivered from sceNpCheckCallback (FUN_0059a1e0, render-thread tail, once per frame; a guest thread), never with a
  shim lock held. NpManager async requests complete immediately (sceNpPollAsync -> 0, result 0).
- **Sign-in (verified at runtime):** DcNetSignin mode 1 = FUN_000bfe10 reaches "signed in" (signin+0x10c = 2,
  +0x157c = 1; FUN_000bece0 true) two frames after it starts with these shims: NetCtl state 3, sceNpGetState 2,
  CheckNpAvailability ok, CheckPlus authorized; the Toolkit2 futures (never busy, return code 0, empty results) do not
  block it. The NpCommerce upsell is not reached.
- **How the game goes online (asm, for the menu work):** DcNetMatching is a task (`+0x5b` top mode, `+0x5c..` steps,
  framework 58da30 SetState / 58dab0 Update; vtable 0x8cdcb0, Update = b64e0 switch on the mode). Mode 0 pops commands
  from a queue at +0x5a0 (0x78-B entries, type byte +8): 1 ba510, 2 ba620, 3 ba1c0 (create room = mode 3 b6b30), 4
  ba6e0 (search with conditions = mode 4 b6cb0), 5 baa20 (join browse-list entry = mode 5 b6e60), 6 baae0, 7 baba0
  (leave), 8 bac60, 9 bad20, 0xa bade0, 0xb baea0, 0xc baf80 / bb070, 0xd bb160, 0xe bb220. Hosting is not a direct
  call: the UI hands DcNet the room info (DcNetInfo thunk b5170 -> FUN_0009c3a0(dcnet, info)) and sets the host bit
  (FUN_0009c5b0: DcNet+0x210 |= bit 4); DcNet_Update -> FUN_0009a8c0 then queues command 3 once signed in and idle.
  Sign-in is DcNetSignin (vtable 0x8cdda8) SetMode(1). The UI callers of these (quest departure / online menu) were not
  located; the user's save has no online access yet, so the milestone used `DCP_NET_TEST` (net_test.cpp) to make those
  same calls.
- **Two-instance result (2026-10-15):** mock lobby + two headless instances in town (own save copies, DCP_NET_PORT
  40001/40002): host signs in, creates room 4096 (int attrs 0x4c = 99, 0x53 = 0x1100 = version 34, 0x78-B info in 0x55
  and 0x57); joiner signs in, its SearchRoom (filter 0x53 EQ 0x1100) lists it, JoinRoomA -> member 0x20; MemberJoined on
  the host; signaling Established both ways; both `sceRudpInitiate`; `RUDP socket 1 conn 1 connected` on both; DcNet
  traffic flows (host first message 173 B, joiner 44 B, then ~1 message/s each way = alive ping/pong, 48 msgs each way by
  vblank 5400); when the host exits the joiner gets OwnerChanged 0x10 -> 0x20 + MemberLeft and closes the slot. Nothing
  loads into a shared session because neither side is in a quest (both idle in town). Same result through the real
  Worker under local `wrangler dev` (server/lobby-worker), over ws:// and over wss:// (TLS via SChannel, net/tls.*).
- **Open:** the UI path (who calls 9c3a0 / 9c5b0 / ba6e0 / baa20 and what the info fields mean, open question 1) and a
  save with online access to test it; session load once the host departs on a quest; public endpoint (STUN from the
  P2P socket) + `endpoint` message before signaling for real NATs; re-Established after an endpoint change; whether
  the lobby-assigned account id should replace the local one in sceNpGetAccountIdA; in-game messages / invitations / presence (Toolkit2) still dropped.

- **Early leave of a hosted room (fixed 2026-10-15, two-PC test):** the host left its room ~15 s after creating it
  (log: LeaveRoom + ContextStop, then "matching mode 7" forever). Two causes, both from the asm:
  1. *Game side (correct behaviour):* scene transitions end the online session. The title / data-load -> inn step
     FUN_0053ce80 (call at 0x53cffe) calls the online interface (vtable 0xc62a10, slot +0x188 = thunk b4e60) ->
     FUN_0009c6c0: DcNet+0xedc = 1, queue matching command 7 (leave, FUN_000baba0) and command 2 (FUN_000ba620); it
     also clears the DcNet host bit (+0x210). Other "go offline" entries with the same shape: 9bb70 (from scene code
     445bd0 / 445c50), 9baf0, 9b8f0. The test PC had no save, so the driver's fixed vblank-1200 host happened in the
     title / new-game flow, before any of those transitions. Reproduced headless: host at vblank 300 + title -> inn
     input -> leave at vblank ~870, trace `baba0 <- 0x9c6f1 <- 0x53d004`. An idle host in town stays in its room
     (real Worker, 200 s, no leave).
  2. *Shim bug:* mode 7 = FUN_000b7480: step 0 leave (b7fb0), step 1 FUN_000b8280 -> step 0 FUN_000bb940 =
     ContextStop wrapper, which then waits for the context callback (b5540 sets the done byte +0x84, bb9b2) before
     DestroyContext. Our sceNpMatching2ContextStop never raised 0x6f03, so DcNetMatching stayed in mode 7 for good
     and could never host or join again in that process. Now ContextStop queues 0x6f03 and a ContextStart on an
     already-connected lobby link queues 0x6f02 (both delivered from the pump); the game then runs DestroyContext and
     returns to mode 0 within a few frames.
  Fix in the driver (`hle/net_test.cpp` step 4): while no member has joined, a room the game ended that way is
  re-hosted (sign-in check, room info, host bit again) once matching is idle and DcNet+0xedc is clear (FUN_0009a390
  clears it). Each re-host is a new room with a new join code in the log ("MATCHING2 created room ... code"), which
  the launcher's Host panel follows. Proper fix later: host from the game's own online menu in town (UI path not
  located, open item above).
- **Room closed in town / join by code never matched (fixed 2026-10-15, live Worker, two headless instances):**
  3. *Host bit:* only FUN_0009c6c0 and FUN_0009c5d0 (interface thunk b4e10) clear the DcNet host bit (mask table
     0xc15b20 = 1,2,4,8,0x10: +0x10 = host; 99e60 / 9a7f0 / 9bd80 only touch bits 1..8). A go-offline call that
     leaves the room in place (inn -> town, seen at vblank ~1131 and ~1444) still clears it, and DcNet_Update helper
     FUN_0009a8c0 then closes the room: host bit off + in room + not closed -> command 9 (FUN_000bad20) -> mode 9
     FUN_000b76f0 -> FUN_000b9c80(edx 1) -> FUN_000bbda0: SetRoomDataInternal flag_filter/flag_attr 0x54000000
     (CLOSED|HIDDEN|0x04000000); the lobby then refuses joins with 0x80550d15. With the bit on, a closed room with
     info@0x40 > 0 is reopened: command 8 (FUN_000bac60) -> mode 8 FUN_000b7530 -> bbda0(edx 0) = attr 0x04000000.
     Mode table at 0x6e4dcc (Update b64e0): 0 b6680, 1 b6970, 2 b6a20, 3 b6b30, 4 b6cb0, 5 b6e60, 6 b7030, 7 b7480,
     8 b7530, 9 b76f0, 10 b78b0, 11 b7970, 12 bdae0-tail, 13 b7a60, 14 b7b60, 21 b7c60. If the host bit is on and
     info@0x40 <= 0 the room is closed (members > 1) or left (alone). Driver step 5: re-set the host bit (9c5b0)
     whenever it is cleared while the host is in its room.
  4. *Attributes:* once in town the game re-sends SetRoomDataExternal with the save's real info (player name, int
     attr 0x4c = 3, packed 0x53 = 0x81102: bits [1:0] = 2, [20:19] = 1 instead of 0) ~every 7 frames, mostly
     unchanged. A search with a join code now drops the attribute filters (the code names the room); identical
     SetRoomDataExternal bodies are answered locally (no Worker request). A search *without* a code still sends the
     game's 0x53 EQ condition, so the driver's condition-less browse cannot find a town room (needs the UI's
     conditions or masked matching; open).
  5. *Endpoint race:* rudp starts STUN only after the UPnP hook, so right after bind the state is Idle and the
     endpoint went out as public IP + local port; a peer that settled (1 s) before the STUN update kept that wrong
     endpoint ("endpoint changed after Established (kept the first)"). Idle now counts as waiting (8 s cap).
  Verified: live Worker, host from the title (vblank 300), room kept/reopened through title -> inn -> town, joiner
  by code finds and joins it, signaling Established (no Rudp traffic: both instances on one PC behind CGNAT =
  hairpin, the same-NAT item); mock lobby on loopback, same flow: Rudp connected, DcNet traffic both ways (27 msgs
  each way by vblank 3600). A real two-PC Internet session is still untested.

## 11. NAT traversal: STUN, hole punching, UPnP, launcher Online tab (step 3 of multiplayer)
### 11.1 STUN (public endpoint of the P2P socket)
Code: `port/src/net/stun.*` (own RFC 5389 Binding Request/Response, IPv4, XOR-MAPPED-ADDRESS preferred over
MAPPED-ADDRESS; no dependency), `UdpCarrier::start_stun` / `stun_result` (`net/carrier.h`), `rudp::public_endpoint`,
`hle/matching2.cpp` (endpoint logic). Tests: `test_net` (codec on synthetic bytes incl. the RFC 5769 2.2 address,
error / other-transaction / truncated cases; a carrier discovering its endpoint through a fake loopback STUN server that
drops the first request, with an unresolvable first server; a server that never answers -> Failed);
`test_net --live-stun` queries the real servers.
- **Same socket:** the query is sent from the carrier's own UDP socket and its I/O thread recognises the answer
  (STUN: top two bits 0 + magic cookie 0x2112A442 at +4; carrier datagrams start with 'DC' 0x44 0x43), so the mapping
  learned is exactly the one peers will hit. Started at `rudp::socket_bind` (after the UPnP hook). DNS runs on a helper
  thread; the I/O thread resends at 0/250/750/1750 ms, 3 s per server, then the next server.
- **Servers:** `$DCP_STUN` = comma list of host:port, `0`/`off` disables; default `stun.cloudflare.com:3478`, then
  `stun.l.google.com:19302`.
- **Endpoint to the lobby** (`endpoint` op, protocol rev 1, unchanged): if the lobby saw us on a public address
  (`hello.your_ip` not loopback/RFC1918/169.254/100.64), we send `{ip, port}` = the STUN mapping; while STUN is still
  running we wait (max 8 s after the bind); if STUN fails or the lobby is local/LAN we send only the local port (the
  lobby uses the address it sees), as before.
- **Hole punching:** on every `signal` the client sends 3 throwaway 4-byte datagrams ('DCPH', dropped by receivers) from
  the P2P socket to the peer endpoint, so our NAT opens a mapping before the SYNs. Signaling Established (-> the game's
  `sceRudpInitiate`, SYN every 100 ms for 15 s from both sides = simultaneous open) is raised for a peer only when (a)
  our own endpoint is final (STUN done/failed/timed out and the `endpoint` reply received) and (b) that peer's endpoint
  has not changed for 1 s (its STUN update may arrive just after the join-time signal). The Worker re-signals both
  sides of every pair on `endpoint`, so both ends converge on the public endpoints.
- **Peer-reflexive port (UdpCarrier):** a SYN from a peer's IP but an unexpected port, while exactly one link to that
  IP is still connecting on that Rudp port, re-targets the link to the observed port (logged `UDPC conn N: peer port
  a -> b`). This covers the race where the game binds its P2P socket only after create/join (seen in the two-instance
  log: `sceNetBind` follows `created room`), so a peer may Establish on the hello-time port before our STUN update
  arrives, and port-translating NATs in general. Test: `test_net` (stale port, link still comes up).
- **Regression check (mock lobby, two headless instances, Work/stage5/in_town_to_lucain.txt, 3600 frames):** room,
  signal, Established and `RUDP conn connected` both ways, first DcNet messages (173 B / 44 B) exchanged; the joiner then
  leaves (matching mode 7) about 25-85 frames later. The build before this work (d184443) does exactly the same with
  that input, so the early leave is the game's reaction to that save/input state, not the NAT code.
- **Live result (2026-10-15, user's network):** `stun.cloudflare.com` answered in one round trip; local UDP 60796 ->
  public 203.0.113.10:15996 (port-translating NAT); a second query from the same socket to `stun.l.google.com` gave
  the same mapping = endpoint-independent mapping, so UDP hole punching should work from this network.
- **Limitation (protocol rev 1 carries one endpoint per member):** two players behind the *same* NAT get each other's
  public endpoint and need NAT hairpinning, which many home routers lack; two instances on one PC against the public
  Worker have the same problem. Local/LAN lobbies keep working (local endpoints). **Proposed rev 2:** `endpoint` gains
  optional `local_ip`/`local_port`, `signal` relays them as `local: {ip, port}`; the client tries the local candidate
  first when the peer's public IP equals its own (SYN to both, first SYNACK wins; needs a carrier that accepts a
  connection from either candidate). **Done (2026-10-16):** without a protocol bump (optional `local_ip` /
  `local_port`, lobby_protocol.md 4/5); Worker, mock lobby, matching2 (`default_route_ip`, `set_peer_alt` when the
  peer's public IP equals ours) and UdpCarrier (SYNs to both while connecting, settle on the first answering address);
  tests: Worker vitest, `test_net` test_lan_candidate. Needs the Worker redeployed; not yet tried on a real LAN.
- Symmetric NATs (mapping differs per destination) cannot be punched without a relay (not in rev 1); the launcher's
  "Test connection" reports this (two servers, same socket).
### 11.2 UPnP port mapping
Code: `port/src/net/upnp.*` on miniupnpc 2.2.8 (BSD-3; CMake FetchContent pinned to tag `miniupnpc_2_2_8`, offline
cache `-DFETCHCONTENT_SOURCE_DIR_MINIUPNPC=<local miniupnpc 2.2.8 checkout>`, static lib, no samples/tests).
- `upnp::install_hook()` (from `sceNetInit`; no network until a bind) registers the carrier port-map hook. On the P2P
  socket's bind a helper thread discovers the IGD (SSDP, 2 s), maps UDP ext = local port (on conflict 718/727 the next
  4 ports; error 725 -> permanent lease), lease 3600 s renewed at half-life, description "OpenCrown". Removed on
  socket close and at process exit (atexit). Every outcome is logged (`UPNP ...`); failures are ignored.
- `$DCP_UPNP=0|off|false` disables it (launcher checkbox).
- Endpoint choice (matching2.cpp, behind a public lobby): UPnP mapping if it succeeded and the router's WAN address is
  public (accepts unsolicited packets, so it also works against a peer behind a symmetric NAT), else the STUN mapping;
  the endpoint update waits for both (max 8 s after the bind).
- **Live (2026-10-15, user's router):** mapping succeeds in ~2 s, but the router's WAN address is 100.64.0.13 (CGNAT,
  100.64/10): the ISP adds a second NAT, so the UPnP mapping alone does not make the port reachable; the endpoint falls
  back to the STUN mapping (endpoint-independent, 11.1), i.e. hole punching is what makes P2P work there.
- Known cost: closing the socket while discovery is still running waits for it (<= ~2 s, game thread).
- Tests: `test_net` checks the env toggle; `test_net --live-upnp` maps and unmaps UDP 47999 on the real router.
### 11.3 Launcher Online tab
Code: `port/tools/launcher/main.cpp` (links `dcp_net`), connection test `port/src/net/online_check.*`.
- **Settings** (`launcher.ini`): `lobby` (default `wss://opencrown-lobby.networkhandler.workers.dev/`), `name`
  (online id, <= 16 chars; empty = Windows user name), `upnp` (default on), `join_code`. Passed to dcp_run as
  environment like `DCP_SAVE_ROOT`: `DCP_LOBBY`, `DCP_NP_NAME`, `DCP_UPNP=0` when off (unset otherwise).
- **Test connection** (worker thread, own sockets, not the game's LobbyClient): WebSocket connect + `hello` (account
  "0", the lobby assigns one; the socket closes right after), STUN from an ephemeral socket to both servers (public
  endpoint + mapping check: same mapping from two servers = endpoint-independent = punchable, different = symmetric),
  UPnP probe of UDP 3658 (map 60 s lease, then removed). One verdict line + details. Test: `test_lobby` runs it against
  the mock lobby (STUN off) and against a dead URL; `test_net --live-check <url>` runs it for real.
  Live (2026-10-15, user's PC vs the deployed Worker): lobby connected (TLS) in 49 ms, sees 203.0.113.10; STUN
  203.0.113.10:15934 via stun.cloudflare.com, mapping endpoint-independent; UPnP mapped but the router's WAN is
  100.64.0.13 (CGNAT); verdict "Ready".
- **Friends** (2026-10-17, lobby_protocol.md 4.1 / 4.2): identity.ini (`net/identity.*`, random account id + secret,
  shared with dcp_run through `DCP_USER_DIR`, which the launcher now always passes), own friend code with Copy, add by
  code, incoming requests (Accept / Decline), sent requests (Cancel), friend list (online first; Online / Online, in
  a room / Offline; Remove asks once more). Each refresh or button is one short lobby session on a worker thread
  (`net/friends.*`, hello with `presence: false`); refreshed when the tab is shown and every 30 s while it is open.
  `--tab Online` opens the tab first. Not yet fed to the game's own friend calls / "Join Friends Room" (friends.md).
- **Host / Join** buttons start dcp_run like Play plus `DCP_NET_TEST=host|join` (and `DCP_JOIN_CODE=<code>` for join):
  the game's own online menu path is still not located (10 "How the game goes online"), so the existing test driver
  (`hle/net_test.cpp`) signs in and creates / searches+joins about 20 s after boot (vblank 1200, needs the player in
  town by then). The host's join code is read from the running game's log (`MATCHING2 created room ... code X`) and
  shown with a Copy button. What does **not** work yet: choosing host/join from inside the game, a shared quest/session
  load (net.md 10 open items), joining a friend from the friend list.

## 12. The game's own online menu (observed live, 2026-10-16)
First runs through the game's real online menu (previous live tests all went through the `DCP_NET_TEST` driver, 11.3).
Source: four labelled runs kept by the user as `<user dir>/logs/Pressed - Join Random Room.log`, `Pressed - Join Friends
Room.log`, `Pressed - Go Offline.log`, `Pressed Go Offline, Go Online.log` (the launcher overwrites `logs/last_run.log`
every start, so each was copied out). Save had online unlocked; player in town; hosted rooms 4117..4121.

### 12.1 Observed flow
1. **Going online always self-hosts.** Whatever the menu choice, the sequence is identical and matches 3.1/3.2:
   `sceNpCheckNpAvailabilityA` -> `GetState` (signed in) -> `GetParentalControlInfoA` -> `CheckPlus` ->
   `Matching2Initialize` -> `sceRudpInit` -> `CreateContextA` -> `ContextStart` -> **`CreateJoinRoomA`** (max_slot 4,
   `world_id` 1, flag_attr 0x86010000, int attrs 0x4c..0x53, bin attrs 0x55/0x57, empty `member_bin_internal`) ->
   `sceNetSocket("RUDP_NetId")` + `sceNetBind(port 3658, vport 4649)` -> STUN -> UPnP -> endpoint update to the lobby.
   So "online" is not host-or-join: **every online player owns a room** and discovery happens on top of that. Our
   launcher's Host / Join split (11.3) is a test harness, not the game's model.
2. **Join Random Room** is the only button that searches: one `SearchRoom` (`start` 1, `max` 20, flag_attr 0x64000000,
   flag_filter 0, `attr_ids` [0x4c, 0x54, 0x55]) with conditions `0x4c >= 1`, `0x4d <= 8`, `0x53 == 528642`. Matches
   3.3 exactly, including the trailing packed-0x53 equality. Returned 0 rooms (nobody else hosting).
3. **Join Friends Room issues nothing at all** - no `SearchRoom`, no friend/presence call, not even an `UNIMPL` line,
   after the usual go-online sequence. The screen filters a friend list that nothing ever populated: the port has no
   friend support anywhere (`grep -i friend port/src/hle` is empty). See 12.3.
4. **Go Offline** is clean and symmetric: `LeaveRoom` -> `sceNetSocketClose` -> UPnP unmap -> `ContextStop` ->
   `DestroyContext` (3.6). **Go Offline then Go Online again works**: availability/parental/Plus re-checked,
   `sceSigninDialogOpen -> OK (headless)`, new context, new room, new socket, STUN and UPnP redone.

### 12.2 The random-search filter cannot match a room we host
Decoding the observed values against 3.4 (create `int_attrs` vs. the search conditions above):

| attr | info field | create | search wants | match |
|---|---|---|---|---|
| 0x4c (76) | u16 @0x40 | 3 | `>= 1` | yes |
| 0x4d (77) | u16 @0x42 | 9 | `<= 8` | **no** |
| 0x53 (83) | packed | 528641 = 0x81101 | `== 528642` = 0x81102 | **no** |

0x53 field by field: bits[1:0] (@0x50, search slot 8) create **1** vs search **2** - the only differing field;
bits[4:2] (@0x52) 0 both; bits[6:5] (@0x54) 0 both; bits[13:7] (@0x58) **34 = 0x22 both**, i.e. the protocol/version
gate agrees; bits[18:14] (@0x5a) 0 both; bits[20:19] (@0x56, search slot 9) 1 both.

Both sides are produced by the game itself, so this is game state, not a lobby bug (filter ops are decoded identically
in `lobby_server.cpp int_match`: 1 EQ, 2 NE, 3 LT, 4 LE, 5 GT, 6 GE). Consequence: **random matchmaking can never match
a room hosted by another copy of our build** in this state - worth fixing before the next two-PC test, since a failed
random search would otherwise look like a transport problem. Open: what sets @0x50 to 1 vs 2 (room state/mode?) and
what @0x42 is (progression gate, cf. the 0x4c join gate in 3.5) - mapper pass `notes/map/online_room_attrs.md`.

### 12.3 What this means for our lobby / netcode (rework, user's call 2026-10-16)
The MVP assumption in 6 and 3.8 - "presence and the friend list are not required, SearchRoom/JoinRoom is enough" - is
wrong for the game's own menu: two of the three online buttons need friend data, and even hosting carries it
(`CreateJoinRoomA` passes up to 2000 account ids from the DcNetSignin friend vector, 3.2). The lobby protocol and our
Np shim need to grow identity and social state rather than rooms-plus-join-codes:
- a stable account identity per player (we currently synthesize an account id per connection, 10) and a **friend list**
  the shim can answer from (`DcNetSignin+0xa0`, 0x180-byte records with the account id at +0x170, 3.2);
- **presence**: each online player publishes the room-info blob (`FUN_000bea20`) and friends read joinable room info
  from it (`FUN_000c7a50`, 3.8) - this, not SearchRoom, is what the Friends screen reads;
- invitations / in-game messages (`FUN_000c84b0`, `FUN_000c0520`/`c1c70`, received `FUN_000c6760`) if the invite path
  is wanted;
- lobby protocol rev: friend add/remove and presence fan-out alongside the existing room ops (rev 2 for local
  candidates is still pending, 11.1).
Not yet decided: where the friend list comes from (per-lobby account registry, a code/nickname exchange, or an
out-of-band list the launcher owns). Needs a mapper pass over DcNetSignin's friend vector and presence parsing before
any rewrite - asm first.

### 12.4 Smaller findings from the same runs
- **Endpoint re-send is skipped on re-online.** `matching2.cpp` guards the lobby `endpoint` op with
  `want_port != g_reported_port || want_ip != g_reported_ip`, and those globals survive `DestroyContext`. The second
  Go Online therefore never re-sent the endpoint. Harmless in these runs (the WebSocket stayed up, so the lobby kept
  the right `Client::udp_ip/udp_port` from the first cycle, and the STUN mapping was unchanged), but it breaks if the
  mapping shifts or the socket reconnects: reset the globals on `ContextStop`/`DestroyContext`.
- **CGNAT on every run** (confirms 11.2): UPnP maps, but the router's WAN is 100.64.0.13, so the endpoint falls back
  to STUN 203.0.113.10:15946. If the peer is also behind CGNAT, hole punching may not succeed and a relay would be
  needed; no two-PC data on that yet.
- Tried and failed quietly, blocking nothing: `sceNpTusSetDataAAsync` / `TusDeleteRequest` / `TusDeleteNpTitleCtx`
  (online save-data slot) and the `sceNpTrophy*` calls (2.5) - all still `UNIMPL`.
- The menu path does **not** need `DCP_NET_TEST` any more: 10's open item "how the game goes online" is answered by
  12.1, so 11.3's test driver can retire once the Friends/Random paths work.
