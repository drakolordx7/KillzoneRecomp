# Repeats a short headless boot until the "module arena exhausted" boot hang shows (or N tries). Usage: bootrepro.ps1 -Tries 30 -Seconds 14 [-ExeDir dir] [-EnvSet "A=1|B=2"]
param([int]$Tries = 30, [int]$Seconds = 14, [int]$Interval = 30, [string]$ExeDir = "D:\KillzoneRecomp\build\agentw", [string]$EnvSet = "")
$fails = 0
for ($i = 1; $i -le $Tries; $i++) {
    & D:\KillzoneRecomp\tools\scripts\run_headless.ps1 -Name "bootrepro_$i" -Seconds $Seconds -Interval $Interval -Extra '--no-launcher' -ExeDir $ExeDir -EnvSet "KZ_IPU=off$(if ($EnvSet) { '|' + $EnvSet })" | Out-Null
    $n = (Select-String -Path "D:\KillzoneRecomp\work\bootrepro_$i\stderr.log" -Pattern "arena exhausted" -SimpleMatch -List -ErrorAction SilentlyContinue | Measure-Object).Count
    "try $i : " + $(if ($n) { "HANG (arena exhausted)" } else { "ok" })
    if ($n) { $fails++; break }
}
"hangs: $fails in $i tries"
