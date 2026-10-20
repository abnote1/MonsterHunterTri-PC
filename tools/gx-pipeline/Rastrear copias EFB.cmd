@echo off
rem Igual que "Jugar con registro.cmd" pero anotando las copias EFB (formato y registro PE).
rem Usado para investigar el brillo blanco. Registro en logs\efbcopy-*.log
setlocal
cd /d "%~dp0\..\.."
set "portExe=%cd%\build-o2\Release\MonsterHunterTri-o2pipe.exe"
if not exist "%portExe%" (echo Falta %portExe% & pause & exit /b 1)
for /f "tokens=1 delims==" %%v in ('set NWII_ 2^>nul') do set "%%v="
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "stamp=%%t"
set NWII_EFBCOPY_TRACE=1
set "log=%cd%\logs\efbcopy-%stamp%.log"
echo Registro: %log%
"%portExe%" "%cd%\game\DATA" "%cd%\monster-hunter-tri-native.toml" > "%log%" 2>&1
pause
