# Instalador del port de Monster Hunter Tri para PC.
# Crea el port a partir de TU copia del juego (ISO/WBFS/RVZ... de la version americana,
# RMHE08) y, si lo tienes, del paquete de texturas HD. El kit no trae nada del juego.
#
#   Instalar.cmd                      (te pregunta por los archivos)
#   Instalar.cmd juego.iso pack.zip   (o arrastra los archivos encima de Instalar.cmd)
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Archivos = @())
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$kit  = Split-Path -Parent $PSScriptRoot          # carpeta del kit: aqui queda el port
$tool = Join-Path $PSScriptRoot 'DolphinTool.exe'
$exe  = Join-Path $kit 'MonsterHunterTri.exe'
$GameId = 'RMHE08'
$MainDolSha1 = 'bf4850739478caaedfe675949eb7c28595a7fde9'   # main.dol del que se recompilo el port
$ExtDisco = @('.iso', '.wbfs', '.rvz', '.wia', '.gcz', '.ciso', '.gcm')
$ExtPaquete = @('.zip', '.7z', '.rar')

function Titulo([string]$t) { Write-Host ''; Write-Host "== $t ==" -ForegroundColor Cyan }
function Ok([string]$t) { Write-Host "  $t" -ForegroundColor Green }
function Aviso([string]$t) { Write-Host "  AVISO: $t" -ForegroundColor Yellow }
function Fallo([string]$t) { Write-Host ''; Write-Host "ERROR: $t" -ForegroundColor Red; exit 1 }
function Preguntar([string]$t) {
  while ($true) {
    $r = (Read-Host "  $t (S/N)").Trim().ToUpper()
    if ($r -eq 'S' -or $r -eq 'SI' -or $r -eq 'Y') { return $true }
    if ($r -eq 'N' -or $r -eq 'NO') { return $false }
  }
}
function Borrar([string]$p) { if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force } }

try { Add-Type -AssemblyName System.Windows.Forms } catch {}
function Elegir-Archivo([string]$titulo, [string]$filtro) {
  $d = New-Object System.Windows.Forms.OpenFileDialog
  $d.Title = $titulo; $d.Filter = $filtro
  if ($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $d.FileName }
  return $null
}
function Elegir-Carpeta([string]$desc) {
  $d = New-Object System.Windows.Forms.FolderBrowserDialog
  $d.Description = $desc
  if ($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $d.SelectedPath }
  return $null
}

Write-Host 'MONSTER HUNTER TRI - instalador del port para PC' -ForegroundColor White
Write-Host 'Necesitas tu copia del juego (version americana, RMHE08). El paquete HD es opcional.'
if (-not (Test-Path -LiteralPath $exe))  { Fallo "Falta MonsterHunterTri.exe en $kit (el kit esta incompleto)." }
if (-not (Test-Path -LiteralPath $tool)) { Fallo "Falta herramientas\DolphinTool.exe (el kit esta incompleto)." }

# --- 1) Archivos de entrada (arrastrados o elegidos) ---
$iso = $null; $hd = $null
foreach ($a in $Archivos) {
  if (-not $a) { continue }
  $a = $a.Trim('"')
  if (Test-Path -LiteralPath $a -PathType Container) { $hd = $a; continue }
  $ext = [IO.Path]::GetExtension($a).ToLower()
  if ($ExtDisco -contains $ext) { $iso = $a }
  elseif ($ExtPaquete -contains $ext) { $hd = $a }
  else { Aviso "No se que hacer con $a (se ignora)." }
}

Titulo '1/5  Copia del juego'
if (-not $iso) {
  Write-Host '  Elige tu copia del juego en la ventana que se abre.'
  $iso = Elegir-Archivo 'Elige tu copia de Monster Hunter Tri (RMHE08)' 'Discos de Wii (*.iso;*.wbfs;*.rvz;*.wia;*.gcz;*.ciso)|*.iso;*.wbfs;*.rvz;*.wia;*.gcz;*.ciso|Todos los archivos (*.*)|*.*'
}
if (-not $iso) { Fallo 'No se ha elegido ninguna copia del juego.' }
if (-not (Test-Path -LiteralPath $iso -PathType Leaf)) { Fallo "No existe $iso" }
Write-Host "  $iso"

