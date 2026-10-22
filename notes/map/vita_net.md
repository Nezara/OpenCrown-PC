# Vita (original Dragon's Crown) netcode vs Pro: feasibility of a Vita <-> Pro translator

Mapper survey, read-only on port/ and server/. Input: user's own US Vita dump PCSE00019, update 1.06
(`Update/eboot.bin`, module name `DraCro_PSV_Patch6`). Tags: **(asm)** read in Thumb-2 disassembly (capstone),
**(inferred)** deduced from structure / call shape, **(unverified)** guess. Addresses are Vita VMAs (the module is a
fixed-address `ET_SCE_EXEC`: text at 0x81000000, data at 0x817b8000). Pro addresses are eboot-relative as in net.md.
Derived files live in `Work/vita/` (gitignored): `update.elf`, `imports.tsv`, `index.pkl`, helper scripts.
Pro reference: `notes/map/net.md` (1-4, 9, 10), `notes/map/lobby_protocol.md`.

## 1. Summary and verdict
The Vita build and Pro run the **same netcode**: same layering (Matching2 room + signaling -> P2P UDP socket -> libRudp ->
`DcNet` packet layer -> NetCtrl command/stream layer), same constants (ports, vports, mux mode, buffer sizes, 4 slots), same
Matching2 attribute ids and bit packing, same `memberId` mapping, same DcNet header + CRC-32 + packet-type table (Vita has
one extra type), same 0x14-byte command header, same 136 `DC_*CMD_*` command names (identical sets, byte search of both images), same explicit-width little-endian stream
primitives. Pro looks like a recompile of this code for x86-64 with the platform libs swapped.

Concrete incompatibilities found (all small and enumerable):
1. Protocol version in the packed room attr: Vita **0x21 (33)**, Pro **0x22 (34)**.
2. Room-info blob (bin attr 0x57 / 0x55): Vita **0x62 B**, Pro **0x78 B**; the first 0x62 bytes of the layout match; Pro appends 22 B.
3. DcNet packet **type 6** (0x340-byte member/profile packet carrying 0x24-byte `SceNpId` blobs) exists on Vita, not on Pro; it is **ad-hoc mode only** (12.6), so irrelevant for Matching2 play.
4. A family of stream primitives (the `long` plus 21 enum-typed ints: 22 writer/reader pairs, 44 functions) is **4 bytes on the wire on Vita, 8 bytes (sign/zero-extended) on Pro**. Used by 33 command classes (12.2).
   This is the only width difference found in the primitive set. 4 more commands (CREATEPLAYER_*, CREATE_PLAYER, EXCHANGE_BONEINFO) have Pro-only additional fields (12.3).
5. Identity: `SceNpId` (0x24 B) on Vita vs 8-byte account ids on Pro (type 6, allowed/blocked user lists: 100 NpIds vs 2000 ids,
   member records 0x78 vs 0x70 B).

**Verdict (updated after the full command comparison, section 12): THIN at the wire level, medium overall only because of the non-wire items.** Not a different sync model: both sides replicate
state through the same command-object stream (`DC_*CMD_*`) with the same packet framing. Of the 136 command classes, **99 are byte-identical, 33 differ only by fields that are 4 B on Vita and 8 B on Pro
(a family of 44 wide primitives in Pro, patchable at the primitive level), 4 are Pro = Vita + extra fields** (CREATEPLAYER_REQUEST/_NOTIFY, CREATE_PLAYER, EXCHANGE_BONEINFO: +362-byte hero "extra" table, +17 u8),
0 raw-struct mismatches (the 0x21d0 player block is the persistent hero record, same layout, pointers sit outside it), type 6 is ad-hoc-only (nothing to fake). A translator needs (a) attr/version rewrite,
(b) room-info pad/truncate, (c) nothing for type 6, (d) a hook in our Pro port making the 44 wide primitives 4 bytes (or a per-class table for the 33), (e) insert/strip the extra table + 17 u8 in 4 commands,
(f) identity mapping NpId <-> account id. What is **not** established is gameplay-state compatibility of the payload *contents* (id sets, items, the per-player virtual hook of the scene-setup dump) - no runtime evidence.
Also note: Vita cannot reach our lobby by itself. The realistic placement is on the Vita side: hook the game's own import stubs (all
net calls go through ~60 NID stubs at 0x8156b3a0..0x8156c5b0) in a Vita plugin (taiHEN-style, unverified feasibility) or run the Vita
build in an emulator whose HLE speaks our lobby/carrier. A "packet sniffing" translator between two real Sony Rudp endpoints would have to
implement Sony's wire Rudp (undocumented) - not needed with the hook approach, because the DcNet packets are whole Rudp messages.

**Rating: careful.** Transport, framing and the wire format of all 136 commands are now statically compared; the remaining gap is payload semantics, which needs a runtime trace.

## 2. Rebuilding the ELF and import inventory
SELF (decrypted, `SCE\0`, version 3, header_len 0x1000, metadata 0x600): ELF header at 0xa0, 2 phdrs at 0xe0, segment-info at 0x120.
Both segments are **plain** (compression field 1; encryption flag 2 only marks the header). (asm)
| seg | file off | size | vaddr | flags |
|---|---|---|---|---|
| 0 text+rodata | 0x1080 | 0x7b7784 | 0x81000000 | R-X |
| 1 data | 0x7b9080 | 0x570eb0 | 0x817b8000 | RW- |
`e_entry` 0x56cc90 = module info (seg 0 offset). Module info: exports 0x56ccec..0x56cd0c, imports 0x56cd0c..0x56d4f8 (**0x34-byte
import records**, 40 libraries, 404 function imports, 3 variable imports in SceLibc). Record: +0 size/version, +4 attr(u16) +6 nfunc(u16)
+8 nvar/ntls, +0x10 libnid, +0x14 libname*, +0x1c func_nid_table*, +0x20 func_entry_table* (the stub addresses). No relocations exist
(fixed address); stubs are ARM `mvn r0,#0; bx lr` placeholders, patched by the loader. Function calls are `blx` to the stub. (asm)
Our script `Work/vita/unself.py` writes a flat ELF (`update.elf`); `Work/vita/imports.py` lists the imports. **No NID database was
available**: function names below are inferred from call shape, argument constants and the Pro equivalents - NIDs are exact, names tagged
(inferred).

Network-related libraries (function count, caller wrappers):
| library | funcs | main users | Pro counterpart |
|---|---|---|---|
| SceNpMatching2 | 21 | DcNetMatching 0x81388cde..0x8138b000 | libSceNpMatching2 (20) - same set + 1 |
| SceLibRudp | 17 | RudpMgr 0x8138eabe..0x8138f5f6 | libSceRudp (17), identical set |
| SceNet | 9 | RudpMgr socket (f8138eabe), init f81395140 | libSceNet (same calls) |
| SceNetCtl | 12 | 0x81397062..0x81397780 | NetCtl state 3 |
| SceNpManager | 9 | sign-in 0x8138f8e6, ctx create | NpManager (GetNpId; no CheckPlus on Vita) |
| SceNpBasic | 17 | friends/presence/messages 0x8138f6fa..0x81391056 | NpToolkit2 friends/presence/invite + InGameMessage |
| SceNpCommon / SceNpUtility / SceNpTus | 5 / 10 / 10 | 0x81390688 / 0x8138ffd4 / 0x8138fa14,0x81563394 | NpCommon / WordFilter / Tus |
| SceNetAdhocMatching | 10 | 0x813977dc..0x81397ae8 | **absent on Pro** (Vita ad-hoc local mode) |
| SceHttp / SceSsl | 2 / 2 | init only | init only |
There is **no SceNpSignaling / SceNpScore / SceNpCommerce**: signaling is the Matching2 signaling API (as on Pro).

