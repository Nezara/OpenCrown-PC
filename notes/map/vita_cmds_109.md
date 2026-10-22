# Vita 1.09 vs Pro: game-data stream, command by command (is 1.09 wire-identical to Pro?)

Mapper survey, read-only. Target `Work/vita/update109.elf` (US PCSE00019 update 1.09, fixed-address Thumb-2). Pro side as in
`vita_net.md` 12.1 (Pro vtables via R_X86_64_RELATIVE, `Work/vita/prorel.pkl`, `Work/vita/cands.pkl`). Builds on `vita_net.md`
(1.06 vs Pro, sections 4, 7, 12) and `vita_net_109.md` (1.09 imports / Matching2). Tags: **(asm)** read in capstone disassembly or
compared by the asm tokenizer, **(inferred)**, **(unverified)**. Scratch tooling (gitignored) in `notes/raw/vita_cmds_109/`:
`vita.py` (1.09 shim of `Work/vita/vita.py`), `ext9.py` (1.09 copy of the wire tokenizer, plus count-down loops whose counter is a
register copy or a stack slot), `run9.py` (name -> vtable), `cmp9.py` / `tok9.py` (comparison), `rld9.py` (run-length diff),
`ids9b.py` / `fac9b.py` (command ids), `prims.txt`, `table.md` (all 136 rows).

## 1. Summary and verdict
**Verdict: as far as static analysis can show, Vita 1.09's game-data stream is wire-identical to Pro's. Cross-play needs no
translator at the DcNet / command level.** (asm, with the caveats in 1.2)

| item | 1.06 vs Pro (vita_net.md) | **1.09 vs Pro** | tag |
|---|---|---|---|
| protocol version (attr 0x53 bits 13:7) | 0x21 vs 0x22 | **0x22 = 0x22** | asm (vita_net_109) |
| room-info blob (bin attr 0x55 / 0x57) | 0x62 vs 0x78 | **0x78 = 0x78, same fields, order and widths** (section 4) | asm |
| wide primitives (`long` + enum ints) | 4 B vs 8 B | **8 B = 8 B** (all 28 simple 1.09 wide writers sign-extend and write 8 B; readers read 8 B, keep the low word) | asm |
| 33 LONG-ONLY commands | differ | **identical** | asm |
| 4 LAYOUT commands (+362-B table, +17 u8) | Pro only | **1.09 writes both, at the same place** | asm |
| other 99 commands | identical | identical | asm |
| command header (0x14 raw bytes) | identical | identical | asm |
| command ids (factory switch id -> class) | 128/136 matched | **GAMESERVER 82/82, SCENEMAN 27/27 identical; 24 of the other 27 have the same ctor id, 3 unresolved on one side** | asm |
| DcNet header, CRC-32, types 0-5 | identical | identical (same min sizes) | asm |
| DcNet type 6 | Vita only, ad-hoc only, 0x340 B | still Vita only and ad-hoc only, **now 0x418 B** | asm |
| 0x21d0 hero block | identical | identical (raw 0x21d0; field-wise serializer token-identical) | asm |

