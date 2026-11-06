# Monster Hunter Tri — port para PC

Port jugable de **Monster Hunter Tri** (Wii, versión americana **RMHE08**) para PC con Windows y Steam Deck (Proton).
Parte de una recompilación estática hecha con [NWiiRecomp](#requisitos) y añade encima todo lo necesario para jugar bien:
30 FPS estables, menú propio, texturas HD, guardado rápido, guardado en la nube, cambio de idioma y arreglos gráficos.

> **Este repositorio no contiene nada del juego.** No incluye el disco, el código del juego recompilado, el
> ejecutable, el runtime de NWiiRecomp ni paquetes de texturas. Necesitas tu propia copia de Monster Hunter Tri.
> Proyecto de aficionados sin relación con Capcom ni Nintendo.

## Qué aporta

| Parte | Archivos | Qué hace |
| --- | --- | --- |
| Canalización GX | `pipeline_tail.inc`, `make_patch.py` | Un hilo lee y decodifica el FIFO gráfico mientras el principal dibuja con OpenGL; el juego ya no espera al dibujado. Decodificación de vértices en paralelo, caché de listas de dibujo, caché de sombreadores en disco. |
| Menú del port | `port_menu.h`, `port_menu_state.h`, `port_menu_font.h` | Tecla **P** o **Select + Start**: resolución interna 1x–5x, filtrado, FXAA, bloom, vsync, pantalla completa, volumen, idioma, controles, nube y guardados. |
| Texturas HD | `hd_textures.h` | Carga paquetes en formato Dolphin (`tex1_..._<XXH64>.png`) desde `texturas_hd\`, con decodificación asíncrona (sin tirones). |
| Guardado rápido | `port_savestate.cpp`, `port_state_io.h`, `port_efb_state.h` | Tres ranuras (F5 / F8), incluidas las copias EFB que solo existen en la GPU. |
| Nube | `port_cloud.h`, `port_cloud.cpp` | Dropbox con OAuth 2 + PKCE (app propia de cada usuario), sube la partida y las ranuras y baja la más nueva al arrancar. |
| Idioma | `port_lang.h` | Inglés, español, francés y (experimental) alemán e italiano en la versión americana. |
| Vídeos THP | `port_thp.cpp`, `port_thp_decode.h` | Decodificador JPEG nativo de los vídeos en un hilo aparte: vídeos a velocidad completa y sin cortes de audio. Se verifica contra el decodificador del juego antes de usarse. |
| Arreglos gráficos | `make_patch.py` | Borrados del EFB con las máscaras de color/alfa/profundidad como la consola, cuadros con varias copias a pantalla (teclado virtual, diálogos en pausa), copias EFB opacas, escalado sin rayas. |
| Rendimiento | `compactar.py`, `include-patches/cpu_context.h.diff`, `port_fastpoll.h`, `port_mem.cpp`, `port_perf.*`, `pipeline_tail.inc` | Unidades generadas compactadas (solo las entradas reales en cada función), accesos a memoria por tabla y sin alias, llamadas indirectas directas, sondeo de eventos barato, memcpy/memset nativos, bucles de caché de golpe, reparto de núcleos y descanso en ocioso. Presentación al ritmo del VI del juego (sin microtirones). En un Ryzen 5 1600X la zona con más monstruos (~15.000 dibujos por cuadro) va a 30 FPS estables (antes ~20). |
| Modo online (la ciudad) | `port_net.cpp`, `servidor/servidor_wfc.py`, `online.ini` | Red del Wii (IOS `/dev/net/*`) sobre sockets del PC: SSL sin cifrar hacia un servidor propio, DNS redirigido. `servidor_wfc.py` emula lo mínimo de Nintendo WFC (NAS, disponibilidad, GameSpy) y [MH3SP](https://github.com/sepalani/MH3SP) hace de servidor de Capcom. El juego abre los dos solo al entrar en el online (también en la Steam Deck, desde el Modo Juego) y los cierra al salir. |
| Biblioteca C del juego | `port_libc.cpp` | El recompilador convirtió `sscanf` (0x8045F37C) en `OSReport`: el online fallaba con el error 0x800A0003. `sscanf` propio sobre la memoria del juego. |
| Mezcla y diagnóstico | `apply_blend.inc`, `frame_dump.inc` | Mezcla GX completa (operaciones lógicas, alfa constante, EFB sin alfa). **F10** guarda un cuadro paso a paso en `logs\volcado-*` (estado de cada dibujo e imágenes del EFB). Gracias a eso se vio que la opción "sin resplandor" no omitía la suma del bloom (imagen quemada). |
| Kit de instalación | `tools/kit/`, `tools/Crear-Kit.ps1`, `Crear kit.cmd` | Otra persona pone su ISO y su paquete HD y `Instalar.cmd` le monta el port. |
| Carpeta portable | `tools/Crear-Portable.ps1`, `Crear carpeta portable.cmd` | Copia el port completo a otra carpeta para llevarlo a otro PC o a la Steam Deck. |

Los cambios sobre el runtime de NWiiRecomp no se guardan como copias: `make_patch.py` lee las fuentes originales de
`export/nWiiRuntime` y genera las versiones parcheadas (comprobando cada punto de anclaje), y `include-patches/`
guarda las diferencias de seis cabeceras. Si tu versión de NWiiRecomp es distinta, `make_patch.py` se detiene
indicando qué parte no coincide.

## Requisitos

- Tu copia de **Monster Hunter Tri (RMHE08)**. Las versiones europea y japonesa no sirven.
- Windows 10/11 x64, **Visual Studio 2022** (C++ de escritorio), **CMake** y **Python 3**.
- **NWiiRecomp** (recompilador estático de Wii a C++) instalado; por defecto se busca en
  `%LOCALAPPDATA%\Programs\NWiiRecomp`.
- [Dolphin](https://dolphin-emu.org) para extraer el disco (y, para el kit, `DolphinTool.exe` en `tools\Dolphin-x64\`).

## Compilar

1. **Extrae el disco** con Dolphin (clic derecho en el juego → Propiedades → Sistema de archivos → clic derecho en el
   disco → *Extraer todo el disco*) y deja la partición de datos en `game\DATA` (con `sys\main.dol`, `files\`...).
   Copia `Sys\GC\dsp_coef.bin` de Dolphin a `game\`.
2. **Genera el código** con NWiiRecomp: edita las rutas de `monster-hunter-tri-native.toml` (`input_game_dir`,
   `output_dir`, `runtime_source_dir`, `sdk_sigdb`) y recompila a `export\`.
3. **Aplica los cambios del port**:
   ```
   python tools\gx-pipeline\make_patch.py
   ```
4. **Compila** (la primera vez tarda bastante). Recomendado, con clang (bastante más rápido):
   doble clic en `tools\gx-pipeline\Compilar con Clang.cmd` (necesita "Herramientas de C++ Clang para
   Windows" y "Herramientas de CMake de C++" del Visual Studio Installer, y Python 3).
   Resultado: `MonsterHunterTri-clang.exe` en la raíz. Con MSVC:
   ```
   powershell -ExecutionPolicy Bypass -File tools\gx-pipeline\Reconstruir-O2Pipe.ps1 [-Deps <carpeta _deps de NWiiRecomp>]
   ```
   Los dos scripts ejecutan antes `compactar.py`, que escribe `tools\gx-pipeline\compacto\` (código del
   juego: no se sube al repositorio).
5. Opcional: pon tu paquete de texturas HD en `texturas_hd\`.

Sin argumentos el ejecutable usa `game\DATA` y el `.toml` de su carpeta.

## Jugar

Controles, menú y configuración de Dropbox: [CONTROLES.md](CONTROLES.md).
Steam Deck: añade `MonsterHunterTri.exe` como juego que no es de Steam con Proton Experimental
(instrucciones en `tools/kit/LEEME.txt`).

### Modo online

El juego se conecta a servidores que corren en tu propio equipo (`online.ini`: `servidor=127.0.0.1`).
Para tenerlos (no se suben aquí, son de otros proyectos):

1. **MH3SP** (AGPL v3) en `servidor\MH3SP`, rama `dev`:
   `git clone -b dev https://github.com/sepalani/MH3SP servidor/MH3SP`.
   En su `config.ini` pon `UseSSL = OFF` en `[OPN]` y `LogToWindow = OFF` en todas las secciones.
2. **Python** para Windows en `servidor\python` (el paquete *embeddable* de python.org, 3.12) y en su
   `python312._pth` añade las líneas `..` y `..\MH3SP`. Sin él, el port usa el Python instalado.

Al entrar en el online el port abre `servidor_wfc.py` y `master_server.py` sin ventana (salida en
`servidor\servidores.log`) si no hay nada escuchando, y los cierra al salir. `arrancar_servidores=0`
lo desactiva (entonces: `Servidor online.cmd`, o `bash servidor_online_deck.sh` en la Deck).
Con el online conectado el menú del port no pausa el juego. Para jugar con otras personas, quien
hospeda abre los puertos TCP 8200-8203 y 9000 y UDP 27900, y los demás ponen su IP en `servidor=`.

### Diagnóstico

Los `.cmd` de `tools\gx-pipeline\` arrancan el juego con registros activados. Variables útiles:
`NWII_HITCH_LOG=1` (tirones), `NWII_FRAMELOG=1` (qué se dibuja en cada cuadro), `NWII_CPU_PROFILE=1`,
`NWII_EFBCOPY_TRACE=1`, `NWII_CLEAR_MAGENTA=1` (zonas sin dibujar en magenta), `NWII_THP_NATIVE=0`
(vídeos con el decodificador del juego), `NWII_GX_SERIAL=1` (sin el hilo gráfico).

Con `NWII_LOG_FILE=1` (o en la Steam Deck) el registro va a `logs\` e incluye `[Ritmo]` (intervalos
entre cuadros entregados y mostrados), `[Interprete]` (entradas al intérprete de reserva) y, en la Deck,
`[Deck]` (frecuencias, GPU y consumo). Para comparar, se puede desactivar cada mejora:
`NWII_DIRECT_CALLS=0`, `NWII_PRESENT_VI=0`, `NWII_PACE_GRID=1`, `NWII_FAST_POLL=1`, `NWII_NATIVE_MEM=0`,
`NWII_CACHE_SCALAR=1`, `NWII_WGP_BATCH=0`, `NWII_IDLE_SLEEP=0`, `NWII_AFFINITY=0`. `NWII_CPU_SAMPLE=1`
saca un perfil del hilo del juego.

## Licencias

- Código de este repositorio: [MIT](LICENSE).
- `gamecontrollerdb.txt`: SDL_GameControllerDB (zlib), ver `tools/licenses/`.
- Atlas de `port_menu_font.h`: fuente DejaVu Sans, ver `tools/licenses/`.
- NWiiRecomp/nWiiRuntime, Dolphin y los paquetes de texturas pertenecen a sus autores y no se incluyen.