Matching2 NID -> role (inferred; NIDs asm-exact):
| NID | role | evidence |
|---|---|---|
| ebb1fe74 | Initialize(poolSize 0x20000, prio 0x10000100, affinity 0, stack 0x4000) | f81395140 step |
| 0124641c | Terminate | f813952c6 step |
| adf578e1 | CreateContext(npId*, commId*, passphrase*, &ctxId u16 -> this+0x1d8) | f81388de2 |
| f9e35566 | RegisterContextCallback (cb 0x81387767) | f81388de2 |
| bb2e7559 / 506454de / 368aa759 | ContextStart / ContextStop / DestroyContext | f81388f00 / f81388fc0 / f81388e66 |
| 65c0feed | GetServerId (-> this+0x1da) | f813890fc |
| c086b560 | GetWorldInfoList (cb 0x81387e17) | f813891fe |
| f049bec1 | SetUserInfo (bin attr 0x5f, 1 byte) | f813893fc |
| 78521326 | second call with bin attr 0x5f and caller data (the 21st import; role unverified, maybe member/user data) | f8138a32a |
| 818a9499 | CreateJoinRoom (maxSlot 4, flagAttr 0x86000000, worldId this+0x224, 8 int attrs, bin 0x55 / 0x57, up to 100 NpIds) | f81389d72 |
| 33f7d5ae | JoinRoom(roomId u64, up to 100 NpIds) | f8138a540 |
| d48baf13 | SearchRoom (start 1, max 0x14, flagAttr 0x64000000, attr ids 0x4c 0x54 0x55) | f8138a09e |
| c8b0c9ee | LeaveRoom (roomId at this+0x290) | f8138a8fa |
| 8f88ac7e / 94784a8c | SetRoomDataExternal (8 int attrs + bins) / SetRoomDataInternal (flag 0x54000000 close, 0x04000000 reopen) | f8138aa7e / f8138abae |
| f486991b | RegisterRoomEventCallback (cb thunk 0x81388cc5 -> f81388b1e) | f81389b80 |
| 1c60bc5b | RegisterSignalingCallback (cb thunk 0x81388da5 -> f81388cde) | f81389b80 |
| 79310806 | SignalingGetLocalNetInfo (struct size field = 0x10; Pro 0x18); result stored at this+0x680..0x688, **no reader in the matching class** | f81389b80 |
| 4e4c55bd | SignalingGetConnectionStatus (in the signaling handler) | f81388cde |

## 3. Q1 - Transport: same as Pro (asm)
- Socket: `sceNetSocket("RUDP_NetId", AF_INET 2, type 6 = P2P datagram, 0)` (f8138eabe, 0x8138eae4). Setsockopt level 0xffff opts
  0x1000, 0x2000, 0x1100 (value 1; Pro uses 0x20000, 0x40000, 0x1200 - platform constants). Bind sockaddr {len 0x10, family 2,
  **port htons(3658 = 0xe4a)**, addr 0, **vport htons(4649 = 0x1229)** at +8}. Same numbers as Pro.
- Rudp: `Init(pool 0x818b7840, 0x100000)`, `GetStatus(buf, 0xf8)`, `EnableInternalIOThread(stack 0x2000, prio 0xbf)` (Pro prio 0x1f4),
  `SetEventHandler(0x8138eced)`, `PollCreate(0x80)` (f8138eef6). Per slot: `CreateContext`, `Bind(ctx, sock, **vport 20 (0x14), mux 1**)`,
  `SetOption(2 send buf, 0x40000)`, `SetOption(3 recv buf, 0x40000)`, `Initiate(ctx, sockaddr{len 0x10, fam 2, peer port at +2, ip at +4,
  vport 4649 at +8})`, `PollControl(add, ctx, events 0xb)` (f8138ef7c). **Max 4 slots**, slot record stride 0x20 (Pro 0x28): +0x10 ctx id,
  +0x14 state (0/1 connecting/2 connected), +0x18 send-pending, +0x1c ptr, +0x20 len, +0x2c tick.
- Pump f8138f0da (every tick, mutex): `PollWait(poll, ev, 0x80, 0)`; READ -> `Read(ctx, buf 0x819f7840 + slot*0x40000, 0x40000, 1)` until
  0x80770016 / 0x80770024, each result = **one whole DcNet packet** -> `DcNet::OnPacket(slot, buf, len)` f813781c0; WRITE -> state 1:
  `GetOption(ctx, 0xe)` then `OnPeerConnected(slot)` f81378046; state 2: `Write(ctx, ptr, len, 1)`; error 0x80770007/16 retry; ev&8 closes.
  Event handler 0x8138ecec: event 2 closes all slots. **Max message 256 KB (0x40000); one outstanding send per peer.** All identical to Pro 4.1.
- Peer connect is triggered from the signaling handler f81388cde (event -> `SignalingGetConnectionStatus` -> `RudpMgr_Connect(slot, ip, port)`),
  as on Pro. Max room members 4.
- The same DcNetMatching drives an **ad-hoc transport** (`SceNetAdhocMatching`, 10 imports) selected by a mode test (f81388500); the DcNet layer
  above is shared. Irrelevant for Pro.

## 4. Q2 - DcNet packet layer: header identical, one extra type (asm)
Header **16 bytes, little endian**, identical to Pro net.md 4.2: `+0 u32 time ms` (sceKernelGetProcessTime / 1000, f813a8a12; Pro FUN_005714b0),
`+4 u32 byte counter` (peer record +0x56c, f813763a4 / f813763ec fill it, then reset), `+8 u32 CRC`, `+0xc u16` (payload length for type 0), `+0xe u8 type`.
CRC: f813ab8a8(buf, len, init -1, final xor -1, table idx **1**) over the whole packet with +8 zeroed; table array at 0x81880b4c; idx 1 = standard
CRC-32 (first entries 0x77073096, 0xee0e612c). Pro uses the same four tables in the same order (checked in its eboot). Mismatch -> error -4.
Receive checks (f813781c0): not in "closed" state, slot < 4, length in [0x10, 0x40000) else error -3, CRC, then peer receive-accounting (-2 on underflow).
Empty `bx lr` hooks f8138740c / f8138740e are called around the CRC (Pro: empty `ret` FUN_000a4490): no endian conversion.