**Command-level comparison:** **136 / 136** Serialize bodies are token-identical (same field order, same widths; RECREATE_SHOT
compared by hand, see 3.3). The 1.09 Deserialize bodies read exactly as many bytes as their Serialize bodies write in all 136 classes.
(The Pro Deserialize side is often inlined in a way the tokenizer can't read, as on 1.06.)

### 1.1 What is left (none of it is in the game-data stream)
1. **Identity at the Matching2 / lobby level:** Vita uses 0x24-byte SceNpId (member lists, block lists, invites), Pro uses account
   ids. This is HLE / lobby work (synthesize NpIds), not stream work. No `DC_*CMD_*` serializer carries an NpId (inferred: none
   of the 136 token streams contains a 0x24-byte field or raw block other than SETUP_SCENE's 2 x 0x40).
2. **Room-info flag word (+0x5e) at publish time:** 1.09 ORs **0x4000**, Pro ORs **0x8000** (byte +0x5f |= 0x80) (asm). No reader of
   either bit was found in either build (scan of byte/halfword tests; other bits 0x20 / 0x200 / 0x1000 are read). Probably a
   platform marker. Harmless unless a peer or our lobby filters on it (unverified).
3. **Semantics, not layout:** item / stage / enemy / quest id sets and save fields of Vita 1.09 vs Pro were not compared. A runtime
   trace is still the final check (unverified).
4. DcNet type 6 (ad-hoc profile packet) still exists on Vita, ad-hoc mode only (receive gate asm). Nothing to do for Pro.

### 1.2 Confidence and limits
- The tokenizer merges branches linearly and does not follow virtual calls. None of the 136 serializers makes a virtual call
  except ELITEINFO_REQUEST / CREATEDROPITEM_ELITEINFO_REQUEST (`V` tokens on the 1.09 read side only; their counted arrays
  `6 + n x 8 B` are identical) (asm).
- Equal token streams mean equal wire layout. Equal values are not established (e.g. the wide fields are sign-extended on 1.09
  and on Pro's `long`, zero-extended by some Pro enum twins; both readers keep only the low 32 bits, so the high half is never
  used) (asm: 1.09 readers `str r0`, Pro `FUN_000ad440` `mov dword ptr [r14], r15d`).
- No serializer reads its third argument (the version / mode parameter passed by the header writer) on 1.09 (asm scan), so
  there are no version-conditional fields.

**Rating: careful → mostly settled.** The static wire comparison is complete. What remains is runtime validation (section 6).

## 2. 1.09 stream primitives (write side; reader next to it)
All little-endian, written byte by byte, via `Reserve(n)` = stream vtable +0xc (asm; widths read in each body). The 1.06 column is a byte match for
raw W/R, s16, u16 and u8 `face`; the other 1.06 / Pro pairings follow address order and are (inferred). Pro equivalents are from vita_net.md 7 / 12.1.
| 1.09 writer / reader | wire B | role | 1.06 | Pro | callers (w) |
|---|---|---|---|---|---|
| 0x81396358 / 0x8139641c | n | raw copy (r2 = n) | 0x8137f69c / 0x8137f760 | a4240 / a42d0 | 25 / 445 |
| 0x813963a8 / 0x81396464 | 1 | u8 | f6ec | a4500.. | 193 |
| 0x81396474 / 0x813964b4 | 1 | u8 | f7b8 | | 210 |
| 0x813968f2 / 0x81396930 | 1 | u8 | 0x8137face | a4b00 | 223 |
| 0x813964c4 / 0x8139650a | 2 | s16 | 0x8137f808 | a4620 | 225 |
| 0x8139654a / 0x8139658e | 2 | u16 | 0x8137f872 | a46f0 | 82 |
| 0x813965ce / 0x8139661e | 4 | u32 | f8da | a47c0 | 571 |
| 0x8139665a / 0x813966aa | 4 | u32 | f94a | a4890 | 175 |
| 0x813966e6 / 0x81396736 | 4 | u32 (float / vector element) | f9ba | a4960 | 209 |
| 0x81396772 / 0x813967ee | 8 | u64 (1 caller) | fa2a (0 callers) | a4a30 (1 caller) | 1 |
| **0x81396832** / 0x813968ae | **8** | 64-bit writer used by the whole wide family | (none) | (inline in each wide prim) | 33 |
| 0x81396972 / 0x81396988 | 8 | 2 x u32 | fb34 | a4bd0 | 27 |
| 0x813969d4 / 0x813969f4 | 12 | 3 x u32 (vec3) | fc00 | a4d60 | 82 |
| 0x81396a52 / 0x81396a86, 0x81396b08 / 0x81396b50 | 20 / 28 | 5 x / 7 x u32 | | | 4 / 2 |
| **0x81398c0a / 0x81398da8** | **8** | `long`: `ldr; asrs #31; bl 0x81396832` / reader `Read(8)`, store low word | 0x81383b14 (4 B) | acb90 / ad440 (8 B) | 26 / 26 |
| **0x8139ab88 + k x 0x78, k = 0..24** (readers at +0x36) | **8** | 25 enum-typed twins, same body as `long` | 0x81386b4c.. (4 B) | 0xb0410, 0xb35f0..0xb45c0 (8 B) | 95 total writer sites incl. the 3 rows above and below |
| 0x813993ea, 0x8139969a | 8 | two more sign-extending wide writers (readers elsewhere) | | | |
| 0x8139902c, 0x81399420 | composite | composites with an inline 8-B wide field (`asrs` + `bl 0x81396832`) | | | |

Answer to "are all 22 Pro wide pairs now 8 B on 1.09?": **yes**. 1.09 has 28 out-of-line wide writers plus 2 inline uses,
all 8 B. The count differs from Pro's 22 pairs (compiler outlining); what matters is that every wide field position in all 136
commands is 8 B on both builds (the per-command token comparison, section 3) (asm).

## 3. 136 commands, 1.09 vs Pro
Method as vita_net.md 12.1: string -> leaf name fn -> vtable slot 7, slot 9 = Serialize, slot 10 = Deserialize (1.09: **136/136
resolved, one candidate each**; the command-name set is identical to 1.06 and Pro). Slot 0 / slot 1 are thunks to slots 9 / 10
(`ldr r2,[r0]; ldr r2,[r2,#0x24]; blx r2`), and slot 11 = the header writer 0x813917ae (asm).

### 3.1 Counts
| class | count | note |
|---|---|---|
| IDENTICAL (Serialize token-identical, wide fields 8 B both) | **135** | incl. the 33 former LONG-ONLY and the 4 former LAYOUT |
| IDENTICAL by hand | **1** | SCENEMANCMD_RECREATE_SHOT (count-switched field, 3.3) |
| still differing | **0** | |
| 1.09 Deserialize size = Serialize size | 136 | the extractor fails on 18 Pro readers (inlined loops), as on 1.06 |

### 3.2 The 37 commands that differed on 1.06 (sizes = fixed wire payload after the 0x14 header)
| command (DC_ dropped) | 1.06 class | 1.06 Vita B | Pro B | 1.09 B | 1.09 vs Pro | wide fields |
|---|---|---|---|---|---|---|
| CAMPCMD_ADDBUFFDISP_NOTIFY / _REQUEST | LONG | 24 | 32 | 32 | IDENT | 2 |
| CAMPCMD_ADDSCORE_NOTIFY / _REQUEST | LONG | 20 | 24 | 24 | IDENT | 1 |
| CAMPCMD_CURSOR_ON_OFF_NOTIFY / _REQUEST | LONG | 6 | 10 | 10 | IDENT | 1 |
| GAMESERVERCMD_ADDSCORE_NOTIFY / _REQUEST | LONG | 24 | 28 | 28 | IDENT | 1 |
| GAMESERVERCMD_BSGITEM_HIDEEN_SCORE_ITEM_GOT_NOTIFY / _REQUEST | LONG | 8 | 12 | 12 | IDENT | 1 |
| GAMESERVERCMD_CONTINUE_NOTIFY / _REQUEST | LONG | 5 | 9 | 9 | IDENT | 1 |
| GAMESERVERCMD_CREATEDROPITEM_REQUEST | LONG | 165 | 169 | 169 | IDENT | 1 |
| GAMESERVERCMD_CREATEOBJECT_REQUEST | LONG | 186 | 210 | 210 | IDENT | 6 |
| GAMESERVERCMD_CREATEOBJGEN_REQUEST | LONG | 198 | 222 | 222 | IDENT | 6 |
| **GAMESERVERCMD_CREATEPLAYER_NOTIFY / _REQUEST** | LAYOUT | 8714 | 9109 | **9109** | IDENT | 4 |
| GAMESERVERCMD_CREATESLIMEINDI_REQUEST | LONG | 192 | 216 | 216 | IDENT | 6 |
| GAMESERVERCMD_RESERECTION_NOTIFY / _REQUEST | LONG | 4 | 8 | 8 | IDENT | 1 |
| GAMESERVERCMD_RESPAWN_NOTIFY / _REQUEST | LONG | 9 | 13 | 13 | IDENT | 1 |
| GAMESERVERCMD_SPELL_OBSERVE_NOTIFY / _REQUEST | LONG | 9 | 17 | 17 | IDENT | 2 |
| GAMESERVERCMD_TRANSFERLP_NOTIFY / _REQUEST | LONG | 8 | 16 | 16 | IDENT | 2 |
| SCENEMANCMD_CREATE_CAMERA | LONG | 200 | 224 | 224 | IDENT | 6 |
| SCENEMANCMD_CREATE_CURSOR | LONG | 204 | 232 | 232 | IDENT | 7 |
| SCENEMANCMD_CREATE_DROPITEM | LONG | 221 | 245 | 245 | IDENT | 6 |
| SCENEMANCMD_CREATE_OBJGEN | LONG | 212 | 236 | 236 | IDENT | 6 |
| **SCENEMANCMD_CREATE_PLAYER** | LAYOUT | 8886 | 9301 | **9301** | IDENT | 9 |
| SCENEMANCMD_CREATE_SLIME | LONG | 204 | 228 | 228 | IDENT | 6 |
| SCENEMANCMD_CREATE_SLIMEINDI | LONG | 206 | 230 | 230 | IDENT | 6 |
| **SCENEMANCMD_EXCHANGE_BONEINFO** | LAYOUT | 17308 | 18032 | **18032** | IDENT | - |
| SCENEMANCMD_NO_CREATE_DROP_WEAPON | LONG | 5 | 9 | 9 | IDENT | 1 |
| SCENEMANCMD_RECREATE_SHOT | LONG | ~239 | (switch) | (switch) | IDENT (hand) | 7 |
| SCENEMANCMD_SETUP_SCENE | LONG | 184 | 188 | 188 | IDENT | 1 |
The other 99 (1.06 IDENT) stay identical on 1.09 (all rows in `notes/raw/vita_cmds_109/table.md`).

### 3.3 Former LAYOUT commands on 1.09 (asm)
| command | 1.09 Serialize | layout |
|---|---|---|
| GAMESERVERCMD_CREATEPLAYER_REQUEST / _NOTIFY (shared ser 0x81471058, deser 0x81471120) | `long` +0x24, `long` +0x28, wide +0x2c, s16 +0x30, **hero record +0x34** (0x813981f8), **362-B extra table +0x2204** (0x8139aa62, 181 x u16), 4 u8 +0x236e.., 3 x u32 +0x2374, 2 x u32 +0x2380, u32 +0x2388, wide +0x238c, composite +0x2390 (0x8139a684), **17 u8 loop at +0x23a0** | = Pro (object offsets = Pro - 4) |
| SCENEMANCMD_CREATE_PLAYER (ser 0x814d1188) | same two additions | = Pro |
| SCENEMANCMD_EXCHANGE_BONEINFO (ser 0x814d1610) | hero record + 362-B table, twice | = Pro |
- Hero-record field serializer 0x813981f8 = Pro `FUN_000a9a70`, token-identical, 8652 wire bytes; the 362-B table writer
  0x8139aa62 = Pro `FUN_000b3390` (asm).
- RECREATE_SHOT (ser 0x814d12c4 -> 0x8139a236): three {s8 count, u16 a @+2, u16 b @+4} parts. Count 0 writes a; count 1 writes a, b;
  count 2 writes b, a; anything else writes nothing (asm, 1.09). Pro `FUN_000b1c70`: case 2 writes +4 then the shared tail, case 1
  +2 then +4, case 0 +2 (asm, partial read; matches the 1.06 hand check).
- GAMESERVERCMD_SETUPSCENE_REQUEST (0x81470e94 -> 0x813977a4): 2 x 2 x u32, 32 x 2 u8, 32 x 2 u8, 5 x u32, 3 u8, 1 u8 = 168 B = Pro (asm).

### 3.4 Command header and ids (asm)
- Header writer 0x813917ae (vtable slot 11): raw **0x14 bytes** from cmd+4..+0x18 (`Write(stream, sp, 0x14)`), then vtable slot 0
  (-> Serialize), then commit 0x8139b78e. Reader 0x81391840: `Read(0x14)` into cmd+4, then slot 1 (-> Deserialize). Same as 1.06
  0x8137adb8 / 0x8137ae30 and Pro `FUN_0009d5e0`.
- Base ctor 0x8139173a(this, id): id u16 at cmd+0xc (header offset +8), vptr base 0x8177062c+8.
- **Factory id -> class maps** (taking the last vtable store per switch case, following direct calls one level):
  | factory | 1.09 | Pro | ids | result |
  |---|---|---|---|---|
  | GAMESERVERCMD | 0x81029fc0 (`subw #0x5dc; cmp #0x53; tbh`) | `FUN_0002a3c0` (jump table 0x6a5ba8) | 0x5dc..0x62f | **82/82 same class per id** |
  | SCENEMANCMD | 0x812d11c4 (`sub #0x7d0; cmp #0x1d; tbh`) | `FUN_00434890` (jump table 0x7540e4) | 0x7d0..0x7ed | **27/27 same** |
- The other 27 classes (CAMPCMD 13, BUFFCMD 5, NETCTRLCMD 5, AICMD 3, TASKCMD 1): base-ctor id argument equal on 24 (e.g. TASKCMD
  0x384, NETCTRL 0x3f5..0x3f8, AICMD 0x1413 / 0x141c / 0x141d, BUFFCMD 0x140e / 0x1410 / 0x142f, CAMPCMD 2..0xd). Not resolved on one or both
  sides: CAMPCMD_NOTIFY, CAMPCMD_REQUEST (both builds, as on 1.06), BUFFCMD_ADDBUFF_REQUEST (Pro has an extra dynamic site),
  NETCTRLCMD_CHANGE_HOST_CTRLID (1.09 {0x3f2, 0x3f4} vs Pro 0x3f4; 0x3f2 is a neighbouring ctor, inferred).
  Their factories were not mapped.

### 3.5 Raw 0x21d0 hero block and the per-player hook
- Scene snapshot writer 0x812c8a4e / restore 0x812c8b18 (1.06 0x812be46c / 0x812be514): per player 0..3 (slot id at +0x2258 != -1):
  u32 index, **raw 0x21d0 from player+0x7c**, u32 length placeholder, per-player extension 0x811f7af2(obj, stream), then the placeholder is patched (asm).
  **Player stride is 0x23ac on 1.09** (1.06 0x2230, Pro 0x23b0 = 1.09 + 4 for the 8-byte pointer): the 0x16a extra table and 17 bytes now live in the 1.09 player object as on Pro (inferred from the stride and 3.3).
- The extension 0x811f7af2 matches Pro `FUN_002f7ff0` in skeleton (same 4-byte fields, same 8 equipment-like groups); its per-class virtual hook is now at **vtable +0x640** (1.06 +0x634, Pro +0xc98). It was not compared beyond the tokens.
- **Not on the wire (inferred):** restore and dump are called back to back in 0x812c9a00 (0x812cae5a, 0x812cae72) into the buffer at scene+0x938c; no command serializer calls them. It looks like a local scene-transition snapshot, so it does not matter for cross-play.

## 4. Other wire layers
### 4.1 DcNet packet layer (1.09 `DcNet_OnPacket` 0x8138ea8e; asm)
| item | 1.09 | Pro |
|---|---|---|
| header | 16 B: +0 time, +4 byte count (receive budget at peer+0x640), +8 CRC, +0xc u16 length, +0xe u8 type | same |
| length check | [0x10, 0x40000) else -3 | same |
| CRC | 0x813cdab2(buf, len, -1, -1, table 1) over the packet with +8 zeroed; table pointer array 0x8183e718, table 1 = standard CRC-32 (0x77073096, 0xee0e612c...) | same |
| type 0 | stream payload (u16 length at +0xc), copied with raw write 0x81396358 | same |
| type 1 | nop | same |
| type 2 / 3 | min 0x38 (0x8138db1a / 0x8138de0a) | same |
| type 4 | min 0x10, loops the 4 peers | same |
| type 5 | min 0x18, u8 at +0x10 vs host ctrl id (net+0x180) | same |
| **type 6** | **min 0x418** (1.06 0x340); refused (-3) unless the ad-hoc test 0x8139c188 (NetCtl ad-hoc state == 3 via 0x813b804a, then a flag at +0x7d0) passes; applies the payload via 0x813a229e | absent (switch bound 5) |
Type 6 is not reachable in Matching2 / Rudp play (receive gate asm; sender side follows the 1.06 analysis, inferred).

### 4.2 Room info (bin attr 0x55 / 0x57), field by field (asm)
1.09 writer 0x8138bee8 / reader 0x8138bf88. Pro writer `FUN_00098fb0` (cursor rdi+0x10) / reader `FUN_00099110` (cursor rdi+0x18).
**Correction to vita_net.md 5:** the Pro names there are swapped, and the field at 0x68 is 4 B, not 8 B.
| wire | struct off | field | 1.09 | Pro |
|---|---|---|---|---|
| 8 | 0x00 | header, 4 x (2 x u8) | 0x813a8f2a | `c6580` |
| 48 | 0x08 | 8 x (4 x u8 + s16) members | 0x813a7502 | `c66a0` |
| 30 | 0x40..0x5c | 15 x s16 (7 pairs + 0x5c) | 0x813a6db8 | `c3310` |
| 2 | 0x5e | u16 flags (publish: 1.09 `|= 0x4000`, Pro `|= 0x8000`) | 0x813a6de2 | `c32a0` |
| 5 | 0x60..0x64 | 5 x u8 | 0x813a6d80 | `c32e0` |
| 1 | 0x65 | bool | 0x813a6d9c | `c32c0` |
| 4 | 0x68 | u32 = DcNetMatching+0x2c4 (worldId per vita_net_109 4.3, inferred) | 0x813a6e4a | `c6230` / `c6210` |
| 2 | 0x66 | u16 = DcNetMatching+0x2c0 (RoomDataInternal +2) | 0x813a6de2 | `c32a0` |
| 8 | 0x70 | u64 = DcNetMatching+0x2d0 (**roomId**) | 0x813a6e8a | `c62b0` / `c6290` |
Sum 108 B written into a fixed 0x78-byte buffer (1.09 publish 0x8138f23e: `f813915a4(buf, 0x78)`; Pro `FUN_0009c3a0`: `mov esi, 0x78`). The
12-byte tail is the unused rest of the buffer (inferred; this replaces the "8 B container" guess). The fill code is the same shape in both:
flag OR, +0x68, +0x66, +0x70 from the matching object, then the writer (asm). Init 0x8138c090 zeroes 0x66 / 0x68 / 0x70 (asm).

### 4.3 Matching-level items already covered elsewhere
Version 0x22, packed attr 0x53, int attrs 0x4c..0x53, bin attrs 0x55 / 0x57 / 0x5f, join gate save+0xaf1e4 vs attr 0x4c: see
vita_net_109.md 4.2; all equal to Pro. Identity (SceNpId vs account id) is the only matching-level difference (1.1 item 1).

## 5. Function table (1.09)
| addr | name | role | conf |
|---|---|---|---|
| 0x81396358 / 0x8139641c | Stream_WriteRaw / ReadRaw(n, flag) | raw copy | high |
| 0x813963a8.. 0x81396b50 | stream primitives | section 2 | high |
| 0x81396832 / 0x813968ae | Stream_WriteS64 / ReadS64 | 8-B writer behind all wide prims | high |
| 0x81398c0a / 0x81398da8 | Stream_WriteLong / ReadLong | 8 B | high |
| 0x8139ab88 + k*0x78 (k<25) / +0x36 | Stream_WriteEnum*/ReadEnum* | 8 B wide twins | high |
| 0x8139173a | NetCmd_ctor(this, id) | base ctor, id at +0xc | high |
| 0x813917ae / 0x81391840 | NetCmd_Write / NetCmd_Read | 0x14 header + Serialize / Deserialize | high |
| 0x8139b744 / 0x8139b78e | Stream_Begin / Stream_Commit | around the command write | med |
| 0x81029fc0 | GameServerCmd_Factory(id 0x5dc..0x62f) | id -> class | high |
| 0x812d11c4 | SceneManCmd_Factory(id 0x7d0..0x7ed) | id -> class | high |
| 0x813981f8 | HeroRecord_Serialize | 0x21d0-B record, 8652 wire B | high |
| 0x8139aa62 | HeroExtra_Serialize | 0x16a table (181 u16) | high |
| 0x8139a236 | RecreateShot_PartSerialize | count-switched field | high |
| 0x813977a4 | SetupSceneReq_Payload | 168 B | high |
| 0x81471058 / 0x81471120 | CreatePlayerReq/Notify Serialize / Deserialize | | high |
| 0x814d1188 / 0x814d1610 / 0x814d12c4 / 0x814d0d28 | CREATE_PLAYER / EXCHANGE_BONEINFO / RECREATE_SHOT / SETUP_SCENE Serialize | | high |
| 0x812c8a4e / 0x812c8b18 | SceneSnapshot_Dump / Restore | raw 0x21d0 per player, local | high / med (role) |
| 0x811f7af2 | PlayerExt_Serialize | hook at vtable +0x640 | med |
| 0x8138ea8e | DcNet_OnPacket | 4.1 | high |
| 0x8138da1a | DcNet_RxError(slot, code) | | med |
| 0x8138db1a / 0x8138de0a | DcNet_RxType2 / RxType3 | | high |
| 0x813a229e | DcNet_ApplyType6 (ad-hoc roster) | | med |
| 0x8139c188 | Matching_IsAdhocActive | type-6 gate | high |
| 0x813cdab2 | Crc32(buf, len, init, xorout, table) | table array 0x8183e718 | high |
| 0x8138bee8 / 0x8138bf88 | RoomInfo_Write / RoomInfo_Read | corrects vita_net_109 "Write 0x8138bdb0" (that is a static initializer) | high |
| 0x8138c090 | RoomInfo_Fill (from save, zeroes the tail) | | med |
| 0x8138f23e | RoomInfo_Publish (flag 0x4000, tail fields, 0x78 buffer) | | high |
| 0x813a6d80..0x813a6ede, 0x813a7502, 0x813a8f2a | RoomInfo byte writer / reader primitives | | high |

## 6. Open questions / runtime checks
1. Runtime confirmation: a Vita 1.09 (emulated) and a Pro peer in one room; check that the first CREATEPLAYER / EXCHANGE_BONEINFO / SETUP_SCENE
   round-trips and no `RxError` fires. The static result predicts it works with no translation.
2. Content id sets (items, stages, enemies, quests, rune / spell ids) of Vita 1.09 vs Pro: not compared (unverified).
3. Meaning of room-info flag bit 0x4000 (Vita) / 0x8000 (Pro); no reader found in either build.
4. The 3 commands whose ids are not resolved on both sides (3.4), and the 4 small factories (CAMP, BUFF, NETCTRL, AI) not mapped.
5. Per-player extension hook (+0x640 / Pro +0xc98) not compared, but it is in a local snapshot, not on the wire (inferred).
6. Type 2 / 3 (member table, 0x28-B payload) contents: sizes equal, field contents not compared (inferred equal, as on 1.06).
