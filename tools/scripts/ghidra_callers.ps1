# Usage: ghidra_callers.ps1 -Addrs "0x2b5928" -Depth 2 -Out D:\KillzoneRecomp\work\callers.txt
param([string]$Addrs, [int]$Depth = 2, [string]$Out)
$env:JAVA_HOME = 'D:\KillzoneRecomp\tools\jdk-21.0.12.1+1'; $env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
& 'D:\KillzoneRecomp\tools\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat' D:\KillzoneRecomp\ghidra Killzone -process SCUS_974.02 -noanalysis -readOnly -scriptPath D:\KillzoneRecomp\tools\ghidra_scripts -postScript KzCallers.java $Addrs $Depth $Out *> "$Out.log"
if (Test-Path $Out) { Get-Content $Out } else { "failed; see $Out.log" }
