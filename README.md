<h1 align="center">OpenCrown PC</h1>

<p align="center"><i>Dragon's Crown Pro Multiplayer, running natively on Windows</i></p>

<p align="center">
  <img src="docs/img/hero.jpg" alt="The Fighter in the Ancient Temple ruins, running in OpenCrown PC" width="100%">
</p>

<p align="center">
  <a href="https://github.com/Nezara/OpenCrown-PC/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/Nezara/OpenCrown-PC?label=release&color=c08a2a"></a>
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows%2010%20%2F%2011%20x64-3a5a8a">
  <img alt="Requires your own copy" src="https://img.shields.io/badge/requires-your%20own%20PS4%20dump-8a3a4a">
  <img alt="Online" src="https://img.shields.io/badge/online-peer--to--peer-3a8a5a">
</p>

<p align="center">
  <b><a href="#getting-started">Getting started</a></b> ·
  <a href="#the-launcher">Launcher</a> ·
  <a href="#controls">Controls</a> ·
  <a href="#playing-online">Online</a> ·
  <a href="#how-it-works">How it works</a> ·
  <a href="#building-from-source">Building</a> ·
  <a href="#faq">FAQ</a>
</p>

---

**OpenCrown PC** lets you play **Dragon's Crown Pro** on a Windows PC from a dump of your own PS4 copy. It is a native launcher, not an emulator: the game's own code runs directly on your CPU, while OpenCrown provides the PS4 system libraries, the renderer, input, saves and online play.

This is a hobby project made by one person, with a lot of help from an AI around the online stuff. It isn't affiliated with or endorsed by Vanillaware, Atlus or Sony.

> [!IMPORTANT]
> **No game files are included, and none will ever be.** You need your own dump of Dragon's Crown Pro (CUSA10454). See [Your dump](#your-dump).

## Support the people who made it

Dragon's Crown Pro is a beautiful game made by a small studio. If you enjoy it here, please make sure they get paid for it.

- 🛒 **Buy Dragon's Crown Pro** on the [PlayStation Store](https://store.playstation.com/en-us/product/UP2611-CUSA10454_00-DRAGONSCROWNPRO1).
- 🦄 **Play Unicorn Overlord**, Vanillaware's tactical RPG, on the [PlayStation Store](https://store.playstation.com/en-us/product/UP0177-PPSA09770_00-UNICORNOVERLORD0).
- ⚔️ **Support Vanillaware's latest game**, [Muramasa: Revenant Blades on Steam](https://store.steampowered.com/app/4253770/Muramasa_Revenant_Blades/). It's the best way to tell them there's a PC audience for their games.

## Features

