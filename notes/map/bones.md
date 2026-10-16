# Bones (ally "bone" characters): local generation, pickup, and the P2P EXCHANGE_BONEINFO command

Mapper pass 2026-10-17. Addresses are eboot-relative (as in `q.py`). Tags: **(asm)** = read in capstone disassembly or
Ghidra pseudo-C cross-checked against asm, **(inferred)** = deduced from structure/usage, **(unverified)** = guess.
Baseline: `net.md` 4.3 (NetCtrl command layer), `friends.md`, `story_progress.md` (hero array), `input_save.md` 3.2.
Nothing in `port/src` was changed. Raw dumps: `notes/raw/bones/` (ignored).

## 1. Summary

**Rating: routine for the port as it is (nothing to shim), careful if a server-side bones feature is still wanted.**

**The premise "bones are uploaded to / downloaded from a server" is wrong for this binary.** There is no server
transport for bones in the eboot at all:

1. **No import carries bone data.** The only Np surfaces are Matching2 (room attrs, user bin attr 0x5f = a settings blob,
   not heroes), Toolkit2 (friends/presence/block list/invite, `friends.md`), `sceNpTus*` (SetDataA of a whole save
   image in 4 slots x 256 KB chunks = cross-save, `c1fa0` / `c22f0`, no static caller; plus the 0x78-byte presence blob
   `be7c0`), WordFilter, InGameMessage. No ranking, no web API, no HTTP request (Http/Ssl are init-only). TUS can only
   address the caller's own account, so it cannot share anything between players anyway.
2. **A "bone" is an ally character record.** The save has a hero array of 74 records (stride 0x21d0, at slot+0xa480).
   Indices 0..0xf = player characters, **0x10..0x2d = 30 "stock" records, 0x2e..0x49 = 28 ally ("bone") records**. The
   dungeon object `CharaBone` (drop item code 6) carries one of those 28 characters; picking it up puts the character in
   the player's carried-bones list and the Temple (town) then handles it. All of this is local save data.
3. **Local bones work fully offline.** The object init picks a random existing ally record not in use, or, if none
   qualifies, generates a brand-new random character (`rand()`-based). No network check on this path (asm/pseudo-C).
4. **The only network traffic is a P2P in-session command, `DC_SCENEMANCMD_EXCHANGE_BONEINFO` (id 0x7e4)**, riding the
   normal game data stream (Rudp, packet type 0, `net.md` 4.2/4.3) between the players of one room. At the start of each
   stage (`AC_STEP_START` of the scene-man state machine) every peer sends up to 2 of its ally records; receivers merge
   them into their own 28 ally slots and answer, in a bounded ping-pong (hop counter <= 4). So "online bones" = allies of
   the other players in your room join your roster, **no server, no persistence beyond the receiver's own save**.
5. The command is skipped when the account has the **UGC (user generated content) parental restriction**
   (`Signin+0x21c`, from `sceNpGetParentalControlInfoA` info byte +2). The shim already returns all-zero flags, so the
   exchange is enabled.
6. **No write-back / "mark used" exists.** Resurrect/bury (Temple) only changes the local save.

Net: the lobby needs **no bones ops**. The game stream already carries the exchange if the Rudp shim relays 18 KB messages
(it does: fragmenting carrier, 256 KB messages). Section 7 gives an optional design if an asynchronous server pool is
still wanted as a new feature; it cannot be a transparent shim of a PS4 call because that call does not exist.

## 2. Function table

