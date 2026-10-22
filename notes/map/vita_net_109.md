# Vita 1.09 network / PSN imports: what our HLE must provide (Matching2 + Rudp + Net/NetCtl/NpManager)

Mapper survey, read-only. Target: `Work/vita/update109.elf` (US PCSE00019 update 1.09, fixed-address ET_SCE_EXEC, Thumb-2,
text 0x81000000..0x81772664, data 0x81773000). Roles/structure follow `vita_net.md` (made on 1.06; its addresses do not apply here).
Tags: **(asm)** read in capstone disassembly, **(inferred)** deduced from call shape / SDK conventions, **(unverified)** guess.
Tooling (scratch, gitignored): `notes/raw/vita_net_109/v9.py` = 1.09 port of `Work/vita/vita.py`+`q.py`+`imports.py`
(`imp <libregex>`, `r <a> <b>`, `callers`, `xr`, `str`; module info read from e_entry, import table at modinfo+0x2c);
`imports.tsv` there = full stub / NID / caller list. `Work/vita/imports.py` is hard-wired to 1.06 offsets and does not work on 1.09.
NID names: `android-port/data/nids.txt` (all 1.09 net NIDs resolve).

## 1. Summary
- 1.09 runs the same layering as 1.06/Pro: DcNetMatching object (Matching2 room + signaling) -> RudpMgr (one P2P UDP socket,
  libRudp, max 4 peers) -> DcNet packets. All Matching2 requests are **async with a request callback**; the game polls a
  done flag. Every Matching2 step returns 0 = pending, 1 = done, 2 = error.
- **Big change vs 1.06: protocol version is 0x22 (= Pro) and the room-info blob must be exactly 0x78 bytes (= Pro).**
  1.09 Vita and Pro now agree on both matching-level items that differed in 1.06 (vita_net.md section 1 items 1-2). (asm)
- Online entry is gated by: `sceNetCheckDialog` (mode 3, result ignored, only status FINISHED needed), `sceNpGetServiceState`
  == 3 (ONLINE), `sceNetCtlInetGetState` == 3, chat / content-rating flags OK. Then Rudp init, Matching2 context, server,
  world, user info, then create-or-search/join.
- **Blocker today (asm + HLE source):** 49 of the listed net NIDs have no HLE handler (unregistered = `[hle] UNIMPL` + fault,
  `android-port/src/vita/hle.cpp:305`). The online-start sequence calls `sceNpMatching2Init` (unregistered) *before* the
  sign-in checks, so choosing online play would fault today (inferred: reached as soon as netobj state 1 runs).
- Rating: **careful** - the call set is small (20 Matching2 + 17 Rudp + 9 Net), but HLE must synthesize the Vita
  Matching2 response structs (linked member lists, NpIds, attr arrays) at the exact offsets in section 4 and run the
  callbacks on a guest thread; libRudp has to be emulated (or replaced) at message level.

## 2. Import stubs (1.09), NID, name, callers
Stub = BLX target; caller = call site @ function start. Full list incl. all call sites: `notes/raw/vita_net_109/imports.tsv`.

### SceNpMatching2 (20; 1.06 had a 21st, 0x78521326, gone in 1.09)
| stub | NID | name | caller fn(s) |
|---|---|---|---|
| 815057cc | ebb1fe74 | Init(poolSize 0x20000, prio 0x10000100, affinity 0, stack 0x4000) | f813b574a (init step 5) |
| 815057bc | 0124641c | Term | f813b5c34 |
| 81504fcc | adf578e1 | CreateContext | f8139cb10 |
| 8150503c | f9e35566 | RegisterContextCallback | f8139cb10 |
| 81504fdc | bb2e7559 | ContextStart | f8139cc2e |
| 81504f6c | 506454de | ContextStop | f8139ccee |
| 81504f4c | 368aa759 | DestroyContext | f8139cb94, f8139cdb6, f813a13a2 |
| 81504f7c | 65c0feed | GetServerLocal | f8139ce2a |
| 81504fec | c086b560 | GetWorldInfoList | f8139ceb6 |
| 8150501c | f049bec1 | SetUserInfo | f8139d144 |
| 81504f9c | 818a9499 | CreateJoinRoom | f8139db0e |
| 8150500c | d48baf13 | SearchRoom | f8139de6e |
| 81504f3c | 33f7d5ae | JoinRoom | f8139e600 |
| 81504ffc | c8b0c9ee | LeaveRoom | f8139e9d2 |
| 81504fac | 8f88ac7e | SetRoomDataExternal | f8139eb74 |
| 81504fbc | 94784a8c | SetRoomDataInternal | f8139ecc2 |
| 8150502c | f486991b | RegisterRoomEventCallback | f8139d8d0, f8139db0e, f8139e600, f8139e9d2 (unregister: cb 0) |
| 81504f2c | 1c60bc5b | RegisterSignalingCallback | f8139d8d0, f8139db0e, f8139e600 |
| 81504f8c | 79310806 | SignalingGetLocalNetInfo | f8139d8d0, f8139db0e, f8139e600 |
| 81504f5c | 4e4c55bd | SignalingGetConnectionStatus | f8139c9ec |

