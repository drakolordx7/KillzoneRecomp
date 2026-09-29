@echo off
rem Build the Killzone runner. Usage: build.bat [Debug|RelWithDebInfo]
set CFG=%1
if "%KZ_JOBS%"=="" set KZ_JOBS=8
if "%CFG%"=="" set CFG=RelWithDebInfo
call "%~dp0vsenv.bat" cmake -S <repo> -B <repo>\build\%CFG% -G Ninja -DCMAKE_BUILD_TYPE=%CFG% || exit /b 1
call "%~dp0vsenv.bat" cmake --build <repo>\build\%CFG% --target killzone -j %KZ_JOBS%
