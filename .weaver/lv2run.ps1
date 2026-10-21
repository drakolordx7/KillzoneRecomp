# Run level N (default 2, Vekta Evacuates) headless from a fresh copy of work\saves\level2. Gameplay starts at about t=165 s (+Shift).
# Usage: lv2run.ps1 -Name x -Seconds 300 -Interval 5 [-Level 2] [-Shift 0] [-Script "170:ly=-1:8;..."] [-ExeDir dir] [-EnvSet "A=1|B=2"]
# Script times are on the same clock as the menu script (menu part ends at t=156+Shift). Up presses = 11-Level (Hope=11).
# -Shift delays the whole menu script (use ~35 with movies on: KZ_IPU=on in -EnvSet, menus react slower).
param([string]$Name = "l2", [int]$Seconds = 300, [int]$Interval = 5, [int]$Level = 2, [int]$Shift = 0, [string]$Script = "", [string]$ExeDir = "D:\KillzoneRecomp\dist\Killzone", [string]$EnvSet = "")
$mc = "D:\KillzoneRecomp\work\saves\${Name}_mc"
if (Test-Path $mc) { Remove-Item -Recurse -Force $mc }
Copy-Item -Recurse D:\KillzoneRecomp\work\saves\level2 $mc
$ups = 11 - $Level
$s = "$(35+$Shift):start:0.3;$(50+$Shift):cross:0.3;$(60+$Shift):cross:0.3;$(70+$Shift):cross:0.3"
for ($i = 0; $i -lt $ups; $i++) { $s += ";" + (88 + $Shift + $i * 3) + ":up:0.3" }
$s += ";$(120+$Shift):cross:0.3;$(132+$Shift):cross:0.3;$(144+$Shift):cross:0.3;$(156+$Shift):cross:0.3"
if ($Script) { $s += ";" + $Script }
$env = "KZ_IPU=off|KZ_MC_ROOT=$mc|KZ_INPUT_SCRIPT=$s"
if ($EnvSet) { $env += "|" + $EnvSet }
& D:\KillzoneRecomp\tools\scripts\run_headless.ps1 -Name $Name -Seconds $Seconds -Interval $Interval -Extra '--no-launcher' -ExeDir $ExeDir -EnvSet $env
