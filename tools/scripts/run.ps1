# Usage: run.ps1 -Name run2 -Seconds 45 -Shots "10,25,45" -Extra "--sample 10"
# Boots killzone.exe against the user's ISO, captures window shots at the given second marks, then kills it.
param([string]$Name = "run", [int]$Seconds = 45, [string]$Shots = "10,25,45", [string]$Config = "RelWithDebInfo", [string]$Extra = "")
$wd = 'D:\KillzoneRecomp'; $iso = 'C:\Users\drakolord\Downloads\Killzone (USA)\Killzone (USA).iso'
$out = "$wd\work\$Name"; New-Item -ItemType Directory -Force $out | Out-Null
$p = Start-Process "$wd\build\$Config\killzone.exe" -ArgumentList (@('--elf', 'game\SCUS_974.02', '--iso', "`"$iso`"") + ($Extra -split " " | Where-Object { $_ })) -WorkingDirectory $wd `
    -RedirectStandardOutput "$out\stdout.log" -RedirectStandardError "$out\stderr.log" -PassThru
$sw = [Diagnostics.Stopwatch]::StartNew()
foreach ($t in ($Shots -split "," | ForEach-Object { [int]$_ })) {
    while ($sw.Elapsed.TotalSeconds -lt $t -and -not $p.HasExited) { Start-Sleep -Milliseconds 250 }
    if ($p.HasExited) { break }
    try { & "$wd\tools\scripts\capture_window.ps1" -ProcessName killzone -Out "$out\t$t.png" } catch { "no window at ${t}s" }
}
while ($sw.Elapsed.TotalSeconds -lt $Seconds -and -not $p.HasExited) { Start-Sleep -Milliseconds 250 }
if ($p.HasExited) { "exited code=0x{0:X8} after {1:N1}s" -f $p.ExitCode, $sw.Elapsed.TotalSeconds } else { Stop-Process -Id $p.Id -Force; "still running at ${Seconds}s (killed)" }
"stdout lines: " + (Get-Content "$out\stdout.log" | Measure-Object -Line).Lines + ", stderr lines: " + (Get-Content "$out\stderr.log" | Measure-Object -Line).Lines
