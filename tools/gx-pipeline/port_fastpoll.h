#pragma once
// tools/gx-pipeline: se incluye a la fuerza (/FI) en las unidades generadas output_N.cpp.
// El codigo recompilado llama a process_pending_callbacks() en CADA vuelta de cada bucle del
// juego (interrupciones, eventos, menu del port...). Esa llamada cuesta mas que muchas vueltas
// de bucle enteras (bucles de skinning, copias...). Aqui solo se hace la llamada real una de
// cada g_port_poll_reload vueltas: las interrupciones llegan unos microsegundos mas tarde, que
// es inapreciable, y el hilo del juego gana tiempo. NWII_FAST_POLL=0 vuelve a llamar siempre.
#include "runtime/cpu_context.h"
#include <cstdlib>
namespace nwii::runtime {
inline int g_port_poll_left = 0;
inline int g_port_poll_reload = [] { const char* e = std::getenv("NWII_FAST_POLL"); const int v = e ? std::atoi(e) : 32; return v < 1 ? 1 : v; }();
inline bool port_ppc_fast(CPUContext& c) {
  if (--g_port_poll_left > 0) return false;
  g_port_poll_left = g_port_poll_reload;
  return nwii::runtime::port_ne_ppc(c);  // sin captura del contexto (cpu_context.h)
}
}  // namespace nwii::runtime
#define process_pending_callbacks(c) port_ppc_fast(c)
