@echo off
rem Perfil de la CPU durante un video (cinematica): que funciones del juego se llevan el tiempo.
rem Resultado en logs\perfil_video.log (lineas [CPUProfile] cada 5 s).
setlocal
cd /d "%~dp0\..\.."
set "portExe=%cd%\MonsterHunterTri.exe"
if not exist "%portExe%" (echo Falta %portExe% & pause & exit /b 1)
for /f "tokens=1 delims==" %%v in ('set NWII_ 2^>nul') do set "%%v="
set NWII_CPU_PROFILE=1
set NWII_CPU_PROFILE_FINE=1
set NWII_HITCH_LOG=1
echo Mira el video de introduccion (o cualquier cinematica de video) unos 30-60 segundos y cierra el juego.
"%portExe%" "%cd%\game\DATA" "%cd%\monster-hunter-tri-native.toml" > "%cd%\logs\perfil_video.log" 2>&1
echo Listo: logs\perfil_video.log
pause
