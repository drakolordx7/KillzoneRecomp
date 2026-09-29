# Usage: run_headless.ps1 -Name h1 -Seconds 90 -Interval 5 [-Extra "--sample 15"]
# Boots killzone.exe without a window (works while the PC is locked). Frames -> work/<Name>/gs_*.png (kzgs readback),
# heartbeat lines ([headless] ...) -> work/<Name>/stderr.log.
param([string]$Name = "h", [int]$Seconds = 90, [int]$Interval = 5, [string]$Config = "RelWithDebInfo", [string]$Extra = "", [switch]$NoIso)
$wd = 'D:\KillzoneRecomp'; $iso = 'C:\Users\drakolord\Downloads\Killzone (USA)\Killzone (USA).iso'
$out = "$wd\work\$Name"; Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $out | Out-Null
$env:PS2X_HEADLESS = '1'; $env:PS2X_HEADLESS_INTERVAL = "$Interval"; $env:PS2X_HEADLESS_SECONDS = "$Seconds"
$env:KZ_SHOT_DIR = $out; $env:KZ_SHOT_INTERVAL = "$Interval"   # frames from the hardware GS (kzgs)
$p = Start-Process "$wd\build\$Config\killzone.exe" -ArgumentList (@('--elf', 'game\SCUS_974.02') + $(if ($NoIso) { @() } else { @('--iso', "`"$iso`"") }) + ($Extra -split " " | Where-Object { $_ })) `
    -WorkingDirectory $wd -RedirectStandardOutput "$out\stdout.log" -RedirectStandardError "$out\stderr.log" -PassThru -NoNewWindow
if (-not $p.WaitForExit(($Seconds + 30) * 1000)) { Stop-Process -Id $p.Id -Force; "hung past limit (killed)" } else { "exited code=$($p.ExitCode)" }
"frames: " + (Get-ChildItem "$out\gs_*.png" -ErrorAction SilentlyContinue).Count
