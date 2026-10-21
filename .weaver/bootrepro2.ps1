# Like bootrepro.ps1 but starts the run with the long time limit (as the level runs do) and kills it after $Watch seconds.
param([int]$Tries = 30, [int]$Watch = 15, [int]$Seconds = 140, [int]$Interval = 1, [string]$ExeDir = "D:\KillzoneRecomp\build\agentw", [string]$EnvSet = "")
$wd = 'D:\KillzoneRecomp'; $iso = 'C:\Users\drakolord\Downloads\Killzone (USA)\Killzone (USA).iso'
$fails = 0
for ($i = 1; $i -le $Tries; $i++) {
    $out = "$wd\work\bootrepro2_$i"; if (Test-Path $out) { Remove-Item -Recurse -Force $out }; New-Item -ItemType Directory -Force $out | Out-Null
    $env:PS2X_HEADLESS = '1'; $env:PS2X_HEADLESS_INTERVAL = "$Interval"; $env:PS2X_HEADLESS_SECONDS = "$Seconds"
    $env:KZ_SHOT_DIR = $out; $env:KZ_SHOT_INTERVAL = "$Interval"; $env:KZ_IPU = 'off'
    foreach ($kv in ($EnvSet -split '[|]' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; Set-Item "env:$k" $v }
    $p = Start-Process "$ExeDir\killzone.exe" -ArgumentList @('--elf', 'game\SCUS_974.02', '--iso', "`"$iso`"", '--no-launcher') -WorkingDirectory $wd -RedirectStandardOutput "$out\stdout.log" -RedirectStandardError "$out\stderr.log" -PassThru -NoNewWindow
    Start-Sleep -Seconds $Watch
    $n = (Select-String -Path "$out\stderr.log" -Pattern "arena exhausted" -SimpleMatch -List -ErrorAction SilentlyContinue | Measure-Object).Count
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    "try $i : " + $(if ($n) { "HANG (arena exhausted)" } else { "ok" })
    if ($n) { $fails++; break }
}
"hangs: $fails in $i tries"
