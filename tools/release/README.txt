KILLZONE PC PORT - alpha {VERSION}
==========================================

A native Windows version of Killzone for the PlayStation 2, made by recompiling the game's code for PC.
You need your own copy of the game. No game data (levels, models, textures, sound, video) is included;
it's all read from your disc image.

What you need
- Killzone for PS2, US version (SCUS-97402), as an .iso you dumped from your own disc.
  Other regions don't work yet.
- Windows 10 or 11, 64-bit, a CPU with AVX2, and a GPU with Direct3D 11, Direct3D 12 or Vulkan.

First start
1. Unzip the folder anywhere and run killzone.exe. The launcher opens.
2. Browse to your Killzone .iso. The launcher checks that it's the supported version.
3. Pick display, graphics and control settings and press PLAY.
   A few small boot files are copied from your disc into the "disc" folder. Everything else is read
   straight from the .iso.

Controls (defaults; listed in the launcher's Controls tab, change them in killzone.ini under [Bindings])
- Mouse: aim
- Left mouse: fire              - Right mouse: hold to aim       - Middle mouse: secondary fire
  (while zoomed, 1 and 3 change the zoom level; hold to aim can be switched to toggle in the launcher)
- W A S D: move                 - Left Shift: sprint             - C / Left Ctrl: crouch
- R: reload                     - G: throw grenade               - Q / mouse wheel: switch weapon
- E / F: use (ladders, mounted guns, pick up)                    - X / Mouse4: special item
- Escape: pause                 - Tab: objectives
Menus: arrow keys or W/S/A/D to move, Enter / Space / E to select, Backspace to go back.
Profile name: just type it. Backspace deletes, Enter confirms, Escape cancels.
Controllers (Xbox, DualShock, DualSense and others) work out of the box with the original PS2 layout.

Settings are saved in killzone.ini next to killzone.exe. Delete it to reset everything.
If you untick "Show this launcher at startup", the game boots straight in. To get the launcher back,
run "killzone.exe --launcher" or set Show=1 under [Launcher] in killzone.ini.

Your saves are in disc\mc0 next to killzone.exe. Keep that folder (and killzone.ini) when you update to
a newer build: copy the new files over the old ones.

Known problems in this build
- Sniper scope: the crosshair snaps back to the centre, and the view jumps if you move the mouse hard.
- The game can lock up on level 2, around the second sniper.
- The intro movies can't be skipped yet.
- Frame rate: about 120 fps at the start of the first mission on a fast PC, but busy fights drop lower
  and there is some stutter.
- Menus and the loading screen can jitter up and down a little.
- Only the first mission has been tested properly. Expect more bugs further in.

If the game crashes or hangs, open an issue at https://github.com/drakolordx7/KillzoneRecomp/issues
and attach killzone.log from the game folder (it's rewritten every start, the previous one is kept as
killzone.prev.log). Say what you were doing and which level you were on.
