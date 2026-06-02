@echo off
setlocal
if not defined WATCOM set WATCOM=%~dp0..\tools\OW
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h;%WATCOM%\h\win

cd /d "%~dp0"
if not exist ..\build mkdir ..\build

echo === Compiling wolfvis_a25.c ===
wcc -zq -bt=windows -ml -ox -s -fo=..\build\wolfvis_a25.obj wolfvis_a25.c
if errorlevel 1 goto :fail

echo === Linking WOLFA25.EXE ===
wlink @link_wolfvis_a25.lnk
if errorlevel 1 goto :fail

echo === Copying WOLFA25.EXE to cd_root_a25 ===
if not exist ..\cd_root_a25 mkdir ..\cd_root_a25
copy /y ..\build\WOLFA25.EXE ..\cd_root_a25\WOLFA25.EXE >nul
if errorlevel 1 goto :fail

echo BUILD OK
dir ..\build\WOLFA25.EXE
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
