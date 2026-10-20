@echo off
rem Juega SIN la suma del bloom (diagnostico) y guarda una captura cada 2 s (se conservan las 10 ultimas)
rem en logs\capturas\sinbloom_N.bmp. Usado para investigar el brillo blanco.
setlocal
cd /d "%~dp0\..\.."
set "portExe=%cd%\build-o2\Release\MonsterHunterTri-o2pipe.exe"
if not exist "%portExe%" (echo Falta %portExe% & pause & exit /b 1)
for /f "tokens=1 delims==" %%v in ('set NWII_ 2^>nul') do set "%%v="
if not exist "%cd%\logs\capturas" mkdir "%cd%\logs\capturas"
set "NWII_SCREENSHOT=%cd%\logs\capturas\sinbloom"
set NWII_SCREENSHOT_KEEP=10
set NWII_FX_NO_BLOOM=1
"%portExe%" "%cd%\game\DATA" "%cd%\monster-hunter-tri-native.toml" > "%cd%\logs\capturas\sinbloom.log" 2>&1
echo Capturas en logs\capturas\sinbloom_*.bmp
pause
