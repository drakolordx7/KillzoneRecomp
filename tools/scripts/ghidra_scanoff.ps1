# Usage: ghidra_scanoff.ps1 -Offs "0x454,0x164" -Out D:\KillzoneRecomp\work\scan.txt [-Mn sw]
param([string]$Offs, [string]$Out, [string]$Mn = "")
$env:JAVA_HOME = 'D:\KillzoneRecomp\tools\jdk-21.0.12.1+1'; $env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
& 'D:\KillzoneRecomp\tools\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat' D:\KillzoneRecomp\ghidra Killzone -process SCUS_974.02 -noanalysis -readOnly -scriptPath D:\KillzoneRecomp\tools\ghidra_scripts -postScript KzScanOff.java $Offs $Out $Mn *> "$Out.log"
if (Test-Path $Out) { "ok: $Out" } else { Get-Content "$Out.log" -Tail 20 }