### SceLibRudp (17, all used by RudpMgr 0x813a2746..0x813a3344)
| stub | NID | name | caller fn(s) |
|---|---|---|---|
| 8150505c | 22c9aa29 | Init | f813a2afa |
| 8150513c | d77928c5 | GetStatus | f813a2afa |
| 8150508c | 3a495e69 | EnableInternalIOThread | f813a2afa |
| 8150507c | 396a24d6 | SetEventHandler | f813a2afa, f813a2b9a (NULL) |
| 8150510c | 943d96ec | PollCreate | f813a2afa |
| 8150512c | c9acbdf0 | PollDestroy | f813a2b9a |
| 8150509c | 3ea0961e | End | f813a2b9a |
| 815050ec | 772c07e8 | CreateContext | f813a2bfa |
| 815050fc | 8b766108 | Bind | f813a2bfa |
| 8150511c | bd87a5ca | SetOption | f813a2bfa (x2) |
| 815050ac | 46b635bd | Initiate | f813a2bfa |
| 8150504c | 18668bd8 | PollControl | f813a2bfa (add), f813a288a / f813a28b6 / f813a2d76 (delete) |
| 815050cc | 55918c7c | PollWait | f813a2d76 |
| 8150506c | 30a9438e | Read | f813a2d76 (x2) |
| 815050dc | 69c9de15 | Write | f813a2d76 (x2) |
| 8150514c | e68005e3 | GetOption | f813a2d76 (x3) |
| 815050bc | 552b2d3e | Terminate | f813a288a, f813a28b6, f813a2d76 (x4) |

### SceNet (9)
| stub | NID | name | caller fn(s) |
|---|---|---|---|
| 8150552c | eb03e265 | Init | f813b574a (step 0), f813af584, f813af5f4 |
| 8150551c | ea3cc286 | Term | f813b5c34, f813afaca, f813afb24 |
| 81504dec | f084fce3 | Socket | f813a2746 |
| 81504d8c | 065505ca | Setsockopt | f813a2746 (x3) |
| 81504dac | 1296a94b | Bind | f813a2746 |
| 81504dbc | 29822b4d | SocketClose | f813a2746, f813a2862, f813a2930, f813a2b9a |
| 81504ddc | 9fa3207b | Htons | f813a2746 (x3), f813a2bfa, f8138faca, f8138fd9c |
| 81504d9c | 07845128 | Ntohs | f8138f4a8 (debug string of the peer port) |
| 81504dcc | 98839b74 | InetNtop | f8138f45a, f8138fdfe, ad-hoc f813b8682/8714/8790 (debug strings) |

### SceNetCtl (14)
| stub | NID | name | caller fn(s) |
|---|---|---|---|
| 8150554c | 495ca1db | Init | f813b574a (step 1), f813af52a, f813af5f4 |
| 8150558c | cd188648 | Term | f813b5c34, f813afa6e, f813afb24 |
| 8150559c | dffc3ed4 | CheckCallback | f813b2076 (per frame), f813b7d24 |
| 815055ac | eaee6185 | InetRegisterCallback | f813b1f78 (boot, cb 0x813b290b), f813b8246 (online, cb 0x813b7bcf), f813b7df4 |
| 8150578c | d0c3bf3f | InetUnregisterCallback | f813b7e10, f813b83e2 |
| 8150543c | 6d26ac68 | InetGetState | f813b8352 (online gate), f813ab692, f813b2260, f813b2532, f813b7cb8, f813b7d24 |
| 8150557c | b26d07f3 | InetGetInfo | f813b8352 (code 3), network-info screen f813b2260 (x20), f813b2532, f813b7cb8, f813b7d24 |
| 8150556c | 6b20ec02 | InetGetResult | f813b290a (Inet callback, event 1) |
| 8150555c | 4ddd6149 | GetNatInfo | f813b2260, f813b2532 (network-info screen) |
| 8150575c | 0961a561 | AdhocGetState | f813b7bea, f813b7c28, f813b812a, f813b81ec |
| 8150576c | 7118c99d | AdhocGetInAddr | same 4 (ad-hoc only) |
| 815057ac | ffa9d594 | AdhocRegisterCallback | f813b7dc2, f813b81c2 |
| 8150577c | a4471e10 | AdhocUnregisterCallback | f813b7dde, f813b81ec |
| 8150579c | ed43b79a | AdhocDisconnect | f813b818c |

