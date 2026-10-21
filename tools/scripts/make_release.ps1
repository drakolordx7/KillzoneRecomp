# Builds a public release zip from a build folder: program files, resources, the Visual C++ runtime DLLs (app-local,
# so players need no redistributable) and tools\release\README.txt. No player data (ini, memory cards, disc files,
# shader cache) goes in.
# Usage: make_release.ps1 -Version 0.1.0 [-From D:\KillzoneRecomp\dist\Killzone]
param([Parameter(Mandatory)][string]$Version, [string]$From = 'D:\KillzoneRecomp\dist\Killzone')
$ErrorActionPreference = 'Stop'
$root = 'D:\KillzoneRecomp'
$name = "Killzone-PC-v$Version-alpha"
$stage = "$root\work\release\$name"
$zip = "$root\work\release\$name.zip"
$crt = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT' -Directory |
    Sort-Object FullName -Descending | Select-Object -First 1

if (Test-Path $stage) { throw "$stage exists; delete it or pick another version" }
New-Item -ItemType Directory -Force "$stage\Killzone" | Out-Null
Copy-Item "$From\killzone.exe" "$stage\Killzone"
Get-ChildItem "$From\*.dll" | Copy-Item -Destination "$stage\Killzone"
Copy-Item -Recurse "$From\resources" "$stage\Killzone\resources"
foreach ($dll in 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    Copy-Item "$($crt.FullName)\$dll" "$stage\Killzone"
}
(Get-Content "$root\tools\release\README.txt" -Raw).Replace('{VERSION}', $Version) | Set-Content -Encoding UTF8 "$stage\Killzone\README.txt"

& "$env:ProgramFiles\7-Zip\7z.exe" a -tzip -mx=9 $zip "$stage\Killzone" | Out-Null
if ($LASTEXITCODE -ne 0) { Compress-Archive -Path "$stage\Killzone" -DestinationPath $zip -CompressionLevel Optimal }
"{0}  {1:N0} MB  (runtime from {2})" -f $zip, ((Get-Item $zip).Length / 1MB), $crt.FullName
