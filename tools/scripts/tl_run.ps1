# One headless gameplay run of the task scene (KZ_FPS=120 unless set in -EnvSet, KZ_IPU=off unless -Ipu, scripted input, run_headless.ps1) with extra env:
#   tl_run.ps1 -Name n -EnvSet "A=1;B=2" [-Config tl] [-Seconds 235] [-Interval 10] [-Ipu]
# -Ipu leaves KZ_IPU unset (movies play). Env vars set here are removed again when the run ends.
param([string]$Name, [string]$EnvSet = '', [string]$Config = 'tl', [int]$Seconds = 235, [int]$Interval = 10, [switch]$Ipu)
$env:KZ_FPS = '120'
if ($Ipu) { Remove-Item Env:KZ_IPU -ErrorAction SilentlyContinue } else { $env:KZ_IPU = 'off' }
$env:KZ_INPUT_SCRIPT = '60:start:0.3;80:cross:0.3;90:cross:0.3;100:cross:0.3;110:cross:0.3;120:cross:0.3;130:cross:0.3;140:cross:0.3;150:cross:0.3;160:cross:0.3'
$set = @()
foreach ($kv in ($EnvSet -split ';' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; Set-Item -Path "Env:$k" -Value $v; $set += $k }
try { & 'D:\KillzoneRecomp\tools\scripts\run_headless.ps1' -Name $Name -Seconds $Seconds -Interval $Interval -Config $Config -Extra '--no-launcher' }
finally { foreach ($k in $set) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue } }
