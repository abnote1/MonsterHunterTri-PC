// tools/gx-pipeline: guardado y cargado rapido (savestates) del port.
//
// Cuando guardar: el codigo recompilado llama a process_pending_callbacks en cada
// salto hacia atras con ctx.pc apuntando a una instruccion desde la que se puede
// reanudar (es el mismo mecanismo que usan las interrupciones). Ahi el estado del
// juego esta completo en ctx y en su memoria. Se espera ademas a un momento
// "tranquilo": sin interrupciones en curso, sin respuestas de IPC/DSP pendientes,
// sin eventos de un solo disparo en el planificador y con la FIFO de graficos vacia.
//
// Cargar: se restaura todo y se salta (longjmp) al despachador del runtime, que
// continua en ctx.pc, exactamente como al volver de una interrupcion.
//
// No se toca la NAND (las partidas del propio juego) ni el contenido del disco.
#include "port_state_io.h"
#include "port_menu_state.h"
#include "port_cloud.h"
#include "port_efb_state.h"
#include "runtime/cpu_context.h"
#include "runtime/event_scheduler.h"
#include "runtime/gx_state.h"
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <iterator>
#include <cstring>

namespace nwii::runtime { extern CPUContext* g_ctx_ptr; }
namespace nwii::runtime::gx { extern GXState g_state; }

namespace port_savestate {

using nwii::runtime::CPUContext;
static constexpr uint32_t kMagic = 0x5354484D;  // "MHTS"
static constexpr uint32_t kVersion = 2;   // 2: con copias EFB (v1 se sigue cargando)

using port_menu::g_save_request;
using port_menu::g_load_request;
using port_menu::g_last_result;
using port_menu::g_result_serial;
static std::chrono::steady_clock::time_point g_save_requested_at;

std::string SlotPath(int slot) {
  char b[64]; std::snprintf(b, sizeof(b), "guardados_rapidos/ranura%d.sav", slot);
  return b;
}

static void Result(int code) { g_last_result.store(code); g_result_serial.fetch_add(1); }

// ---- memoria: paginas de 4 KiB, las que son todo ceros no se escriben
static void SaveMemory(port_state::IO& io, std::vector<uint8_t>& mem) {
  const size_t page = 4096, pages = (mem.size() + page - 1) / page;
  uint32_t n = uint32_t(pages); io.pod(n);
  std::vector<uint8_t> flags(pages, 0);
  for (size_t p = 0; p < pages; ++p) {
    const size_t off = p * page, len = std::min(page, mem.size() - off);
    for (size_t i = 0; i < len; ++i) if (mem[off + i]) { flags[p] = 1; break; }
  }
  io.bytes(flags.data(), flags.size());
  for (size_t p = 0; p < pages; ++p) if (flags[p]) io.bytes(mem.data() + p * page, std::min(page, mem.size() - p * page));
}
static void LoadMemory(port_state::IO& io, std::vector<uint8_t>& mem) {
  const size_t page = 4096;
  uint32_t n = 0; io.pod(n);
  if (!io.ok || n != (mem.size() + page - 1) / page) { io.ok = false; return; }
  std::vector<uint8_t> flags(n); io.bytes(flags.data(), n);
  for (size_t p = 0; p < n && io.ok; ++p) {
    const size_t off = p * page, len = std::min(page, mem.size() - off);
    if (flags[p]) io.bytes(mem.data() + off, len); else std::memset(mem.data() + off, 0, len);
  }
}

// ---- registros de la CPU y estado del runtime en CPUContext
static void CpuState(port_state::IO& io, CPUContext& c, uint64_t& tb) {
  io.tag("CPU0");
  io.pod(c.gpr); io.pod(c.fpr); io.pod(c.ps1); io.pod(c.cr);
  io.pod(c.pc); io.pod(c.lr); io.pod(c.ctr); io.pod(c.xer); io.pod(c.msr); io.pod(c.fpscr);
  io.pod(c.srr0); io.pod(c.srr1); io.pod(c.dmau); io.pod(c.dmal); io.pod(c.gqr); io.pod(c.sprg);
  io.pod(c.in_callback); io.pod(c.callback_depth); io.pod(c.exception_pc); io.pod(c.inst_count);
  io.pod(c.reservation_addr); io.pod(c.dispatch_saved_ctx); io.pod(c.ext_resched_pending);
  io.pod(c.dec_value); io.pod(c.dec_written_tb); io.pod(c.dec_irq_pending);
  io.pod(tb);
  // pila de copias de seguridad de las retrollamadas (vacia en un punto tranquilo)
  std::vector<CPUContext::BackupState> bk;
  if (io.saving) { auto s = c.backup_stack; while (!s.empty()) { bk.insert(bk.begin(), s.top()); s.pop(); } }
  uint32_t n = uint32_t(bk.size()); io.pod(n);
  if (!io.saving) bk.resize(n);
  for (auto& b : bk) io.pod(b);
  if (!io.saving && io.ok) { while (!c.backup_stack.empty()) c.backup_stack.pop(); for (auto& b : bk) c.backup_stack.push(b); }
  io.tag("MEM0");
  if (io.saving) { SaveMemory(io, c.mmu.mem1); SaveMemory(io, c.mmu.mem2); }
  else { LoadMemory(io, c.mmu.mem1); LoadMemory(io, c.mmu.mem2); }
  io.pod(c.mmu.locked_cache);
}

// Copias EFB (solo en la GPU): resolucion nativa, RGBA.
static void EfbState(port_state::IO& io, std::vector<port_efb::Tex>& efb) {
  io.tag("EFB0");
  uint32_t n = uint32_t(efb.size()); io.pod(n);
  if (!io.saving) { if (n > 4096) { io.ok = false; return; } efb.assign(n, {}); }
  for (auto& t : efb) {
    io.pod(t.addr); io.pod(t.w); io.pod(t.h);
    if (!io.saving) { if (t.w > 1024 || t.h > 1024) { io.ok = false; return; } t.rgba.resize(size_t(t.w) * t.h * 4); }
    if (!t.rgba.empty()) io.bytes(t.rgba.data(), t.rgba.size());
  }
}

static void AllState(port_state::IO& io, CPUContext& c, uint64_t& tb, std::vector<port_efb::Tex>& efb) {
  uint32_t magic = kMagic, version = kVersion; io.pod(magic); io.pod(version);
  if (!io.saving && (magic != kMagic || (version != 1 && version != kVersion))) { io.ok = false; return; }
  CpuState(io, c, tb);
  io.tag("GXS0");
  static_assert(std::is_trivially_copyable_v<nwii::runtime::gx::GXState>, "GXState");
  io.pod(nwii::runtime::gx::g_state);
  if (io.saving || version >= 2) EfbState(io, efb);
  PortState_fifo_parser(io);
  io.tag("HW00");
  nwii::runtime::hw::PortState_hw_ai(io); nwii::runtime::hw::PortState_hw_cp(io);
  nwii::runtime::hw::PortState_hw_di(io); nwii::runtime::hw::PortState_hw_dsp(io);
  nwii::runtime::hw::PortState_hw_ipc(io); nwii::runtime::hw::PortState_hw_pe(io);
  nwii::runtime::hw::PortState_hw_pi(io); nwii::runtime::hw::PortState_hw_vi(io);
  io.tag("IOS0");
  PortState_ios(io); PortState_ios_kernel(io); PortState_usb(io); PortState_fs(io);
  PortState_pad(io); PortState_os_stubs(io);
  io.tag("END0");
}

static bool Quiet(CPUContext& c) {
  return !c.in_callback && c.callback_depth == 0 && c.dispatch_saved_ctx == 0 && c.backup_stack.empty() &&
         c.pending_cb_count.load() == 0 && !c.vblank_pending.load() &&
         nwii::runtime::EventScheduler::get().port_only_recurring() &&
         nwii::runtime::hw::PortState_hw_dsp_idle() && nwii::runtime::hw::PortState_hw_ipc_idle() &&
         PortState_ios_idle() && PortState_usb_idle() && PortState_gx_idle();
}

static void WriteAsync(int slot, std::vector<uint8_t>&& data) {
  std::thread([slot, d = std::move(data)]() {
    std::error_code ec; std::filesystem::create_directories("guardados_rapidos", ec);
    const std::string path = SlotPath(slot), tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
      if (!f) { std::printf("[Guardado] error escribiendo %s\n", tmp.c_str()); Result(100 + slot); return; } }
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::filesystem::remove(path, ec); std::filesystem::rename(tmp, path, ec); }
    std::printf("[Guardado] ranura %d guardada (%.1f MB)\n", slot, d.size() / 1048576.0);
    port_cloud::NotifySlotSaved(slot);
    Result(slot);
  }).detach();
}