| addr | proposed name | role | conf |
|---|---|---|---|
| 0x4296a0 | `SceneMan::UpdateAC` (steps `AC_STEP_*`) | online scene state machine; **sender** of EXCHANGE_BONEINFO in step `AC_STEP_START`: `42abd4..42ae38` (skipped if UGC restricted, `42abc6`) | asm |
| 0x4355f0 | `SceneMan::OnCommand(cmd)` | jump-table dispatcher of received scene-man commands; EXCHANGE_BONEINFO case = `435ba3..4364af` (receiver + reply) | asm |
| 0x434890 | `SceneMan::NewCommand(id)` | command factory, `id - 0x7d0` < 0x1e, table 0x7540e4 | asm |
| 0x437d60 | `SceneMan::BuildBoneEntries(out*, max, skipSelected)` | fills up to `max` (<=2) 0x233c-byte entries, see 3.2 | asm |
| 0x4412f0 / 0x441300 / 0x441370 | `BoneInfoCmd::Name` / `Write` / `Read` | vtable 0xb8db58 (+0x10 = address point), serializer, deserializer | asm |
| 0x9d570 / 0x9d5c0 | `NetCmd::NetCmd(this, id)` / dtor | command base (header 0x28 B) | asm |
| 0x8a1c0 | `Heroes::InsertAlly(heroes, ctx, ?, hero*, extra*, evictRandom, evictOldest)` | merge a received/new character into the 28 ally slots, see 4.3 | asm (pseudo-C) |
| 0x89e20 / 0x89de0 | `Heroes::FindByIdentity` / `FindPtr` | identity compare over 74 records, see 3.1 | asm |
| 0x8a0f0 / 0x8a0d0 | `Heroes::Ally(i)` | `heroes + (i + 0x2e) * 0x21d0`, `i` in 0..27, else null | asm |
| 0x89320 | `Heroes::StoreInStock(hero*)` | first empty record of 0x10..0x2d gets the hero, flags = (flags & ~5) \| 1 | asm |
| 0x88d60 | `Heroes::CopyInto(idx, hero*)` | copy a 0x21d0 record into slot `idx` | asm |
| 0x8b150 / 0x8bb20 / 0x8bb00 | `Heroes::ExtraTable` / `Extra(i)` | `heroes + 0x9c620` / `+ i * 0x16a` (i < 0x4a) | asm |
| 0x89f40 | `Heroes::CountEmptyAllySlots` | counts hero+0x27 == 0xff over the 28 ally slots | asm |
| 0x8a500 / 0x89210 | `Heroes::GenerateRandomAlly` | `rand() % 6` -> generator 89210; sets flag bit 3 (0x08) on the result | asm |
| 0x893c0 / 0x89440 | `Heroes::IndexByUid` / `ByUid` | search all 74 by `hero+0x1f6c` (uid, -1 = none) | asm |
| 0x81810 | `Save::Heroes(slotIdx)` | `*(0xc1dc60 + [x+0x20eb15]*8)`: pointer to the current save slot; heroes = result + 0xa480 | asm |
| 0x81fd0 | `Hero::IsValid` | `hero+0x27 != 0xff` | asm |
| 0x18e480 | `DcDropItem_CharaBone::ctor` | object size 0x5ca0, vtable 0x982418 (= 0x982408 + 0x10), name "DcDropItem_CharaBone" | asm |
| 0x2b95b0 | `DropItem::Create(kind, ..., code)` | factory; code 6 -> `2bbeb6` (alloc 0x5ca0, ctor 18e480) via jump table 0x710d24 | asm |
| 0x44d100 | `DropItemCodeByName(name)` | strcasecmp chain; **"CharaBone" -> 6** (used by the stage-data parsers 5d5f0 / 456330) | asm |
| 0x18ee90 | `CharaBone::Init` | picks the character, see 4.1 | asm (pseudo-C) |
| 0x18e780 | `CharaBone::PickRandomAlly` | shuffled scan of the 28 ally slots with exclusions, see 4.1 | asm (pseudo-C) |
| 0x18f180 | `CharaBone::Spawn` | builds the visual (name / class from the embedded hero) | inferred |
| 0x1902f0 | `CharaBone::OnPickup` | stores the embedded hero in the ally table, appends to the carried list | asm (pseudo-C) |
| 0x190090 / 0x190790 | `CharaBone::FindSlot` / `Player::AppendCarried(allyIdx)` | free slot or evictable slot; first -1 entry of the carried list | asm |
| 0x3bd5e0 | `Temple::BuildBoneList` | collects allies with flag bit 2 (0x04) that are valid and found in the table -> list window | asm (pseudo-C) |
| 0x3c3130 | `Temple::Update` (scene Town00, 13 states) | state 8: moves a hero (uid `player+0x1588`, fee `player+0x158c`) into the stock via 89320 and clears its slot | asm (pseudo-C), semantics unverified |
| 0x3be420 / 0x3bf530 / 0x3c0dd0 | `DcSysObj_Temple` / event names | events `EventRevive`, `EventRevive2`, `EventBurialStart`, `EventBurialEnd`, `EventPrayerFirst`, `EventPrayer`, beggar events | asm (strings) |
| 0xc19f0 | `Signin::IsUgcRestricted` (iface slot 0x258 via 0xb5000 -> 0x9b090) | byte `Signin+0x21c`, set to 1 at `c719e` when NpHelper+0x8e != 0 | asm |
| 0x59bd20 | `NpHelper::StartParentalCheck` | `sceNpGetParentalControlInfoA(req, user, &+0x8f (age), &+0x8c (info))`; info = {content, chat, ugc} | asm |

