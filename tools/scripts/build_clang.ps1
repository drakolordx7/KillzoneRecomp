# Opt-in clang-cl build of the runner into build\<Name> (the shipping build stays MSVC: build.bat).
#   build_clang.ps1 [-Name clang] [-Cmake '-DKZ_CLANG_LTO=thin -DCMAKE_AR=D:/LLVM/bin/llvm-lib.exe'] [-Jobs 12] [-LlvmBin D:\LLVM\bin]
# Variants (docs/findings.md "clang-cl build"): plain = no -Cmake; LTO = -DKZ_CLANG_LTO=thin plus the llvm-lib archiver;
# PGO = a -DKZ_CLANG_PGO_GEN=ON build, one gameplay run with LLVM_PROFILE_FILE=<dir>\kz_%p.profraw, llvm-profdata merge, then
# -DKZ_CLANG_PGO_USE=<merged .profdata>. Everything (build, temp) stays on D:.
# The first run copies build\RelWithDebInfo's FFmpeg prefix and shader cache (no re-download) and points FetchContent at its
# dependency sources (read only).
param([string]$Name = 'clang', [string]$Cmake = '', [int]$Jobs = 12, [string]$LlvmBin = 'D:\LLVM\bin')
$wd = 'D:\KillzoneRecomp'; $ref = "$wd\build\RelWithDebInfo"; $b = "$wd\build\$Name"
$tmp = "$wd\work\tmp"; New-Item -ItemType Directory -Force $tmp, "$b\ThirdParty" | Out-Null
$env:TEMP = $tmp; $env:TMP = $tmp
if (-not (Test-Path "$b\ThirdParty\ffmpeg-prefix")) { Copy-Item -Recurse "$ref\ThirdParty\ffmpeg-prefix" "$b\ThirdParty\ffmpeg-prefix" }
if (-not (Test-Path "$b\cache") -and (Test-Path "$ref\cache")) { Copy-Item -Recurse "$ref\cache" "$b\cache" }
$llvm = $LlvmBin.Replace('\', '/')
$args1 = @('-S', $wd, '-B', $b, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
    "-DCMAKE_C_COMPILER=$llvm/clang-cl.exe", "-DCMAKE_CXX_COMPILER=$llvm/clang-cl.exe", "-DCMAKE_LINKER=$llvm/lld-link.exe",
    "-DFETCHCONTENT_SOURCE_DIR_RAYLIB=$ref/_deps/raylib-src", "-DFETCHCONTENT_SOURCE_DIR_IMGUI=$ref/_deps/imgui-src",
    "-DFETCHCONTENT_SOURCE_DIR_RLIMGUI=$ref/_deps/rlimgui-src") + ($Cmake -split ' ' | Where-Object { $_ })
& "$wd\tools\scripts\vsenv.bat" cmake @args1
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$wd\tools\scripts\vsenv.bat" cmake --build $b --target killzone -j $Jobs
