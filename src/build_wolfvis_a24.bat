@echo off
setlocal
if not defined WATCOM set WATCOM=%~dp0..\tools\OW
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h;%WATCOM%\h\win

cd /d "%~dp0"
if not exist ..\build mkdir ..\build

echo === Compiling wolfvis_a24.c ===
wcc -zq -bt=windows -ml -ox -s -fo=..\build\wolfvis_a24.obj wolfvis_a24.c
if errorlevel 1 goto :fail

echo === Linking WOLFA24.EXE ===
wlink @link_wolfvis_a24.lnk
if errorlevel 1 goto :fail

echo === Copying WOLFA24.EXE to cd_root_a24 ===
if not exist ..\cd_root_a24 mkdir ..\cd_root_a24
copy /y ..\build\WOLFA24.EXE ..\cd_root_a24\WOLFA24.EXE >nul
if errorlevel 1 goto :fail

echo BUILD OK
dir ..\build\WOLFA24.EXE
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
