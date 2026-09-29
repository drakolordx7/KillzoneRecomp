@echo off
call "%~dp0vsenv.bat" cmake -S <repo>\ext\PS2Recomp -B <repo>\build\ps2recomp -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF || exit /b 1
call "%~dp0vsenv.bat" cmake --build <repo>\build\ps2recomp -j 8