- 🖥️ **Runs natively on Windows.** No PS4 emulator; the game's code runs as a regular Windows program.
- 🎨 **Rewritten renderer** for OpenGL 3.3, at 1080p or **4K** (the PS4 Pro mode).
- 🎮 **Gamepads** (Xbox, DualShock / DualSense and others through SDL3), up to 4 local players, with rumble.
- ⌨️ **Keyboard and mouse** with mouse attacks and the fairy pointer on the mouse, all rebindable.
- 🔄 **Dynamic button prompts.** On-screen prompts switch between PlayStation buttons and your keyboard / mouse keys, following whichever you used last, and show your own bindings.
- 💾 **PS4-compatible saves.** Save files are byte-identical to the console's, so you can bring your PS4 progress over and take it back.
- 🌐 **Online co-op** for up to 4 players, using the game's own netcode over direct peer-to-peer connections, with room codes and friend codes.
- ⏭️ **Optional story and tutorial skip** on new characters (details [below](#what-is-changed-in-the-game)).
- 🪟 **Launcher** for your dump, saves, controls, display, online settings and bug reports.

## Screenshots

<table>
  <tr>
    <td width="50%"><img src="docs/img/stage1-fight.jpg" alt="Fighting in the Ancient Temple"></td>
    <td width="50%"><img src="docs/img/launcher-game.jpg" alt="The launcher's Game page"></td>
  </tr>
  <tr>
    <td align="center"><sub>Stage 1, the Ancient Temple</sub></td>
    <td align="center"><sub>The launcher, ready to play</sub></td>
  </tr>
</table>

<sub>All screenshots are from OpenCrown running a personal dump. The game art belongs to Vanillaware / Atlus.</sub>

## Requirements

| | |
|---|---|
| **OS** | Windows 10 or 11, 64-bit |
| **GPU** | Anything with OpenGL 3.3 (most GPUs from the last ~12 years) |
| **CPU** | A modern x86-64 CPU (the game's code runs on it directly) |
| **Game** | Your own dump of **Dragon's Crown Pro (CUSA10454)** |
| **Online** | An Internet connection. UPnP on your router helps but isn't required. |

## Your dump

OpenCrown needs the game's files from **a PS4 you own**: the **decrypted** `eboot.bin` together with the rest of the game folder (the files the game reads at run time and the `sce_sys` folder).

> [!WARNING]
> **PKG files won't work.** A PKG from the PS4 or the PlayStation Store is encrypted. OpenCrown can't decrypt it, and it never will. It also won't help you get the game any other way. Dump the copy you own with the usual homebrew tools for your console.

Your dump is only ever **read**. OpenCrown never writes to it or modifies it; the small patches described below (34 bytes in total) are applied in memory each time you play.

## Getting started

1. **Download** the latest `OpenCrownPC-*-win64.zip` from [Releases](https://github.com/Nezara/OpenCrown-PC/releases/latest).
2. **Unzip** it anywhere, for example `C:\Games\OpenCrownPC`. Keep the three files together: `OpenCrownPC Launcher.exe`, `dcp_run.exe` and `SDL3.dll`.
3. **Run `OpenCrownPC Launcher.exe`.** Windows SmartScreen may warn you because the program isn't code-signed (it's a hobby project). Click *More info → Run anyway*.
4. On the **Game** page, **browse to your dump folder** (the one containing `eboot.bin`). The launcher checks the folder and tells you if anything is missing.
5. The first time, it asks **where to keep your saves**. The default is `Documents\Dragons Crown Pro\saves`.
6. Press **Play**. 🎉

Your settings, saves and logs all live in `Documents\Dragons Crown Pro\` unless you choose a different save folder.

## The launcher

The launcher is a small window with a sidebar. Once your dump is set, it uses the game's own artwork from your dump, so the exe itself contains no game art.

| Page | What it does |
|---|---|
| **Game** | Choose and check your dump folder, then press **Play**. The game starts in its own window; the launcher writes everything it prints to `logs\last_run.log`. |
| **Controls** | Rebind every keyboard and mouse action and adjust the fairy pointer speed. Saved to `input.ini`, which you can also edit by hand. Delete it to restore the defaults. |
| **Display** | Windowed or borderless fullscreen, window size, and **4K rendering** (PS4 Pro mode). |
| **Online** | Your online name, your **friend code**, adding friends by code, incoming and outgoing friend requests, UPnP, a connection test and the lobby server address. |
| **Save Folder** | Where your saves live. Point it at a folder with a `SAVEDATA00` from your PS4 to keep playing your console progress. |
| **Report a Bug** | Sends a short note plus the end of your last log, so problems can be fixed. You can **preview** exactly what is sent first. See [Privacy](#privacy). |

Launcher settings are stored in `launcher.ini` next to your saves and logs.

## Controls

Gamepads work out of the box with the PS4 layout. Keyboard and mouse defaults are below; everything can be changed on the **Controls** page.

| Action | Default | | Action | Default |
|---|---|---|---|---|
| Move | **W A S D** | | Attack (□) | **Left mouse** |
| Jump (✕) | **Space** | | Special (○) | **Right mouse** |
| Item / interact (△) | **E** | | Evade / L1 · R1 | **Left Shift** |
| L2 / R2 | **Z** / **C** | | L3 / R3 | **Left Ctrl** / **V** |
| D-pad | **Arrow keys** | | Pause (Options) | **Esc** |
| Character menu (touchpad) | **Tab** | | Town map | **M** |
| Look / right stick | **I J K L** | | Fairy pointer | **Hold middle mouse** |

**The fairy pointer:** hold the middle mouse button and move the mouse to steer Rannie's fairy to treasure, doors and runes. Press **E** to tap while pointing. Mouse attacks are paused while you hold it. If you'd rather not hold a button, you can set an on/off key.

**Button prompts follow you:** the game's on-screen prompts ("Press ✕", "Press TOUCHPAD to open Town Map" and so on) show PlayStation buttons while you use a gamepad. As soon as you touch the keyboard or mouse they switch to key caps and mouse icons labelled with your current bindings, and they switch back when you pick the pad up again. The key and mouse icons are OpenCrown's own drawings; the renderer swaps them in for the game's button sprites.

**Handy extra:** **F1** shows performance stats in the window title.

## Saves

Saves use the PS4's own format, byte for byte, so the same files work on your console, on PS4 emulators and here. To continue a console playthrough, copy your `SAVEDATA00` folder into your OpenCrown save folder (or point the **Save Folder** page at it). Copy it back the same way.

Keep a backup of your saves before moving them between devices. It's free insurance.

## What is changed in the game

Very little. The PS4 system libraries the game calls (kernel, file system, audio, controller, saves, fonts, networking) are **translated** to Windows equivalents, and the PS4 **renderer is rewritten** to draw the game's frames with OpenGL. **Everything else is the game's own native code**, running unchanged.

The game's code is patched in **six places, 34 bytes in total**. The patches are applied in memory each time you play and never written to your dump:

- **Story skip** (2 bytes). The game has its own "start from the second half" skip, but it normally only offers it once a class has cleared the game. One 2-byte change offers it to every new character.
- **Skip reminder** (26 bytes in four places). If you say no to the story skip, the game asks once more with a reminder that online play unlocks partway through the story, then respects your answer. Four small jumps send the game to a few lines of OpenCrown's own code that show the reminder.
- **Tutorial question** (6 bytes). Instead of starting the tutorial automatically, the game asks *"Begin the tutorial?"* on a save's first character.

Each patch checks that the original bytes are exactly what it expects first; if they aren't, it leaves that spot alone.

## How it works

<p align="center">
  <img src="docs/img/how-it-runs.svg" alt="Diagram: your dump goes into the loader, which runs the game's own code; the game talks to translated PS4 libraries and a rewritten PC renderer" width="100%">
</p>

The PS4 and a PC share the same kind of processor (x86-64), so the game's machine code doesn't need to be emulated or recompiled. OpenCrown's **loader** reads your decrypted `eboot.bin`, maps it into memory the way the PS4 would, links the game's calls to the system libraries to OpenCrown's own implementations, and then jumps into the game's entry point.

From there the game runs exactly as it was written. Whenever it asks the "PS4" for something (open a file, play a sound, read the controller, save the game, draw a frame), the request goes to OpenCrown's code, which does the same job with Windows, SDL3 and OpenGL. The renderer is the biggest piece: the game hands over PS4 GPU command lists, and OpenCrown turns them into OpenGL draws, decoding the PS4's tiled textures and shaders along the way.

The `dcp_run.exe` next to the launcher is the program that actually runs the game; the launcher sets things up and starts it for you.

## Playing online

<p align="center">
  <img src="docs/img/online.svg" alt="Diagram: both players briefly talk to the lobby server to find each other, then connect directly peer to peer" width="100%">
</p>

Online play uses **the game's own matchmaking and netcode**. The lobby server only introduces players to each other:

1. **Host or join a room.** When you go online in the game, it opens a room on the lobby server. Others can find it in the room list, or join with a **room code** or from your **friends** list.
2. **Handshake.** The server swaps the players' network addresses so they can reach each other.
3. **Peer to peer.** The players connect **directly** over UDP. All gameplay traffic goes between the players, never through the server. NAT traversal uses STUN and hole punching, plus UPnP if your router allows it.

**Bones** (the ally records you find in the catacombs and resurrect at the temple) are swapped by the game itself over that same peer connection when a stage starts, just like on PS4. There's no central bones database.

The lobby is a small open-source Cloudflare Worker that lives in [`server/lobby-worker`](server/lobby-worker). Anyone can run their own and point the **Online** page at it.

**Friends:** each player has a friend code like `ABCD-1234`. Add someone by their code on the **Online** page. Once they accept, you can see when they're online and use *Join Friend's Room* in the game.

> [!NOTE]
> Online play unlocks at the same point in the story as on PS4. Taking the story skip on a new character gets you there right away.

## Privacy

- **The lobby server sees:** your online name, a random account ID, your public IP and port (needed to connect players), your rooms and your friends list. It doesn't see or relay gameplay.
- **Other players see:** your online name and your IP address, the same as in any peer-to-peer game.
- **Bug reports** contain your short note and the last part of `last_run.log`. Before sending, your Windows user name, PC name, home folder path and account secret are scrubbed out, and you can preview the whole report first. Reports are rate limited and only kept to fix bugs.
- Nothing is sent anywhere unless you go online or send a bug report.

## Known issues

This is a 1.0 from a hobby project, so expect a few rough edges:

- **Two players behind the same router** may not be able to connect to each other yet. Different houses work fine.
- **Very strict networks** (some mobile or carrier-grade NAT connections) can stop the direct connection. There's no relay server yet.
- **Friend status** can take about 10 seconds to update.
- **Windows user names with non-English characters** (e.g. `C:\Users\Jürgen`) can break file paths. Choosing a save folder with a plain path works around it.
- **Odd or unusual text can appear briefly on scene transitions.** The PS4's system text library isn't included, so some of the text it would supply has to be swapped for generic stand-in text. Most of these cases have been caught and fixed, but a few outliers may still slip through. If you spot one, a bug report saying where it happened helps.
- The programs **aren't code-signed**, so SmartScreen and some antivirus tools may be suspicious at first.

Found something else? Use the launcher's **Report a Bug** page or [open an issue](https://github.com/Nezara/OpenCrown-PC/issues).

## Building from source

You'll need:

- **CMake** 3.24+ and **Ninja**
- **MinGW-w64 GCC** (UCRT build, e.g. [WinLibs](https://winlibs.com/)) with C++20 support

SDL3, Dear ImGui, FreeType and the other dependencies are downloaded by CMake on the first configure.

```bash
cmake -S port -B port/build -G Ninja
cmake --build port/build
```

That gives you `OpenCrownPC Launcher.exe`, `dcp_run.exe` and `SDL3.dll` in `port/build`, plus a few developer tools (`dcp_cpk`, `dcp_tex`, `dcp_audio`, `dcp_viewer`, ...) and the tests (`test_*.exe`). The tests run without any game files.

The lobby server builds and tests separately with Node.js; see [`server/lobby-worker/README.md`](server/lobby-worker/README.md).

### Repository layout

| Folder | What's inside |
|---|---|
| `port/src/loader` | Loads the eboot and links it against our libraries |
| `port/src/hle` | The translated PS4 system libraries |
| `port/src/render` | The rewritten renderer (PS4 GPU → OpenGL) |
| `port/src/formats` | Readers for the game's file formats (CPK archives, textures, audio, ...) |
| `port/src/net` | Lobby client, peer-to-peer transport, STUN / UPnP, friends |
| `port/tools` | The launcher, `dcp_run` and developer tools |
| `port/tests` | Unit tests (synthetic data only, no game files) |
| `server/lobby-worker` | The lobby server (Cloudflare Worker, TypeScript) |
| `notes/map` | Reverse-engineering notes on how the game's systems work |
| `Work/tools`, `Work/ghidra_scripts` | Analysis scripts used while researching the game |

## FAQ

<details>
<summary><b>Is this an emulator?</b></summary>

No. An emulator pretends to be the whole console. OpenCrown runs the game's own code directly and only replaces the PS4 parts it talks to (system libraries and graphics). That's why it can be small and fast, but it also means it only runs Dragon's Crown Pro.
</details>

<details>
<summary><b>Can I download the game somewhere and use it?</b></summary>

No. Please buy the game and dump your own copy. This project won't link to, host or help with pirated copies, and it can't decrypt PKG files.
</details>

<details>
<summary><b>Can I play online with people on a real PS4?</b></summary>

No. OpenCrown players play with each other through the OpenCrown lobby. PlayStation Network isn't involved.
</details>

<details>
<summary><b>Will my PS4 save work?</b></summary>

Yes. Copy your `SAVEDATA00` folder into the save folder. The format is identical, and it works the other way round too.
</details>

<details>
<summary><b>Are there cheats or trainers?</b></summary>

No, and there won't be. The goal is to play the game as it was made.
</details>

<details>
<summary><b>Why is SmartScreen / my antivirus warning me?</b></summary>

The programs aren't code-signed (signing certificates cost money), and a program that loads another program's code into memory looks unusual to antivirus heuristics. The full source is right here if you'd like to check it or build it yourself.
</details>

## Credits

- **Vanillaware** and **Atlus** for making Dragon's Crown, a game that's still gorgeous years later.
- **Nezara**, project lead, bug hunting, diagnosing and a lot of testing, again and again and again. 
- **Claude** (Anthropic), coding and reverse-engineering contributor.
- The [shadPS4](https://github.com/shadps4-emu/shadPS4) project, used as a reference to compare against during early stages of development. 
- [SDL](https://www.libsdl.org/), [Dear ImGui](https://github.com/ocornut/imgui), [FreeType](https://freetype.org/) and [MiniUPnPc](https://miniupnp.tuxfamily.org/), which make this possible.

## Legal

*Dragon's Crown* and *Dragon's Crown Pro* are trademarks of their respective owners. OpenCrown PC is an independent, non-commercial fan project. The repository contains no code, art, audio or other data from the game (apart from the screenshots above, shown to illustrate the project), and it requires a legally obtained copy that you dumped yourself. It is not affiliated with or endorsed by Vanillaware, Atlus, SEGA or Sony Interactive Entertainment.

## Help keep the server running

OpenCrown is free and always will be. Donations go **only** toward hosting the online server; there are no perks, rewards or early builds, and everyone gets the same thing whether they donate or not.

<p align="center">
  <a href="https://ko-fi.com/nezara"><img alt="Support the server on Ko-fi" src="https://img.shields.io/badge/Ko--fi-support%20the%20server-ff5e5b?logo=ko-fi&logoColor=white"></a>
</p>
