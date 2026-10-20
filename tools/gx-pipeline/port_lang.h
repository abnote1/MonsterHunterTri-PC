#pragma once
// tools/gx-pipeline: idioma del juego.
// El juego lee el idioma de la configuracion de la Wii (SYSCONF, clave IPL.LNG) con
// SCGetLanguage y lo traduce con una tabla (0x80580DB8, 2 bytes por idioma de la Wii:
// cartel de la correa y sufijo de archivos _eng/_fre/_ger/_ita/_spa). La version
// americana trae los cinco idiomas en el disco pero su tabla manda aleman e italiano
// a ingles; se corrige en memoria antes de arrancar.
#include "port_menu_state.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace port_lang {

// Orden del menu: 0 ingles, 1 espanol, 2 frances, 3 aleman, 4 italiano.
inline const char* Name(int i) {
  static const char* n[] = {"English", "Español", "Français", "Deutsch", "Italiano"};
  return (i >= 0 && i < 5) ? n[i] : "English";
}
inline const char* Code(int i) {
  static const char* c[] = {"en", "es", "fr", "de", "it"};
  return (i >= 0 && i < 5) ? c[i] : "en";
}
inline int FromCode(const char* s) {
  for (int i = 0; i < 5; ++i) if (std::strncmp(s, Code(i), 2) == 0) return i;
  return -1;
}
// Valor de IPL.LNG de la Wii: 0 japones, 1 ingles, 2 aleman, 3 frances, 4 espanol, 5 italiano, 6 neerlandes.
inline uint8_t WiiLanguage(int i) {
  static const uint8_t v[] = {1, 4, 3, 2, 5};
  return (i >= 0 && i < 5) ? v[i] : 1;
}

// Cambia IPL.LNG dentro de un SYSCONF (formato SCv0). Devuelve false si no lo encuentra.
inline bool PatchSysconf(std::vector<uint8_t>& data, int lang) {
  if (lang < 0 || data.size() != 0x4000 || std::memcmp(data.data(), "SCv0", 4) != 0) return false;
  auto word = [&](size_t p) { return unsigned(data[p]) * 256 + data[p + 1]; };
  const unsigned n = word(4);
  if (n > 512 || 6 + 2 * (n + 1) >= data.size() - 4) return false;
  for (unsigned i = 0; i < n; ++i) {
    const size_t at = word(6 + i * 2), end = word(8 + i * 2);
    if (at >= data.size() - 4 || end > data.size() - 4 || at >= end) return false;
    const size_t name = (data[at] & 31) + 1;
    const unsigned type = data[at] >> 5;
    if (type == 3 && name == 7 && at + 1 + name < end && std::memcmp(data.data() + at + 1, "IPL.LNG", 7) == 0) {
      data[at + 1 + name] = WiiLanguage(lang);
      return true;
    }
  }
  return false;
}

// Tabla del juego (RMHE08): {cartel, idioma del juego} por idioma de la Wii.
// Original: 00 01 | 00 01 | 00 01 | 02 02 | 03 05 | 00 01 | 00 01 | 00 00
// Cartel: 0 ingles, 1 aleman, 2 frances, 3 espanol, 4 italiano.
// Idioma del juego: 1 _eng, 2 _fre, 3 _ger, 4 _ita, 5 _spa.
template <class Read8, class Write8>
inline bool PatchGameTable(Read8 read8, Write8 write8) {
  constexpr uint32_t kTable = 0x80580DB8;
  static const uint8_t kOrig[16] = {0, 1, 0, 1, 0, 1, 2, 2, 3, 5, 0, 1, 0, 1, 0, 0};
  for (uint32_t i = 0; i < 16; ++i) if (read8(kTable + i) != kOrig[i]) return false;  // otra version del juego
  write8(kTable + 2 * 2 + 0, 1); write8(kTable + 2 * 2 + 1, 3);   // aleman
  write8(kTable + 5 * 2 + 0, 4); write8(kTable + 5 * 2 + 1, 4);   // italiano
  return true;
}

}  // namespace port_lang
