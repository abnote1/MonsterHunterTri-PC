@echo off
rem Juega guardando el registro del port en logs\ (para analizar problemas).
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "stamp=%%t"
set NWII_HITCH_LOG=1
set "log=%~dp0logs\juego-%stamp%.log"
echo Registro: %log%
"%~dp0MonsterHunterTri.exe" > "%log%" 2>&1
echo El proceso ha terminado con codigo %errorlevel%.
pause