# Identificador del disco: con DolphinTool (vale para todos los formatos) o leyendo la cabecera del ISO.
$cab = ''
try { $cab = (& $tool header -i "$iso" 2>&1 | Out-String) } catch { $cab = '' }
if ($cab -notmatch 'RM[A-Z]{2}08' -and [IO.Path]::GetExtension($iso).ToLower() -eq '.iso') {
  $fs = [IO.File]::OpenRead($iso); $b = New-Object byte[] 6; [void]$fs.Read($b, 0, 6); $fs.Close()
  $cab = [Text.Encoding]::ASCII.GetString($b)
}
if ($cab -match $GameId) { Ok "Monster Hunter Tri, version americana ($GameId)." }
elseif ($cab -match 'RMHP08') { Fallo 'Es la version europea (RMHP08). Este port solo funciona con la americana (RMHE08).' }
elseif ($cab -match 'RMHJ08') { Fallo 'Es la version japonesa (RMHJ08). Este port solo funciona con la americana (RMHE08).' }
else { Fallo 'Ese archivo no parece Monster Hunter Tri de Wii (RMHE08). Si es un .7z/.zip con el ISO dentro, descomprimelo primero.' }

Titulo '2/5  Texturas HD (opcional)'
if (-not $hd) {
  if (Preguntar 'Tienes el paquete de texturas HD (por ejemplo TRI-HD) descargado?') {
    Write-Host '  Elige el archivo comprimido (.zip/.7z/.rar). Si ya lo tienes descomprimido, pulsa Cancelar y elige la carpeta.'
    $hd = Elegir-Archivo 'Paquete de texturas HD' 'Comprimidos (*.zip;*.7z;*.rar)|*.zip;*.7z;*.rar|Todos los archivos (*.*)|*.*'
    if (-not $hd) { $hd = Elegir-Carpeta 'Carpeta con las texturas HD (las imagenes tex1_...png)' }
  }
}
if ($hd) { Write-Host "  $hd" } else { Write-Host '  Sin texturas HD (se pueden anadir despues volviendo a ejecutar el instalador).' }

# --- 2) Requisitos: espacio y runtime de Visual C++ ---
Titulo '3/5  Comprobaciones'
$drive = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($kit))
$libreGB = [math]::Round($drive.AvailableFreeSpace / 1GB, 1)
if ($libreGB -lt 4) { Fallo "Hace falta unos 4 GB libres en $($drive.Name) y hay $libreGB GB." }
Ok "Espacio libre: $libreGB GB"

function Hay-VCRuntime {
  foreach ($k in 'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x64') {
    try { $v = Get-ItemProperty -Path $k -ErrorAction Stop; if ($v.Installed -eq 1 -and $v.Major -ge 14 -and $v.Minor -ge 30) { return $true } } catch {}
  }
  return (Test-Path (Join-Path $kit 'msvcp140.dll')) -and (Test-Path (Join-Path $kit 'vcruntime140_1.dll'))
}
if (Hay-VCRuntime) { Ok 'Runtime de Visual C++: instalado' }
else {
  Aviso 'Falta el runtime de Visual C++ 2015-2022 (x64), necesario para el port.'
  if (Preguntar 'Lo descargo de Microsoft (aka.ms/vs/17/release/vc_redist.x64.exe) y lo instalo ahora?') {
    $vc = Join-Path $env:TEMP 'vc_redist.x64.exe'
    try { Invoke-WebRequest -Uri 'https://aka.ms/vs/17/release/vc_redist.x64.exe' -OutFile $vc -UseBasicParsing }
    catch { Fallo "No se pudo descargar: $($_.Exception.Message). Instalalo a mano y vuelve a ejecutar el instalador." }
    $p = Start-Process -FilePath $vc -ArgumentList '/install', '/passive', '/norestart' -Wait -PassThru
    if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010 -and $p.ExitCode -ne 1638) { Fallo "El instalador de Visual C++ termino con codigo $($p.ExitCode)." }
    Ok 'Runtime de Visual C++ instalado.'
  } else { Fallo 'Sin el runtime de Visual C++ no funciona. Instalalo y vuelve a ejecutar el instalador.' }
}