## 3. Data structures

### 3.1 Hero array (save slot+0xa480, 74 x 0x21d0) - bone-relevant offsets (asm)
| off | meaning |
|---|---|
| 0x00 (dword) | flags. bit0 = stock record (set by 89320 / 8a1c0 for 0x10..0x2d); **bit2 (0x04) = carried/awaiting at the Temple** (Temple list, excluded from re-picking); **bit3 (0x08) = auto-generated** (set by 8a500, evictable by 8a1c0 pass 2); mask 0xdb clears bits 2 and 5 on insert |
| 0x27 (byte) | class; **0xff = empty record** |
| 0x29 (byte) | bit 7 cleared on insert |
| 0x104..0x109 (6 B), 0x10c.. (blob compared by 17bb0), 0x110 (u32), 0x114 (u16) | **identity** compared by 89e20 (the only way a record is recognised as "the same character"). Not an account id: no PSN id is stored in the record |
| 0x116.. | short name (story_progress.md) |
| 0x1f6c (int) | uid, -1 = none; allocated from slot+0xaf1e0 at creation (story_progress.md 2.3); eviction picks the smallest uid |
| 0x21ac..0x21cc | tail words copied explicitly by the exchange / object code |
Index map: 0x00..0x0f players, **0x10..0x2d stock (30)**, **0x2e..0x49 allies (28)**; ally slot `a` = hero index `a + 0x2e`.
Extra table: `heroes + 0x9c620`, 74 x 0x16a bytes, one per hero index (purpose not decoded; copied with every bone).
Payload offsets in SAVE0.DAT: `0x50 + slot * 0xaf8d4 + 0xa480 + idx * 0x21d0` (hero), `... + 0x9c620 + idx * 0x16a` (extra).

### 3.2 EXCHANGE_BONEINFO command (id 0x7e4, size 0x46a4 + header) (asm: 4296a0 sender, 441300 / 441370 serializer)
Built on the stack: `NetCmd(this, 0x7e4)`, vtable 0xb8db58+0x10.
| off | size | meaning |
|---|---|---|
| 0x14 | u8 | (receiver reply) destination bitmask = `1 << senderSlot` (435c02) |
| 0x1b | u8 | sender slot (receiver skips its own slot `cmp [scene+0x168f0]`) |
| 0x28 | u16 | mode: **0** = plain data / end of chain, **1** = initial send (sender 4296a0), **2** = reply "I added N new" |
| 0x2a | u8 | mode 1: count 2; mode 2: number of newly added characters at the sender |
| 0x2b | u8 | hop counter (reply sets hop+1; stops when hop > 3) |
| 0x2c | 0x21d0 | entry 0 hero record |
| 0x21fc | 0x16c | entry 0 extra (0x16a copied; layout `+0x21d8..` also holds the hero tail words) |
| 0x2368 | 0x21d0 | entry 1 hero record |
| 0x4538 | 0x16c | entry 1 extra |
Serializer order (441300): u16 `+0x28`, u8 `+0x2a`, u8 `+0x2b`, hero(`+0x2c`) via a9a70, extra(`+0x21fc`) via b3390, hero(`+0x2368`),
extra(`+0x4538`). Entry stride in the *source* array = 0x233c (hero 0x21d0 + extra 0x16c). Slot 0 of the serializer `a4620` writes a u16,
`a4560` a byte (bit-exact per-field encodings of a9a70 / b3390 not decoded; **the on-wire bytes are not the raw struct**, so a server
must treat the blob as opaque).

