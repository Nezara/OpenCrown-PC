# Damage / HP map (trainer)

Addresses are ELF vaddrs (image base 0, as in q.py). Evidence = capstone asm (q.py dis) + one instrumented run.

## Character HP fields (the "character" object; actors point to it at actor+0x9b8)
| offset | type | meaning | evidence |
|---|---|---|---|
| +0x000 | ptr | vtable. Player char 0x9af4e8; enemies seen 0x9c4a88, 0x91e278 | run log |
| +0x0b0 | int32 | current HP | FUN_00281b60 `mov eax,[[rdi+0x9b8]+0xb0]`; every HP store below |
| +0x104 | float | max HP | FUN_00281b80 `vcvttss2si eax,[[rdi+0x9b8]+0x104]`; FUN_000e3f50 stores it from FUN_000e3210 |
| +0x108 / +0x0b8 | float / int16 | second gauge max / current (clamped together with HP) | FUN_000e3be0 |
| +0x1e0 | ptr | owning actor | FUN_000d8360 0xd83aa |
| +0x760 | u32 | flags; mask at [0xc15b20+0x20] forces HP stores to 0 | all HP store sites |
| vt+0x9d8 / +0x9e0 | vcall | HP upper cap used by every store (`min(new, cap)`, then `max(.,0)`) | 0xdb5bc, 0xd8a72 |

Actor: vtable 0x9aded8 = player actor (the only vtable whose slot 0x1130 = FUN_001d3130); enemy actors 0x9c3530,
0x9157f0. Actor+0x9b8 = character (FUN_00281ae0). Boss gauge (DcUi_BossHpGauge, ctor FUN_004b76f0) sums
FUN_00281b60 / FUN_00281b80 over its boss list (FUN_004b7af0).

## Damage apply: FUN_000d8360 (this = victim character, rsi = attacker actor or null, rdx = hit info)
Frame: [rbp-0x200] = this, [rbp-0x238] = attacker, [rbp-0x224] = signed damage (from hit info; negated when the flag
bit 0 of [hit+0x40] is set), ebx = old HP (FUN_00281b60 at 0xd8709).
```
d8a00  mov rcx,[rbp-0x200]       <- trainer hook (5-byte jmp, displaced instruction re-run in the relay)
d8a07  mov eax,ebx
d8a09  sub eax,[rbp-0x224]       new = old - damage
d8a0f..d8a28  if old>0 && new<=0: call [vt+0x6a0] (death / KO handling, may return a new value)
d8a2e..d8a7f  clamp to [0, min(maxHP, vt+0x9e0 cap)]
d8a83  mov [r15+0xb0],eax        store HP
```
Other HP writers (not hooked): heals FUN_000db530 / FUN_000daf60, set-to-max FUN_000e3be0 / FUN_000e30a0 /
FUN_000e31c0 (17 callers) / FUN_000ec420, stat recalc FUN_000e3f50, forced HP=min(0,cap) in FUN_000cad80 and
FUN_000cda50 (instant-kill / special paths, e.g. pits - god mode does NOT cover these), ctor FUN_00037cd0.

## Trainer - REMOVED (user, 2026-10-15; not to be retried)
The "strong gear" damage-scaling trainer (hooks at 0xd8a00 / 0xd87bc / 0xd8843, commits 66aca83..51dfcb7) was removed:
in the user's play-test bosses took the scaled damage but never registered as killed - they kept their behaviour,
the game softlocked with them immortal and the camera stopped following the player. Likely cause (not investigated
further): boss death goes through its own phase / script logic, which a single hit dropping HP straight to 0 skips.
The notes below stay as RE knowledge of the damage code only.

Hook at 0xd8a00 calls on_damage(rbp, rbx) before the subtraction; installed only with DCP_STRONG=1 (or
DCP_TRAINER_LOG=1, which logs every event before adjustment). "Strong gear":
- victim is a player (local or AI ally) -> damage = max(1, damage / MULT);
- otherwise -> damage = min(damage * MULT, max(old HP, damage)) (int64 product, so no int32 overflow; the game's own
  clamp at d8a2e..d8a7f still runs). MULT = DCP_STRONG_MULT, default 50, clamped 1..1000.
