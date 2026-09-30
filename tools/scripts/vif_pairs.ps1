# A/B of two variants run concurrently, N times: vif_pairs.ps1 -Tag t -N 3 -A "X=0" -B "" [-Config pd] [-ExeA killzone.exe] [-ExeB killzone.exe] [-Profile] [-Ipu]
param([string]$Tag, [int]$N = 3, [string]$A, [string]$B, [string]$Config = 'pd', [string]$ExeA = 'killzone.exe', [string]$ExeB = 'killzone.exe', [switch]$Profile, [switch]$NoStats)
$run = Join-Path $PSScriptRoot 'vif_run.ps1'
for ($i = 1; $i -le $N; $i++) {
    $sb = { param($s, $t, $c, $e, $x, $p, $n) $a = @{Name=$t; Config=$c; EnvSet=$e; Exe=$x}; if ($p) { $a.Profile = $true }; if ($n) { $a.NoStats = $true }; & $s @a }
    $ja = Start-Job -ScriptBlock $sb -ArgumentList $run, "${Tag}_a$i", $Config, $A, $ExeA, $Profile.IsPresent, $NoStats.IsPresent
    $jb = Start-Job -ScriptBlock $sb -ArgumentList $run, "${Tag}_b$i", $Config, $B, $ExeB, $Profile.IsPresent, $NoStats.IsPresent
    Wait-Job $ja, $jb | Out-Null
    Receive-Job $ja | Out-Null; Receive-Job $jb | Out-Null
    python "$PSScriptRoot\vif_metric.py" "${Tag}_a$i" "${Tag}_b$i"
}
