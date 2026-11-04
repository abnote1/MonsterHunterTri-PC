@echo off
rem Crea el kit de instalacion (..\MonsterHunterTri-Kit y .zip) con el ejecutable rapido (clang) y el runtime de Visual C++.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Crear-Kit.ps1"
pause
