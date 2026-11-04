@echo off
rem Crea o actualiza la carpeta portable (..\MonsterHunterTri-Portable) con el ejecutable rapido (clang).
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Crear-Portable.ps1"
pause
