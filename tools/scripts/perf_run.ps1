# One headless gameplay run for performance A/B work (same protocol as run_headless.ps1 + the scripted input in the task
# notes), at High priority so other game instances / builds on the machine disturb it less.
#   perf_run.ps1 -Name x [-Config RelWithDebInfo] [-EnvSet "A=1;B=2"] [-Profile]
# Prints: <name> (<env>): fps=<vif counter delta t=150..200 / 50>. Output in work\<name>\ (stderr.log, gs_*.png).
param([string]$Name, [string]$Config = 'RelWithDebInfo', [string]$EnvSet = '', [switch]$Profile)
$wd = 'D:\KillzoneRecomp'; $iso = 'C:\Users\drakolord\Downloads\Killzone (USA)\Killzone (USA).iso'
$out = "$wd\work\$Name"; Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $out | Out-Null
$env:KZ_FPS = '120'; $env:KZ_IPU = 'off'
$env:KZ_INPUT_SCRIPT = '60:start:0.3;80:cross:0.3;90:cross:0.3;100:cross:0.3;110:cross:0.3;120:cross:0.3;130:cross:0.3;140:cross:0.3;150:cross:0.3;160:cross:0.3'
$env:KZ_PROFILE = $(if ($Profile) { '160,40' } else { '' })
$env:PS2X_HEADLESS = '1'; $env:PS2X_HEADLESS_INTERVAL = '10'; $env:PS2X_HEADLESS_SECONDS = '235'
$env:KZ_SHOT_DIR = $out; $env:KZ_SHOT_INTERVAL = '10'
$set = @()
foreach ($kv in ($EnvSet -split ';' | Where-Object { $_ })) { $p = $kv -split '=', 2; Set-Item "env:$($p[0])" $p[1]; $set += $p[0] }
$p = Start-Process "$wd\build\$Config\killzone.exe" -ArgumentList @('--elf', 'game\SCUS_974.02', '--iso', "`"$iso`"", '--no-launcher') `
    -WorkingDirectory $wd -RedirectStandardOutput "$out\stdout.log" -RedirectStandardError "$out\stderr.log" -PassThru -NoNewWindow
try { $p.PriorityClass = 'High' } catch {}
if (-not $p.WaitForExit((235 + 30) * 1000)) { Stop-Process -Id $p.Id -Force }
foreach ($k in $set) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
$vals = @{}
Get-Content "$out\stderr.log" | Select-String 'headless\] t=' | ForEach-Object {
    $t = [int]([regex]::Match($_.Line, 't=([0-9]+)s').Groups[1].Value); $vals[$t] = [int]([regex]::Match($_.Line, 'vif=([0-9]+)').Groups[1].Value) }
$fps = if ($vals.ContainsKey(200) -and $vals.ContainsKey(150)) { [math]::Round(($vals[200] - $vals[150]) / 50, 1) } else { -1 }
"$Name ($EnvSet): fps=$fps"
