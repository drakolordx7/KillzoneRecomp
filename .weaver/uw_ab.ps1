# Ultrawide issue #10 A/B: per frame rate / vsync setting, how many world views were built with the wrong (unwidened) aspect in frames flagged wide.
# Needs the KZ_WIDE_LOG viewbuild trace (src/kz_widescreen.cpp). KZ_WIDE_DEFER=0 = old behaviour, 1 = fixed. Headless (hidden window), no audio.
# Usage: .weaver\uw_ab.ps1 -Cases "60,1,60;120,0,;144,1,144" -Defer 0,1 [-Seconds 260] [-Extra "PS2X_STRESS_YIELD=97"]   case = fps,vsync,fakeFifoHz
param([string]$Cases = "120,,", [int[]]$Defer = @(0,1), [int]$Seconds = 260, [string]$Extra = "", [string]$ExeDir = "D:\KillzoneRecomp\build\agentb", [string]$Tag = "ab")
foreach ($c in ($Cases -split ';')) {
  $p = $c -split ','; $fps = $p[0]; $vs = $p[1]; $fifo = $p[2]
  foreach ($d in $Defer) {
    $name = "${Tag}_${fps}_v${vs}_f${fifo}_d$d"
    $e = "KZ_ASPECT=21:9|KZ_FPS=$fps|KZ_WINDOW_SIZE=1720x720|KZ_WIDE_LOG=1|KZ_AUDIO_VOLUME=0|KZ_WIDE_DEFER=$d"
    if ($vs -ne "") { $e += "|KZ_VSYNC=$vs" }
    if ($fifo -ne "") { $e += "|KZGS_FAKE_FIFO_HZ=$fifo" }
    if ($Extra) { $e += "|" + $Extra }
    & D:\KillzoneRecomp\.weaver\lv2fast.ps1 -Name $name -Seconds $Seconds -Interval 60 -ExeDir $ExeDir -EnvSet $e | Out-Null
    $log = "D:\KillzoneRecomp\work\$name\stdout.log"
    $w = 0; $bad = 0; $y = 0
    foreach ($l in (Select-String -Path $log -Pattern "viewbuild wide=1|world-yield" -Raw)) {
      if ($l -match "world-yield") { $y++ } elseif ($l -match "aspect=([0-9.]+)") { $w++; if ([double]$matches[1] -lt 2.0) { $bad++ } }
    }
    "{0}: fps={1} vsync={2} fifo={3} defer={4}: wide views {5}, built unwidened {6} ({7:P2}), yields inside world {8}" -f $name, $fps, $vs, $fifo, $d, $w, $bad, $(if ($w) { $bad / $w } else { 0 }), $y
    Remove-Item "D:\KillzoneRecomp\work\$name\gs_*.png" -ErrorAction SilentlyContinue
  }
}
