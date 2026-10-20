#pragma once
// tools/gx-pipeline: guardado en la nube (Dropbox) del port.
// Sube y baja la partida del propio juego (archivos de la NAND del titulo) y las
// ranuras de guardado rapido a la carpeta de una app de Dropbox creada por el propio
// usuario (clave de la app en dropbox_clave.txt, en la carpeta del juego). El permiso
// concedido se guarda cifrado con DPAPI en nube_dropbox.dat.
#include <string>

namespace port_cloud {

void Init();                       // carga credenciales y permiso; arranca el hilo de la nube
void StartupSync();                // antes de arrancar el juego: baja lo que sea mas nuevo en la nube
void Shutdown();                   // al salir: sube lo pendiente (espera unos segundos como mucho)

void NotifyNandSaved(const std::string& nand_path);  // el juego escribio un archivo de la NAND
void NotifySlotSaved(int slot);                      // se escribio guardados_rapidos/ranuraN.sav

void SetAuto(bool on);             // subida automatica (ajustes_port.ini: nube_auto)
bool Auto();
bool HasCredentials();             // hay dropbox_clave.txt
bool Connected();                  // hay permiso guardado
bool Busy();
std::string Account();             // correo de la cuenta conectada (si se conoce)
std::string Status();              // ultima situacion, para el menu

void RequestConnect();             // abre el navegador para dar permiso
void RequestDisconnect();
void RequestUpload();              // sube todo ahora (la nube pasa a ser como lo local)
void RequestDownload();            // baja todo: ranuras al momento, la partida al reiniciar
bool PopMessage(std::string& out); // avisos para mostrar en pantalla

}  // namespace port_cloud
