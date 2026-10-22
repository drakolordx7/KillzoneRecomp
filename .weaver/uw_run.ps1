# Ultrawide flicker test: level 2 gameplay with walking/turning, KZ_ASPECT=21:9, bursts of consecutive presented frames (swap chain back buffer),
# then .weaver\wide_check.py counts bad frames. Headless (hidden window), no audio.
# Usage: .weaver\uw_run.ps1 -Name x -ExeDir build\agentb -Fps 120 [-Vsync 1] [-Fifo 144] [-EnvSet "A=1|B=2"] [-Seconds 260] [-Burst 48] [-Keep] [-Idle]  (-Idle: no walking, motionless scene, static consistency check instead of the HUD-position check)
param([string]$Name = "uw", [string]$ExeDir = "D:\KillzoneRecomp\build\agentb", [int]$Fps = 120, [string]$Vsync = "", [string]$Fifo = "", [string]$EnvSet = "",
      [int]$Seconds = 260, [int]$Burst = 48, [string]$Aspect = "21:9", [string]$Win = "2580x1080", [switch]$Keep, [switch]$Idle, [int]$From = 170)
$sc = "135:ly=-1:25;162:rx=1:4;170:ly=-1:25;197:rx=-1:6;205:ly=-1:30;240:rx=1:5;250:ly=-1:30;285:rx=-1:5;292:ly=-1:30;330:rx=1:6;340:ly=-1:30"
if ($Idle) { $sc = "" }
$e = "KZ_ASPECT=$Aspect|KZ_FPS=$Fps|KZ_WINDOW_SIZE=$Win|KZ_SHOT_BURST=$Burst|KZ_SHOT_BURST_PRESENT=2|KZ_SHOT_BURST_FROM=$From|KZGS_ASPECT_LOG=1|KZ_AUDIO_VOLUME=0"
if ($Vsync -ne "") { $e += "|KZ_VSYNC=$Vsync" }
if ($Fifo -ne "") { $e += "|KZGS_FAKE_FIFO_HZ=$Fifo" }
if ($EnvSet) { $e += "|" + $EnvSet }
& D:\KillzoneRecomp\.weaver\lv2fast.ps1 -Name $Name -Seconds $Seconds -Interval 3 -Script $sc -ExeDir $ExeDir -EnvSet $e | Out-Null
$o = python D:\KillzoneRecomp\.weaver\wide_check.py D:\KillzoneRecomp\work\$Name $(if ($Idle) { "--static" } else { "--expect-wide" }) --keep-bad 2>&1
# pillarbox bursts at the start (loading/menus) are not gameplay: count only frames from the first wide burst on
$o | Select-Object -Last 3
if (-not $Keep) { Remove-Item D:\KillzoneRecomp\work\$Name\gb_*.png -ErrorAction SilentlyContinue }
