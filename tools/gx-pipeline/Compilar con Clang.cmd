@echo off
rem Compila el port con clang-cl en build-clang\ (ver Reconstruir-Clang.ps1).
rem Doble clic para lanzarlo. Para menos memoria: arrastra o ejecuta con  -Jobs 2
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Reconstruir-Clang.ps1" %*
echo.
pause
