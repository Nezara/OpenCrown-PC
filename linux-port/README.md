# OpenCrown for Linux / Steam Deck

The Linux build of OpenCrown. It is a separate source tree from the Windows build in `port/`: the same design (the
user's own eboot runs under our loader and PS4 system-library shims, rendered with OpenGL through SDL3), rewritten
for Linux where Windows APIs were used. No game files are included; players supply their own dump.

Players: download the AppImage from the GitHub releases and see `packaging/README.txt` for setup, including the
Steam Deck.

## Building
Ubuntu 22.04 or newer (other distros work with the equivalent packages):

```sh
sudo apt install build-essential g++-12 cmake ninja-build pkg-config libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libdecor-0-dev \
  libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev libpulse-dev libasound2-dev \
  libpipewire-0.3-dev libudev-dev libdbus-1-dev
./build.sh
```

SDL3, FreeType, Mbed TLS, miniupnpc and Dear ImGui are downloaded by CMake on the first configure. `./build.sh`
builds the launcher (`opencrown-launcher`), the game runner (`dcp_run`), the developer tools (`dcp_cpk`, `dcp_tex`,
`dcp_audio`, `dcp_sprite`, `dcp_viewer`, `dcp_lobby`) and runs the tests (none need game files).
`DCP_RENDER=OFF ./build.sh` builds a headless `dcp_run` without SDL3.

## Packaging
`sh packaging/make-appimage.sh` (on Ubuntu 22.04) builds `dist/OpenCrown-<version>-x86_64.AppImage` (plus its `.zsync` for AppImageUpdate and a copy named `OpenCrown-Linux-<version>-x86_64.AppImage` for launchers up to 1.0.6): the
launcher and `dcp_run` with libstdc++ linked in, so players only need glibc 2.35 or newer (SteamOS 3.5+, Ubuntu
22.04+, Debian 12+, Fedora, Arch). SDL loads X11 / Wayland / audio / GL / controller support from the system at run
time. The AppImage carries the third-party notices and license texts (`packaging/THIRD-PARTY-NOTICES.txt`).
appimagetool 1.9.1 is downloaded once (pinned by SHA-256).

## Steam Deck and touch controls
- Launcher: usable with the controller (D-pad / stick, A select, B back, L1/R1 pages, Start = Play) or the touch
  screen, with a built-in folder browser and Steam's on-screen keyboard (Steam+X) in Game Mode.
- Game, Steam Input on (normal when started from Steam): the right trackpad moves the fairy pointer, its click
  interacts. Steam Input off: right trackpad = fairy pointer, left trackpad click = town map.
- Touch screen: touching a spot sends the fairy there; a short tap also interacts.
- Game Mode turns touches into left clicks for non-Steam games; those clicks steer the fairy like touches
  (`touch_as_mouse`, auto = in Game Mode). With Steam Input on, add *Touchscreen Native Support* to the shortcut's
  controller layout (Action Sets -> Add Always-On command -> System) so the game gets real touches.
- On a Steam Deck mouse buttons press no game buttons, so a click never attacks (`mouse_attacks`, auto = off on a
  Deck); the keyboard still works.
- All of these are on the launcher's Controls page (`touch_screen`, `deck_trackpads`, `touch_as_mouse`,
  `mouse_attacks` in input.ini).

## Updates
The launcher checks the GitHub releases (Update entry at the bottom left), shows the release notes, downloads the
new AppImage, verifies its SHA-256 and replaces itself in place, so a Steam shortcut keeps working.

## State (v1.0.1)
Ported and tested on Ubuntu 22.04: dump tools (output identical to the Windows build), the game headless and
rendered (same behaviour as Windows on the same dump), audio, keyboard/mouse and gamepad input, saves
(byte-identical PS4 format, shared with Windows), the launcher and the updater. Not yet tested: a real Steam Deck
(controls, touch, Game Mode), Wayland desktops, and online play against the live lobby server. Feedback is welcome
on Discord or through the launcher's Report a Bug page. Saves, settings and logs live in
`${XDG_DATA_HOME:-~/.local/share}/OpenCrown/`.
