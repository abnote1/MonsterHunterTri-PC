# Controles actuales de mando PC / Xbox

Actualizado: 9 de octubre de 2026. Usa **Classic Style 1** (Options → Control Settings). Preferencia de File1 configurada.

| Mando PC | Acción |
| --- | --- |
| Stick izquierdo | Moverse |
| Stick derecho | Cámara |
| A | Esquivar / cancelar |
| B | Ataque secundario / interactuar / confirmar |
| Y | Sacar arma / ataque principal |
| X | Usar objeto |
| Start | Menú |
| Back | Patada |
| LB | Centrar cámara; mantener para seleccionar objetos con X/B |
| RB | Correr / acción según arma |
| LT / RT | ZL / ZR del mando clásico (función según situación) |

Los avisos del juego mantienen las letras del mando Wii: A Wii = B PC, B Wii = A PC, X Wii = Y PC, Y Wii = X PC, Minus Wii = Start PC. Cruceta conservada como alternativa de cámara/navegación del mando clásico. Sticks con zona muerta y escala continua; cámara derecha analógica.

Referencia del esquema Wii: [manual oficial de Capcom](https://static.capcom.com/monsterhunter/tri_manual.pdf).

**Classic Style 2 utiliza el stick derecho para ataques**; el esquema moderno requiere Style 1. Si juegas sin mando con teclado/Nunchuk, selecciona Freestyle.

Validación: 225 comprobaciones con mandos SDL virtuales y cámara observada en Arena. Sin mando físico conectado durante esta tanda; pulsaciones físicas del Diswoe pendientes. Versión anterior y partida/configuración anterior recuperables según ESTADO.md.

## Menú del port (11 de octubre de 2026)

Pulsa **P** en el teclado o **Back + Start** a la vez en el mando. El juego queda en pausa (también el sonido) mientras el menú está abierto.

| Menú | Teclado | Mando |
| --- | --- | --- |
| Elegir opción | Flechas o W/S | Cruceta o stick izquierdo |
| Cambiar valor | Flechas o A/D | Cruceta o stick izquierdo |
| Aceptar | Intro o Espacio | A |
| Volver | Escape o Retroceso | B |
| Cerrar | P | Back + Start (o Start) |

Opciones:
- **Guardado rápido**: 3 ranuras; guarda o carga todo el estado del juego al instante. Archivos en `guardados_rapidos\`. No sustituye al guardado del propio juego ni toca sus partidas.
- **Gráficos**: resolución interna 1x–5x, texturas Original/HD, filtrado de texturas (trilineal / anisotrópico 2x–16x), antialiasing FXAA, resplandor, sincronización vertical, pantalla completa, relación de aspecto 4:3/16:9/21:9/32:9, mantener proporción, píxeles nítidos y contador de FPS. Se ven al momento (el juego solo adopta 4:3 ↔ panorámico al arrancar).
- **Sonido** (volumen), **Controles** (tabla resumida) y **Salir del juego**.
- **Nube (Dropbox)**: sube la partida del juego y las 3 ranuras de guardado rápido a tu Dropbox (ver abajo).
- **Idioma del juego**: English, Español, Français, Deutsch o Italiano. Se aplica al reiniciar el juego. Inglés, español y francés son los que esta versión del juego trae activados; alemán e italiano vienen en el disco y el port los activa, pero son experimentales (puede quedar algún texto en inglés).

Teclas rápidas sin abrir el menú: **F5** guardar, **F8** cargar, **F6** cambiar de ranura (aviso abajo a la derecha).

Texturas HD: copia un paquete de texturas en formato Dolphin (por ejemplo el "Manual Install" de TRI-HD Project, solo interfaz/HUD) dentro de la carpeta `texturas_hd\` del proyecto. Los ajustes se guardan en `ajustes_port.ini`.

### Guardado en la nube (Dropbox)

El juego guarda en una carpeta propia de tu Dropbox (**Aplicaciones/<nombre de tu app>**) y no puede ver nada más. Hay que crear una vez una "app" de Dropbox gratuita (2 minutos):

1. Entra en [Dropbox App Console](https://www.dropbox.com/developers/apps) con tu cuenta y pulsa **Create app**.
2. Elige **Scoped access** y **App folder**, ponle un nombre (por ejemplo "MH Tri port de TuNombre"; tiene que ser único en Dropbox) y créala.
3. En la pestaña **Permissions**, marca `account_info.read`, `files.metadata.read`, `files.content.read` y `files.content.write`, y pulsa **Submit** (abajo). Si cambias permisos después de conectar, en el juego pulsa **Desconectar la cuenta** y vuelve a conectar: el permiso guardado no se actualiza solo.
4. En la pestaña **Settings**, en **OAuth 2 → Redirect URIs**, añade exactamente `http://localhost:53682/` y pulsa **Add**. Comprueba que **Allow public clients (Implicit Grant & PKCE)** está en **Allow**.
5. Copia el **App key** (no hace falta el App secret) y guárdalo en un archivo de texto llamado `dropbox_clave.txt` en la carpeta del juego (junto a `MonsterHunterTri.exe`), solo la clave.
6. En el juego: **P → Nube (Dropbox) → Conectar con Dropbox**. Se abre el navegador (Alt+Tab si estás en pantalla completa); inicia sesión, pulsa **Permitir** y vuelve al juego.

Funcionamiento:
- **Subida automática** (activada por defecto): la partida se sube unos segundos después de guardar en el juego, cada ranura al pulsar F5, y lo pendiente al salir.
- Al arrancar, si la partida de la nube es más nueva (por ejemplo, jugaste en otro PC con la misma app y la misma cuenta), se baja antes de empezar. Lo que se sustituye se copia antes en `nube_respaldos\`. Si las dos cambiaron, gana la más reciente y la otra queda en `nube_respaldos\` (o en el historial de versiones de Dropbox).
- **Subir ahora** deja la nube igual que este PC. **Descargar de la nube** baja las ranuras al momento; la partida se aplica al reiniciar el juego (mientras juegas, la partida en memoria es la que vale).
- El permiso se guarda cifrado para tu usuario de Windows en `nube_dropbox.dat`. **Desconectar la cuenta** lo borra y lo revoca. No compartas `nube_dropbox.dat`.
- Cada ranura ocupa unos 45 MB; con las 3 ranuras y la partida son unos 140 MB de los 2 GB gratuitos.

## Documentación anterior (distribución sustituida)

# Controles del prototipo

Abre **Probar prototipo.cmd**. El Diswoe cableado indicado aparece en Windows como **Xbox 360 Controller** y el port lo reconoce automáticamente como mando clásico Pro de Wii. Puedes conectarlo antes o después de abrir el juego. Al desconectarlo se recuperan el teclado y el ratón. El contador de FPS sigue arriba a la izquierda.

| Mando PC/Xbox | Mando clásico Pro de Wii |
| --- | --- |
| Palanca izquierda | Palanca izquierda: movimiento |
| Palanca derecha | Cámara en estilo 1 / ataques en estilo 2 |
| A / B / X / Y | A / B / X / Y, por nombre |
| Cruceta | Cruceta |
| LB / RB | L / R |
| LT / RT | ZL / ZR; se detectan a partir de un cuarto del recorrido |
| Start | + |
| Back | − |
| Botón central Xbox, si Windows lo entrega | Home |

En **estilo clásico 1**, X saca el arma y la palanca derecha mueve la cámara. En **estilo clásico 2**, la palanca derecha controla los ataques. Elige el tipo dentro de las opciones del juego. [Manual oficial de Capcom](https://static.capcom.com/monsterhunter/tri_manual.pdf).

Las acciones concretas dependen del tipo de control clásico y del arma seleccionados dentro del juego. Las letras se asignan por nombre: A del mando de PC confirma como A de Wii. En mandos genéricos, los perfiles SDL establecen primero sus botones equivalentes de Xbox. Los clics de las palancas no tienen botón equivalente en el clásico Pro. La vibración queda pendiente.

La conexión física y un informe neutral del Diswoe están comprobados. Las pulsaciones, palancas, gatillos, desconexión/reconexión y retorno al teclado pasan pruebas con mandos virtuales SDL; el juego se comprueba por la misma ruta WPAD con entradas de diagnóstico Classic y una NAND clonada. Queda verificar todos los botones físicos en partida y daño real a los animales.

Si otro mando no se reconoce, el registro lo identifica con nombre y GUID. El archivo `gamecontrollerdb.txt` permite añadir su perfil SDL; su [base de perfiles](https://github.com/mdqinc/SDL_GameControllerDB) y licencia se incluyen en el proyecto. No se adivina la distribución de un mando desconocido.

## Teclado y ratón

Con el mando desconectado, pulsa la ventana del juego para que reciba el teclado.

| Acción | Tecla o ratón |
| --- | --- |
| Moverse | WASD |
| Confirmar, hablar o interactuar | Espacio o botón izquierdo |
| Cancelar; otras acciones B según el juego | Botón derecho |
| Abrir el menú del juego | Escape o 2 |
| Correr mientras te mueves | Mayús o E |
| Centrar la cámara; mantener abre la selección de objetos | Ctrl o Q |
| Cruceta: cámara o navegar por los menús | Flechas |
| Usar objeto | 1 |
| Sacar o guardar el arma; botón + de Wii | Intro |
| Atacar con el arma desenvainada; botón A | Botón izquierdo o Espacio |
| Simular mando horizontal (experimental) | Mantener F |
| Simular mando vertical (experimental) | Mantener T |
| Simular un movimiento del mando (experimental) | Mantener R |
| Botón − de Wii | Retroceso |

En la aldea se han comprobado movimiento, seguimiento automático de cámara, menú, guardado y recuperación mediante entradas de diagnóstico. La traducción de estas teclas hasta el mando virtual pasa sus pruebas. Queda comprobarlas con el teclado físico, así como la carrera y el giro manual de cámara dentro del juego.

Para guardar la partida de prueba: **Escape → Quit Game → Main Menu → Yes → Save your progress? → Yes**. Su recuperación después de reiniciar se ha comprobado en esta sesión.

Las pulsaciones digitales breves que detecta el programa se conservan hasta una lectura válida del juego, aunque se hayan soltado antes. Mantener y soltar un botón siguen funcionando; varias pulsaciones del mismo botón entre dos lecturas se agrupan en una. No se ha probado el teclado físico. Intro y clic izquierdo conservan su asignación válida; F/T/R permiten probar otros movimientos del mando, pero no se afirma que resuelvan el daño a los animales.

El port sigue lento y tiene errores de imagen. Consulta **ESTADO.md** para saber qué está comprobado y qué queda pendiente.
