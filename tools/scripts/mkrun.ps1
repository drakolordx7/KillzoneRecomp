# Snapshot of a build dir's runnable files, so the build can relink while runs use the snapshot:
#   mkrun.ps1 -Tag x2 [-From pd]   ->  build\r_x2\killzone.exe (+pdb, dlls; resources/cache are junctions)
param([string]$Tag, [string]$From = 'pd')
$b = 'D:\KillzoneRecomp\build'; $src = "$b\$From"; $dst = "$b\r_$Tag"
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item "$src\killzone.exe", "$src\killzone.pdb" $dst -Force
Get-ChildItem "$src\*.dll" | ForEach-Object { if (-not (Test-Path "$dst\$($_.Name)")) { Copy-Item $_.FullName $dst } }
foreach ($d in 'resources', 'cache') { if (Test-Path "$dst\$d") { Remove-Item -Recurse -Force "$dst\$d" }; Copy-Item -Recurse "$src\$d" "$dst\$d" }
"$dst ready"
