# Volume: the game's sound options and our launcher volumes

Implementation: `port/src/hle/volume.{h,cpp}` (category hook), master gain in `port/src/hle/audio.cpp` (`to_float`),
launcher Sound tab (`port/tools/launcher/main.cpp`, launcher.ini keys `volume`, `volume_music`, `volume_sfx`,
`volume_voice`, `mute`). Env: `DCP_VOL_MASTER`, `DCP_VOL_MUSIC`, `DCP_VOL_SFX`, `DCP_VOL_VOICE` (0..100, default 100),
`DCP_MUTE=1`, `DCP_VOL_LOG=1` (log every category set).

## Categories (from the dump: DRACRO.CPK `snd/dc_sound_acf.h`, the ACF's generated header)
17 categories; 4 per playback. Main group: **0 BGM, 1 SE, 2 VOICE, 3 NARRATION**. Sub: 4 BGM_VHL, 5..10 PLAYER_<class>,
11 ENEMY_ALL, 12 SE_OTHER, 13 NPC, 14 SE_SYSTEM, 15 SE_FOOT. 16 = All.

## Functions
| addr | name | evidence |
|---|---|---|
| 5fa410 | criAtomExAcf_GetCategoryIndexById | ACF at [0xebf848]; lookup via 60d290; "E2013013001:Specified category id '%d' is not found." |
| 5c3910 | criAtomExCategory_SetVolumeById(id, float) | 5fa410 -> index; category table [[0xebdc20]+0x18] stride 0x108; stores volume at +0xb0, applies via 5d7770 under lock 5c7250/5c7270 |
| 5c39a0 | criAtomExCategory_xxxById(id, int) | same lookup, -> 5c39c0 (used by 585ac0, not volume) |
| 585a60 | game SetCategoryVolume(sound, u32 id, u8 level) | ready check [FUN_00568800()+0xb0] != 0 && [..+0x1eb8]&1; then `vol = min(1.0 (0x775a20), level / 127.0 (0x775a1c))`, tail-jmp 5c3910 |
| 5842e0 | game SetBgmPlayerVolume(bgm, u8 level) | same ready check; `level / [0x775a10]` -> 5cba40 (player set-volume) on player [bgm+0x28] |
| 68300 | options apply (virtual, no direct callers) | see below; runs **every frame** (1488 calls in 1500 frames) |

## FUN_00068300 (options apply), asm 0x68502..0x6860b
Option dword at [this+0xc4] (four bytes, each 0..10 from the save/options menu). Each byte: `min(1.0, b / 10.0)` (0x6afa20 = 10.0,
0x6afa24 = 1.0) `* 127.0` (0x6afa28) truncated to int, then:
- byte +0xc4 -> 5842e0([sound+0x1de8], v)   (BGM option: the BGM player's volume, not a category)
- byte +0xc5 -> 585a60(sound, **3**, v)  NARRATION
- byte +0xc6 -> 585a60(sound, **1**, v)  SE
- byte +0xc7 -> 585a60(sound, **2**, v)  VOICE
585a60 is only called from here; 5c3910 only from 585a60; nobody sets category 0.

## Our hook
5-byte `jmp` at **eboot+0x585a95** (first instruction after the ready check: `movzx eax, bl` + `vmovss xmm1,[rip..]`, bytes
checked `0f b6 c3 c5 fa 10 0d`). Branch targets in 585a60 are only 585a90 / 585a95, so nothing lands in the patched bytes.
Relay: `mov edi, r14d; movzx esi, bl; pop rbx; pop r14; pop rbp; mov rax, on_category; jmp rax` (tail call, rsp back at the
return address). `on_category(id, level)` computes the game's value `min(1, level/127)`, multiplies by music/sfx/voice
(2 and 3 = voice), calls 5c3910 itself; on id 3 (first of the three calls) it also calls 5c3910(0, music).
Installed only if a category volume is not 100% (or DCP_VOL_LOG). The game's own option values still apply on top.
Master: `volume::master()` multiplies every port's samples in the sceAudioOut shim before the sink (SDL / paced/WAV).

## Verification (2026-10-15, headless `--render`, DCP_AUDIO_WAV captures, RMS per 2 s window)
- Boot, 1500 frames: base startup sound RMS 124 / peak 6190; DCP_VOL_MASTER=25 -> 31 / 1547; DCP_VOL_SFX=0 -> 0 (it is an SE);
  DCP_VOL_VOICE=0 -> unchanged; DCP_MUTE=1 -> 0.
- Title -> town (user save copy, Work/stage5/in_town_to_lucain.txt, 4000 frames): base music RMS ~5000-6000 / peak 17548;
  DCP_VOL_MUSIC=0 -> 0 everywhere; SFX=0+VOICE=0 -> same as base; all at 50 -> exactly half (peak 8774).
- Voice: no voice line occurred in the scripted windows, so only the value path is verified (DCP_VOL_LOG: category 2/3
  `game 1.000 -> 0.400` with DCP_VOL_VOICE=40).
