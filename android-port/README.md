# OpenCrown Mobile (Android)

The Android build of OpenCrown, **in beta**: `OpenCrownMobile-*-arm64.apk` on the [Releases](https://github.com/Nezara/OpenCrown-PC/releases/latest) page. Unlike the PC and Linux builds it runs the
game's **PS Vita** version: the user's own Vita eboot runs under the Dynarmic ARM JIT, every system module it imports
is our own implementation (HLE), and the picture is drawn with Vulkan. No game files are included; players supply
their own dump.

## What you need
- Your own decrypted PS Vita dump of Dragon's Crown (US, PCSE00019): the base game and the **1.09 update**.
- An Android 9+ device with a 64-bit ARM CPU and Vulkan 1.1. A controller is recommended; on-screen touch controls
  appear while none is connected.

Copy the dump to `Internal storage/OpenCrown/dump/` (the base game in `Game/`, the update in `Update/`) and start
**OpenCrown Mobile**. Saves, settings, `identity.ini` (your online account) and `log.txt` live in
`Internal storage/OpenCrown/`. Saves from OpenCrown PC can be imported on the launcher's Save Folder page.

## The launcher
Same pages as the PC launcher: **Game** (dump check), **Save Folder** (import a PC save, export your save for the PC), **Controls** (controller
buttons, tap an action and press a button; touch-control opacity), **Display**, **Online** (player name, lobby server,
UPnP, Test connection, friends with friend codes) and **Report a Bug** (with a preview of exactly what is sent).

## Online
The Vita 1.09 update speaks the same network protocol as Dragon's Crown Pro, so OpenCrown Mobile and OpenCrown PC play
together through the same lobby server: rooms are found with the game's own search and play runs peer-to-peer.
Online play starts at the stables in town (a character needs the first half of the story done, or the story skip).

## In the game
- Optional story and tutorial skip on new characters, as on PC.
- Text entry (the saved chat messages) opens the Android keyboard.
- In town, tapping the screen opens the Town Map: that is the Vita game's own behaviour.

## Building
On Windows with Git Bash: Android SDK command-line tools (platform 35, build-tools 35.0.0, NDK 27.3.13750724),
JDK 21, CMake and Ninja, plus unpacked source trees of SDL 3.4.18, Boost 1.86.0 (headers), Dynarmic (Vita3K fork,
commit 1fcd216), FreeType 2.13.3, Vulkan-Headers and SPIRV-Headers 1.3.296, Mbed TLS 3.6.2 and miniupnpc 2.2.8 in one
tools folder:

```sh
DCP_TOOLS=/path/to/tools android/build_apk.sh            # android/build_apk.sh install = adb install afterwards
```

The script builds `libmain.so` with the NDK, the Java launcher and SDL's activity without Gradle, and signs the APK
with a debug key (`android-port/build-android/OpenCrownMobile.apk`). The same CMake project also builds a Windows
`mobile_run.exe` (MinGW) for development, with the net tests `test_net` / `test_lobby`.

## State
Working on a test device (Retroid Pocket 3+): the game from boot through dungeons, saves, audio, controller and touch
input, the launcher, and online play with OpenCrown PC in both directions. Performance in heavy combat is 45-60 fps on
that device and is being worked on. Not yet tested: other devices and GPUs, online across two different networks, and
long co-op sessions.