### SceNpManager (10)
| stub | NID | name | caller fn(s) |
|---|---|---|---|
| 815055bc | 04d9f484 | sceNpInit | f813b574a (step 2), f813af2ce |
| 815055cc | 19e40ae1 | sceNpTerm | f813b5c34, f813af95a, f813afb24 |
| 815055dc | 3b0ae9a9 | sceNpCheckCallback | f813b2076, f813b710a |
| 815055ec | 44239c35 | RegisterServiceStateCallback | f813b1f78 (cb 0x813b28cb), f813b574a step 10 |
| 815055fc | d9e6e56c | UnregisterServiceStateCallback | f813b2034, f813b5c34 |
| 81504eec | 54060df6 | sceNpGetServiceState | f810767a6 |
| 81504edc | 3c94b4b4 | sceNpManagerGetNpId | f810767a6, f81077048, f8139cb10, f813a4b56, f813ae342, f814f7c98, f814f7eec |
| 81504f1c | fe835967 | GetAccountRegion | f810767a6 |
| 81504efc | 60c575b1 | GetChatRestrictionFlag | f810767a6, f813a5004 |
| 81504f0c | af0073b2 | GetContentRatingFlag | f810767a6, f813a50ee |

### SceNpBasic (9), SceNpCommon (4), SceNetAdhocMatching (10)
| lib | stub: NID name (callers) |
|---|---|
| NpBasic | 8150566c efb91a99 Init (step 6); 8150565c 389bcb3b Term; 8150564c 20146aec CheckCallback (f813b170a, f813b710a); 8150545c 3ade3628 RegisterInGameDataMessageHandler (f813ab692, step 11); 8150544c 363ff161 Unregister... (f813ab82a, f813b5c34); 81504e9c 7a5020a5 SendInGameDataMessage (invites: f813a3e0e, f813a3e66, f813a57b6, f813ac50a); 81504e8c 3b0a7f47 RecordPlaySessionLog (f8139c2b6, f8139c308 = on member join, f813b12e4); 81504e7c 364531a8 GetPlaySessionLog / 81504eac fb0f7fdf ...Size (f81076f54, f813b0b34) |
| NpCommon | 8150567c 441d8b4e sceNpAuthInit (step 3); 8150568c 6093b689 sceNpAuthTerm; 81504ebc e807d0bc sceNpSetPlatformType (invite senders); 81504ecc fb8d82e5 sceNpCmpNpId (block-list filters f8139be9e, f8139bf08; f81076a18; invites) - returns 0 when equal, **0x80550609 = not equal** (asm: loop continues on that value) |
| AdhocMatching | 8150611c Init, 815060dc Term, 815060ec Create, 815060fc Delete, 8150613c Start, 815060bc Stop, 815060cc SetHelloOpt, 8150612c SelectTarget, 815060ac CancelTarget, 8150610c SendData - all in 0x813b8408..0x813b882c (local ad-hoc mode only) |

**HLE coverage today (grep of `REG(` in android-port/src/hle):** missing = all 20 Matching2; SceNet Socket/Setsockopt/Bind/SocketClose/
Htons/Ntohs/InetNtop; NpManager GetNpId/GetAccountRegion/GetChatRestrictionFlag/GetContentRatingFlag; NpCommon CmpNpId/SetPlatformType;
NpBasic Get/RecordPlaySessionLog(+Size)/SendInGameDataMessage; NetCtl AdhocGetInAddr/AdhocDisconnect; all 10 AdhocMatching.
Rudp: all 17 registered (offline stubs). `sceSysmoduleLoadModule` records any id (system.cpp).

## 3. Online flow (order, gates, stalls)
Module/init helpers: `f813b5686(core, n)` = sysmodule-loaded bit n (word core+0x116fcc; id table 0x815cab48: n0=0x01 NET, 2=0x15 NP,
4=0x12 NP_BASIC, 6=0x19 NP_UTILITY, 7=0x1c NP_MATCHING2, **8=0x21 RUDP** (inferred names: vitasdk sysmodule ids)). All RudpMgr
functions return early unless bit 8 is set (asm). `f813b574a(core, step)` = one-time init step (done bits core+0x116fd0):
0 NetInit({mem, 0x20000, 0}), 1 NetCtlInit, 2 sceNpInit({commId*, passphrase*, signature*}, 0), 3 NpAuthInit, 4 NpTusInit,
5 Matching2Init, 6 NpBasicInit(0), 7/8 no-op, 9 NpLookupInit, 10 NpRegisterServiceStateCallback, 11 InGameDataMessage handler, 12 (f813abcf6, not read).
Boot f813b5d18: load 10 sysmodules, steps 0,2,3,6. (asm)

