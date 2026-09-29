# Usage: ghidra_callers.ps1 -Addrs "0x2b5928" -Depth 2 -Out <repo>\work\callers.txt
param([string]$Addrs, [int]$Depth = 2, [string]$Out)
$env:JAVA_HOME = '<repo>\tools\jdk-21.0.12.1+1'; $env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
& '<repo>\tools\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat' <repo>\ghidra Killzone -process SCUS_974.02 -noanalysis -readOnly -scriptPath <repo>\tools\ghidra_scripts -postScript KzCallers.java $Addrs $Depth $Out *> "$Out.log"
if (Test-Path $Out) { Get-Content $Out } else { "failed; see $Out.log" }