### 3.3 CharaBone object (size 0x5ca0) (asm)
`+0x3934` embedded hero record (0x21d0); `+0x5ae0..+0x5b00` hero tail words; `+0x5b04` embedded extra; `+0x5c91` byte "no character"
(set when no eligible ally exists and none can be generated); `+0x5c93` byte from `FUN_00571ca0(0, 5)`; `+0x5c70` sub-object vtable 0x8d7b18.

### 3.4 Player-object carried list (asm)
Player object `+0x9318 .. +0x9388`: **28 ints**, each an ally slot index (0..27) or -1. Appended by 190790 (first -1), consulted by
18e780 / 190090 so a carried bone is not picked again or evicted.

## 4. Answers

### 4.1 Q1-Q3 placement: how a bone object gets its character (local, no network) (asm / pseudo-C)
1. Stage data names an object/held item "CharaBone" -> code 6 (44d100) -> factory -> ctor 18e480. **Which stages/enemies carry it,
   and the spawn probability, are in stage data parsed by 5d5f0 / 456330; not decoded.**
2. `Init` (18ee90): reads a stage-dependent value (`2a050(player+0x80)`, level-like) then `PickRandomAlly` (18e780).
3. `PickRandomAlly`: seeds with `srand`, shuffles indices 0..27 (4a030 fills the permutation), scans in that order for the first ally slot
   that is valid, `flags & 0xc == 0` (first filter), not equal (82e30 = same character) to any of the 4 players' selected allies
   (`player+0x2288 + k*0x23b0`), not in the carried list (`+0x9318..`), and not already on a CharaBone object in the stage (the object list
   built by 2c4700). Returns the ally slot or -1.
4. -1 and a free ally slot exists -> `GenerateRandomAlly` (8a500): class `rand() % 6`, level from the stage value, flag bit 3.
   If no ally is eligible and 89f40 says all 28 slots are full -> `+0x5c91 = 1` (no bone).
5. The chosen record is copied into the object at `+0x3934` (+ extra, tail words).
**Count/randomness:** one character per CharaBone object; random order via libc `rand` seeded per call. No stage/area filter on the
character itself (only the exclusions above); the stage decides how many objects exist.

### 4.2 Pickup and Temple (asm for the table writes, semantics partly unverified)
- Pickup `1902f0`: needs the carrier player (`285840`, vtable +0x150 true), looks the embedded hero up in the ally table by identity (8a570);
  if absent finds a slot via `190090` (empty slot, else any ally not carried and not selected), writes it with `8a110`
  (copy + flags & 0xdb + `87e20`), then appends the slot to the carried list (190790) and re-runs the party refresh (`2c4700`).
- Temple `3bd5e0` lists allies with flag bit 2 set; `3c3130` runs the Temple dialog/events (revive, burial, prayer; strings in the table).
  State 8 (verified table writes): `89320` stores a hero in the first empty record of 0x10..0x2d, `82bd0` clears the source slot, the
  gold counter at `slot+0xaf238` is reduced by the fee (`player+0x158c`, clamped to 0..99,999,999). Which button is revive and which is bury
  is **(unverified)**; both are local save edits. **No server call, no "used" mark anywhere.**

### 4.3 Receive rules for exchanged characters (`InsertAlly` 8a1c0, pseudo-C + cross-check) 
Found by identity (89e20):
- in the stock range 0x10..0x2d: record overwritten, flags = (flags & ~5) | 1;
- in the ally range: record overwritten (88d60), flags & 0xdb, then 86800 / 87e20 refresh;
- else not found: first empty ally slot; else (arg `evictRandom`) an ally with `(flags & 0xc) == 8` (generated, not carried); else (arg `evictOldest`)
  the ally with flag bit 2 clear and the smallest uid. Returns the slot or -1.
In OnCommand the receiver counts a character as new when `FindPtr` returned null before the insert. Reply: if `new > 0` mode 2, `+0x2a = new`,
hop = recv hop + 1, sent only when hop <= 3 (`436407`); at hop 0 it also republishes the room info (`98da0` -> `438d00` -> iface 0x310 / 0x1c8).

