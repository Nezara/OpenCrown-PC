# Damage / HP map

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

Player actors: vtable slot 0x4d8 == FUN_002dfca0 (player actor base class; every enemy vtable seen has FUN_0028c5f0
there). Exactly 7 vtables in the loaded image have it: the 6 class actors (Fighter 0x9aded8, Wizard 0xb70290,
0x9946a8, 0x99f730, 0xa79370, 0xae8dc0) and the abstract base 0x8d4220.

## Damage apply: FUN_000d8360 (this = victim character, rsi = attacker actor or null, rdx = hit info)
Frame: [rbp-0x200] = this, [rbp-0x238] = attacker, [rbp-0x224] = signed damage (from hit info; negated when the flag
bit 0 of [hit+0x40] is set), ebx = old HP (FUN_00281b60 at 0xd8709).
```
d8a00  mov rcx,[rbp-0x200]
d8a07  mov eax,ebx
d8a09  sub eax,[rbp-0x224]       new = old - damage
d8a0f..d8a28  if old>0 && new<=0: call [vt+0x6a0] (death / KO handling, may return a new value)
d8a2e..d8a7f  clamp to [0, min(maxHP, vt+0x9e0 cap)]
d8a83  mov [r15+0xb0],eax        store HP
```
vt+0x6a0 is FUN_000dbe00 for all chr vtables seen (enemies, players, projectile chr 0xa914f0); it returns 0. Boss
deaths go through their own phase / script logic, not only this HP store.

Hits that never reach the HP store at 0xd8a00:
- d8790: hit result +0x34 (rbp-0x15c) non-zero -> if hit type r12 == 4, or r12 == 0 and FUN_0028b190(victim actor)
  (actor+0x103 in {0xaf, 0xb0, 0xb4}), the amount goes out as message 0x1428 (vt+0x168, payload @+0x28 amount,
  @+0x2c attacker id FUN_00286d20, @+0x30 flag) and HP is not touched here; any other r12 skips HP entirely.
- d8669: hit types 2/3 (guard) -> d8843 / FUN_000d52b0 with FUN_0018a720(hit) - no HP store.

Other HP writers: heals FUN_000db530 / FUN_000daf60, set-to-max FUN_000e3be0 / FUN_000e30a0 / FUN_000e31c0
(17 callers) / FUN_000ec420, stat recalc FUN_000e3f50, forced HP=min(0,cap) in FUN_000cad80 and FUN_000cda50
(instant-kill / special paths, e.g. pits), ctor FUN_00037cd0.

## Stats
Max HP = FUN_000e3210 (sums char+0x768 effect list via FUN_00009100 into an accumulator, FUN_000821f0 with base stats
at char+0xa4, capped by vt+0x9d8); FUN_000e3f50 stores it and 7 more derived stats at +0x108..+0x120. Weapon AT
(menu "AT 8-12") not located.

Damage formula: FUN_0029dcb0 -> FUN_000e6230(victim chr, hit result) -> FUN_000e5780(victim chr, attacker actor, ..):
attacker chr +0x10c/+0x110, FUN_000e4a10(attacker chr, magic flag = hit [+0x40] bit 21) reads +0x114; victim
+0x118/+0x11c and FUN_000e50c0 = defence side; result +0x1c = damage; then +0x120 of both via FUN_0015ee30,
FUN_000f9330 (victim). Wizard derived stats seen: 10c 11, 110 8, 114 13, 118 8, 11c 7, 120 15 (names unconfirmed).
