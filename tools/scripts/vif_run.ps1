# One headless gameplay run (same protocol as perf_run.ps1) with PS2X_VIF1_STATS=1 so vif_metric.py can read worker ms/frame.
#   vif_run.ps1 -Name x [-Config pd] [-EnvSet "A=1;B=2"] [-Profile] [-Seconds 235] [-Ipu] [-NoStats] [-Exe killzone.exe]
param([string]$Name, [string]$Config = 'pd', [string]$EnvSet = '', [switch]$Profile, [int]$Seconds = 235, [switch]$Ipu, [string]$Exe = "killzone.exe", [switch]$NoStats)
$wd = 'D:\KillzoneRecomp'; $iso = 'C:\Users\drakolord\Downloads\Killzone (USA)\Killzone (USA).iso'
$out = "$wd\work\$Name"; Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force $out | Out-Null
$env:KZ_FPS = '120'
if ($Ipu) { Remove-Item env:KZ_IPU -ErrorAction SilentlyContinue } else { $env:KZ_IPU = 'off' }
$env:KZ_INPUT_SCRIPT = '60:start:0.3;80:cross:0.3;90:cross:0.3;100:cross:0.3;110:cross:0.3;120:cross:0.3;130:cross:0.3;140:cross:0.3;150:cross:0.3;160:cross:0.3'
$env:KZ_PROFILE = $(if ($Profile) { '160,40' } else { '' })
$env:KZ_PROFILE_OUT = $(if ($Profile) { "$out\profile.txt" } else { '' })
$env:PS2X_VIF1_STATS = $(if ($NoStats) { '' } else { '1' })
$env:_NT_SYMBOL_PATH = "$wd\build\$Config"   # the PDB sits next to the exe (linked with /PDBALTPATH:%_PDB%)
$env:PS2X_HEADLESS = '1'; $env:PS2X_HEADLESS_INTERVAL = '10'; $env:PS2X_HEADLESS_SECONDS = "$Seconds"
$env:KZ_SHOT_DIR = $out; $env:KZ_SHOT_INTERVAL = '10'
$set = @()
foreach ($kv in ($EnvSet -split ';' | Where-Object { $_ })) { $p = $kv -split '=', 2; Set-Item "env:$($p[0])" $p[1]; $set += $p[0] }
$p = Start-Process "$wd\build\$Config\$Exe" -ArgumentList @('--elf', 'game\SCUS_974.02', '--iso', "`"$iso`"", '--no-launcher') `
    -WorkingDirectory $wd -RedirectStandardOutput "$out\stdout.log" -RedirectStandardError "$out\stderr.log" -PassThru -NoNewWindow
try { $p.PriorityClass = 'High' } catch {}
if (-not $p.WaitForExit(($Seconds + 30) * 1000)) { Stop-Process -Id $p.Id -Force }
foreach ($k in $set) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
"$Name done, exit=$($p.ExitCode)"