static void DoSave(CPUContext& c, int slot) {
  std::vector<uint8_t> buf; buf.reserve(96u << 20);
  port_state::IO io; io.saving = true; io.buf = &buf;
  uint64_t tb = c.read_timebase();
  std::vector<port_efb::Tex> efb;
  if (!port_efb::RequestDump(efb)) std::printf("[Guardado] aviso: no se pudieron leer las copias EFB de la GPU\n");
  AllState(io, c, tb, efb);
  WriteAsync(slot, std::move(buf));
}

[[noreturn]] static void FinishLoad(CPUContext& c) {
  std::longjmp(c.exception_jmp_buf, 1);
}

static bool DoLoad(CPUContext& c, int slot) {
  std::ifstream f(SlotPath(slot), std::ios::binary);
  if (!f) { std::printf("[Guardado] ranura %d vacia\n", slot); Result(200 + slot); return false; }
  std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  // Validar antes de tocar nada: leer en un contexto de prueba no es posible (MMU
  // de 88 MB), asi que se comprueban cabecera y marcas leyendo en seco.
  if (buf.size() < 16) { Result(300 + slot); return false; }
  uint32_t magic = 0, version = 0; std::memcpy(&magic, buf.data(), 4); std::memcpy(&version, buf.data() + 4, 4);
  if (magic != kMagic || (version != 1 && version != kVersion)) { std::printf("[Guardado] ranura %d de otra version\n", slot); Result(300 + slot); return false; }
  // El hilo de graficos descarta lo que tuviera a medias.
  PortState_gx_reset();
  // Copia en memoria del estado actual: si el archivo resultara danado a mitad
  // de la carga, se vuelve a este punto en vez de dejar el juego roto.
  std::vector<uint8_t> backup; backup.reserve(96u << 20);
  uint64_t tb_now = c.read_timebase();
  { port_state::IO b; b.saving = true; b.buf = &backup; std::vector<port_efb::Tex> none; AllState(b, c, tb_now, none); }
  { std::lock_guard<std::mutex> lk(c.cb_mutex); while (!c.pending_callbacks.empty()) c.pending_callbacks.pop(); c.pending_cb_count.store(0); }
  port_state::IO io; io.saving = false; io.buf = &buf;
  uint64_t tb = 0;
  std::vector<port_efb::Tex> efb;
  AllState(io, c, tb, efb);
  if (!io.ok) {
    std::printf("[Guardado] ranura %d danada: se vuelve al punto anterior\n", slot);
    port_state::IO r; r.saving = false; r.buf = &backup; uint64_t tbr = 0; std::vector<port_efb::Tex> none;
    AllState(r, c, tbr, none);
    Result(300 + slot);
    return false;  // el estado actual sigue intacto (mismo punto de reanudacion)
  }
  // Reloj del invitado: continuar desde el valor guardado.
  const uint64_t us = (tb / c.tb_freq) * 1000000 + (tb % c.tb_freq) * 1000000 / c.tb_freq;
  c.tb_start = std::chrono::steady_clock::now() - std::chrono::microseconds(us);
  if (c.inst_count < tb) c.inst_count = tb;  // read_timebase = min(instrucciones, tiempo real)
  nwii::runtime::EventScheduler::get().port_rebase(c.read_timebase());
  c.vblank_pending.store(false);
  port_menu::g_tex_reload.store(true);      // texturas desde la memoria cargada
  if (version >= 2) port_efb::RequestRestore(std::move(efb));   // copias EFB de la partida guardada
  else std::printf("[Guardado] ranura antigua (sin copias EFB): algunas imagenes pueden salir negras hasta que el juego las rehaga\n");
  port_menu::g_gx_after_load.store(true);   // el hilo principal descarta el ultimo cuadro
  std::printf("[Guardado] ranura %d cargada (pc=0x%08X)\n", slot, c.pc);
  Result(-slot);
  return true;
}

// Llamado desde process_pending_callbacks en el hilo del juego.
void GuestHook(CPUContext& c) {
  if (const int slot = g_load_request.exchange(0)) {
    g_save_request.store(0);
    if (DoLoad(c, slot)) FinishLoad(c);
    return;
  }
  const int slot = g_save_request.load();
  if (!slot) return;
  if (g_save_requested_at.time_since_epoch().count() == 0) g_save_requested_at = std::chrono::steady_clock::now();
  if (Quiet(c)) {
    g_save_request.store(0); g_save_requested_at = {};
    DoSave(c, slot);
  } else if (std::chrono::steady_clock::now() - g_save_requested_at > std::chrono::seconds(3)) {
    g_save_request.store(0); g_save_requested_at = {};
    std::printf("[Guardado] no se encontro un momento seguro para guardar\n");
    Result(400 + slot);
  }
}

}  // namespace port_savestate
