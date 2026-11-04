// tools/gx-pipeline: THPVideoDecode (0x804E1120) en nativo.
// El decodificador del juego tarda ~45 ms por cuadro en la CPU emulada y los videos
// (29,97 FPS) iban a ~24 FPS, por detras del sonido. Este decodifica el mismo cuadro en
// ~5 ms en un hilo aparte: mientras, el juego sigue atendiendo sus interrupciones (el
// sonido necesita respuesta cada pocos milisegundos; bloquearlo 5 ms seguidos cortaba el
// audio de las cinematicas). Se protege solo: los dos primeros cuadros los decodifica el juego y se comparan
// con los de aqui; si no coinciden, se sigue usando el del juego y se avisa en el registro.
//   NWII_THP_NATIVE=0  desactiva el decodificador nativo
#include "port_thp_decode.h"
#include "port_perf.h"
#include "runtime/cpu_context.h"
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace nwii::runtime { bool process_pending_callbacks(CPUContext&); }

namespace port_thp {

using nwii::runtime::CPUContext;

// Puntero a memoria del invitado con 'len' bytes seguidos disponibles (o null).
static uint8_t* Guest(CPUContext& c, uint32_t addr, size_t len) {
  const uint32_t pa = addr & 0x3FFFFFFF;
  if (pa < c.mmu.mem1.size() - 8) return pa + len <= c.mmu.mem1.size() - 8 ? &c.mmu.mem1[pa] : nullptr;
  if (pa >= 0x10000000 && pa - 0x10000000 < c.mmu.mem2.size() - 8)
    return pa - 0x10000000 + len <= c.mmu.mem2.size() - 8 ? &c.mmu.mem2[pa - 0x10000000] : nullptr;
  return nullptr;
}
static size_t GuestLeft(CPUContext& c, uint32_t addr) {
  const uint32_t pa = addr & 0x3FFFFFFF;
  if (pa < c.mmu.mem1.size() - 8) return c.mmu.mem1.size() - 8 - pa;
  if (pa >= 0x10000000 && pa - 0x10000000 < c.mmu.mem2.size() - 8) return c.mmu.mem2.size() - 8 - (pa - 0x10000000);
  return 0;
}

enum class Mode { kChecking, kNative, kOff };
static Mode g_mode = [] { const char* e = std::getenv("NWII_THP_NATIVE"); return e && e[0] == '0' ? Mode::kOff : Mode::kChecking; }();
struct Pending { bool valid = false; uint32_t y = 0, u = 0, v = 0; int w = 0, h = 0; std::vector<uint8_t> Y, U, V; };
static Pending g_pending;
static int g_checked_ok = 0;

// Compara la salida del juego (ya en memoria) con la nuestra del cuadro anterior.
static void CheckPrevious(CPUContext& c) {
  Pending& p = g_pending;
  if (!p.valid) return;
  p.valid = false;
  const size_t ny = size_t(p.w) * p.h, nc = ny / 4;
  uint8_t* gy = Guest(c, p.y, ny); uint8_t* gu = Guest(c, p.u, nc); uint8_t* gv = Guest(c, p.v, nc);
  if (!gy || !gu || !gv) { g_mode = Mode::kOff; std::printf("[THP] no se pudo comprobar el decodificador nativo: se usa el del juego\n"); return; }
  int maxd = 0; size_t big = 0;
  auto cmp = [&](const uint8_t* a, const uint8_t* b, size_t n) {
    for (size_t i = 0; i < n; ++i) { const int d = std::abs(int(a[i]) - int(b[i])); if (d > maxd) maxd = d; if (d > 4) ++big; }
  };
  cmp(gy, p.Y.data(), ny); cmp(gu, p.U.data(), nc); cmp(gv, p.V.data(), nc);
  const double frac = double(big) / double(ny + 2 * nc);
  std::printf("[THP] comprobacion %dx%d: diferencia maxima %d, %.3f%% de pixeles con mas de 4\n", p.w, p.h, maxd, frac * 100.0);
  if (frac < 0.001) {
    if (++g_checked_ok >= 2) { g_mode = Mode::kNative; std::printf("[THP] decodificador nativo verificado: videos a velocidad completa\n"); }
  } else {
    g_mode = Mode::kOff;
    std::printf("[THP] el decodificador nativo no coincide con el del juego: se usa el del juego\n");
  }
}

// Hilo de decodificacion (uno basta: ~5 ms por cuadro).
struct Job {
  std::mutex m; std::condition_variable cv;
  bool busy = false, ready = false, done = false, ok = false, started = false;
  uint32_t key[4] = {};
  const uint8_t* data = nullptr; size_t size = 0; uint8_t *y = nullptr, *u = nullptr, *v = nullptr;
};
static Job& J() { static Job* j = new Job; return *j; }   // nunca se destruye (el hilo sigue esperando al salir)
static void Worker() {
  port_perf::PinHelper();
  Job& j = J();
  for (;;) {
    const uint8_t* data; size_t size; uint8_t *y, *u, *v;
    { std::unique_lock<std::mutex> l(j.m); j.cv.wait(l, [&j] { return j.ready; }); j.ready = false;
      data = j.data; size = j.size; y = j.y; u = j.u; v = j.v; }
    Frame fr;
    const bool ok = DecodeFrame(data, size, y, u, v, &fr);
    { std::lock_guard<std::mutex> l(j.m); j.ok = ok; j.done = true; }
  }
}

// En la entrada de THPVideoDecode(file r3, tileY r4, tileU r5, tileV r6, work r7).
// true = hecho (r3 = 0, vuelve al llamador); false = que lo haga el juego.
bool Decode(CPUContext& c) {
  if (g_mode == Mode::kOff) return false;
  if (g_mode == Mode::kChecking) CheckPrevious(c);
  if (g_mode == Mode::kOff) return false;
  const uint32_t file = c.gpr[3], ty = c.gpr[4], tu = c.gpr[5], tv = c.gpr[6];
  const size_t avail = GuestLeft(c, file);
  const uint8_t* data = Guest(c, file, 16);
  if (!data || avail < 16) return false;
  const size_t size = avail < (4u << 20) ? avail : (4u << 20);   // un cuadro cabe de sobra
  Frame fr;
  if (!DecodeFrame(data, size, nullptr, nullptr, nullptr, &fr, true)) return false;
  const size_t ny = size_t(fr.w) * fr.h, nc = ny / 4;
  if (g_mode == Mode::kChecking) {
    Pending& p = g_pending;
    p.Y.assign(ny, 0); p.U.assign(nc, 0); p.V.assign(nc, 0);
    if (!DecodeFrame(data, size, p.Y.data(), p.U.data(), p.V.data(), &fr)) return false;
    p.valid = true; p.y = ty; p.u = tu; p.v = tv; p.w = fr.w; p.h = fr.h;
    return false;   // esta vez decodifica el juego; se compara en la siguiente llamada
  }
  uint8_t* oy = Guest(c, ty, ny); uint8_t* ou = Guest(c, tu, nc); uint8_t* ov = Guest(c, tv, nc);
  if (!oy || !ou || !ov) return false;
  Job& j = J();
  const uint32_t key[4] = {file, ty, tu, tv};
  {
    std::lock_guard<std::mutex> l(j.m);
    // Si se vuelve aqui tras atender una interrupcion, el trabajo de este cuadro ya esta en marcha.
    if (!j.busy || std::memcmp(j.key, key, sizeof(key)) != 0) {
      if (j.busy && !j.done) { /* otro cuadro a medias (no deberia pasar): se espera abajo */ }
      else {
        std::memcpy(j.key, key, sizeof(key));
        j.data = data; j.size = size; j.y = oy; j.u = ou; j.v = ov;
        j.busy = true; j.done = false; j.ok = false; j.ready = true;
        if (!j.started) { j.started = true; std::thread(Worker).detach(); }
        j.cv.notify_one();
      }
    }
  }
  for (;;) {
    {
      std::lock_guard<std::mutex> l(j.m);
      if (j.done && std::memcmp(j.key, key, sizeof(key)) == 0) {
        j.busy = false; j.done = false;
        if (!j.ok) return false;          // lo rehace el decodificador del juego
        c.gpr[3] = 0;
        c.pc = c.lr;
        return true;
      }
      if (j.done && j.busy) { j.busy = false; j.done = false; }   // terminado el otro: lanzar el nuestro en la siguiente vuelta
      if (!j.busy) {
        std::memcpy(j.key, key, sizeof(key));
        j.data = data; j.size = size; j.y = oy; j.u = ou; j.v = ov;
        j.busy = true; j.done = false; j.ok = false; j.ready = true;
        j.cv.notify_one();
      }
    }
    // Mientras tanto, como en un bucle del juego: atender interrupciones (sonido, VI, disco).
    c.pc = 0x804E1120;
    if (nwii::runtime::process_pending_callbacks(c)) return true;   // se vuelve aqui al terminar la interrupcion
    c.inst_count += 256;
    std::this_thread::yield();
  }
}

}  // namespace port_thp