| # | step (function) | calls / checks | stall or error (asm unless tagged) |
|---|---|---|---|
| 1 | netobj state 1 sub 1 -> f81076868 -> f813b8246 | init steps 1,7,8,9,10,4,5,12,11 (**Matching2Init here**), `NetCtlInetRegisterCallback(0x813b7bcf, netctl, &cid@+0x58)`, `sceNetCheckDialogInit` (mode +0x50 = **3 PSN_ONLINE**, sdkVersion 0x03570071, commId 12 B at +0x54, age byte +0x6c = 13) | - |
| 2 | f813b8352 (polled) | `sceNetCheckDialogGetStatus` must return **2** (FINISHED); `GetResult` called but **result not read**; `Term`; `NetCtlInetGetState` -> netctl+0x5c; `NetCtlInetGetInfo(3 = ETHER_ADDR, buf)`: ret 0 or 0x80412108 accepted, 6 bytes -> +0x60 | stalls while status != 2 |
| 3 | f810767a6 | `sceNpGetServiceState`: 1 -> obj+0xa4 = 0, 2 -> 1, **3 -> 2**; state 2/3: GetNpId(obj+0xa8), GetAccountRegion(obj+0xfc, obj+0xf8), GetChatRestrictionFlag(obj+0xf0), GetContentRatingFlag(obj+0xf4, obj+0x100) | - |
| 4 | f813a34a8 | online iff obj+0xa4 == 2 **and** netctl+0x5c == **3** (f813b803e) | else error -100, state 1 fails |
| 5 | f813a50ee | `GetContentRatingFlag(&isRestricted, &age)`: ret in (0x80550001, 0x80550003] or 0x8055050b -> -100; isRestricted && age < 13 -> message 1 + fail | HLE: return 0, isRestricted 0 |
| 6 | f813a5004 | `GetChatRestrictionFlag(&flag)`: same error codes; flag != 0 -> message 0 (dialog f8105bb00) | HLE: return 0, flag 0 |
| 7 | f813a51f8 sub 5 | obj+0x1340 = 1 ("online ready"); create/search/join flows require it (f813a34d4) | - |
| 8 | f8139d4d4 sub 0 (f8139d496) | RudpMgr init (section 4.6) | - |
| 9 | f8139d4d4 sub 1 (f8139cb10) | GetNpId(this+0x1f4), CreateContext, RegisterContextCallback(0x8139bcf5, this) | ret < 0 -> -100 |
| 10 | f8139d4d4 sub 2 (f8139cc2e) | ContextStart(ctxId, timeout u64 0); waits for **any** context callback (this+0x64), errorCode (this+0x60) >= 0 = success (event code not checked) | stalls until the context callback fires |
| 11 | f8139d7e8 sub 0 (f8139ce2a) | GetServerLocal(ctxId, this+0x21a); **status byte this+0x21c must be 1** | else 0x80550c2d -> -100 |
| 12 | sub 1 (f8139ceb6) | GetWorldInfoList; picks world | no match -> 0x80550d11 |
| 13 | sub 2 (f8139d144) | SetUserInfo(bin attr 0x5f, 1 byte) | waits request cb |
| 14a | create f813a056a | queue netobj request type 5 (f813a617e, connection check, inferred) -> CreateJoinRoom -> callbacks + LocalNetInfo -> room ready | - |
| 14b | search f813a0332 / join f813a0108 | same check -> SearchRoom -> user picks index this+0x7f0 (f8139e948; >= count -> -7) -> JoinRoom | join errors 0x80550d19 -> **-8**, 0x80550d15 -> **-7**, 0x80550d0f -> **-6**, other -> -100 |
| 14c | join gate (f8139e600) | save value at save+0xaf1e4 (f813846ba) must be <= room attr 0x4c (this+0x68c) | else -8, (same as 1.06/Pro) |
| 15 | signaling cb event 0x5102 | SignalingGetConnectionStatus -> RudpMgr_Connect(slot, &addr, port) | - |
| 16 | RudpMgr pump (f813a2d76, from DcNetMatching update f813a0f68) | PollWait; WRITE in state 1 -> GetOption(14) >= 0 -> connected -> DcNet_OnPeerConnected | - |
User-visible error: DcNetMatching+0x7e8 (first error wins) is pushed to DcNet+0x18c by f8138e7ca; raw SCE code at +0x778. UI reads it
through 0x814f6848 (virtual; message mapping not traced). Inet callback 0x813b7bcf sets bit 1 of netctl+0x80 on any event (disconnect flag).
Service-state callback 0x813b28cb posts {type 2, state} to the game event queue; boot Inet callback 0x813b290b: event 1 -> InetGetResult, events 1/3 posted.

