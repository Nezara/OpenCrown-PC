# OpenCrown PC

OpenCrown PC runs **Dragon's Crown Pro** (PS4) natively on Windows from your own console dump. It includes a launcher and online multiplayer. It is a fan project and is not affiliated with Vanillaware or Atlus.

## Support the game

Please buy the game: **[Dragon's Crown Pro on the PlayStation Store](https://store.playstation.com/en-us/product/UP2611-CUSA10454_00-DRAGONSCROWNPRO1)**.

Vanillaware's latest title is also worth supporting: **[Muramasa: Revenant Blades on Steam](https://store.steampowered.com/app/4253770/Muramasa_Revenant_Blades/)**.

## You need your own dump

This repository contains no game code, assets or data. To play, you need your own dump of Dragon's Crown Pro (CUSA10454) taken from a PS4 you own: the decrypted `eboot.bin` and the game files. A PKG from the PS4 is encrypted. OpenCrown PC cannot decrypt it and never will.

## What is changed in the game

Very little. The PS4 system libraries the game calls are translated to Windows equivalents, and the PS4 renderer is rewritten for PC. Everything else is the game's own native code running unchanged.

Two byte patches are applied in memory at run time; your dump on disk is never modified. One offers the game's own story skip to every new character (normally it only appears once a class has cleared the game). The other asks "Begin the tutorial?" instead of starting the tutorial automatically.

## The launcher

Run `OpenCrownPC Launcher.exe` and point it at your dump folder. It checks that `eboot.bin` and the game files are there. From the launcher you can pick a save folder, rebind keyboard/mouse and gamepad controls, set display options (including 4K / PS4 Pro mode), and configure online play and friends. **Play** starts the game in its own window. Settings are kept in `launcher.ini`, and each run writes `logs/last_run.log`. Saves are byte-identical to the PS4 format, so you can move them between console and PC.

## Online

The game's own online code does the work, and the lobby server only helps players find each other. The server (a small Cloudflare Worker, `server/lobby-worker`) lists rooms, handles join codes and friend codes, and swaps the players' network addresses for the initial handshake. After that the players connect directly to each other over UDP (STUN, hole punching and UPnP), and gameplay traffic never passes through the server. The game shares bones (ally records) itself over that peer connection, the same way it does on PS4.

## Building

You need CMake, Ninja and a MinGW-w64 GCC (UCRT) toolchain with C++20. SDL3 and the other libraries are fetched by CMake. The project lives in `port/`:

```
cmake -S port -B port/build -G Ninja
cmake --build port/build
```

## Credits

Built by Nezara with Claude (Anthropic) as a coding contributor.
