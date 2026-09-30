# Builds the player-facing folder dist\Killzone from build\RelWithDebInfo.
# Contains no game code or data: the player points the launcher at their own Killzone (USA) disc image; the small boot
# files are extracted from it on first run into disc\.
# Usage: package.ps1 [-Config RelWithDebInfo] [-WithPdb]
param([string]$Config = "RelWithDebInfo", [switch]$WithPdb)
$ErrorActionPreference = 'Stop'
$root = 'D:\KillzoneRecomp'
$src = "$root\build\$Config"
$dst = "$root\dist\Killzone"

if (-not (Test-Path "$src\killzone.exe")) { throw "build first: $src\killzone.exe missing" }
Remove-Item -Recurse -Force $dst -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dst | Out-Null

Copy-Item "$src\killzone.exe" $dst
Get-ChildItem "$src\*.dll" | Copy-Item -Destination $dst
Copy-Item -Recurse "$src\resources" "$dst\resources"
if ($WithPdb -and (Test-Path "$src\killzone.pdb")) { Copy-Item "$src\killzone.pdb" $dst }   # symbols for crash reports (1.8 GB)

@"
KILLZONE - PC port (static recompilation of the PS2 game)
=========================================================

What you need
- Your own copy of Killzone for PlayStation 2, NTSC-U (SCUS-97402), dumped to an .iso file.
- Windows 10/11 x64, a CPU with AVX2, a Direct3D 11 / 12 or Vulkan capable GPU.

First start
1. Run killzone.exe. The launcher opens.
2. Browse to your Killzone .iso. The launcher checks it is the supported version (v1.00).
3. Pick display, graphics and control settings, then press PLAY.
   The few small boot files are extracted from your disc into the "disc" folder; the game data is read
   directly from the .iso.

Controls (defaults; shown in the launcher's Controls tab, change them in killzone.ini, section [Bindings])
- Mouse: aim (applied directly to the view, no stick acceleration)
- Left mouse: fire              - Right mouse: zoom / scope      - Middle mouse: secondary fire
- W A S D: move                 - Left Shift: sprint             - C / Left Ctrl: crouch
- R: reload                     - G: throw grenade               - Q / mouse wheel: switch weapon
- E / F: use (ladders, emplaced guns, pick up)                  - X / Mouse4: special item
- Escape: pause                 - Tab: objectives                - Space / Enter: confirm in menus
Controllers (Xbox, DualShock, DualSense, ...) work out of the box with the original PS2 layout.

Settings live in killzone.ini next to killzone.exe (the launcher writes it). Delete it to reset everything.
Uncheck "Show this launcher at startup" to boot straight into the game. To get the launcher back, run
"killzone.exe --launcher" (or set Show=1 under [Launcher] in killzone.ini).

Status (test build)
- Frame rate: "Match monitor refresh" (default) runs the game at your monitor's rate. Measured in gameplay:
  about 117-120 fps on 120 Hz, about 130-140 fps on 144 Hz.
- The game plays its full intro movies before the main menu, as on the PS2.
- The virtual memory card (profiles, saves) is kept in the killzone.exe folder.
- If something goes wrong, run with the launcher and check the Display tab, or delete killzone.ini to reset.

This package contains no Sony or Guerrilla code or assets.
"@ | Set-Content -Encoding UTF8 "$dst\README.txt"

$size = (Get-ChildItem -Recurse $dst | Measure-Object -Sum Length).Sum / 1MB
"packaged $dst ({0:N0} MB)" -f $size
