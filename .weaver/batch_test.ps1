# Batch test of a build before a release. Runs headless and silent; results in work\bt_<name>\.
#   A intro skip (movies on), B boot under forced yields (old boot hang), C scope aim with a fake scope weapon (level 1),
#   D level 2 for -L2Seconds, then E alone: level-1 gameplay frame rate (vif delta t=150..200).
param([string]$ExeDir = 'D:\KillzoneRecomp\build\RelWithDebInfo', [int]$L2Seconds = 240)
$ErrorActionPreference = 'Continue'
$rh = 'D:\KillzoneRecomp\tools\scripts\run_headless.ps1'
$menu = '60:start:0.3;80:cross:0.3;90:cross:0.3;100:cross:0.3;110:cross:0.3;120:cross:0.3;130:cross:0.3;140:cross:0.3;150:cross:0.3;160:cross:0.3'
$jobs = @(
    Start-Job { param($rh, $e) & $rh -Name bt_A -Seconds 50 -Interval 10 -Extra '--no-launcher' -ExeDir $e -EnvSet 'KZ_SKIP_INTRO=1|KZ_MOVIE_LOG=1' } -ArgumentList $rh, $ExeDir
    Start-Job { param($rh, $e) & $rh -Name bt_B -Seconds 60 -Interval 20 -Extra '--no-launcher' -ExeDir $e -EnvSet 'KZ_IPU=off|PS2X_STRESS_YIELD=2' } -ArgumentList $rh, $ExeDir
    Start-Job { param($rh, $e, $m) & $rh -Name bt_C -Seconds 200 -Interval 50 -Extra '--no-launcher' -ExeDir $e -EnvSet "KZ_FPS=120|KZ_IPU=off|KZ_AUDIO_VOLUME=0|KZ_AIM_LOG=2|KZ_FAKE_SCOPE=1|KZ_MC_ROOT=D:\KillzoneRecomp\game\mc0|KZ_INPUT_SCRIPT=$m;172:zoomkey:24;179:mx=600;185:mx=-300;191:mx=60" } -ArgumentList $rh, $ExeDir, $menu
    Start-Job { param($e, $s) & D:\KillzoneRecomp\.weaver\lv2fast.ps1 -Name bt_D -Seconds $s -Interval 20 -ExeDir $e } -ArgumentList $ExeDir, $L2Seconds
)
$jobs | Wait-Job | Receive-Job | Out-Null
& $rh -Name bt_E -Seconds 205 -Interval 50 -Extra '--no-launcher' -ExeDir $ExeDir -EnvSet "KZ_FPS=120|KZ_IPU=off|KZ_INPUT_SCRIPT=$menu" | Out-Null

$w = 'D:\KillzoneRecomp\work'
function Beats($n) { Select-String -Path "$w\$n\stderr.log" -Pattern '^\[headless\] t=(\d+)s .* vif=(\d+)' | ForEach-Object { [pscustomobject]@{ t = [int]$_.Matches[0].Groups[1].Value; vif = [int]$_.Matches[0].Groups[2].Value } } }
"A intro skip: " + ((Select-String -Path "$w\bt_A\stdout.log" -Pattern 'skipped \(Skip intro' -List) -ne $null) + "; frames " + (Get-ChildItem "$w\bt_A\gs_*.png").Count
"B forced yields: arena-exhausted lines " + (Select-String -Path "$w\bt_B\stdout.log", "$w\bt_B\stderr.log" -Pattern 'arena exhausted').Count + "; last beat " + ((Beats bt_B | Select-Object -Last 1) | ForEach-Object { "t=$($_.t) vif=$($_.vif)" })
"C scope:"; python -I "$w\sc_an.py" "$w\bt_C\stdout.log" 2>&1 | Select-Object -Last 4
$d = Beats bt_D; "D level 2: last beat " + (($d | Select-Object -Last 1) | ForEach-Object { "t=$($_.t) vif=$($_.vif)" }) + "; vif in last 40 s " + (($d | Select-Object -Last 1).vif - ($d | Where-Object t -le (($d | Select-Object -Last 1).t - 40) | Select-Object -Last 1).vif)
$e = Beats bt_E; $a = $e | Where-Object t -eq 150; $b = $e | Where-Object t -eq 200
"E level 1 fps (t150-200): " + ($(if ($a -and $b) { [math]::Round(($b.vif - $a.vif) / 50.0, 1) } else { 'n/a' }))
