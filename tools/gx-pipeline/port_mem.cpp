// tools/gx-pipeline: memcpy (0x80004000, tambien hace de memmove) y __fill_mem (0x8000429C) del
// juego en nativo. En la Steam Deck se llevaban ~10 % del hilo del juego copiando byte a byte por
// la MMU emulada. Si algun rango no es RAM normal (registros, FIFO grafico, cache bloqueada),
// se deja la version del juego.
#include "runtime/cpu_context.h"
#include <cstdlib>
#include <cstring>

namespace port_mem {
using nwii::runtime::CPUContext;

static uint8_t* Guest(CPUContext& c, uint32_t addr, uint32_t len) {
  const uint32_t pa = addr & 0x3FFFFFFF;
  const size_t m1 = c.mmu.mem1.size() - 8, m2 = c.mmu.mem2.size() - 8;
  if (pa < m1) return size_t(pa) + len <= m1 ? &c.mmu.mem1[pa] : nullptr;
  if (pa >= 0x10000000 && pa - 0x10000000 < m2) return size_t(pa - 0x10000000) + len <= m2 ? &c.mmu.mem2[pa - 0x10000000] : nullptr;
  return nullptr;
}
static const bool g_off = [] { const char* e = std::getenv("NWII_NATIVE_MEM"); return e && e[0] == '0'; }();

// memcpy(r3 destino, r4 origen, r5 bytes): vuelve con r3 = destino.
bool Copy(CPUContext& c) {
  if (g_off) return false;
  const uint32_t n = c.gpr[5];
  if (n == 0) { c.pc = c.lr; return true; }
  if (n > (64u << 20)) return false;
  if ((c.gpr[3] & 0x3FFFFFFF) < 0x10 || (c.gpr[4] & 0x3FFFFFFF) < 0x10) return false;   // direccion 0xC es especial
  uint8_t* d = Guest(c, c.gpr[3], n);
  uint8_t* s = Guest(c, c.gpr[4], n);
  if (!d || !s) return false;
  std::memmove(d, s, n);
  c.inst_count += 16 + n / 2;
  c.pc = c.lr;
  return true;
}
// __fill_mem(r3 destino, r4 valor, r5 bytes)
bool Fill(CPUContext& c) {
  if (g_off) return false;
  const uint32_t n = c.gpr[5];
  if (n == 0) { c.pc = c.lr; return true; }
  if (n > (64u << 20)) return false;
  if ((c.gpr[3] & 0x3FFFFFFF) < 0x10) return false;
  uint8_t* d = Guest(c, c.gpr[3], n);
  if (!d) return false;
  std::memset(d, int(c.gpr[4] & 0xFF), n);
  c.inst_count += 16 + n / 4;
  c.pc = c.lr;
  return true;
}
}  // namespace port_mem
