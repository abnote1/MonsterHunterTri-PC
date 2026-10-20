@echo off
rem Instala el port de Monster Hunter Tri a partir de tu copia del juego.
rem Puedes arrastrar el ISO (y el paquete de texturas HD) encima de este archivo.
setlocal
cd /d "%~dp0"
powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0herramientas\Instalar.ps1" %*
echo.
pause
