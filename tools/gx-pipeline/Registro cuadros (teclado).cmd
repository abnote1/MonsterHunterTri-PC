@echo off
rem Registro por cuadro (teclado virtual): que dibuja el juego en cada cuadro.
rem Resultado en logs\cuadros.log (lineas [Cuadro]).
setlocal
cd /d "%~dp0\..\.."
set "portExe=%cd%\build-o2\Release\MonsterHunterTri-o2pipe.exe"
if not exist "%portExe%" set "portExe=%cd%\MonsterHunterTri.exe"
if not exist "%portExe%" (echo Falta el ejecutable & pause & exit /b 1)
if not exist "%cd%\logs" mkdir "%cd%\logs"
for /f "tokens=1 delims==" %%v in ('set NWII_ 2^>nul') do set "%%v="
set NWII_FRAMELOG=1
set NWII_HITCH_LOG=1
echo Abre el teclado virtual (crear personaje o poner nombre), dejalo abierto unos 10 segundos y cierra el juego.
"%portExe%" "%cd%\game\DATA" "%cd%\monster-hunter-tri-native.toml" > "%cd%\logs\cuadros.log" 2>&1
echo Listo: logs\cuadros.log
pause
