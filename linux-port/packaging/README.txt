OpenCrown for Linux / Steam Deck - test build (no game files included - you need your own PS4/PS5 dump)

Needs: a 64-bit Linux with glibc 2.35 or newer (SteamOS 3.5+, Ubuntu 22.04+, Debian 12+, Fedora, Arch, ...)
and Vulkan or OpenGL 3.3. The game draws with Vulkan (SDL GPU) when the graphics driver has it and switches
to OpenGL by itself when it does not. If the picture stays black or the game does not start, set the
launcher's Settings > Renderer to OpenGL.

Room text chat (online rooms only; switch it off on the launcher's Online page): press Enter (the "Open room
chat" key on the Controls page) to type, Enter sends, Esc closes. On a Steam Deck: double tap the screen
and hold (tap, then touch and hold the same spot); Steam's on-screen keyboard opens with the chat box. B closes the chat box without sending. /mute <name> in chat
mutes a player.

Desktop Linux:
1. Make it executable: right-click the .AppImage > Properties > allow executing, or
   chmod +x OpenCrown-*.AppImage
2. Run it. First start: choose where your saves go (default is fine).
3. Game page: pick the folder holding your own dump (eboot.bin + the .CPK files).
4. Online page: set your player name; add friends with friend codes. Press PLAY.

Steam Deck:
1. Desktop Mode: copy the .AppImage and your dump folder to the Deck (internal storage or SD card),
   make the .AppImage executable (Properties > Permissions), run it once and set it up as above.
2. Steam > Games > Add a Non-Steam Game > browse to the .AppImage > Add. Now it starts from Game Mode.
3. Controls with Steam Input (the normal case): sticks and buttons work as a controller; the right
   trackpad moves the fairy pointer and its click interacts. Without Steam Input (controller settings >
   disable Steam Input): right trackpad = fairy pointer, click = interact, left trackpad click = town map.
   Tapping the touch screen sends the fairy to that spot; a short tap also interacts.
   Both can be switched off on the launcher's Controls page.

Steam Deck Game Mode (no keyboard or mouse needed):
- The launcher works with the controller: D-pad / left stick move, A selects, B goes back (or closes
  a window), L1 / R1 switch pages, right stick scrolls, Start = PLAY. The bottom bar shows these hints.
- The first start can be done entirely with the controller: answer the save-folder question, then on
  the Game page press Browse... and find your dump in the built-in folder browser (Home, SD card and
  /media, /mnt buttons; folders holding a complete dump are marked [dump]); open it and press
  "Select this folder".
- The touch screen works too: tap = click, drag a list to scroll it.
- Text fields (player name, friend code, bug note, folder path): selecting one with the controller or
  the touch screen opens Steam's on-screen keyboard. If it does not appear, press Steam + X. Friend
  codes can also be pasted with the Paste button.
- Key bindings on the Controls page need a keyboard or mouse (B cancels); the game's controller
  buttons do not need setting up.

Updates: the launcher checks GitHub for a newer release when it starts (can be switched off). The Update
entry at the bottom left of the sidebar says "Update available" when there is one; its page shows what the
new version fixes, and "Download and install" downloads the new AppImage, checks its SHA-256, replaces this
file and restarts the launcher. The file keeps its name (and the old version number in it) after an update,
so a Steam shortcut to it keeps working. The folder holding the .AppImage must be writable; if it is not,
use "Open release page" and download the new version by hand.

Saves, settings and logs: ~/.local/share/OpenCrown/ (saves are byte-identical to the PS4 format).
Problems: Report a Bug page (Preview shows what is sent), or send ~/.local/share/OpenCrown/logs/last_run.log.