## 4. Structures the game passes / reads (all 32-bit, little endian)
### 4.1 Callbacks (signatures from the thunks; asm)
| callback | address | args as read | handling |
|---|---|---|---|
| context | 0x8139bcf5 (f8139bcf4) | (u16 ctxId, u16 event, u8 eventCause, s32 errorCode, void* arg) | stores event -> this+0x50 (u16), cause -> +0x52, error -> +0x60, done flag +0x64 = 1; event **0x6f01** sets "context dead" bit (bit[4] of +0x1e8); every later step checks that bit |
| request (generic) | 0x8139be71 / 0x8139c6d5 / 0x8139c731 / 0x8139c78d (static), member variants 0x8139be42 / 0x8139c6a6 / ... | (u16 ctxId, u32 reqId, u16 event, s32 errorCode, const void* data, void* arg) | if reqId == this+0x54: error -> +0x60, done +0x65 = 1 |
| GetWorldInfoList | 0x8139be27 -> f8139bd46 | same | if data && reqId match && error >= 0: response {+0 world* array, +4 worldNum}; copies **at most 1** world (0x40 B, 16 words) to this+0x220, count +0x2a0 |
| CreateJoinRoom | 0x8139c587 -> f8139c586 | same | data = response, `*(data+0)` = RoomDataInternal* -> f8139c3d8 (4.3) |
| JoinRoom | 0x8139c647 -> f8139c646 | same | same parser f8139c3d8 |
| SearchRoom | 0x8139c131 -> f8139bf08 | same | 4.4 |
| room event | 0x8139c9d3 -> f8139c80c | (u16 ctxId, u64 roomId [r2:r3], u16 event, const void* data, void* arg) | 4.5 |
| signaling | 0x8139cad3 -> f8139c9ec | (u16 ctxId, u64 roomId [r2:r3], u16 memberId, u16 event, s32 errorCode, void* arg) | 0x5101 (dead): close slot `(memberId>>4)-1`; **0x5102 (established)**: GetConnectionStatus + Rudp connect; others ignored |
| Rudp event | 0x813a2997 -> f813a2930 | (s32 eventId, s32 soc, data*, len, addr*, addrlen, void* arg) | event 2: close all 4 slots, close + reopen the socket |
RequestOptParam (all requests, 0x10 B): `+0 cbFunc, +4 cbFuncArg (this), +8 timeout u32 = 0, +0xc appReqId u16 = 0`. reqId out -> this+0x54.

### 4.2 Calls and request structs (offsets = struct offsets; asm)
| call | arguments / struct |
|---|---|
| CreateContext | (SceNpId* this+0x1f4, commId* = *(core+0x116f88), passphrase* = *(core+0x116f8c), u16* ctxId -> this+0x218) |
| ContextStart / Stop | (ctxId, u64 timeout 0) / (ctxId) |
| GetServerLocal | (ctxId, out* this+0x21a): +0 u16 serverId, **+2 u8 status, must be 1** |
| GetWorldInfoList | req {+0 u16 serverId}; World entry 0x40 B: **+4 = worldId** (compared / reused; +0 unread) |
| SetUserInfo | req (12 B) {+0 u16 serverId, +4 BinAttr*, +8 num 1}; BinAttr (12 B) {+0 u16 id 0x5f, +4 ptr, +8 size 1}; value 0 or 2 (f8138c05e, save flag) |
| CreateJoinRoom | req: +0 worldId (this+0x264), +8 u64 lobbyId 0, +0x10 maxSlot **4**, +0x14 flagAttr **0x86000000**, +0x18 roomBinAttrInternal* {0x57} / +0x1c 1, +0x20 searchableIntAttrExternal* / +0x24 **8**, +0x28/+0x2c 0, +0x30 roomBinAttrExternal* {0x55} / +0x34 1, +0x38..+0x4c 0, **+0x50 blockedUser* (SceNpId[], 0x24 stride) / +0x54 count <= 100**, +0x58..+0x64 0, +0x68 sigOptParam* -> 8 B {+0 u8 type 1 (mesh, inferred), rest 0} |
| IntAttr (8 B) | {+0 u16 id, +4 u32 value}: ids 0x4c..0x53; 0x4c..0x52 = 7 s16 from the room-info struct (+0x40.., 0x4e masked to u16), 0x53 = packed: [1:0] w8, [4:2] w10 (99 -> 5), [6:5] w11, [13:7] w12, [18:14] w13, [20:19] w9 (words at this+0x6ac..0x6c0 = room-info shorts) |
| bin attrs 0x55 / 0x57 | both carry the **same serialized room-info blob** (core+0x1a98, size core+0x140): 0x57 if size < 0x100, 0x55 if size <= 0x100 |
| SearchRoom | req: +0 option **0x19**, +4 worldId, +8 u64 lobbyId 0, +0x10 startIndex **1**, +0x14 max **0x14**, +0x18 flagFilter **0**, +0x1c flagAttr **0x64000000**, +0x20 IntSearchFilter* / +0x24 n, +0x28/+0x2c 0, +0x30 attrId* (u16 0x4c, 0x54, 0x55) / +0x34 **3** |
| IntSearchFilter (12 B) | {+0 u8 op (1..6 = EQ, NE, LT, LE, GT, GE, inferred), +4 u16 attrId, +8 u32 value}; up to 10 from this+0x7f8 conditions (attr idx 0..7 -> 0x4c+idx; idx 8/9/10 feed the packed value) + always last {EQ, 0x53, packed}: [1:0] cond8, [4:2] cond10, [6:5] f8138c05e (0/2), **[13:7] version 0x22** (f8138c05a), [18:14] f8138c028 (0/2), [20:19] cond9 |
| JoinRoom | req: +0 u64 roomId, +0x18 optData.data[0] = caller byte (0 from search list), +0x28 optData.len **1**, +0x30 blockedUser* / +0x34 count <= 100 |
| LeaveRoom | req: +0 u64 roomId (this+0x2d0), +8..+0x1f 0 (optData) |
| SetRoomDataExternal | req: +0 roomId, +8 IntAttr* / +0xc **8**, +0x10/+0x14 0, +0x18 BinAttr* {0x55} / +0x1c 1 |
| SetRoomDataInternal | req: +0 roomId, +8 flagFilter, +0xc flagAttr: close = (0x54000000, 0x54000000), reopen = (0x54000000, 0x04000000); +0x10 BinAttr* {0x57} / +0x14 1; rest 0. Game tests flagAttr **0x40000000** as "closed" (0x8139f876) |
| SignalingGetLocalNetInfo | (info*): **+0 size = 0x10** (in), +4 localAddr -> this+0x79c, +8 mappedAddr -> +0x798, +0xc natStatus -> +0x7a0 (no reader found) |
| SignalingGetConnectionStatus | (ctxId, u64 roomId, u16 memberId, s32* connStatus (ignored), u32* addr -> peer+0x58, u16* port -> peer+0x5c (network order, reused as-is)) |

