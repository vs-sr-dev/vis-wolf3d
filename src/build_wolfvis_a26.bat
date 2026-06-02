@echo off
setlocal
if not defined WATCOM set WATCOM=%~dp0..\tools\OW
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h;%WATCOM%\h\win

cd /d "%~dp0"
if not exist ..\build mkdir ..\build

echo === Compiling wolfvis_a26.c ===
wcc -zq -bt=windows -ml -ox -s -fo=..\build\wolfvis_a26.obj wolfvis_a26.c
if errorlevel 1 goto :fail

echo === Linking WOLFA26.EXE ===
wlink @link_wolfvis_a26.lnk
if errorlevel 1 goto :fail

echo === Copying WOLFA26.EXE to cd_root_a26 ===
if not exist ..\cd_root_a26 mkdir ..\cd_root_a26
copy /y ..\build\WOLFA26.EXE ..\cd_root_a26\WOLFA26.EXE >nul
if errorlevel 1 goto :fail

echo BUILD OK
dir ..\build\WOLFA26.EXE
exit /b 0

:fail
echo BUILD FAILED
exit /b 1
