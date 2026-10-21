# Faster menu path (gameplay at about t=100 s). Same params as lv2run.ps1 plus script times relative to t=0 of the run.
param([string]$Name = "l2", [int]$Seconds = 300, [int]$Interval = 5, [int]$Level = 2, [string]$Script = "", [string]$ExeDir = "D:\KillzoneRecomp\dist\Killzone", [string]$EnvSet = "")
$mc = "D:\KillzoneRecomp\work\saves\${Name}_mc"
if (Test-Path $mc) { Remove-Item -Recurse -Force $mc }
Copy-Item -Recurse D:\KillzoneRecomp\work\saves\level2 $mc
$ups = 11 - $Level
$s = '30:start:0.3;38:cross:0.3;46:cross:0.3;54:cross:0.3'
for ($i = 0; $i -lt $ups; $i++) { $s += ";" + (66 + $i * 1.5) + ":up:0.2" }
$t = 66 + $ups * 1.5 + 3
$s += ";${t}:cross:0.3;" + ($t + 6) + ":cross:0.3;" + ($t + 12) + ":cross:0.3"
if ($Script) { $s += ";" + $Script }
$env = "KZ_IPU=off|KZ_MC_ROOT=$mc|KZ_INPUT_SCRIPT=$s"
if ($EnvSet) { $env += "|" + $EnvSet }
& D:\KillzoneRecomp\tools\scripts\run_headless.ps1 -Name $Name -Seconds $Seconds -Interval $Interval -Extra '--no-launcher' -ExeDir $ExeDir -EnvSet $env