# --- 3) Extraer el disco ---
Titulo '4/5  Extrayendo el juego (unos minutos)'
$game = Join-Path $kit 'game'
$data = Join-Path $game 'DATA'
$extraer = $true
if (Test-Path -LiteralPath (Join-Path $data 'sys\main.dol')) {
  $h = (Get-FileHash -Algorithm SHA1 -LiteralPath (Join-Path $data 'sys\main.dol')).Hash.ToLower()
  $n = @(Get-ChildItem -LiteralPath (Join-Path $data 'files') -Recurse -File -ErrorAction SilentlyContinue).Count
  if ($h -eq $MainDolSha1 -and $n -ge 4700) {
    $extraer = Preguntar 'El juego ya esta extraido en esta carpeta. Extraerlo otra vez?'
  }
}
if ($extraer) {
  $tmp = Join-Path $kit '_extraccion'
  Borrar $tmp
  & $tool extract -i "$iso" -o "$tmp" -g
  if ($LASTEXITCODE -ne 0) { Borrar $tmp; Fallo "DolphinTool no pudo extraer el disco (codigo $LASTEXITCODE). La copia puede estar danada." }
  $dol = Get-ChildItem -LiteralPath $tmp -Recurse -File -Filter 'main.dol' | Where-Object { $_.Directory.Name -eq 'sys' } | Select-Object -First 1
  if (-not $dol) { Borrar $tmp; Fallo 'La extraccion no contiene sys\main.dol.' }
  $raiz = $dol.Directory.Parent.FullName
  $h = (Get-FileHash -Algorithm SHA1 -LiteralPath $dol.FullName).Hash.ToLower()
  if ($h -ne $MainDolSha1) {
    Aviso 'El ejecutable del juego (main.dol) no es el que usa el port: copia modificada, NKit u otra revision.'
    if (-not (Preguntar 'Seguir de todos modos? (lo normal es que el juego falle)')) { Borrar $tmp; Fallo 'Cancelado.' }
  } else { Ok 'main.dol verificado.' }
  New-Item -ItemType Directory -Force -Path $game | Out-Null
  Borrar $data
  Move-Item -LiteralPath $raiz -Destination $data
  Borrar $tmp
  $n = @(Get-ChildItem -LiteralPath (Join-Path $data 'files') -Recurse -File).Count
  if ($n -lt 4700) { Aviso "Solo hay $n archivos del juego (se esperan 4747). La copia puede estar incompleta (NKit?)." }
  else { Ok "Juego extraido: $n archivos." }
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'dsp_coef.bin') -Destination (Join-Path $game 'dsp_coef.bin') -Force
New-Item -ItemType Directory -Force -Path (Join-Path $kit 'nand') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $kit 'guardados_rapidos') | Out-Null

# --- 4) Texturas HD ---
Titulo '5/5  Texturas HD'
if (-not $hd) { Write-Host '  Se omite.' }
else {
  $tmp2 = $null; $src = $hd
  if (Test-Path -LiteralPath $hd -PathType Leaf) {
    $tmp2 = Join-Path $kit '_texturas'
    Borrar $tmp2; New-Item -ItemType Directory -Force -Path $tmp2 | Out-Null
    Write-Host '  Descomprimiendo...'
    $ext = [IO.Path]::GetExtension($hd).ToLower()
    $okx = $false
    if ($ext -eq '.zip') { try { Expand-Archive -LiteralPath $hd -DestinationPath $tmp2 -Force; $okx = $true } catch {} }
    if (-not $okx) { try { $null = & tar.exe -xf "$hd" -C "$tmp2" 2>&1; $okx = ($LASTEXITCODE -eq 0) } catch { $okx = $false } }
    if (-not $okx) {
      Borrar $tmp2
      Fallo "Windows no puede descomprimir $([IO.Path]::GetFileName($hd)). Descomprimelo con 7-Zip y vuelve a ejecutar el instalador eligiendo la carpeta (el juego ya queda extraido)."
    }
    $src = $tmp2
  }
  $pngs = @(Get-ChildItem -LiteralPath $src -Recurse -File -Filter 'tex1_*.png')
  $dds = @(Get-ChildItem -LiteralPath $src -Recurse -File -Filter 'tex1_*.dds')
  if ($pngs.Count -eq 0) {
    if ($dds.Count -gt 0) { Aviso 'El paquete esta en formato DDS y el port solo carga PNG: se omite.' }
    else { Aviso 'No hay texturas (tex1_*.png) en el paquete: se omite.' }
  } else {
    $rm = Get-ChildItem -LiteralPath $src -Recurse -Directory -Filter $GameId | Select-Object -First 1
    if ($rm) { $src = $rm.FullName }
    $dst = Join-Path $kit 'texturas_hd'
    & robocopy "$src" "$dst" 'tex1_*.png' /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { Fallo "Error copiando las texturas (robocopy $LASTEXITCODE)." }
    Ok "$($pngs.Count) texturas HD instaladas en texturas_hd\."
    if ($dds.Count -gt 0) { Aviso "$($dds.Count) texturas DDS ignoradas (solo se cargan PNG)." }
  }
  if ($tmp2) { Borrar $tmp2 }
}

Titulo 'Listo'
Write-Host "  El port esta en: $kit"
Write-Host '  Juega con MonsterHunterTri.exe. Menu del port: tecla P (o Back + Start en el mando).'
Write-Host '  Steam Deck: copia esta carpeta entera a la Deck y sigue LEEME.txt.'
if (Preguntar 'Jugar ahora?') { Start-Process -FilePath $exe -WorkingDirectory $kit }
