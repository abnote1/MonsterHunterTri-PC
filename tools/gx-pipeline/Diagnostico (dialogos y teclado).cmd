@echo off
rem Diagnostico de: pantalla negra con dialogos fuera de la aldea, teclado virtual al crear
rem personaje, cortes en cinematicas y desincronizacion de audio.
rem - Borra la pantalla en MAGENTA: si una zona sale magenta, el juego no dibujo nada ahi;
rem   si sale negra, dibujo algo negro (una imagen que el port no tiene bien).
rem - Guarda una captura cada 50 ms (las 40 ultimas, 2 s) en logs\diag\pantalla_N.bmp
rem - Registra las copias de imagen del juego (EFB) y los tirones en logs\diag\diag.log
setlocal
cd /d "%~dp0\..\.."
set "portExe=%cd%\build-o2\Release\MonsterHunterTri-o2pipe.exe"
if not exist "%portExe%" set "portExe=%cd%\MonsterHunterTri.exe"
if not exist "%portExe%" (echo Falta %portExe% & pause & exit /b 1)
for /f "tokens=1 delims==" %%v in ('set NWII_ 2^>nul') do set "%%v="
if exist "%cd%\logs\diag" rmdir /s /q "%cd%\logs\diag"
mkdir "%cd%\logs\diag"
set "NWII_SCREENSHOT=%cd%\logs\diag\pantalla"
set NWII_SCREENSHOT_KEEP=40
rem una captura cada 50 ms (2 s de historial): para ver parpadeos
set NWII_SCREENSHOT_MS=50
set NWII_CLEAR_MAGENTA=1
set NWII_EFBCOPY_TRACE=1
set NWII_FXTRACE2=1
set NWII_HITCH_LOG=1
echo Reproduce los problemas (cada uno unos segundos):
echo   Abre el teclado virtual (crear personaje), espera 5 segundos con el parpadeo
echo   en pantalla y cierra el juego enseguida (se guardan los ultimos 2 segundos).
echo Al terminar cierra el juego; las capturas y el registro quedan en logs\diag.
"%portExe%" "%cd%\game\DATA" "%cd%\monster-hunter-tri-native.toml" > "%cd%\logs\diag\diag.log" 2>&1
echo El proceso ha terminado con codigo %errorlevel%.
pause
