# Builds the player-facing folder dist\Killzone from build\RelWithDebInfo.
# Contains no game data: the player points the launcher at their own Killzone (USA) disc image; the small boot
# files are extracted from it on first run into disc\.
# Usage: package.ps1 [-Config RelWithDebInfo] [-WithPdb]
param([string]$Config = "RelWithDebInfo", [switch]$WithPdb)
$ErrorActionPreference = 'Stop'
$root = 'D:\KillzoneRecomp'
$src = "$root\build\$Config"
$dst = "$root\dist\Killzone"

if (-not (Test-Path "$src\killzone.exe")) { throw "build first: $src\killzone.exe missing" }
# Update in place: replace the program files only. The player's data in the folder stays: killzone.ini (settings),
# mc0/mc1 (memory cards: profiles and saves), disc (files extracted from their .iso) and cache (shader cache).
New-Item -ItemType Directory -Force $dst | Out-Null
Get-ChildItem $dst -File | Where-Object { $_.Extension -in '.exe', '.dll', '.pdb', '.txt' } | Remove-Item -Force
if (Test-Path "$dst\resources") { Remove-Item -Recurse -Force "$dst\resources" }

Copy-Item "$src\killzone.exe" $dst
Get-ChildItem "$src\*.dll" | Copy-Item -Destination $dst
Copy-Item -Recurse "$src\resources" "$dst\resources"
if ($WithPdb -and (Test-Path "$src\killzone.pdb")) { Copy-Item "$src\killzone.pdb" $dst }   # symbols for crash reports (1.8 GB)

(Get-Content "$root\tools\release\README.txt" -Raw).Replace('{VERSION}', 'dev build') | Set-Content -Encoding UTF8 "$dst\README.txt"

$size = (Get-ChildItem -Recurse $dst | Measure-Object -Sum Length).Sum / 1MB
"packaged $dst ({0:N0} MB)" -f $size
