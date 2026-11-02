# Usage: ghidra_inspect.ps1 -Code "0x1BFF3C+0x1BFEAC" -Data "0x5DA364" -Out work\inspect.c
param([string]$Code = "-", [string]$Data = "-", [string]$Out)
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$env:JAVA_HOME = "$root\tools\jdk-21.0.12.1+1"; $env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
& "$root\tools\ghidra_12.1.3_PUBLIC\support\analyzeHeadless.bat" "$root\ghidra" Killzone -process SCUS_974.02 -noanalysis -readOnly -scriptPath "$root\tools\ghidra_scripts" -postScript KzInspect2.java $Code $Data $Out *> "$Out.log"
if (Test-Path $Out) { "ok: $Out" } else { Get-Content "$Out.log" -Tail 20 }
