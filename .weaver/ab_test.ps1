# A/B of two builds, run one after the other on a quiet machine: level-1 frame rate (vif delta t=150..200) and the
# scope check (fake scope weapon, aim offsets must stay 0). Usage: ab_test.ps1 -A <exe dir> -B <exe dir> [-EnvSet "X=1"]
param([Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][string]$B, [string]$EnvSet = '')
$rh = 'D:\KillzoneRecomp\tools\scripts\run_headless.ps1'
$menu = '60:start:0.3;80:cross:0.3;90:cross:0.3;100:cross:0.3;110:cross:0.3;120:cross:0.3;130:cross:0.3;140:cross:0.3;150:cross:0.3;160:cross:0.3'
$w = 'D:\KillzoneRecomp\work'
$extra = if ($EnvSet) { "|$EnvSet" } else { '' }
foreach ($side in @(@('A', $A), @('B', $B))) {
    $n = $side[0]; $dir = $side[1]
    & $rh -Name "ab_fps_$n" -Seconds 205 -Interval 50 -Extra '--no-launcher' -ExeDir $dir -EnvSet "KZ_FPS=120|KZ_IPU=off|KZ_INPUT_SCRIPT=$menu$extra" | Out-Null
    & $rh -Name "ab_sc_$n" -Seconds 200 -Interval 50 -Extra '--no-launcher' -ExeDir $dir -EnvSet "KZ_FPS=120|KZ_IPU=off|KZ_AUDIO_VOLUME=0|KZ_AIM_LOG=2|KZ_FAKE_SCOPE=1|KZ_MC_ROOT=D:\KillzoneRecomp\game\mc0|KZ_INPUT_SCRIPT=$menu;172:zoomkey:24;179:mx=600;185:mx=-300;191:mx=60$extra" | Out-Null
    $e = Select-String -Path "$w\ab_fps_$n\stderr.log" -Pattern '^\[headless\] t=(\d+)s .* vif=(\d+)' | ForEach-Object { [pscustomobject]@{ t = [int]$_.Matches[0].Groups[1].Value; vif = [int]$_.Matches[0].Groups[2].Value } }
    $a1 = $e | Where-Object t -eq 150; $b1 = $e | Where-Object t -eq 200
    "$n ($dir): fps " + $(if ($a1 -and $b1) { [math]::Round(($b1.vif - $a1.vif) / 50.0, 1) } else { 'n/a' })
    python -I "$w\sc_an.py" "$w\ab_sc_$n\stdout.log" 2>&1 | Select-Object -Last 1
}