### 4.4 Send rules (`BuildBoneEntries` 437d60 and 4296a0)
1. Sender (every peer, at each `AC_STEP_START`, unless UGC restricted): 2 entries from 437d60 with `skipSelected = 0`: first the 4 players'
   selected allies (`player+0x2288`, `+0x4638`, `+0x69e8`, `+0x8d98`, uid at `-0x274`), valid and with an extra record; if none, random
   valid allies that are not flag bit 3, in shuffled order (4a030 with 0x1c), until `max` entries. Entry flags get `|= 0x80` at `+0x21a8` /
   `+0x2339` (and for entries that are one of the sender's own player characters, idx <= 0xf, the identity is registered via `a3270`).
2. Mode 1 request handled by a receiver: answers with up to 2 of its own entries (`skipSelected = 0`); mode 2: up to `+0x2a` entries (max 2),
   `skipSelected = 1`.
3. Dest: broadcast (`vtable+0x160(cmd, 1, 1)` on the scene man) for the sender; the reply sets the single-bit mask.

### 4.5 What the port does today (Q4)
- `np.cpp`: Toolkit2 NIDs succeed/empty (irrelevant here), `sceNpTusCreateNpTitleCtxA/CreateRequest/PollAsync` stubs, `sceNpGetParentalControlInfoA`
  zeroes 3 info bytes (no restriction, line 82) -> `Signin+0x21c = 0` -> exchange allowed.
- The exchange travels in the game data stream through the Rudp/UDP carrier (fragments of 1200 B, messages up to 256 KB, `rudp.cpp`): **no server
  involvement, nothing stubbed**. Two clients in one room should therefore already exchange allies. **Unverified at runtime**: nobody has logged
  a 0x7e4 command yet; a worthwhile check is a two-instance run starting any stage and diffing the ally slots (heroes 0x2e..0x49) before/after.
- Offline: bones work (4.1, no network gating).

## 5. Dependencies
Imports: none for bones except `sceNpGetParentalControlInfoA` (gate) and the shared Rudp stream. Subsystems: net.md 4.2/4.3 (stream, NetCtrl commands),
story_progress.md / input_save.md (hero array, save layout), friends.md (nothing shared).

## 6. Open items
1. Stage-side spawn: which stages / which objects hold "CharaBone", chance, count (parsers 5d5f0 / 456330, stage BRB data in the CPK, not
   inspected).
2. Temple semantics: which state/button is revive vs bury, cost formulas, what clears flag bit 2, the fate of the extra record (3c3130 states 0..12).
3. Exact field encoding of a9a70 / b3390 (the serializers), only needed if a server or a tool must read the blob.
4. Meaning of the 0x16a extra record (appearance/costume/skill set?) and of the registration `a3270` at the end of 437d60.
5. Runtime confirmation of the exchange (section 4.5) and per-frame stream size limits for a single 18 KB command (the per-buffer byte count word at
   `DcNet+0xc8/+0x148` is 16-bit, so 18 KB fits).
6. Whether `vtable+0x160(cmd, 1, 1)` broadcasts to all members or only the host (second/third args unknown).

## 7. Server design input (optional new feature; the original has none)
The user's picture "like PS4" does not match the code. If an asynchronous pool is still wanted:
- **Record:** opaque blob = the in-memory entry as the game builds it (hero 0x21d0 + extra 0x16c = 0x233c bytes) or the serialized form of 441300.
  The in-memory form is easier to inject (it is what `InsertAlly(heroes, ctx, ?, hero*, extra*, 1, 1)` consumes) but is a raw save-struct dump; the wire
  form must be produced by the game's own serializer. Key = identity fields (3.1: `+0x104..0x109`, `+0x10c`, `+0x110`, `+0x114`); stage key is
  meaningless because the game has no stage filter on characters.
- **Ops:** `bone_put {blob}` at stage start (or when the user quits), `bone_get {n}` random n, no `consumed` (the original never consumes; picking up
  copies the character).
- **Injection point (own code, not a PS4 call):** the cleanest hook is to synthesize a received EXCHANGE_BONEINFO (mode 0) into the command dispatcher, or
  call `8a1c0` directly, at `AC_STEP_START` time. Both are code patches beyond HLE and carry risk (save corruption if the blob is malformed: validate class
  and size, treat as untrusted input - the identity compare reads attacker-controlled bytes).
- **Recommendation:** drop server bones for the first release; the room-level P2P exchange already covers "play online and get other players' allies".
