@echo off
rem Juega con el bloom tal cual y guarda una captura cada 2 s (se conservan las 10 ultimas)
rem en logs\capturas\normal_N.bmp. Usado para investigar el brillo blanco.
setlocal
cd /d "%~dp0\..\.."
set "portExe=%cd%\build-o2\Release\MonsterHunterTri-o2pipe.exe"
if not exist "%portExe%" (echo Falta %portExe% & pause & exit /b 1)
for /f "tokens=1 delims==" %%v in ('set NWII_ 2^>nul') do set "%%v="
if not exist "%cd%\logs\capturas" mkdir "%cd%\logs\capturas"
set "NWII_SCREENSHOT=%cd%\logs\capturas\normal"
set NWII_SCREENSHOT_KEEP=10
"%portExe%" "%cd%\game\DATA" "%cd%\monster-hunter-tri-native.toml" > "%cd%\logs\capturas\normal.log" 2>&1
echo Capturas en logs\capturas\normal_*.bmp
pause
