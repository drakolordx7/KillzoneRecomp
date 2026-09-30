# N boots of S seconds (hang check, patch 0025): every boot must exit with code 0 and its `dma=` counter must keep advancing in every 10 s heartbeat.
#   tl_boots.ps1 -Tag b -N 12 -Seconds 50 [-Parallel 3] [-Config tl] [-EnvSet 'KZ_FPS=120']
# Boots run Parallel at a time (more than one instance also stresses the start-up race the IOP module loader has under load).
param([string]$Tag = 'boot', [int]$N = 12, [int]$Seconds = 50, [int]$Parallel = 3, [string]$Config = 'tl', [string]$EnvSet = 'KZ_FPS=120')
$wd = 'D:\KillzoneRecomp'
$iso = 'C:\Users\drakolord\Downloads\Killzone (USA)\Killzone (USA).iso'
$sb = {
    param($wd, $iso, $name, $seconds, $config, $envset)
    $out = "$wd\work\$name"; Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $out | Out-Null
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "$wd\build\$config\killzone.exe"
    $psi.Arguments = "--elf game\SCUS_974.02 --iso `"$iso`" --no-launcher"
    $psi.WorkingDirectory = $wd; $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $psi.EnvironmentVariables['PS2X_HEADLESS'] = '1'; $psi.EnvironmentVariables['PS2X_HEADLESS_INTERVAL'] = '10'
    $psi.EnvironmentVariables['PS2X_HEADLESS_SECONDS'] = "$seconds"; $psi.EnvironmentVariables['KZ_IPU'] = 'off'
    foreach ($kv in ($envset -split ';' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; $psi.EnvironmentVariables[$k] = $v }
    $p = [System.Diagnostics.Process]::Start($psi)
    $so = $p.StandardOutput.ReadToEndAsync(); $se = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(($seconds + 30) * 1000)) { $p.Kill(); $code = 'HUNG' } else { $code = $p.ExitCode }
    $err = $se.Result; Set-Content "$out\stderr.log" $err; Set-Content "$out\stdout.log" $so.Result
    $dma = @($err -split "`n" | Where-Object { $_ -match 'headless\] t=\d+s .* dma=(\d+)' } | ForEach-Object { [long]([regex]::Match($_, 'dma=(\d+)').Groups[1].Value) })
    $adv = $true; for ($i = 1; $i -lt $dma.Count; $i++) { if ($dma[$i] -le $dma[$i - 1]) { $adv = $false } }
    "{0,-12} exit={1} heartbeats={2} dma={3} advancing={4} errors={5}" -f $name, $code, $dma.Count, ($dma -join ','), $adv, ([regex]::Matches($err, 'exception|arena exhausted|missing function|unhandled')).Count
}
$jobs = @(); $res = @()
for ($i = 1; $i -le $N; $i++) {
    $jobs += Start-Job -ScriptBlock $sb -ArgumentList $wd, $iso, "${Tag}_$i", $Seconds, $Config, $EnvSet
    if ($jobs.Count -ge $Parallel) { $done = Wait-Job -Job $jobs -Any; $res += Receive-Job $done; $jobs = @($jobs | Where-Object { $_.Id -ne $done.Id }); Remove-Job $done }
}
foreach ($j in $jobs) { Wait-Job $j | Out-Null; $res += Receive-Job $j; Remove-Job $j }
$res
"boots: $N, bad: " + @($res | Where-Object { $_ -notmatch 'exit=0 ' -or $_ -match 'advancing=False' }).Count