Attacker not checked (player magic / projectiles are separate objects, e.g. vt 0xa962c8 for Sorcerer orbs).

Player test: actor (char+0x1e0) vtable slot 0x4d8 == FUN_002dfca0 (player actor base class; every enemy vtable seen
has FUN_0028c5f0 there). Per-class actor vtables differ: Fighter 0x9aded8, Sorcerer 0xb70290 (char vt 0xb718d0).
The earlier god/one-hit version matched only the Fighter vtables, so for the user's Sorcerer god mode never fired
(the reported "F5/F6 does not work"; the hotkeys themselves did register in last_run.log). God/one-hit were removed.

Not done: real equipment stats. Max HP = FUN_000e3210 (sums char+0x768 effect list via FUN_00009100 into an
accumulator, FUN_000821f0 with base stats at char+0xa4, capped by vt+0x9d8); FUN_000e3f50 stores it and 7 more
derived stats at +0x108..+0x120. Weapon AT (menu "AT 8-12") not located; menu numbers are unchanged by the trainer.

Verified (user's Sorcerer save copy, Work/stage4/in_stage.txt, DCP_TRAINER_LOG=1): off, enemies (200 HP) take 4-5 per
orb, ~40 events; on, 2 events, each kill from full HP; a player hit of 33 on 313/313 flagged player 1 (-> 1).
Not checked: other classes (Amazon, Dwarf, Elf, Fighter with new test), bosses, online, pit/special kill paths.
Play-test 2026-10-15: user (Wizard = our "Sorcerer") reported goblins (early castle stage) taking no damage with
strong gear on. Ruled out headless (2026-10-15): (1) player test - a scan of the loaded image finds exactly 7 vtables
with slot 0x4d8 = FUN_002dfca0: the 6 class actors (Fighter 0x9aded8, Wizard 0xb70290, 0x9946a8, 0x99f730, 0xa79370,
0xae8dc0; 0x9946a8 / 0x99f730 seen as AI-ally attackers) and the abstract base 0x8d4220 (one pure-virtual slot), so no
enemy can pass it unless it is one of those classes; (2) death veto - vt+0x6a0 is FUN_000dbe00 for all chr vtables
seen (enemies, players, projectile chr 0xa914f0), it returns 0; (3) relay - no xmm is live across 0xd8a00.
Hits that never reach the HP store at 0xd8a00 (unaffected by the trainer):
- d8790: hit result +0x34 (rbp-0x15c) non-zero -> if hit type r12 == 4, or r12 == 0 and FUN_0028b190(victim actor)
  (actor+0x103 in {0xaf, 0xb0, 0xb4}), the amount goes out as message 0x1428 (vt+0x168, payload @+0x28 amount,
  @+0x2c attacker id FUN_00286d20, @+0x30 flag) and HP is not touched here; any other r12 skips HP entirely.
- d8669: hit types 2/3 (guard) -> d8843 / FUN_000d52b0 with FUN_0018a720(hit) - no HP store.
Both now have log-only hooks (d87bc, d8843). With strong gear on the trainer writes a compact log to stderr
(launcher: logs/last_run.log): the first 40 events and the first 3 per (path, victim actor vtable), as
"trainer: hit <before> -> <after> hp ..." / "deferred hit" / "guard-type hit". Next goblin play-test log decides.
Stat route tried (hook after FUN_000e3f50's last store, x10 on player +0x10c/+0x110/+0x114): Wizard orb damage only
4 -> 6, so orb damage is mostly not from those derived stats; dropped. Damage formula for reference: FUN_0029dcb0 ->
FUN_000e6230(victim chr, hit result) -> FUN_000e5780(victim chr, attacker actor, ..): attacker chr +0x10c/+0x110,
FUN_000e4a10(attacker chr, magic flag = hit [+0x40] bit 21) reads +0x114; victim +0x118/+0x11c and FUN_000e50c0 =
defence side; result +0x1c = damage; then +0x120 of both via FUN_0015ee30, FUN_000f9330 (victim). Wizard derived stats
seen: 10c 11, 110 8, 114 13, 118 8, 11c 7, 120 15 (names unconfirmed).
