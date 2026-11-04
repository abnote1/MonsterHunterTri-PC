# Crea el kit de instalacion del port (sin nada del juego): otra persona pone su ISO
# (RMHE08) y su paquete de texturas HD y Instalar.cmd le monta el port.
#
#   powershell -ExecutionPolicy Bypass -File tools\Crear-Kit.ps1 [-Destino ruta] [-SinZip]
param([string]$Destino = "", [switch]$SinZip)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Destino) { $Destino = Join-Path (Split-Path -Parent $root) 'MonsterHunterTri-Kit' }
# Ejecutable: el de clang (el rapido) si existe; si no, el de MSVC.
$exe = $null
foreach ($c in 'MonsterHunterTri-clang.exe', 'build-o2\Release\MonsterHunterTri.exe', 'build-o2\Release\MonsterHunterTri-o2pipe.exe', 'MonsterHunterTri.exe') {
  $p = Join-Path $root $c
  if (Test-Path $p) { $exe = $p; break }
}
if (-not $exe) { throw "No encuentro el ejecutable del port en $root" }
Write-Host "Ejecutable: $exe"
$src = Join-Path $PSScriptRoot 'kit'
$dolphin = Join-Path $PSScriptRoot 'Dolphin-x64'
Write-Host "Destino: $Destino"
if (Test-Path $Destino) { Remove-Item $Destino -Recurse -Force }
$h = Join-Path $Destino 'herramientas'
New-Item -ItemType Directory -Force -Path $h | Out-Null

Copy-Item $exe (Join-Path $Destino 'MonsterHunterTri.exe')
# Runtime de Visual C++ junto al .exe (Instalar.ps1 ya no necesita descargarlo si esta)
$vsDirs = @()
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) { $vsDirs = @(& $vswhere -all -products * -prerelease -property installationPath) }
$vsDirs += @(Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*' -Directory -ErrorAction SilentlyContinue | ForEach-Object FullName)
$crt = $vsDirs | ForEach-Object { Get-ChildItem (Join-Path $_ 'VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT') -Directory -ErrorAction SilentlyContinue } |
       Sort-Object { $_.Parent.Parent.Name } -Descending | Select-Object -First 1
$crtDir = if ($crt) { $crt.FullName } elseif (Test-Path "$env:WINDIR\System32\msvcp140.dll") { "$env:WINDIR\System32" } else { $null }
if ($crtDir) {
  foreach ($d in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') { $f = Join-Path $crtDir $d; if (Test-Path $f) { Copy-Item $f $Destino -Force } }
  Write-Host "Runtime de Visual C++: $crtDir"
} else { Write-Warning 'No encontre el runtime de Visual C++: Instalar.cmd ofrecera descargarlo.' }
$ctl = Join-Path $root 'CONTROLES.md'
if (Test-Path $ctl) {
  (Get-Content $ctl -Encoding UTF8) -replace 'port de Pepe', 'port de TuNombre' | Set-Content (Join-Path $Destino 'CONTROLES.md') -Encoding UTF8
}
foreach ($f in 'gamecontrollerdb.txt', 'cache_sombreadores.bin') {
  if (Test-Path (Join-Path $root $f)) { Copy-Item (Join-Path $root $f) $Destino }
}
$toml = Get-Content (Join-Path $root 'monster-hunter-tri-native.toml') |
        Where-Object { $_ -notmatch '^\s*(input_game_dir|output_dir|runtime_source_dir|sdk_sigdb|symbols_csv)\s*=' }
Set-Content -Path (Join-Path $Destino 'monster-hunter-tri-native.toml') -Value $toml -Encoding ASCII
foreach ($f in 'Instalar.cmd', 'Jugar con registro.cmd', 'LEEME.txt') { Copy-Item (Join-Path $src $f) $Destino }
Copy-Item (Join-Path $src 'Instalar.ps1') $h
Copy-Item (Join-Path $dolphin 'DolphinTool.exe') $h
Copy-Item (Join-Path $dolphin 'Sys\GC\dsp_coef.bin') $h
Copy-Item (Join-Path $dolphin 'COPYING') (Join-Path $h 'Dolphin-COPYING.txt')
if (Test-Path (Join-Path $dolphin 'Licenses')) { Copy-Item (Join-Path $dolphin 'Licenses') (Join-Path $h 'Dolphin-Licenses') -Recurse }

if (-not $SinZip) {
  $zip = "$Destino.zip"
  if (Test-Path $zip) { Remove-Item $zip -Force }
  Compress-Archive -Path (Join-Path $Destino '*') -DestinationPath $zip -CompressionLevel Optimal
  Write-Host ("Zip: {0} ({1:N0} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
}
Write-Host 'Listo.'
