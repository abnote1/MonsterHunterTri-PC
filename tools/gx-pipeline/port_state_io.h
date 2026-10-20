#pragma once
// tools/gx-pipeline: guardado/cargado rapido (savestates).
// Cada archivo del runtime parcheado anade una funcion PortState_<nombre>(IO&)
// que guarda o restaura sus variables estaticas con este ayudante.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <vector>

namespace port_state {

struct IO {
  bool saving = true;
  std::vector<uint8_t>* buf = nullptr;
  size_t pos = 0;
  bool ok = true;
  void bytes(void* p, size_t n) {
    if (!ok) return;
    if (saving) { const uint8_t* b = static_cast<const uint8_t*>(p); buf->insert(buf->end(), b, b + n); return; }
    if (pos + n > buf->size()) { ok = false; return; }
    std::memcpy(p, buf->data() + pos, n); pos += n;
  }
  template <class T> void pod(T& v) { static_assert(std::is_trivially_copyable_v<T>, "pod"); bytes(&v, sizeof(T)); }
  template <class T> void atom(std::atomic<T>& a) { T v = a.load(); pod(v); if (!saving && ok) a.store(v); }
  template <class T> void deque(std::deque<T>& d) {
    uint32_t n = uint32_t(d.size()); pod(n);
    if (saving) { for (auto& x : d) pod(x); return; }
    d.clear(); for (uint32_t i = 0; i < n && ok; ++i) { T x{}; pod(x); d.push_back(x); }
  }
  template <class T> void vec(std::vector<T>& v) {
    uint32_t n = uint32_t(v.size()); pod(n);
    if (!saving) { if (n > (1u << 28)) { ok = false; return; } v.resize(n); }
    if (n) bytes(v.data(), size_t(n) * sizeof(T));
  }
  // Marca de seccion para detectar archivos desalineados.
  void tag(const char* t) {
    uint32_t v = 0; std::memcpy(&v, t, 4);
    uint32_t r = v; pod(r);
    if (!saving && r != v) ok = false;
  }
};

}  // namespace port_state

// Serializadores de cada archivo parcheado (definidos en las copias de tools/gx-pipeline).
namespace nwii::runtime::hw {
void PortState_hw_ai(port_state::IO&);
void PortState_hw_cp(port_state::IO&);
void PortState_hw_di(port_state::IO&);
void PortState_hw_dsp(port_state::IO&);
bool PortState_hw_dsp_idle();
void PortState_hw_ipc(port_state::IO&);
bool PortState_hw_ipc_idle();
void PortState_hw_pe(port_state::IO&);
void PortState_hw_pi(port_state::IO&);
void PortState_hw_vi(port_state::IO&);
}
void PortState_ios(port_state::IO&);
bool PortState_ios_idle();
void PortState_ios_kernel(port_state::IO&);
void PortState_usb(port_state::IO&);
bool PortState_usb_idle();
void PortState_fs(port_state::IO&);
void PortState_pad(port_state::IO&);
void PortState_os_stubs(port_state::IO&);
void PortState_fifo_parser(port_state::IO&);
bool PortState_gx_idle();           // FIFO vacio: nada a medio leer
void PortState_gx_reset();          // descarta buffers y cuadros pendientes (hilo de lectura)
