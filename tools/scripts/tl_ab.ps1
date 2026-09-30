# Interleaved sequential A/B runs of the headless gameplay scene (patch 0025): one instance at a time, variants rotated per round.
#   tl_ab.ps1 -Tag x -Rounds 3 -VariantList 'old=PS2X_EE_CYCLE_SCALE=1|new=' [-Config tl] [-Seconds 235] [-Timeline]
# A variant is name=ENV1=v1;ENV2=v2 (empty = defaults). Every run: KZ_FPS=120 KZ_IPU=off, the scripted input of the task, run_headless.ps1.
# fps = (vif counter at t=200 - at t=150) / 50 from the [headless] lines; mean CPU load of the machine over the run is logged next to it.
# Output: work\<Tag>_<variant>_<round>\ (stderr.log, gs_*.png), a summary line per run and the median per variant at the end.
param([string]$Tag, [int]$Rounds = 3, [string]$VariantList, [string]$Config = 'tl', [int]$Seconds = 235, [switch]$Timeline, [switch]$Ipu)
$wd = 'D:\KillzoneRecomp'
$results = @{}
$Variants = @($VariantList -split '\|')   # 'name=ENV=v;ENV2=v2|name2=' (a real array does not survive powershell -File)
for ($r = 1; $r -le $Rounds; $r++) {
    $order = @($Variants)
    for ($k = 0; $k -lt ($r - 1) % $Variants.Count; $k++) { $order = $order[1..($order.Count - 1)] + $order[0] }   # rotate the order each round
    foreach ($v in $order) {
        $name, $envset = $v -split '=', 2
        $run = "${Tag}_${name}_$r"
        if ($Timeline) { $envset = ("KZ_TIMELINE=$wd\work\$run.csv;" + $envset).TrimEnd(';') }
        $load = Start-Process typeperf -ArgumentList '"\Processor(_Total)\% Processor Time"', '-si', '1', '-sc', "$($Seconds + 30)", '-o', "$wd\work\$run.load.csv", '-y' -PassThru -WindowStyle Hidden
        $a = @('-NoProfile', '-File', "$wd\tools\scripts\tl_run.ps1", '-Name', $run, '-EnvSet', "`"$envset`"", '-Config', $Config, '-Seconds', "$Seconds")
        if ($Ipu) { $a += '-Ipu' }
        $p = Start-Process powershell -ArgumentList $a -Wait -PassThru -WindowStyle Hidden -RedirectStandardOutput "$wd\work\$run.out"
        Stop-Process -Id $load.Id -Force -ErrorAction SilentlyContinue
        $log = Get-Content "$wd\work\$run\stderr.log" -ErrorAction SilentlyContinue
        $vif = @{}
        foreach ($ln in $log) { if ($ln -match 'headless\] t=(\d+)s .* vif=(\d+)') { $vif[[int]$Matches[1]] = [long]$Matches[2] } }
        $fps = if ($vif.ContainsKey(150) -and $vif.ContainsKey(200)) { ($vif[200] - $vif[150]) / 50.0 } else { [double]::NaN }
        $cpu = (Get-Content "$wd\work\$run.load.csv" -ErrorAction SilentlyContinue | Select-Object -Skip 1 | ForEach-Object { ($_ -split ',')[1].Trim('"') } | Where-Object { $_ -match '^[\d.]+$' } | ForEach-Object { [double]$_ } | Measure-Object -Average).Average
        "{0,-22} fps={1,6:N1}  cpu={2,5:N1}%  {3}" -f $run, $fps, $cpu, ($log | Select-String 'time limit|exited' | Select-Object -First 1)
        if (-not $results.ContainsKey($name)) { $results[$name] = @() }
        $results[$name] += $fps
    }
}
foreach ($n in $results.Keys) {
    $s = $results[$n] | Sort-Object
    "{0,-10} runs: {1}   median {2:N1}" -f $n, (($results[$n] | ForEach-Object { '{0:N1}' -f $_ }) -join ' / '), $s[[int][math]::Floor(($s.Count - 1) / 2)]
}
