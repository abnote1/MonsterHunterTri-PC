@echo off
rem Juega borrando la pantalla en magenta: los huecos sin geometria se ven magenta. Captura cada 2 s (10 ultimas)
rem en logs\capturas\magenta_N.bmp. Usado para investigar el brillo blanco.
setlocal
cd /d "%~dp0\..\.."
set "portExe=%cd%\build-o2\Release\MonsterHunterTri-o2pipe.exe"
if not exist "%portExe%" (echo Falta %portExe% & pause & exit /b 1)
for /f "tokens=1 delims==" %%v in ('set NWII_ 2^>nul') do set "%%v="
if not exist "%cd%\logs\capturas" mkdir "%cd%\logs\capturas"
set "NWII_SCREENSHOT=%cd%\logs\capturas\magenta"
set NWII_SCREENSHOT_KEEP=10
set NWII_CLEAR_MAGENTA=1
"%portExe%" "%cd%\game\DATA" "%cd%\monster-hunter-tri-native.toml" > "%cd%\logs\capturas\magenta.log" 2>&1
echo Capturas en logs\capturas\magenta_*.bmp
pause
