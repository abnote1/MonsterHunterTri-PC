#pragma once
// tools/gx-pipeline: copias EFB en el guardado rapido.
// Algunas imagenes (iluminacion del personaje, retrato del perfil...) el juego las
// dibuja una vez y las copia a una "textura" que en el port solo existe en la GPU.
// El guardado rapido las lee de la GPU (hilo de OpenGL) y al cargar las vuelve a subir.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace port_efb {
struct Tex { uint32_t addr = 0, w = 0, h = 0; std::vector<uint8_t> rgba; };
struct Shared {
  std::mutex m; std::condition_variable cv;
  bool dump_req = false, dump_done = false; std::vector<Tex> dumped;
  bool restore_req = false; std::vector<Tex> restore;
};
inline Shared& S() { static Shared s; return s; }
// Hilo del juego: pide al hilo de OpenGL una copia de todas (espera como mucho ms).
inline bool RequestDump(std::vector<Tex>& out, int ms = 2000) {
  auto& s = S(); std::unique_lock<std::mutex> l(s.m);
  s.dumped.clear(); s.dump_done = false; s.dump_req = true;
  const bool ok = s.cv.wait_for(l, std::chrono::milliseconds(ms), [&s] { return s.dump_done; });
  s.dump_req = false;
  if (ok) out.swap(s.dumped);
  return ok;
}
// Hilo del juego: las copias de la partida cargada sustituyen a las actuales.
inline void RequestRestore(std::vector<Tex>&& v) {
  auto& s = S(); std::lock_guard<std::mutex> l(s.m);
  s.restore = std::move(v); s.restore_req = true;
}
void Service();   // hilo de OpenGL, en cada vuelta (renderer_gl.cpp)
}  // namespace port_efb