### 4.3 Create/Join response: RoomDataInternal as f8139c3d8 reads it (asm)
| offset | width | read into / meaning |
|---|---|---|
| +0 | u16 | this+0x410 (serverId, inferred) |
| +2 | u16 | this+0x2c0 |
| +4 | u32 | this+0x2c4 (worldId, inferred) |
| +0x10 | u64 | **roomId** -> this+0x2d0 |
| +0x30 | ptr | member list head; each member -> f8139c308 (slot = (memberId>>4)-1), next = member+0 |
| +0x38 | ptr | **me**: NpId (+4) -> this+0x2da, memberId (+0x30) -> this+0x32c, joinDate (+0x28) -> this+0x324 |
| +0x3c | ptr | **owner**: NpId -> this+0x376, memberId -> this+0x3c8, joinDate -> this+0x3c0 |
| +0x48 | u32 | flagAttr -> this+0x684 |
| +0x50 | ptr | RoomBinAttrInternal[0]: +0x10 data ptr, **+0x14 size must be 0x78**, else room info ignored |
| +0x54 | u32 | -> this+0x688 (bin attr count, inferred) |
RoomMemberDataInternal: +0 next*, **+4 SceNpId (0x24)**, +0x28 u64 joinDate, **+0x30 u16 memberId** (memberId = (slot+1)<<4). Nothing else read.
Peer record (this+0x414 + slot*0x9c): +0 in-use, +1 optData flag, +2 NpId, +0x26 NpId copy, +0x4c joinDate, +0x54 memberId, +0x58 peer addr, +0x5c peer port.
Joining a member also calls `sceNpBasicRecordPlaySessionLog(NpId*, 0x200-B template 0x815ca188)` unless it is me.

### 4.4 SearchRoom response (f8139bf08; asm)
Response: +8 roomNum (u32), +0xc RoomDataExternal* (list, next at +0). Per room: **+6 u16** -> rec+8 (curMemberNum, inferred), **+0x18 u64 roomId**,
**+0x38 SceNpId* owner** (copied twice, optional), **+0x48 IntAttr* (reads value +4 of 8 consecutive 8-B entries = attrs 0x4c..0x53 in order)**,
**+0x58 BinAttr* {+4 ptr, +8 size}** (0x55 blob, <= 0x100). Rooms equal to the current roomId or with an owner in the block list (CmpNpId == 0) are dropped.
Kept rooms: 0x198-B records in a vector at this+0x2b0 (begin +0x2b4 / end +0x2b8). HLE must return the 8 int attrs positionally even though attrId list is (0x4c, 0x54, 0x55).

### 4.5 Room events (f8139c80c; asm)
| event | data the game reads | action |
|---|---|---|
| 0x1101 member joined | +0 member* (memberId +0x30), +8 u8 (optData[0], inferred) | add peer slot |
| 0x1102 member left | +0 member* | close slot (f8139c7ba: DcNet drop, Rudp terminate) |
| 0x1103 kicked out | - | bit[2] of +0x1e8 |
| 0x1104 room destroyed | - | bit[1] of +0x1e8 |
| 0x1105 owner changed | +2 u16 new owner memberId | owner fields from that peer slot |
| 0x1106 room data internal updated | +4 u32* newFlagAttr -> +0x684; +0x1c RoomBinAttrInternal** / +0x20 num: [0] +0x10 ptr, **+0x14 size == 0x78** | re-parse room info, int attrs -> this+0x68c.. |
| 0x1107, 0x1108 | - | ignored |

### 4.6 RudpMgr (this = DcNetMatching+0x6d8; asm)
- Socket: `sceNetSocket("RUDP_NetId", 2, 6 (P2P dgram), 0)`; `Setsockopt(s, 0xffff, 0x1000 / 0x2000 / 0x1100, &1, 4)` (0x1100 = NBIO, others unverified);
  `Bind(s, {u8 len 0x10, u8 fam 2, port htons(3658) @+2, addr 0 @+4, vport htons(4649) @+8}, 0x10)`. Socket fd at +8, open flag +0x94.
