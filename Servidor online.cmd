@echo off
rem Modo online (ciudad): abre los dos servidores en este PC, cada uno en su ventana.
rem   servicios de Nintendo WFC (servidor\servidor_wfc.py) y servidor de Capcom (servidor\MH3SP, MH3SP).
rem Hace falta Python 3 (python.org). Si Windows pregunta por el cortafuegos, permite el acceso.
setlocal
cd /d "%~dp0servidor"
set "PY=python"
where py >nul 2>nul && set "PY=py -3"
start "MH3 online - servicios Nintendo (no cerrar)" cmd /k %PY% servidor_wfc.py
start "MH3 online - servidor Capcom MH3SP (no cerrar)" /d "%~dp0servidor\MH3SP" cmd /k %PY% master_server.py
echo Servidores abiertos en dos ventanas. Ya puedes abrir el juego y entrar en el modo online.
timeout /t 4 >nul
