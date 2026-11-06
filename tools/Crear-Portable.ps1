# Crea una carpeta portable del port de Monster Hunter Tri: se puede arrastrar a otro PC
# o copiar a la Steam Deck y funciona sin instalar nada. Se puede volver a ejecutar tras
# recompilar: actualiza el ejecutable y el juego sin tocar las partidas ni los ajustes
# que ya haya en el destino (salvo que sean mas antiguos que los de aqui).
#
#   powershell -ExecutionPolicy Bypass -File tools\Crear-Portable.ps1 [-Destino D:\ruta]
param([string]$Destino = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Destino) { $Destino = Join-Path (Split-Path -Parent $root) 'MonsterHunterTri-Portable' }
# Ejecutable: el de clang (el rapido) si existe; si no, el de MSVC.
$exe = $null
foreach ($c in 'MonsterHunterTri-clang.exe', 'build-o2\Release\MonsterHunterTri.exe', 'build-o2\Release\MonsterHunterTri-o2pipe.exe', 'MonsterHunterTri.exe') {
  $p = Join-Path $root $c
  if (Test-Path $p) { $exe = $p; break }
}
if (-not $exe) { throw "No encuentro el ejecutable del port en $root" }
if (-not (Test-Path (Join-Path $root 'game\DATA'))) { throw "No encuentro game\DATA en $root" }
Write-Host "Origen : $root"
Write-Host "Destino: $Destino"
Write-Host "Ejecutable: $exe"
New-Item -ItemType Directory -Force -Path $Destino | Out-Null

# 1) Ejecutable (arranca sin argumentos: usa game\DATA y el toml de su carpeta)
Copy-Item $exe (Join-Path $Destino 'MonsterHunterTri.exe') -Force

# 2) Runtime de Visual C++ junto al .exe (por si el otro PC no lo tiene instalado)
# Se busca en todas las instalaciones de Visual Studio (2022, 18 Insiders...).
$vsDirs = @()
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) { $vsDirs = @(& $vswhere -all -products * -prerelease -property installationPath) }
$vsDirs += @(Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*' -Directory -ErrorAction SilentlyContinue | ForEach-Object FullName)
$crt = $vsDirs | ForEach-Object { Get-ChildItem (Join-Path $_ 'VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT') -Directory -ErrorAction SilentlyContinue } |
       Sort-Object { $_.Parent.Parent.Name } -Descending | Select-Object -First 1
if ($crt) {
  foreach ($d in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    $f = Join-Path $crt.FullName $d
    if (Test-Path $f) { Copy-Item $f $Destino -Force }
  }
  Write-Host "Runtime de Visual C++: $($crt.FullName)"
} elseif (Test-Path "$env:WINDIR\System32\msvcp140.dll") {
  foreach ($d in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
    $f = Join-Path "$env:WINDIR\System32" $d
    if (Test-Path $f) { Copy-Item $f $Destino -Force }
  }
  Write-Host 'Runtime de Visual C++: copiado de System32'
} else {
  Write-Warning 'No encontre el runtime de Visual C++; el otro PC necesitara tenerlo instalado (vc_redist.x64.exe).'
}

# 3) Copias de carpetas
function Copiar-Carpeta([string]$nombre, [switch]$Espejo) {
  $src = Join-Path $root $nombre
  if (-not (Test-Path $src)) { return }
  $dst = Join-Path $Destino $nombre
  if ($Espejo) { $opts = @('/MIR') } else { $opts = @('/E', '/XO') }   # datos del usuario: nunca borrar ni pisar lo mas nuevo
  & robocopy $src $dst @opts /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "Error copiando $nombre (robocopy $LASTEXITCODE)" }
  Write-Host "Copiado: $nombre"
}
Copiar-Carpeta 'game' -Espejo          # disco extraido del juego (tu copia)
Copiar-Carpeta 'texturas_hd'           # paquete HD (tu copia; no se puede redistribuir)
Copiar-Carpeta 'nand'                  # partidas del juego
Copiar-Carpeta 'guardados_rapidos'     # ranuras F5/F8

# 4) Archivos sueltos
foreach ($f in 'gamecontrollerdb.txt', 'CONTROLES.md', 'dropbox_clave.txt', 'cache_sombreadores.bin') {
  $src = Join-Path $root $f
  if (Test-Path $src) { Copy-Item $src $Destino -Force }
}
# Ajustes del PC solo si el destino no tiene los suyos (la Deck usa ajustes_port_deck.ini aparte)
$aj = Join-Path $Destino 'ajustes_port.ini'
if (-not (Test-Path $aj) -and (Test-Path (Join-Path $root 'ajustes_port.ini'))) { Copy-Item (Join-Path $root 'ajustes_port.ini') $aj }
# Configuracion sin las rutas del recompilador (no hacen falta para jugar)
$toml = Get-Content (Join-Path $root 'monster-hunter-tri-native.toml') |
        Where-Object { $_ -notmatch '^\s*(input_game_dir|output_dir|runtime_source_dir|sdk_sigdb|symbols_csv)\s*=' }
Set-Content -Path (Join-Path $Destino 'monster-hunter-tri-native.toml') -Value $toml -Encoding ASCII
# El permiso de Dropbox esta cifrado para este usuario de Windows: no sirve en otro equipo
foreach ($f in 'nube_dropbox.dat', 'nube_estado.ini', 'credenciales_google.json') { Remove-Item (Join-Path $Destino $f) -ErrorAction SilentlyContinue }

