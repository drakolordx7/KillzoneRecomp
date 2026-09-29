@echo off
rem Usage: vsenv.bat <command...>  -- runs command inside the VS 2026 x64 dev environment
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
%*
