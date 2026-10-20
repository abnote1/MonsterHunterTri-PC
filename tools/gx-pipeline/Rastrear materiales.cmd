@echo off
rem Vuelca los materiales 3D (TEV, texturas, canal de color y luces). Registro en logs\mat-*.log
setlocal
cd /d "%~dp0\..\.."
set "portExe=%cd%\build-o2\Release\MonsterHunterTri-o2pipe.exe"
if not exist "%portExe%" (echo Falta %portExe% & pause & exit /b 1)
for /f "tokens=1 delims==" %%v in ('set NWII_ 2^>nul') do set "%%v="
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "stamp=%%t"
set NWII_MATTRACE=1
set "log=%cd%\logs\mat-%stamp%.log"
echo Registro: %log%
"%portExe%" "%cd%\game\DATA" "%cd%\monster-hunter-tri-native.toml" > "%log%" 2>&1
pause
