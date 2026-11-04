# Compila el port con clang-cl (el LLVM que trae Visual Studio 2022) en build-clang\.
# Mismo codigo y mismas optimizaciones por unidad que Reconstruir-O2Pipe.ps1, pero con el
# optimizador de LLVM, que suele sacar bastante mas del codigo recompilado del juego.
# Resultado: build-clang\MonsterHunterTri-clang.exe (build-o2 no se toca).
#
# Requisito (una vez): Visual Studio Installer > Modificar > Componentes individuales >
#   "Herramientas de C++ Clang para Windows" (y "Herramientas de CMake de C++ para Windows").
#
# Uso: powershell -ExecutionPolicy Bypass -File tools\gx-pipeline\Reconstruir-Clang.ps1 [-Jobs 4] [-Deps <_deps de NWiiRecomp>]
#   -Jobs: compilaciones a la vez (cada una puede usar 2-4 GB de RAM con las unidades grandes).
param([int]$Jobs = 4, [string]$Deps = "$env:LOCALAPPDATA/Programs/NWiiRecomp/source/build/_deps", [switch]$Limpio)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$build = Join-Path $root 'build-clang'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'No encuentro vswhere.exe: hace falta Visual Studio 2022.' }
# Busca clang-cl en todas las instalaciones de Visual Studio (Community, Build Tools, Preview...)
# y, si no, en LLVM instalado aparte o en el PATH.
$vsAll = @(& $vswhere -all -products * -prerelease -property installationPath)
if (-not $vsAll) { throw 'No encuentro ninguna instalacion de Visual Studio 2022.' }
$vs = $null; $clang = $null
foreach ($v in $vsAll) {
  $c = Join-Path $v 'VC\Tools\Llvm\x64\bin\clang-cl.exe'
  if (Test-Path $c) { $vs = $v; $clang = $c; break }
}
if (-not $clang) {
  foreach ($c in @((Join-Path $env:ProgramFiles 'LLVM\bin\clang-cl.exe'), (Get-Command clang-cl.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source))) {
    if ($c -and (Test-Path $c)) { $clang = $c; break }
  }
  # para vcvars, ninja y cmake se usa la primera instalacion con C++
  foreach ($v in $vsAll) { if (Test-Path (Join-Path $v 'VC\Auxiliary\Build\vcvars64.bat')) { $vs = $v; break } }
}
if (-not $clang) {
  Write-Host 'Instalaciones de Visual Studio encontradas:'
  $vsAll | ForEach-Object { Write-Host "  $_  (clang-cl: $(Test-Path (Join-Path $_ 'VC\Tools\Llvm\x64\bin\clang-cl.exe')))" }
  throw "Falta clang-cl. En el Visual Studio Installer pulsa Modificar en la instalacion de arriba, pestana 'Componentes individuales', busca 'Clang' y marca 'Compilador de C++ Clang para Windows' (o 'C++ Clang Compiler for Windows'). Despues vuelve a ejecutar este script."
}
if (-not $vs) { throw 'No encuentro vcvars64.bat de Visual Studio (herramientas de C++).' }
$cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$ninja = Join-Path $cmakeDir 'Ninja\ninja.exe'
$cmake = Join-Path $cmakeDir 'CMake\bin\cmake.exe'
if (-not (Test-Path $cmake)) { $cmake = 'C:\Program Files\CMake\bin\cmake.exe' }
if (-not (Test-Path $ninja)) { throw "No encuentro ninja.exe: instala 'Herramientas de CMake de C++ para Windows' en el Visual Studio Installer." }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$lld = Join-Path (Split-Path $clang -Parent) 'lld-link.exe'

$deps = $Deps -replace '\\', '/'
$inc = (Join-Path $PSScriptRoot 'o2-pipeline.cmake') -replace '\\', '/'
# -ffp-contract=off: sin FMA automaticas, para que los calculos de coma flotante den lo mismo que con MSVC.
$cxx = '/DWIN32 /D_WINDOWS /GR /EHsc /utf-8 /w /clang:-ffp-contract=off /clang:-mtune=znver2'
$cc = '/DWIN32 /D_WINDOWS /utf-8 /w'
if ($Limpio -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }
# Unidades generadas compactadas (compactar.py): solo rehace las que han cambiado.
$compactar = Join-Path $PSScriptRoot 'compactar.py'
if (Test-Path $compactar) {
  if (Get-Command py -ErrorAction SilentlyContinue) { & py -3 $compactar } else { & python $compactar }
  if ($LASTEXITCODE -ne 0) { throw "compactar.py ha fallado (codigo $LASTEXITCODE). Hace falta Python 3." }
}

$linker = ''
if (Test-Path $lld) { $linker = "-DCMAKE_LINKER=`"$lld`"" }
$bat = Join-Path $env:TEMP 'mhtri-clang-build.cmd'
@"
@echo off
call "$vcvars" >nul || exit /b 1
"$cmake" -S "$root\export" -B "$build" -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_C_COMPILER=$clang" "-DCMAKE_CXX_COMPILER=$clang" $linker -DCMAKE_BUILD_TYPE=Release "-DCMAKE_EXE_LINKER_FLAGS=/MAP" -DNWII_PREVIEW_NAME=clang -DNWII_HOST_AVX2=ON "-DCMAKE_CXX_FLAGS=$cxx" "-DCMAKE_C_FLAGS=$cc" "-DCMAKE_PROJECT_MonsterHunterTri_INCLUDE=$inc" "-DFETCHCONTENT_SOURCE_DIR_SDL2=$deps/sdl2-src" "-DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$deps/tomlplusplus-src" "-DFETCHCONTENT_SOURCE_DIR_GLAD=$deps/glad-src" -DFETCHCONTENT_UPDATES_DISCONNECTED=ON || exit /b 2
"$cmake" --build "$build" --target MonsterHunterTri -j $Jobs || exit /b 3
"@ | Set-Content -Path $bat -Encoding ASCII

Write-Host "Visual Studio: $vs"
Write-Host "clang-cl:      $clang"
Write-Host "Compilando en $build con $Jobs trabajos (la primera vez tarda mucho: es todo el codigo del juego)..."
$t = Get-Date
& cmd.exe /c "`"$bat`""
$rc = $LASTEXITCODE
if ($rc -eq 2) { throw 'La configuracion de CMake ha fallado (mira los mensajes de arriba).' }
if ($rc -ne 0) { throw "La compilacion ha fallado (codigo $rc). Si es por falta de memoria, repite con -Jobs 2: continua donde lo dejo." }
$exe = Get-ChildItem -Path $build -Recurse -Filter 'MonsterHunterTri-clang.exe' | Select-Object -First 1
if (-not $exe) { throw 'Compilado, pero no encuentro MonsterHunterTri-clang.exe.' }
Copy-Item $exe.FullName (Join-Path $root 'MonsterHunterTri-clang.exe') -Force
"Compilado en {0:N0} min: {1}" -f ((Get-Date) - $t).TotalMinutes, $exe.FullName
"Copiado a $root\MonsterHunterTri-clang.exe (junto a game\): ya se puede abrir o probar con 'Prueba B (clang).cmd'."