- Init (f813a2afa): `Init(pool 0x81875ed0, 0x100000)`, `GetStatus(buf, 0xf8)`, `EnableInternalIOThread(0x2000, 0xbf)`, `SetEventHandler(0x813a2997, this)`, `PollCreate(0x80)` -> +4. Results not checked.
- Connect (f813a2bfa): `CreateContext(NULL, NULL, &ctx)`, `Bind(ctx, sock, vport 20, mux 1)`, `SetOption(ctx, 2, &0x40000, 4)`, `SetOption(ctx, 3, &0x40000, 4)`,
  `Initiate(ctx, {0x10, 2, port, addr, vport htons(4649)}, 0x10, 0)`, `PollControl(poll, 1 add, ctx, 0xb)`; slot state 1.
- Pump (f813a2d76): skips PollWait when no slot is active; `PollWait(poll, ev[0x80] (8 B: +0 ctx, +4 u16 req, +6 u16 rtn), 0x80, u64 0)`.
  READ (1): `Read(ctx, 0x819b5ed2 + slot<<18, 0x40000, 1, NULL)` until 0x80770016 / 0x80770024, each result = one DcNet packet -> f8138ea8e(slot, buf, n).
  WRITE (2): state 1 -> `GetOption(ctx, 14, &v, 4)`, ret|v >= 0 -> state 2 + DcNet_OnPeerConnected f8138e914; state 2 with pending send -> `Write(ctx, ptr, len, 1)`
  (0x80770016 / 0x80770007 / 0 / short = retry). ERROR (8): GetOption(14) >= 0 -> close. Unknown ctx: drain to 0x81975ed0, Terminate on error.
  Pending send times out after f8146c1b4(60)*10 ticks. Send (f813a3248): only if state 2 and nothing pending. Slots: this+0x10 + k*0x20: +0 ctx, +4 state, +8 pending, +0xc ptr, +0x10 len, +0x14 sent, +0x18 read, +0x1c tick.

### 4.7 SceNpId (what the game needs)
0x24 bytes, copied whole (memcpy 0x24) between GetNpId, member records, block lists, invites; compared only with sceNpCmpNpId (and strncmp in f81076a18).
Layout per SDK (inferred): +0 online id char[16] + term + 3 pad, +0x14 opt[8], +0x1c reserved[8]. HLE needs a stable unique 0x24-B value per player whose
first 16 bytes are the display name (inferred). NpManager also supplies: service state (must be **3**), account region, chat flag 0, content rating 0.

