# Usage: ghidra_inspect.ps1 -Code "0x1BFF3C+0x1BFEAC" -Data "0x5DA364" -Out <repo>\work\inspect.c
param([string]$Code = "-", [string]$Data = "-", [string]$Out)
$env:JAVA_HOME = '<repo>\tools\jdk-21.0.12.1+1'; $env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
& '<repo>\tools\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat' <repo>\ghidra Killzone -process SCUS_974.02 -noanalysis -readOnly -scriptPath <repo>\tools\ghidra_scripts -postScript KzInspect2.java $Code $Data $Out *> "$Out.log"
if (Test-Path $Out) { "ok: $Out" } else { Get-Content "$Out.log" -Tail 20 }
