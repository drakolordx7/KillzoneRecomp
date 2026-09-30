# Runs variant A and variant B (environment switches of the same binary, or two build dirs) at the same time, N times.
# Concurrent pairs see the same machine load, which is what makes small differences readable on a shared machine.
#   perf_pairs.ps1 -Tag t -N 3 -A "PS2X_LAZY_TIMERS=0" -B "" [-ConfigA dirA -ConfigB dirB]
# Runs are named <Tag>_a<i> / <Tag>_b<i>; feed them to perf_metric.py (add PS2X_SCHED_STATS=1 to both for the pacing sleep).
param([string]$Tag, [int]$N = 3, [string]$A, [string]$B, [string]$ConfigA = 'RelWithDebInfo', [string]$ConfigB = 'RelWithDebInfo')
$run = Join-Path $PSScriptRoot 'perf_run.ps1'
for ($i = 1; $i -le $N; $i++) {
    $ja = Start-Job -ScriptBlock { param($s, $t, $c, $e) & $s -Name $t -Config $c -EnvSet $e } -ArgumentList $run, "${Tag}_a$i", $ConfigA, $A
    $jb = Start-Job -ScriptBlock { param($s, $t, $c, $e) & $s -Name $t -Config $c -EnvSet $e } -ArgumentList $run, "${Tag}_b$i", $ConfigB, $B
    Wait-Job $ja, $jb | Out-Null
    Receive-Job $ja; Receive-Job $jb
}
