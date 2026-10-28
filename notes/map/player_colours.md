# Player colours and online player numbering

## Summary
Two questions for the room chat overlay: the per-player colour (1P..4P) and which player number a room member gets.
Rating **routine**. 1P red, 2P blue, 3P yellow, 4P green (also confirmed in-game by the user).

## 1. Colour table
| Address | What | Confidence |
|---|---|---|
| 0x74d3a0 | 4 x `ScePadLightBarParam` {r,g,b,pad}: 1P (127,0,0) red, 2P (0,0,127) blue, 3P (127,95,0) yellow, 4P (0,127,0) green | asm (bytes read) |
| 0x398fe0 | cursor per-frame tail: `idx = (int16)vfunc[+0x1c8](player obj) - 11`, idx 0..3; `Pad_SetLightBar(handle, &table[idx])` via 0x5a3100 (call at 0x39929f) | asm |
| 0x74d190..0x74d1f0 | 7 rows x 4 u32 UI part ids, per row `base + 7*player` (used by 0x3992c0 / 0x399420, same idx) | asm (values); meaning "per-player sprite parts" inferred |

No RGB table for on-screen UI was found in code: all 17 functions using the `vfunc 0x1c8 - 11` player index
(0x398330..0x39bf40, the fairy cursor family) only reference the light-bar table and part-id tables, so the on-screen
player colours are most likely baked into per-player sprite art (unverified). For a PC overlay the light-bar values
doubled (255,0,0 / 0,0,255 / 255,190,0 / 0,255,0) are the closest code-side source (inferred).

## 2. Player index of a room member
- DcNet peer index = `(memberId >> 4) - 1` (net.md 5.3, asm).
- Player index = index in the player table `sys+0x16918` (stride 0x1c, 4 entries, value = peer index, 0xff free).
  Host creates the table with itself in entry 0 (0x9dbd0 / 0x9df90, asm).
- Join: the leader handles cmd 0x3e8 in 0xa1710 (0xa17bc..0xa187f): **first free entry (lowest index with 0xff)** gets
  the sender's peer index; the joiner learns its own index from cmd 0x3ee (byte to `sys+0x168f0`, 0xa210f); the full
  table goes out with 0x3ef. asm.
- Leave: 0xa06c0 clears the lost peer's entry (0xa0b48), so the next joiner reuses the lowest free player index - the
  same "lowest free" rule as the lobby's member ids.
- So player index == memberId/16 - 1 in the normal flow, but it is **not computed from the member id** (inferred
  equivalence). Divergence cases: two joins whose 0x3e8 reach the leader in a different order than the lobby issued
  ids; a member id freed/reused by the lobby before the game drops the old table entry (or vice versa); the host
  (0x10, entry 0) leaving: entry 0 is freed and the lobby gives 0x10 to the next joiner - still consistent.
- Open: PlayerMgr slot (`vfunc 0x1c8 - 11`) == player-table index is from traces (town_split.md), not read in asm.
