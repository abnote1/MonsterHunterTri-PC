@echo off
rem Igual que "Jugar con registro.cmd" pero con escala interna 1x (prueba de carga de GPU).
rem para que Claude analice las bajadas. Por defecto usa la version recien compilada de build-o2;
rem arrastra otro .exe encima de este archivo para usar ese.
setlocal
cd /d "%~dp0\..\.."
set "portExe=%cd%\build-o2\Release\MonsterHunterTri-o2pipe.exe"
if not "%~1"=="" set "portExe=%~1"
if not exist "%portExe%" (echo Falta %portExe% & pause & exit /b 1)
for /f "tokens=1 delims==" %%v in ('set NWII_ 2^>nul') do set "%%v="
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "stamp=%%t"
set NWII_HITCH_LOG=1
set NWII_TEXTURE_PROFILE=1
set "log=%cd%\logs\juego1x-%stamp%.log"
echo Registro: %log%
echo Juega normalmente; al cerrar el juego, avisa a Claude.
"%portExe%" "%cd%\game\DATA" "%cd%\tools\gx-pipeline\escala1x.toml" > "%log%" 2>&1
echo El proceso ha terminado con codigo %errorlevel%.
pause
