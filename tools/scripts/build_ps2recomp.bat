@echo off
call "%~dp0vsenv.bat" cmake -S D:\KillzoneRecomp\ext\PS2Recomp -B D:\KillzoneRecomp\build\ps2recomp -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF || exit /b 1
call "%~dp0vsenv.bat" cmake --build D:\KillzoneRecomp\build\ps2recomp -j 8
