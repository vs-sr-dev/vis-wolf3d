@echo off
setlocal
if not defined WATCOM set WATCOM=%~dp0..\tools\OW
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h;%WATCOM%\h\win

cd /d "%~dp0"
if not exist ..\build mkdir ..\build

echo === Compiling wavtest.c ===
wcc -zq -bt=windows -ml -ox -s -fo=..\build\wavtest.obj wavtest.c
if errorlevel 1 goto :fail

echo === Linking WAVTEST.EXE ===
wlink @link_wavtest.lnk
if errorlevel 1 goto :fail

echo === Staging WAVTEST.EXE into cd_root_wavtest ===
if not exist ..\cd_root_wavtest mkdir ..\cd_root_wavtest
copy /y ..\build\WAVTEST.EXE ..\cd_root_wavtest\WAVTEST.EXE >nul
if errorlevel 1 goto :fail

echo BUILD OK
dir ..\build\WAVTEST.EXE
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