# Modo online (la ciudad): servidores locales (servicios de Nintendo emulados + MH3SP, AGPL) y el
# Python embebido de Windows con el que el juego los abre solo al entrar en el online.
function Copiar-Servidores([string]$Dst, [switch]$ConDatos) {
  $srvSrc = Join-Path $root 'servidor'
  if (-not (Test-Path $srvSrc)) { Write-Warning 'No hay carpeta servidor: el kit ira sin modo online.'; return }
  $ex = @('/XD', '__pycache__', 'logs', '/XF', '*.log', '*.tmp')
  if (-not $ConDatos) { $ex += @('*.db', 'wfc_perfiles.json') }   # sin cuentas ni perfiles de nadie
  & robocopy $srvSrc (Join-Path $Dst 'servidor') /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP @ex | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "Error copiando servidor (robocopy $LASTEXITCODE)" }
  $global:LASTEXITCODE = 0
  foreach ($f in 'online.ini', 'Servidor online.cmd', 'servidor_online_deck.sh') {
    if (Test-Path (Join-Path $root $f)) { Copy-Item (Join-Path $root $f) $Dst -Force }
  }
  if (-not (Test-Path (Join-Path $Dst 'servidor\python\python.exe'))) { Write-Warning 'Falta servidor\python: el online necesitara Python instalado.' }
  Write-Host 'Copiado: modo online (servidor)'
}
Copiar-Servidores $Destino -ConDatos   # tu cuenta del servidor (Capcom ID) va contigo

# 5) Lanzador con registro (para mandar a Claude si algo falla)
$cmd = @'
@echo off
rem Juega guardando el registro del port en logs\ (para analizar problemas).
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "stamp=%%t"
set NWII_HITCH_LOG=1
set "log=%~dp0logs\juego-%stamp%.log"
echo Registro: %log%
"%~dp0MonsterHunterTri.exe" > "%log%" 2>&1
echo El proceso ha terminado con codigo %errorlevel%.
pause
'@
Set-Content -Path (Join-Path $Destino 'Jugar con registro.cmd') -Value $cmd -Encoding ASCII

# 6) Instrucciones
$leeme = @'
MONSTER HUNTER TRI - PORT PARA PC (carpeta portable)
=====================================================

Esta carpeta lleva todo lo necesario: el ejecutable, tu copia del juego (game\),
tus partidas (nand\), los guardados rápidos, las texturas HD y los ajustes.
Es para tus propios equipos: no la compartas (contiene el juego y el paquete HD,
que no se pueden redistribuir).

EN UN PC CON WINDOWS
--------------------
1. Copia la carpeta entera donde quieras (disco interno, externo, USB rápido...).
2. Doble clic en MonsterHunterTri.exe.
   Requisitos: Windows 10/11 de 64 bits y una tarjeta gráfica con OpenGL 3.3.
   Si Windows dice que falta MSVCP140.dll o VCRUNTIME140.dll, instala el
   "Visual C++ Redistributable x64": https://aka.ms/vs/17/release/vc_redist.x64.exe
3. Menú del port: tecla P (o Back + Start en el mando). Ver CONTROLES.md.
   Si algo falla, usa "Jugar con registro.cmd" y manda el archivo de logs\.

EN LA STEAM DECK (con Proton)
-----------------------------
1. Copia la carpeta entera a la Deck (por USB, microSD o red), por ejemplo a
   /home/deck/Games/MonsterHunterTri
2. Pasa a Modo Escritorio y abre Steam.
3. Juegos > "Añadir un juego que no es de Steam a mi biblioteca" > Explorar.
   Cambia el filtro a "Todos los archivos", elige MonsterHunterTri.exe y pulsa Añadir.
4. En la biblioteca: clic derecho en MonsterHunterTri > Propiedades > Compatibilidad >
   marca "Forzar el uso de una herramienta de compatibilidad de Steam Play" y elige
   "Proton Experimental" (o la versión de Proton más reciente).
5. Vuelve a Modo Juego y ábrelo desde la biblioteca (pestaña "No Steam").
6. Controles: la Deck funciona como un mando de Xbox. El menú del port se abre con
   los botones View (dos cuadrados) y Menu (tres rayas) a la vez.
7. La primera vez en la Deck se crean ajustes propios (ajustes_port_deck.ini):
   resolución interna 2x y pantalla completa. Si no se mantiene a 30 FPS, baja a 1x
   en P > Gráficos. Deja el limitador de FPS de la Deck desactivado o en 60.
8. Guardado en la nube (Dropbox): conéctalo desde el Modo Escritorio la primera vez
   (necesita navegador). Después funciona también en Modo Juego.
9. Si algo falla: en Propiedades > Opciones de lanzamiento pon
       PROTON_LOG=1 %command%
   y el registro aparecerá en /home/deck/steam-<número>.log

PARTIDAS Y AJUSTES
------------------
- nand\               partida del propio juego
- guardados_rapidos\  ranuras F5 / F8
- ajustes_port.ini    ajustes del PC;  ajustes_port_deck.ini  ajustes de la Deck
- Dropbox: el permiso no viaja con la carpeta; en cada equipo hay que pulsar
  "Conectar con Dropbox" una vez. Con la subida automática, la partida más nueva
  se baja sola al arrancar en el otro equipo.
'@
Set-Content -Path (Join-Path $Destino 'LEEME.txt') -Value $leeme -Encoding UTF8

$size = (Get-ChildItem $Destino -Recurse -File | Measure-Object Length -Sum).Sum / 1GB
Write-Host ("Listo: {0} ({1:N1} GB)" -f $Destino, $size)