## 5. Function table (1.09; 1.06 equivalent from vita_net.md where matched by role / byte match)
| 1.09 | 1.06 | name | role | conf |
|---|---|---|---|---|
| 0x8139bcf4 | 0x81387766 | M2_OnContextEvent | context cb | high |
| 0x8139bd46 / 0x8139be26 | 0x81387e16 | M2_OnWorldInfoList (+ thunk) | world copy | high |
| 0x8139be42 / 0x8139be70 | - | M2_OnRequestDone (member / static) | generic req cb | high |
| 0x8139be9e | - | NpId_IsBlocked | CmpNpId over block list (f81017cf4()->+0x74, stride 0x198, NpId +0x16c) | med |
| 0x8139bf08 / 0x8139c130 | - | M2_OnSearchRoom (+ thunk) | 4.4 | high |
| 0x8139c1ac / 0x8139c23a | - | M2_MySlot / OwnerSlot | | high |
| 0x8139c308 | - | M2_AddMember | peer record fill | high |
| 0x8139c3d8 | 0x81388706 | M2_ParseRoomDataInternal | 4.3 | high |
| 0x8139c586 / 0x8139c646 | - | M2_OnCreateJoin / OnJoin | request cbs | high |
| 0x8139c7ba | - | M2_CloseSlot | | high |
| 0x8139c80c / 0x8139c9d2 | 0x81388b1e / 0x81388cc4 | M2_OnRoomEvent (+ thunk) | 4.5 | high |
| 0x8139c9ec / 0x8139cad2 | 0x81388cde / 0x81388da4 | M2_OnSignalingEvent (+ thunk) | 0x5101/0x5102 | high |
| 0x8139cb10 | 0x81388de2 | M2_CreateContext | | high |
| 0x8139cb94 / 0x8139cdb6 | 0x81388e66 | M2_DestroyContext | | high |
| 0x8139cc2e / 0x8139ccee | 0x81388f00 / 0x81388fc0 | M2_ContextStart / Stop steps | | high |
| 0x8139ce2a | 0x813890fc | M2_GetServerLocal step | | high |
| 0x8139ceb6 | 0x813891fe | M2_GetWorldInfoList step | | high |
| 0x8139d144 | 0x813893fc | M2_SetUserInfo step | | high |
| 0x8139d496 / 0x8139d55a | - | RudpMgr start / end wrappers | | high |
| 0x8139d4d4 / 0x8139d58e / 0x8139d7e8 | - | M2_StartFlow / StopFlow / ServerFlow | sub-state machines | high |
| 0x8139d8d0 | 0x81389b80 | M2_RegisterRoomCallbacks | room/sig cb + LocalNetInfo | high |
| 0x8139d964 | 0x81389bfa | M2_BuildRoomAttrs | int + bin attrs | high |
| 0x8139db0e | 0x81389d72 | M2_CreateJoinRoom step | | high |
| 0x8139de6e | 0x8138a09e | M2_SearchRoom step | | high |
| 0x8139e600 / 0x8139e948 | 0x8138a540 | M2_JoinRoom step / join search index | progression gate | high |
| 0x8139e9d2 | 0x8138a8fa | M2_LeaveRoom step | | high |
| 0x8139eb74 / 0x8139ecc2 | 0x8138aa7e / 0x8138abae | M2_SetRoomDataExternal / Internal steps | | high |
| 0x8139ee52 | - | M2_UpdateRoom (int + ext) | | med |
| 0x8139fd88 / 0x813a0108 / 0x813a0332 / 0x813a056a | - | M2 flows: join (invite) / join (search) / search / create | | med |
| 0x813a0f68 | - | DcNetMatching_Update | state switch (0x15 states), then Rudp pump | med |
| 0x813a1122 / 0x813a13a2 | 0x8138ed76 area | DcNetMatching ctor / dtor | | med |
| 0x813a2746 / 0x813a2858 / 0x813a2862 | 0x8138eabe | RudpMgr_OpenSocket / wrapper / CloseSocket | | high |
| 0x813a28b6 | 0x8138ec0c | RudpMgr_CloseSlot | | high |
| 0x813a2930 / 0x813a2996 | 0x8138ecec | RudpMgr_OnEvent (+ thunk) | | high |
| 0x813a29de | 0x8138ed76 | RudpMgr_ctor | | high |
| 0x813a2afa / 0x813a2b9a | 0x8138eef6 / 0x8138ee96 | RudpMgr_Init / End | | high |
| 0x813a2bfa | 0x8138ef7c | RudpMgr_Connect | | high |
| 0x813a2d76 | 0x8138f0da | RudpMgr_Pump | | high |
| 0x813a3248 | 0x8138f578 | RudpMgr_Send | | high |
| 0x8138ea8e / 0x8138e914 | 0x813781c0 / 0x81378046 | DcNet_OnPacket / OnPeerConnected | | high (byte match) |
| 0x8138c05a | 0x813755c2 | ProtocolVersion = **0x22** | | high |
| 0x8138c05e / 0x8138c028 | 0x813755c6 / 0x81375590 | search fields A / B (0 or 2 from save flags) | | med |
| 0x8138c15a / 0x8138bf88 / 0x8138bdb0 | 0x813756a2 / 0x8137552c / 0x813754c8 | RoomInfo ctor / Read / Write | | med (Write by byte match) |
| 0x8138e7ca | - | DcNet_SetError (+0x18c) | | high |
| 0x810767a6 | - | NpState_Refresh | service state, NpId, flags | high |
| 0x81076868 / 0x810768f0 | - | Online_Start / Online_End steps | | high |
| 0x813a34a8 / 0x813a34d4 | - | Online_IsConnected / IsReady | | high |
| 0x813a5004 / 0x813a50ee | - | Check chat / content restriction | | high |
| 0x813a51f8 / 0x813a5624 | - | netobj state 1 (online start) / state dispatcher | | high |
| 0x813b5686 / 0x813b56b2 / 0x813b56fe | - | Sysmodule IsLoaded / Load / Unload | | high |
| 0x813b574a / 0x813b5c34 | 0x81395140 / 0x813952c6 | NpHelper init / term step | | high |
| 0x813b5d18 / 0x813b7896 / 0x813b78ee | - | boot init / online init / online term | | high |
| 0x813b8246 / 0x813b8352 / 0x813b83e2 | - | NetCheck start / poll / end | | high |
| 0x813b803e / 0x813b804a | - | NetCtl inet state == 3 / ad-hoc state == 3 | | high |
| 0x813b8408..0x813b882c | 0x813977dc..0x81397ae8 | ad-hoc matching wrappers | | high |

## 6. Open questions
1. Context events: only 0x6f01 is special and the ContextStart wait accepts any callback with error >= 0; which event code HLE should send
   (Start = 0x6f02, inferred) does not matter to this code, but other readers of this+0x50 were not searched.
2. Netobj request type 5 (f813a617e -> state 5 handler f813a3c80 -> f81076e38) before create/search: internal check, not traced to the end; confirm at runtime it completes once online.
3. SearchRoom response room +6 and RoomDataInternal +2 / +0x54 semantics (game copies them; meaning inferred); World entry +0 unread.
4. Setsockopt 0x1000 / 0x2000 names (crypto / signature, unverified); HLE can accept and ignore.
5. JoinRoom 5th-argument byte from the invite path (f8139fd88, f8139e118 not read).
6. Whether the 33 "long-only" and 4 layout command differences of 1.06 (vita_net.md section 12) still hold for 1.09; the matching level now equals Pro (version 0x22, info 0x78), so the command stream may too. Not re-compared.
7. Error-to-message mapping (DcNet+0x18c consumer is virtual, 0x814f6848) not traced.
