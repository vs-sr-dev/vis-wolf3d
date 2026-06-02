@echo off
setlocal
if not defined WATCOM set WATCOM=%~dp0..\tools\OW
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h;%WATCOM%\h\win

cd /d "%~dp0"
if not exist ..\build mkdir ..\build

echo === Compiling wavtsta.c ===
wcc -zq -bt=windows -ml -ox -s -fo=..\build\wavtsta.obj wavtsta.c
if errorlevel 1 goto :fail

echo === Linking WAVTSTA.EXE ===
wlink @link_wavtsta.lnk
if errorlevel 1 goto :fail

echo === Staging WAVTSTA.EXE into cd_root_wavtsta ===
if not exist ..\cd_root_wavtsta mkdir ..\cd_root_wavtsta
copy /y ..\build\WAVTSTA.EXE ..\cd_root_wavtsta\WAVTSTA.EXE >nul
if errorlevel 1 goto :fail

echo BUILD OK
dir ..\build\WAVTSTA.EXE
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