| type | Vita handler | min size | role | Pro |
|---|---|---|---|---|
| 0 | inline 0x8137838e | >0x10 | game data stream: append payload to double-buffered stream (peer+0x50.., index byte at net+0x1670, 0x4c stride) | same |
| 1 | nop | 0x10 | alive notify | same |
| 2 | f81377056 | 0x38 (0x28 payload) | member/ready table | same |
| 3 | f81377322 | 0x38 | member table broadcast (copies 0x28 B to net+0xf0) | same |
| 4 | inline 0x813782e4 (loops the 4 peers, sets a flag bit via f8137602e) | 0x10 | alive request (role from Pro table; handler shape differs from Pro's FUN_0009ba40, unverified) | present |
| 5 | inline 0x81378328 | 0x18 (8 B payload) | small control word, compared with host ctrl id (net+0x180) | same |
| **6** (ad-hoc only, 12.6) | **f8137748e** | **0x340 (payload 0x330)** | **copies a member/profile block into DcNetMatching+0x280..0x3b0 (u16, u32, 16 B, 2 bytes, two 0x24-byte SceNpId-sized blobs at packet +0x2a and +0xa2, further ints/shorts); requires f81388500 true else error -3** | **absent (Pro's table stops at 5; unknown types are ignored)** |
Per-peer templates (stride 0x538, Pro 0x2d8) initialised in OnPeerConnected f81378046: type bytes 1 at peer+0x1a2, 4 at +0x1b2, 2 at +0x1c2, 3 at
+0x1fa (offsets relative to Pro's identical +0x20 / +0x38 spacing); type 6 template at peer+0x228 (0x340 B, sender f813769da); recv buffers 2 x 0x40000
+ 2 x 0x2000. Alive request every 60 ticks like Pro (not re-verified).

## 5. Q3 - Version, room attributes, room info
- **Protocol version constant: f813755c2 returns 0x21 (asm)**; Pro FUN_00098f50 returns 0x22. Callers of the Vita getter: SearchRoom f8138a09e (bits 13:7 of the packed attr),
  search-condition builder f8138dec4 (slot 12 = version, slot 11 = bits 6:5 from f813755c6), presence builder f81390594, and UI functions 0x8102e478,
  0x813042dc, 0x813046ee. Packed int attr **0x53 uses the same bit-field insert sequence as Pro 3.4** (f81389bfa builds it from the room info, f8138a09e from live state): 2 bits [1:0], 3 bits [4:2] (value 99 -> 5), 2 bits [6:5], **7 bits [13:7] = version**, 5 bits [18:14] (f81375590, default 2), 2 bits [20:19]. Int attrs 0x4c..0x52 = seven signed values from the info struct (14 shorts at struct 0x40.. stored as 8 words at this+0x574), 0x53 packed.
- Attribute ids identical: int attrs 0x4c..0x53, bin attr 0x55 (external, length <= 0x100, Pro <= 0x100), 0x57 (internal, Vita length < 0x100, Pro <= 0xff), user bin 0x5f.
  CreateJoinRoom: maxSlot 4, flagAttr 0x86000000, worldId from this+0x224 (Pro matching+0xd0). SearchRoom flagAttr 0x64000000. Close/reopen via SetRoomDataInternal 0x54000000 / 0x04000000.
- **Room info: Vita 0x62 B (UpdatedRoomDataInternal 0x1106 handler f81388b1e checks `size == 0x62`, create/join response f81388706 as well); Pro 0x78 B (asm).**
  Wire layout (writer f813754c8, reader f8137552c; Pro writer FUN_00099110, reader FUN_00098fb0), little endian, explicit widths:
  | wire | field | Vita | Pro |
  |---|---|---|---|
  | 8 B | header (4 x u16 / 8 x u8) | yes | yes |
  | 48 B | 8 x (4 x u8 + u16) member entries (struct offset 0x08, area to 0x40) | yes | yes |
  | 30 B | 15 x s16 at struct 0x40..0x5e (seven pairs + 0x5c) | yes | yes |
  | 2 B | u16 at 0x5e | yes | yes |
  | 2 B | u8 at 0x60, u8 at 0x61 | yes | yes |
  | +22 B | u8 at 0x62, 0x63, 0x64; bool 0x65; u16 at 0x66; 8 B at 0x68; 8 B at 0x70 (probably two account ids, unverified) | **no** | **yes** |
  Wire sum Vita 90 B + 8 B (container, unidentified) = 0x62; Pro 112 + 8 = 0x78. The 8 B container overhead appears on both (origin not located).
  A translator pads (Vita -> Pro) with 22 zero bytes or truncates (Pro -> Vita). Pro's meaning for the tail fields is unknown (zero may be fine).
- Create/join response: stores roomId (this+0x290), own member (+0x280), owner NpId copies (+0x29a, +0x312 : 0x24 B each), room ints at +0x574 (attr 0x4c..0x53 as 8 words).
- **Progression gate on join (asm)**: after JoinRoom success, f8138a540 compares the save value at `save + 0xaf1e4` (via f8136d2e2) with the room's attr 0x4c (this+0x574); if the room value is
  lower than the local one the join is rejected (error code -8 stored at this+0x6c0, room left). The Pro does the same with the same save offset 0xaf1e4 (net.md 3.5), so
  the two saves share this field.
- Error codes: Matching2 0x80550d19 / d15 / d0f are mapped to -8 / -7 / -6 (Pro: -100 generic, -6); anything else -100 (0xffffff9c).

## 6. Q4 - memberId to slot (asm)
`slot = (memberId >> 4) - 1`, valid only 0..3 else 0xff: `ubfx r,r,#4,#12; subs #1; cmp #4` in the signaling handler (0x81388d38), room-event handler (0x81388b60),
create/join response (0x81388802), owner/me lookups (0x81388bea). Identical to Pro (net.md 5.3). Member records are 0x78 B (Pro 0x70) at this+0x38c, stride 0x78.

## 7. Q5 - Serialization layer
- **Stream class** (reader/writer in one object; error flag at +0x48; virtual `Reserve(n)` at vtable+0xc; write pointer +0x10, ring base +0x14, bytes written +0x3c):
  primitives are **explicit width, written byte by byte (low byte first = little endian)**, no `rev`/`revsh` instruction exists anywhere in the Vita code range
  0x81000000..0x8156b000 (scan: 0 hits; the hits above 0x8156b000 are data). So there is **no endian swapping**; raw struct blocks go out by `memcpy` (f8137f69c).
  Primitive table (write side, Vita -> Pro): u8 f8137f6ec / f7b8 / face -> a4500..a4b00; s16 f8137f808, u16 f872 -> a4620, a46f0; u32 f8da, f94a, f9ba -> a47c0, a4890, a4960;
  2xu32 fb34 -> a4bd0; 3xu32 fc00 -> a4d60; 4xu32 (f8138149a) -> a7fb0; raw copy f69c -> a4240; u64 fa2a (0 callers) -> a4a30 (1 caller);
  **`long`-like int: f81383b14 (4 bytes, 28 call sites, reader f813840a8) -> Pro FUN_000acb90 (movsxd, writes 8 bytes, 22 callers)**. All other primitive widths match.
- Callers of the primitives: **432 Vita functions (381 of them are vtable entries) vs 404 Pro functions** (write side, counted over all primitive writers).
  Pairs of write/read virtuals sit next to each other (e.g. 0x8152a488 / 0x8152a52c). The **136 distinct `DC_*CMD_*` command-name strings; the set is identical to Pro's** (name-by-name comparison of both images, no difference either way).
- **Command header (asm):** f8137adb8 writes a raw **0x14-byte** header (16 B from cmd+4 and a u32 from cmd+0x14 = cmd+4..+0x18), then virtual Serialize(stream, ver), then commit (f8138745a);
  reader f8137ae30 reads 0x14 raw. Pro FUN_0009d5e0: identical (16 B from cmd+8 + u32 from cmd+0x18; offsets +4 because the vptr is 8 bytes).
- **Scene-setup player dump (asm):** Vita f812be46c (writer; reader f812be514). Per player 0..3 (valid if slot id at +0x2258 != -1): `u32 index`, **raw 0x21d0 bytes from player+0x7c**, `u32 0` placeholder,
  then the custom per-player serialization f811f316c(obj, stream) (Pro FUN_002f7ff0), then the placeholder is patched with the written length. **Player stride 0x2230 (Pro 0x23b0), block size 0x21d0 = identical to Pro.**
  Same size suggests identical POD layout (no pointers); **now checked in 12.5: it is the persistent hero record, layout identical, no pointers inside.**
- **Field-by-field comparison** (Vita offset = Pro offset - 4 everywhere, vptr 4 vs 8 B):
  1. `DC_SCENEMANCMD_SETUP_SCENE` (name fn 0x8152a45c, write 0x8152a488, read 0x8152a52c; Pro 4403d0 / FUN_004403e0): 2 x raw 0x40 B, u32, u32, 2xu32 (+0xac / +0xb0), u8, u8, **`long` (Vita 4 B at +0xb8, Pro 8 B at +0xbc)**, u8, 4xu32, u8, u32 x4. All other fields identical in order and width.
  2. `DC_GAMESERVERCMD_RESPAWN_REQUEST` (Vita 0x8142fe04, Pro FUN_000359a0): u32, **long (4 vs 8 B)**, u8. Wire size 9 vs 13 B.
  3. `DC_GAMESERVERCMD_CREATEDROPITEM_REQUEST` (Vita 0x8142f9fc, Pro FUN_00035570), top-level fields only: composite object writer (Vita f81383b64 / Pro FUN_000ac3d0), vec3, 2xu32, vec3, u32, s16, u8, s16, u8, float, u32, u8, u8 - identical sequence and widths (the nested composite was not compared).
  Automatic signature matching of all classes was attempted and rejected as unreliable (the leaf name fn has no prologue, so the next-function heuristic picked helpers); **superseded by the full comparison in section 12** (vtable slot 7/9/10, all 136 classes). Note: the 'name fn 0x8152a45c' above belongs to START_SETUP_SCENE; SETUP_SCENE's is 0x8152a47c (write 0x8152a488 / read 0x8152a52c are right).
- Raw copies: the 0x21d0 player block and 0x40-byte blocks. They are native-endian memcpy of POD; Vita/Pro are both little endian. **Original PS3 (big endian) cross play cannot be explained by this code**: no swap logic exists in the Vita stream layer
  (inferred: either the PS3 build swaps on its side, or cross play was not through these raw blocks; unverified).

## 8. Q6 - Other gates that could stop a Vita peer joining a Pro room
1. Version attr 0x53 EQUAL (0x21 vs 0x22): hard filter in SearchRoom on both; the lobby could alias, but then the peer's own `size == 0x62/0x78` checks and the real payloads still matter.
2. Room-info size check (0x62 vs 0x78) in the room-event handler: a mismatched blob is silently dropped (Vita f81388b1e skips; Pro rejects), so room state stays default.
3. Join progression gate (save+0xaf1e4 vs room attr 0x4c): same on both; applies per local save.
4. DcNet type 6: **resolved, not a gate** - only the ad-hoc matching callback ever sets the send bit and the receiver refuses it outside ad-hoc mode (12.6).
5. Member identity: Vita copies SceNpId from the member records (`+0x29a`, `+0x312`) and sends it in type 6; a Pro-origin member has no 0x24-byte NpId (Pro has account id + online id). The lobby would need to synthesize NpIds.
6. Region/title: Vita uses the NP communication id (global at core+0x116f88) in CreateContext; irrelevant on our own lobby. No PS Plus check on Vita (no `CheckPlus` import). No NAT-type gate (net info stored, never read here).
7. The 4 wire-incompatible `long` fields (section 7) corrupt any command stream containing them (no framing resync: stream bytes are positional).
No content checksum, version string or title-id compare against the executable was found in the matching or DcNet code (not exhaustively searched outside 0x81375000..0x813a0000).

## 9. Q7 - Verdict detail, unknowns, runtime trace
**Medium.** Work items for a translator: version attr; room-info pad/truncate; type-6 policy; `long` width (patching Pro's primitive pair FUN_000acb90 + its reader to 4 bytes removes the stream rewrite; otherwise a per-class table for the ~22-28 `long` classes);
NpId <-> account-id mapping. Thick is excluded: same sync model (host-authoritative state replication by serialized command objects, same command names, same header, no frame/input lockstep numbering in the packet header).

Could not determine (static):
1. ~~Whether the other ~130 serialize pairs match Pro field by field~~ **done, section 12** (99 identical / 33 long-only / 4 layout).
2. Pro's semantic use of the 22 extra room-info bytes and the 8 B container overhead of the info blob.
3. ~~Type 6~~ ad-hoc only (12.6); Pro ignores unknown types (switch bound 5, asm).
4. ~~Raw 0x21d0 block / nested composite writers~~ equal (12.5; the composites compare token-identical). Still open: the per-player virtual hook in the scene-setup dump (Vita +0x634, Pro +0xc98).
5. Differences from game logic versions (Vita 1.06 vs Pro build): stage/quest ids, items, enemy ids shared between the two? Only verifiable at runtime.
6. How the Vita reaches our lobby (plugin vs emulator) and thread/timing behaviour of Vita Rudp vs our carrier.
7. Alive/heartbeat timing details and the stream flush size limit per packet on Vita (not read).
8. PS3 big-endian cross-play story (section 7).

A runtime trace would need to show: (a) packet captures (DcNet level, after Rudp) from a Vita<->Vita session (or Vita3K with a loopback shim) and Pro<->Pro for the same scenario (town, quest start, a few seconds in-quest): per type counts, sizes, command ids seen, to confirm classes and `long` fields;
(b) the 4 player 0x21d0-byte blocks at scene setup on both builds from the same save, to diff offsets and values; (c) whether Vita sends type 6 at join and when (host/client), and its contents; (d) the room-info blob as published on create (0x62 vs 0x78) and the tail bytes on Pro;
(e) a Pro joining with a patched 4-byte `long` and padded info against a Vita host: first failing command.

## 10. Function table (Vita VMA; Pro in brackets where matched)
| addr | proposed name | role | conf |
|---|---|---|---|
| 0x8138ed76 | RudpMgr_ctor | slots init (4 x 0x20), mutex "FnMutex" | high |
| 0x8138eabe | RudpMgr_OpenSocket(this, flag) | "RUDP_NetId" socket, bind 3658 / vport 4649 [bd4c0] | high |
| 0x8138ec0c | RudpMgr_CloseSlot | [bd400] | high |
| 0x8138ee96 | RudpMgr_End | [bd730] | high |
| 0x8138eef6 | RudpMgr_Init | [bd600] | high |
| 0x8138ef7c | RudpMgr_Connect(slot, ip, port) | [bd8b0] | high |
| 0x8138f0da | RudpMgr_Pump | [bdae0] | high |
| 0x8138ecec | RudpMgr_EventHandler | event 2 = close all [bd240] | med |
| 0x8138f578 | RudpMgr_Send(slot, buf, len) | called from 0x8138e860 [be170] | med (body not read) |
| 0x813781c0 | DcNet_OnPacket | [9bd80] | high |
| 0x81378046 | DcNet_OnPeerConnected | [9bbb0] | high |
| 0x81376f56 | DcNet_RxError(slot, code) | [9b8f0] | med |
| 0x813763a4 / 0x813763ec | DcNet_SendPacket (header+CRC; second variant sends) | [9ae00] | high |
| 0x813ab8a8 | Crc32(buf,len,init,final,table) | [572340] | high |
| 0x813a8a12 | TimeMs | [5714b0] | high |
| 0x81377056 / 0x81377322 / 0x8137748e | RxType2 / RxType3 / RxType6 | [9b560] / inline / none | high / high / med |
| 0x813769da | SendType6 (template peer+0x228, 0x340 B) | none | med |
| 0x8137f69c | NetStream_WriteRaw | [a4240] | high |
| 0x8137f760 | NetStream_ReadRaw(flag) | | med |
| 0x8137f8da / 0x8137face / 0x8137f808 | write u32 / u8 / s16 | [a47c0 / a4b00 / a4620] | high |
| 0x81383b14 / 0x813840a8 | write / read `long` (4 B) | [acb90] 8 B | high |
| 0x8137adb8 / 0x8137ae30 | NetCtrl_WriteCommand / ReadCommand (0x14 header) | [9d5e0] | high |
| 0x812be46c / 0x812be514 | SceneSetup player dump write / read | [43b9d0] | high |
| 0x8152a488 / 0x8152a52c | SETUP_SCENE serialize write / read | [4403e0] | high |
| 0x8142fe04 | RESPAWN_REQUEST serialize | [359a0] | high |
| 0x81388b1e | Matching_OnRoomEvent (events 0x1101..0x1108) | [b5ee0] | high |
| 0x81388cde | Matching_OnSignalingEvent | [b6350] | high |
| 0x81388706 | Matching_OnCreateJoinResponse | [b5a90 area] | med |
| 0x81389d72 | Matching_CreateJoinRoom | [b8a10] | high |
| 0x8138a540 | Matching_JoinRoom (with progression gate) | [b9890] | high |
| 0x8138a09e | Matching_SearchRoom | [b8fd0] | high |
| 0x81389bfa | Matching_BuildRoomAttrs | [bba60] | high |
| 0x8138dec4 | Matching_BuildSearchConditions | | med |
| 0x81388de2 / 0x81388e66 | CreateContext / DestroyContext steps | [bb2e0 / b5350] | med |
| 0x81388f00 / 0x81388fc0 / 0x813890fc / 0x813891fe / 0x813893fc | ContextStart / ContextStop / GetServerId / GetWorldInfoList / SetUserInfo steps | [bb3e0 / bb940 / b87c0 / bb4f0 / bb770] | high |
| 0x8138a8fa / 0x8138aa7e / 0x8138abae | LeaveRoom / SetRoomDataExternal / SetRoomDataInternal steps | [b7fb0 / bc020 / bbda0] | high |
| 0x8138a32a | (member/user bin 0x5f variant) | none | low |
| 0x81389b80 | register room / signaling callbacks + LocalNetInfo | | high |
| 0x813755c2 | ProtocolVersion (=0x21) | [98f50] (=0x22) | high |
| 0x81375590 / 0x813755c6 | RoomInfo_SearchFieldB / SearchFieldA | [98f60 / 98f00] | med |
| 0x813754c8 / 0x8137552c / 0x813756a2 | RoomInfo_Write / Read / ctor | [99110 / 98fb0 / 98da0] | high |
| 0x81395140 / 0x813952c6 | NpHelper init step / term step (Net, NetCtl, NpManager, Matching2, NpCommon, NpBasic, Ssl, Http, NpUtility) | [59b160 / 59b360] | high |
About 60 functions are listed (plus the 404 import stubs in `Work/vita/imports.tsv`). Matching2 / Rudp / Net import stubs sit at 0x8156b3a0..0x8156b800 and 0x8156c270..0x8156c4d0.

## 11. Data structures
- Packet header (16 B): see section 4. Per-peer record stride 0x538: +0x194 type-0 header template, +0x1a2.. templates (types 1,4,2,3,6), +0x56c byte counter, +0x568 receive budget, +0x578 received total, +0x6c4 flag word, +0x6b8/+0x6bc 0x2000-B buffers, +0x618/+0x61c 0x40000-B buffers (two-buffer stream, index byte net+0x1670, record stride 0x4c, length u16 at +0x84).
- DcNetMatching (this): +0x1d8 ctxId u16, +0x1da serverId u16, +0x224 worldId, +0x274/+0x278 world list begin/end (0x12c stride), +0x280 0x330-byte room/member block (type 6 target), +0x290 roomId u64, +0x38c member records (stride 0x78), +0x54/+0x60/+0x65 request id / result / done, +0x574..0x590 room ints (attrs 0x4c..0x53), +0x5c0 RudpMgr, +0x6c0 error, +0x78c mutex, +0x1a4/+0x1a8/+0x1ac state bit words (bit table at 0x81880ab8).
- RudpMgr (+0x5c0 in DcNetMatching): +0 vptr, +4 poll id, +8 socket, +0xc inited, +0x10 slots[4] x 0x20, +0x90 mutex, +0x94 flag.

## 12. Command-by-command comparison (all 136 `DC_*CMD_*` classes, both builds)
Tags as above. Scratch tooling: `Work/vita/ext.py` (tokenizer for both ISAs), `run_all.py` (name -> vtable), `compare.py`, `rld.py` (diff of two
serializers), `ids.py`. Everything here is static; nothing was run.

### 12.1 Method (what changed vs section 7)
- **Locating Serialize/Deserialize (asm).** The command name string is referenced by a tiny *leaf* "name" virtual (load string address, return). Vita leaf functions
  have no `push {..lr}`, so the earlier "next function start" lookup attributed the xref to the preceding function - that was the reason the signature match was
  unreliable (e.g. `0x8152a45c` is the name fn of START_SETUP_SCENE; SETUP_SCENE's is `0x8152a47c`). Correct chain:
  string xref -> leaf name fn -> its vtable slot **7** -> **slot 9 = Serialize (write)**, **slot 10 = Deserialize (read)**, same slot numbers on both builds
  (Vita: 4-byte slots, thumb bit set, vtable data in segment 0 rodata, e.g. SETUP_SCENE vtable 0x8179d1c8, name slot 0x8179d1e4;
  Pro: 8-byte slots filled by **R_X86_64_RELATIVE** relocations, not present in the file image: the RELA is dyn tag 0x6100002f (+0x8040, size 0x8abfa0 from the
  dynlib data base 0xc1a660; 378 779 relative entries), e.g. SETUP_SCENE vptr 0xb8d168, name slot 0xb8d1a0, ser 0xb8d1b0, deser 0xb8d1b8).
  **136/136 classes resolved on both sides**, one slot each, no ambiguity. (One Pro name string, `DC_TASKCMD_SYNC_ACTSTEP`, has no code xref in the
  string index; found by a raw rip-relative scan at 0x6a810 -> slot 0x8c5de8.)
- **Wire tokenizer (inferred from asm, run on every Serialize).** Walks the body in address order, recursing into direct callees and tail calls. A token is
  a stream `Reserve(n)` call (Vita: `movs r1,#n` then `blx r2` through stream vtable+0xc; Pro: `mov esi,n` then `call [reg+0x18]`) or a raw copy (Vita 0x8137f69c, Pro 0xa4240).
  Constant-trip loops are expanded (Vita: `cmp rN,#k` or countdown `subs`; Pro: `cmp reg,k` minus init/step of the counter), count-driven loops stay a group.
  The stream is plain little-endian bytes, so adjacent fixed fields are coalesced (u16+u16 == u32 on the wire). Raw blocks >= 16 B stay `Bn`.
  Limits: branches are merged linearly (a conditional field counts once per code path), virtual calls are not followed; one class needed a manual path
  check (RECREATE_SHOT, 12.6). Classification is on the **Serialize** side; the **Deserialize** side was machine-compared as a cross-check where the
  extractor can read it: **117 of 136 match on both sides** (95 identical + 22 long-only); 14 Deserialize bodies inline their reads in a way the extractor
  cannot read (their Serialize side is clean); 5 are the layout cases (4) and RECREATE_SHOT.
- **Wide primitives (asm).** Pro has not one but **22 writer/reader pairs** that put 8 bytes on the wire for a 4-byte in-memory value: `FUN_000acb90`/`ad440` (`long`,
  `movsxd` sign-extend; the one found earlier), `0xb0410`/`0xb1c00` and 20 enum-typed twins `0xb35f0..0xb4560` + readers (zero-extend `mov r14d,[rsi]` or
  `movsxd`). Their Vita twins write 4 bytes (`0x81383b14` `long`, `0x81386b4c`, ... not enumerated). So the width difference is a *family* of 44 Pro
  functions, not a single primitive. All other primitive widths (u8/u16/u32/3xu32/4xu32/raw) are identical. The only genuine 64-bit primitive (Pro `a4a30`,
  1 caller) appears in no command serializer.

### 12.2 Counts
| class | count | meaning |
|---|---|---|
| IDENTICAL | **99** | same wire bytes (26 of them carry no payload: both builds use one shared empty `ret` stub for Serialize/Deserialize, header only) |
| LONG-ONLY | **33** | identical except the wide fields: 4 B on Vita, 8 B on Pro; 1..9 such fields per command (column W) |
| LAYOUT | **4** | Pro appends fields (12.3) |
| RAW | **0** | only SETUP_SCENE contains raw blocks (2 x 0x40 B), same size on both; no raw struct differs |
| UNRESOLVED | **0** | all 136 found on both sides; residual caveats in 12.5 / 12.6 |
By family: GAMESERVERCMD 82 (62 identical, 18 long-only, 2 layout), SCENEMANCMD 27 (16 / 9 / 2), CAMPCMD 13 (7 / 6 / 0), BUFFCMD 5, NETCTRLCMD 5, AICMD 3, TASKCMD 1 (all identical).

### 12.3 LAYOUT commands (Pro = Vita + additions; asm via `rld.py` run-length diff, everything else token-identical)
| command | Vita -> Pro difference |
|---|---|
| GAMESERVERCMD_CREATEPLAYER_REQUEST / _NOTIFY | Pro writes, right after the hero record, an extra **0x16a = 362-byte table** (181 x u16, `FUN_000b3390`) and appends **17 more u8** at the end (Pro object +0x23a4..0x23b4; Vita ends 2 u8 after the last u32 group). Plus 4 wide fields. Fixed part 8714 B (Vita) vs 9093 B (Pro) |
| SCENEMANCMD_CREATE_PLAYER | same two additions (+362 B, +17 u8) plus 8-9 wide fields |
| SCENEMANCMD_EXCHANGE_BONEINFO | Pro writes the 362-byte extra table after **each** of the two hero records (Vita: two hero records back to back at cmd+0x28 and +0x21f8, no extra table). +724 B. About 17308 B (Vita) vs 18032 B (Pro) |

The 362-byte "extra" record is the per-hero 0x16a table that Pro's save layout also has (bones.md 3.1); the Vita image uses the same 0x16a and 0xc620 constants in
its save code, so the data exists on Vita but is not put on the wire (inferred). Vita -> Pro: insert 362 zero bytes (and 17 zero u8); Pro -> Vita: drop them.
Whether zeros are harmless for Pro is **unverified** (meaning of the extra table undecoded).

### 12.4 LONG-ONLY handling
Cheapest: in our Pro port make the 44 wide primitives (22 pairs, 12.1) 4 bytes. Then all 33 LONG-ONLY commands and the wide fields of the 4 LAYOUT commands
become byte-identical to Vita with no stream parsing. Reading direction: Vita sends 4 B; the Pro readers already store only the low dword (`dword ptr [r14]`), so
truncation/extension is not a semantic problem (inferred). An out-of-band translator would instead need a per-command table of wide-field offsets (column W gives
the count; positions follow from the Vita sequence and the Pro field list) - larger and unnecessary.

### 12.5 Raw 0x21d0 player block (scene setup, Pro `FUN_0043b9d0`, Vita `0x812be46c`)
- **What it is:** one persistent **hero record** (bones.md 3.1 / story_progress.md 4: hero array stride 0x21d0 at save slot+0xa480). Vita uses the same stride and the same
  slot constants (movw sites for 0xa480, 0x21d0, 0xaf8d4, 0xaf1e4 in its save code) and the same field accessors at the same record offsets (asm), e.g. last-visited stage u16 at
  +0x21ca (0xffff = none): Vita 0x8136912a vs Pro 0x83b10; loop index byte at +0x21a7, clamp 1..9: Vita 0x81368984 vs Pro 0x82ef0; uid at +0x1f6c; party bytes at +0x21c4..0x21c8.
- **Layout check (asm):** the field-wise hero-record serializer used by CREATEPLAYER / EXCHANGE_BONEINFO (Vita `0x813828c4`, Pro `FUN_000a9a70`) is token-for-token identical: u32, u16,
  210 u8 in two arrays, ints/shorts, name / alternate-name blocks at +0x138 / +0x140 / +0x148, nine equipment sets at +0x178 + k*0x354 (852 wire bytes each), uid +0x1f6c, eight arrays of 30 u16 from +0x1f70
  (stride 0x3c), 0x56 skill bytes at +0x2150, the 5-byte group at +0x21c4, then +0x21c9 / +0x21ca. The tool diff left only the trip of the 5-byte loop, confirmed by hand (`cmp.w sb,#5`, Vita 0x81383368).
  The landmark offsets appear verbatim in the Pro function (0x138 .. 0x21ca).
- **Pointers inside the copied range: none found.** Both builds keep a **pointer + handle just after the block**: Pro ptr at block+0x21d0 (8 B), id at +0x21d8 (`FUN_00026f90`);
  Vita ptr at +0x21d0 (4 B), id at +0x21d4 (`0x810274c6`); then the slot id (Vita block+0x21dc, Pro block+0x21e0). The 4-byte growth comes exactly from that pointer, outside the 0x21d0 bytes.
  The slot stride differs (0x2230 vs 0x23b0) and the block starts at slot+0x7c (Vita) vs slot+0xa8 (Pro) - irrelevant on the wire.
  Verdict: **same layout, raw copy is safe** (inferred from save-format identity plus serializer identity; not diffed on real data).
- **Not resolved (open):** the per-player extension that follows the block in the scene-setup dump (Vita `0x811f316c`, Pro `FUN_002f7ff0`). Skeleton identical
  (sub-struct writers `0x813849ca`/`0xae3b0`, `0x8115ab7c`/`0x225030`, vector writers `0x8100d5d8`/`0x8ce0`, tail u8s; the Pro vector writer's extra trailing 4 is the empty-count path), but it calls a
  per-class **virtual hook** (Vita vtable +0x634, Pro +0xc98; base impls `0x811a5e6e` / `0x2990e0`, shared by ~330 vtables per build, about a dozen overrides on each, none compared) and a
  size virtual (+0x4c / +0xd0). This is the one place where the static comparison stops at a virtual call. It belongs to the scene-setup dump/restore pair
  (Pro `FUN_0043b9d0` / `FUN_0043b800` into a buffer at scene+0x9450); **none of the 136 command serializers has an unresolved virtual call.**

### 12.6 Other findings
- **Packet type 6 (asm + inferred): ad-hoc mode only.** Sender `0x813769da` is reached only from the per-peer send scheduler `0x81376c2a`, when pending-bit 6 of the peer flag word
  (+0x6c4; mask table at 0x81880ab8, entry 6) is set. The only code that sets bit 6 is `0x81378e12` (the orr is at 0x81379094), which fills the 0x340-byte template (peer+0x228) from a
  member/profile structure (two 0x24-byte SceNpId blobs + member records). Its only caller is `0x81379190`, reached through the thunk `0x81379416`, whose address is passed (at 0x81379c8a) to the wrapper
  `0x8139790e` that uses the `sceNetAdhocMatching` imports: an **ad-hoc matching event callback** (switch over 10 events). The receiver `0x8137748e` rejects a type-6 packet with
  error -3 unless the ad-hoc mode test `0x81388500` is true, and stores the 4-member roster (0x78-byte records with SceNpIds) into DcNetMatching+0x280... In infrastructure mode (Matching2 + Rudp), which
  is what Pro emulates, the bit is never set and a received type 6 is refused. **Pro has nothing to fake or drop**: it never has to send type 6 and its receive switch already ignores types > 5.
  (Unverified at runtime that a Vita in Matching2 mode never takes the ad-hoc path.)
- **Is the stream the only gameplay channel? Yes as far as imports and DcNet go (asm/inferred):** the Matching2 import set (section 2) has no room-message send; DcNet packet types are
  0 (stream), 1/4 alive, 2/3 member table, 5 control word, 6 ad-hoc profile. NpBasic messages are invites only.
- **Command ids (asm):** the id is the second argument of the command base constructor (Vita `0x8137ad42(this,id)`, Pro `FUN_0009d570(this,id)`) or a header u16 store next to it; extracted at the
  vtable-store sites: **128 of 136 classes share an id on both builds**; the SCENEMANCMD_CREATE_* / RECREATE_SHOT ids (set later in the header) line up with the Pro factory jump table
  (`FUN_00434890`, ids 0x7d0..0x7ed): SLIME 0x7d1, SLIMEINDI 0x7d2, OBJGEN 0x7d3, DROPITEM 0x7d5, CURSOR 0x7d6, CAMERA 0x7d7, RECREATE_SHOT 0x7d8, SETUP_SCENE 0x7dc, EXCHANGE_BONEINFO 0x7e4 on both
  (the Pro table row for CREATE_PLAYER / 0x7d4 was ambiguous). No id disagreement found; `CAMPCMD_REQUEST` could not be resolved on the Pro side (unverified).
- **RECREATE_SHOT:** the linear scan over-counted a switch-structured 3-part field (`0x81385ba2` / `FUN_000b1c70`: three {s8 count, u16[2]} parts). Hand-checked on both: count 0 -> 1 u16 (+2; Pro's count-0 branch read from a filtered listing, inferred),
  count 1 -> 2 u16 (+2 then +4), count 2 -> 2 u16 (+4 then +2), otherwise none, same on both builds. Treated as LONG-ONLY (sizes in the table are approximate).
- **Count-driven arrays:** ELITEINFO_REQUEST and CREATEDROPITEM_ELITEINFO_REQUEST are `6 B + n x 8 B` on both builds.

### 12.7 Verdict update
Wire level: **99 identical + 33 long-only + 4 additive layout = a thin, mechanical translation** (patch the 44 wide primitives to 4 B inside our Pro port, then insert/strip the extra table and
17 u8 in 4 commands, plus the section-1 items: version attr, room-info pad, identity mapping). No raw struct with differing size or pointer content, no second gameplay channel, no ad-hoc dependency.
Remaining risk is semantic, not structural: stage / quest / enemy / item id sets of Vita 1.06 vs the Pro build, the meaning of the Pro-only extra data, and the per-player virtual hook. A runtime trace is still needed.

### 12.8 Per-command table
Columns: class; W = fields that are 4 B on Vita and 8 B on Pro (counted on the Pro Serialize); bytes = fixed part of the wire payload after the 0x14-byte header (`+` = plus a count-driven array);
sequence = Vita Serialize, fixed fields coalesced, `L` = the explicit `long` prim, `Bn` raw block, `(..)*` count-driven group (other wide fields are plain 4 B on Vita and not marked).

| command (DC_ dropped) | class | W | Vita B | Pro B | Vita sequence |
|---|---|---|---|---|---|
| AICMD_CHANGE_COORDINATE | IDENT | - | 2 | 2 | `2` |
| AICMD_SHOT_HOMING | IDENT | - | 24 | 24 | `24` |
| AICMD_SHOT_REFLECT | IDENT | - | 16 | 16 | `16` |
| BUFFCMD_ADDBUFF_NOTIFY | IDENT | - | 29 | 29 | `29` |
| BUFFCMD_ADDBUFF_REQUEST | IDENT | - | 29 | 29 | `29` |
| BUFFCMD_DELBUFF_NOTIFY | IDENT | - | 4 | 4 | `4` |
| BUFFCMD_DELBUFF_REQUEST | IDENT | - | 4 | 4 | `4` |
| BUFFCMD_LEVER_GACHA | IDENT | - | 8 | 8 | `8` |
| CAMPCMD_ADDBUFFDISP_NOTIFY | LONG | 2 | 24 | 32 | `L 20` |
| CAMPCMD_ADDBUFFDISP_REQUEST | LONG | 2 | 24 | 32 | `L 20` |
| CAMPCMD_ADDSCORE_NOTIFY | LONG | 1 | 20 | 24 | `L 16` |
| CAMPCMD_ADDSCORE_REQUEST | LONG | 1 | 20 | 24 | `L 16` |
| CAMPCMD_COOK_STATE_SHARE | IDENT | - | 1 | 1 | `1` |
| CAMPCMD_COOK_STATE_UP | IDENT | - | 0 | 0 | `(empty)` |
| CAMPCMD_CURSOR_ON_OFF_NOTIFY | LONG | 1 | 6 | 10 | `L 2` |
| CAMPCMD_CURSOR_ON_OFF_REQUEST | LONG | 1 | 6 | 10 | `L 2` |
| CAMPCMD_EAT_UPDATE | IDENT | - | 0 | 0 | `(empty)` |
| CAMPCMD_NOTIFY | IDENT | - | 0 | 0 | `(empty)` |
| CAMPCMD_REQUEST | IDENT | - | 0 | 0 | `(empty)` |
| CAMPCMD_REST_TIME_ZERO_NOTIFY | IDENT | - | 0 | 0 | `(empty)` |
| CAMPCMD_REST_TIME_ZERO_REQUEST | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_ADDEVENTLOG_EVENTMSG_NOTIFY | IDENT | - | 8 | 8 | `8` |
| GAMESERVERCMD_ADDEVENTLOG_EVENTMSG_REQUEST | IDENT | - | 8 | 8 | `8` |
| GAMESERVERCMD_ADDNARRATION_EVENTMSG_NOTIFY | IDENT | - | 4 | 4 | `4` |
| GAMESERVERCMD_ADDNARRATION_EVENTMSG_REQUEST | IDENT | - | 4 | 4 | `4` |
| GAMESERVERCMD_ADDSCORE_NOTIFY | LONG | 1 | 24 | 28 | `4 L 16` |
| GAMESERVERCMD_ADDSCORE_REQUEST | LONG | 1 | 24 | 28 | `4 L 16` |
| GAMESERVERCMD_BSGITEM_CHANGELOCK_NOTIFY | IDENT | - | 7 | 7 | `7` |
| GAMESERVERCMD_BSGITEM_CHANGELOCK_REQUEST | IDENT | - | 7 | 7 | `7` |
| GAMESERVERCMD_BSGITEM_CHANGESTATE_NOTIFY | IDENT | - | 8 | 8 | `8` |
| GAMESERVERCMD_BSGITEM_CHANGESTATE_REQUEST | IDENT | - | 8 | 8 | `8` |
| GAMESERVERCMD_BSGITEM_HIDEEN_SCORE_ITEM_GOT_NOTIFY | LONG | 1 | 8 | 12 | `4 L` |
| GAMESERVERCMD_BSGITEM_HIDEEN_SCORE_ITEM_GOT_REQUEST | LONG | 1 | 8 | 12 | `4 L` |
| GAMESERVERCMD_CALCSCORETOEXP_NOTIFY | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_CALCSCORETOEXP_REQUEST | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_CHANGE_HOST_CTRLID | IDENT | - | 8 | 8 | `8` |
| GAMESERVERCMD_CONNECTEDINFO_REQUEST | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_CONNECT_END | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_CONNECT_REQUEST | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_CONTINUE_NOTIFY | LONG | 1 | 5 | 9 | `L 1` |
| GAMESERVERCMD_CONTINUE_REQUEST | LONG | 1 | 5 | 9 | `L 1` |
| GAMESERVERCMD_CREATEDROPITEM_ELITEINFO_REQUEST | IDENT | - | 6+ | 6+ | `6 (8)*` |
| GAMESERVERCMD_CREATEDROPITEM_REQUEST | LONG | 1 | 165 | 169 | `88 L 73` |
| GAMESERVERCMD_CREATEFIXEDFAIRY_REQUEST | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_CREATEFIXEDSHOTACTOR_REQUEST | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_CREATEFIXEDTHIEF_REQUEST | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_CREATEOBJECT_REQUEST | LONG | 6 | 186 | 210 | `150 L 32` |
| GAMESERVERCMD_CREATEOBJGEN_REQUEST | LONG | 6 | 198 | 222 | `162 L 32` |
| GAMESERVERCMD_CREATEPLAYER_NOTIFY | LAYOUT | 4 | 8714 | 9109 | `L L 8706` |
| GAMESERVERCMD_CREATEPLAYER_REQUEST | LAYOUT | 4 | 8714 | 9109 | `L L 8706` |
| GAMESERVERCMD_CREATESLIME | IDENT | - | 4 | 4 | `4` |
| GAMESERVERCMD_CREATESLIMEINDI_REQUEST | LONG | 6 | 192 | 216 | `156 L 32` |
| GAMESERVERCMD_ELITEINFO_REQUEST | IDENT | - | 6+ | 6+ | `6 (8)*` |
| GAMESERVERCMD_EXTEND_NOTIFY | IDENT | - | 6 | 6 | `6` |
| GAMESERVERCMD_EXTEND_REQUEST | IDENT | - | 6 | 6 | `6` |
| GAMESERVERCMD_GAMEOVER_NOTIFY | IDENT | - | 4 | 4 | `4` |
| GAMESERVERCMD_GAMEOVER_REQUEST | IDENT | - | 4 | 4 | `4` |
| GAMESERVERCMD_GET_CTRLID_REQUEST | IDENT | - | 6 | 6 | `6` |
| GAMESERVERCMD_INCENEMYKILLNUM | IDENT | - | 4 | 4 | `4` |
| GAMESERVERCMD_LOCAL_ALL_CREATE_REQUEST | IDENT | - | 2 | 2 | `2` |
| GAMESERVERCMD_MARK_NOTIFY | IDENT | - | 6 | 6 | `6` |
| GAMESERVERCMD_MARK_REQUEST | IDENT | - | 6 | 6 | `6` |
| GAMESERVERCMD_NARRATIONFORCEEND_NOTIFY | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_NARRATIONFORCEEND_REQUEST | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_OPENTREASUREBOX_NOTIFY | IDENT | - | 8 | 8 | `8` |
| GAMESERVERCMD_OPENTREASUREBOX_REQUEST | IDENT | - | 8 | 8 | `8` |
| GAMESERVERCMD_REQUEST_EXEC_NOTIFY | IDENT | - | 4 | 4 | `4` |
| GAMESERVERCMD_RESERECTION_NOTIFY | LONG | 1 | 4 | 8 | `L` |
| GAMESERVERCMD_RESERECTION_REQUEST | LONG | 1 | 4 | 8 | `L` |
| GAMESERVERCMD_RESET_STAGE_REQUEST | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_RESPAWN_NOTIFY | LONG | 1 | 9 | 13 | `4 L 1` |
| GAMESERVERCMD_RESPAWN_REQUEST | LONG | 1 | 9 | 13 | `4 L 1` |
| GAMESERVERCMD_ROUNDEND_REQUEST | IDENT | - | 4 | 4 | `4` |
| GAMESERVERCMD_ROUND_STAGE_SHARE_REQUEST | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_RUNEBUFFEFFECT_NOTIFY | IDENT | - | 23 | 23 | `23` |
| GAMESERVERCMD_RUNEBUFFEFFECT_REQUEST | IDENT | - | 23 | 23 | `23` |
| GAMESERVERCMD_RUNESTONE_ON_NOTIFY | IDENT | - | 7 | 7 | `7` |
| GAMESERVERCMD_RUNESTONE_ON_REQUEST | IDENT | - | 7 | 7 | `7` |
| GAMESERVERCMD_RUNE_INVOKE_EFFECT_NOTIFY | IDENT | - | 17 | 17 | `17` |
| GAMESERVERCMD_RUNE_INVOKE_EFFECT_REQUEST | IDENT | - | 17 | 17 | `17` |
| GAMESERVERCMD_RUNE_INVOKE_NOTIFY | IDENT | - | 16 | 16 | `16` |
| GAMESERVERCMD_RUNE_INVOKE_REQUEST | IDENT | - | 16 | 16 | `16` |
| GAMESERVERCMD_SETUPSCENE_END | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_SETUPSCENE_REQUEST | IDENT | - | 168 | 168 | `168` |
| GAMESERVERCMD_SETUPSCENE_START_REQUEST | IDENT | - | 0 | 0 | `(empty)` |
| GAMESERVERCMD_SLOWMOTION_NOTIFY | IDENT | - | 8 | 8 | `8` |
| GAMESERVERCMD_SLOWMOTION_REQUEST | IDENT | - | 8 | 8 | `8` |
| GAMESERVERCMD_SPELL_OBSERVE_NOTIFY | LONG | 2 | 9 | 17 | `1 L 4` |
| GAMESERVERCMD_SPELL_OBSERVE_REQUEST | LONG | 2 | 9 | 17 | `1 L 4` |
| GAMESERVERCMD_STAGE_FLOOR_FLAG_NOTIFY | IDENT | - | 8 | 8 | `8` |
| GAMESERVERCMD_STAGE_FLOOR_FLAG_REQUEST | IDENT | - | 8 | 8 | `8` |
| GAMESERVERCMD_SYNC_DIFFICULTYMAN_NOTIFY | IDENT | - | 26 | 26 | `26` |
| GAMESERVERCMD_SYSTEM_SETTING_NOTIFY | IDENT | - | 5 | 5 | `5` |
| GAMESERVERCMD_SYSTEM_SETTING_REQUEST | IDENT | - | 5 | 5 | `5` |
| GAMESERVERCMD_TASKSYNC_REQUEST | IDENT | - | 7 | 7 | `7` |
| GAMESERVERCMD_TRANSFERLP_NOTIFY | LONG | 2 | 8 | 16 | `L L` |
| GAMESERVERCMD_TRANSFERLP_REQUEST | LONG | 2 | 8 | 16 | `L L` |
| GAMESERVERCMD_UPDATE_CONNECTEDINFO | IDENT | - | 4 | 4 | `4` |
| GAMESERVERCMD_UPDATE_LEADER_PLAYER | IDENT | - | 4 | 4 | `4` |
| GAMESERVERCMD_UPDATE_THEIF_EXP | IDENT | - | 4 | 4 | `4` |
| GAMESERVERCMD_UPDATE_THEIF_LV | IDENT | - | 4 | 4 | `4` |
| GAMESERVERCMD_UPDATE_TREASURE_NO_TRAP_FLAG | IDENT | - | 4 | 4 | `4` |
| GAMESERVERCMD_WORLDMAP_START_REQUEST | IDENT | - | 4 | 4 | `4` |
| NETCTRLCMD_CHANGE_HOST_CTRLID | IDENT | - | 8 | 8 | `8` |
| NETCTRLCMD_CHECK_ALIVE_NOTIFY | IDENT | - | 7 | 7 | `7` |
| NETCTRLCMD_CHECK_ALIVE_REQUEST | IDENT | - | 7 | 7 | `7` |
| NETCTRLCMD_CONNECT_CHECK_CLEAR | IDENT | - | 0 | 0 | `(empty)` |
| NETCTRLCMD_CONNECT_CHECK_COUNT | IDENT | - | 16 | 16 | `16` |
| SCENEMANCMD_COMPLETE_CONNECT_NOTIFY | IDENT | - | 4 | 4 | `4` |
| SCENEMANCMD_COMPLETE_SHARE_NOTIFY | IDENT | - | 0 | 0 | `(empty)` |
| SCENEMANCMD_CONNECT_NG | IDENT | - | 0 | 0 | `(empty)` |
| SCENEMANCMD_CREATE_CAMERA | LONG | 6 | 200 | 224 | `158 L 38` |
| SCENEMANCMD_CREATE_CURSOR | LONG | 7 | 204 | 232 | `158 L 38 L` |
| SCENEMANCMD_CREATE_DROPITEM | LONG | 6 | 221 | 245 | `158 L 59` |
| SCENEMANCMD_CREATE_OBJGEN | LONG | 6 | 212 | 236 | `158 L 50` |
| SCENEMANCMD_CREATE_PLAYER | LAYOUT | 9 | 8886 | 9301 | `158 L 42 L 8678` |
| SCENEMANCMD_CREATE_SLIME | LONG | 6 | 204 | 228 | `158 L 42` |
| SCENEMANCMD_CREATE_SLIMEINDI | LONG | 6 | 206 | 230 | `158 L 44` |
| SCENEMANCMD_DELETE_TASK_NOTIFY | IDENT | - | 6 | 6 | `6` |
| SCENEMANCMD_EXCHANGE_BONEINFO | LAYOUT | - | 17308 | 18032 | `17308` |
| SCENEMANCMD_KILL_OBJECT | IDENT | - | 2 | 2 | `2` |
| SCENEMANCMD_LOCAL_ALL_SEND_COMPLETE | IDENT | - | 0 | 0 | `(empty)` |
| SCENEMANCMD_NO_CREATE_DROP_WEAPON | LONG | 1 | 5 | 9 | `L 1` |
| SCENEMANCMD_RECREATE_SHOT | LONG | 7 | ~239 | ~267 | `158 L 77` |
| SCENEMANCMD_REQUEST_LOCAL_ALL_CREATE | IDENT | - | 1 | 1 | `1` |
| SCENEMANCMD_REQUEST_LOCAL_ALL_SYNC | IDENT | - | 1 | 1 | `1` |
| SCENEMANCMD_RESET_STAGE | IDENT | - | 0 | 0 | `(empty)` |
| SCENEMANCMD_ROUND_RESULT | IDENT | - | 12 | 12 | `12` |
| SCENEMANCMD_SETUP_SCENE | LONG | 1 | 184 | 188 | `B64 B64 18 L 34` |
| SCENEMANCMD_SHARE_KILLED_NOTIFY | IDENT | - | 2 | 2 | `2` |
| SCENEMANCMD_START_CONNECT_NOTIFY | IDENT | - | 7 | 7 | `7` |
| SCENEMANCMD_START_SETUP_SCENE | IDENT | - | 0 | 0 | `(empty)` |
| SCENEMANCMD_UPDATE_CONNECTED | IDENT | - | 4 | 4 | `4` |
| SCENEMANCMD_WORLDMAP_SELECT_ROUND | IDENT | - | 18 | 18 | `18` |
| SCENEMANCMD_WORLDMAP_START | IDENT | - | 4 | 4 | `4` |
| TASKCMD_SYNC_ACTSTEP | IDENT | - | 1 | 1 | `1` |

### 12.9 Function addresses added by this pass
| Vita | Pro | role | conf |
|---|---|---|---|
| 0x813828c4 | FUN_000a9a70 | hero-record field serializer (0x21d0-byte record, field-wise) | high |
| 0x810266d8 | FUN_00025ed0 | hero-record header sub-struct writer | high |
| 0x81385ba2 | FUN_000b1c70 | RECREATE_SHOT count-switched 3-part field | med |
| 0x8137ad42 | FUN_0009d570 | command base ctor (this, id) | high |
| 0x81378e12 | none | fills the type-6 template, sets pending bit 6 (ad-hoc only) | high |
| 0x81379190 / 0x81379416 | none | ad-hoc matching event handler / thunk | med |
| 0x81376c2a | (send scheduler, Pro equivalent not located this pass) | per-peer pending-bit scheduler, picks the packet type to send | med |
| 0x811f316c / 0x813849ca | FUN_002f7ff0 / FUN_000ae3b0 | per-player extension serializer of the scene-setup dump / its first sub-struct | med |
| 0x810274c6 | FUN_00026f90 | block (slot) -> player object lookup via the pointer + handle after the 0x21d0 block | high |
| 0x81383b14 / 0x813840a8 | FUN_000acb90 / ad440 | `long` write / read (4 B vs 8 B) | high |
| 0x81386b4c | FUN_000b3a00 (one of 20 enum twins 0xb35f0..0xb4560) | enum-typed int write (4 B vs 8 B) | high |
| (Vita factory not located) | FUN_00434890 | SCENEMAN command factory, ids 0x7d0..0x7ed (Pro only located) | med |
