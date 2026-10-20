# Compila build-o2\Release\MonsterHunterTri-o2pipe.exe: unidades generadas /O2 + canalizacion GX.
# Reutiliza los objetos ya compilados en build-o2 (solo recompila gx.cpp, main.cpp e ios.cpp).
# Uso: powershell -ExecutionPolicy Bypass -File tools\gx-pipeline\Reconstruir-O2Pipe.ps1
param([int]$Jobs = 5, [string]$Deps = "$env:LOCALAPPDATA/Programs/NWiiRecomp/source/build/_deps")
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$cmake = 'C:\Program Files\CMake\bin\cmake.exe'
$deps = $Deps -replace '\\','/'
$inc = (Join-Path $PSScriptRoot 'o2-pipeline.cmake') -replace '\\','/'
& $cmake -S (Join-Path $root 'export') -B (Join-Path $root 'build-o2') -G "Visual Studio 17 2022" -A x64 `
  -DNWII_PREVIEW_NAME=o2pipe -DNWII_HOST_AVX2=ON `
  "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /W3 /GR /EHsc /bigobj /utf-8 /MP$Jobs /wd4102 /wd4996" `
  "-DCMAKE_PROJECT_MonsterHunterTri_INCLUDE=$inc" `
  "-DFETCHCONTENT_SOURCE_DIR_SDL2=$deps/sdl2-src" "-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$deps/tomlplusplus-src" `
  "-DFETCHCONTENT_SOURCE_DIR_GLAD=$deps/glad-src" -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
if ($LASTEXITCODE -ne 0) { throw "Configuracion fallida: $LASTEXITCODE" }
$t = Get-Date
& $cmake --build (Join-Path $root 'build-o2') --config Release --target MonsterHunterTri --parallel 2
if ($LASTEXITCODE -ne 0) { throw "Compilacion fallida: $LASTEXITCODE" }
"Compilado en {0:N0} min: build-o2\Release\MonsterHunterTri-o2pipe.exe" -f ((Get-Date)-$t).TotalMinutes
