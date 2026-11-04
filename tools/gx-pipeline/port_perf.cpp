// tools/gx-pipeline: ver port_perf.h
#include "port_perf.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <cstring>
#include <thread>
#include <vector>
#include <fstream>
#include <iterator>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace port_perf {
static std::atomic<uint64_t> g_idle_us{0};
void AddIdleUs(uint64_t us) { g_idle_us.fetch_add(us, std::memory_order_relaxed); }
uint64_t IdleUs() { return g_idle_us.load(std::memory_order_relaxed); }
unsigned Cores() { const unsigned n = std::thread::hardware_concurrency(); return n ? n : 1; }

#ifdef _WIN32
static std::atomic<HANDLE> g_threads[kCount] = {};
static HANDLE g_guest_full = nullptr;
static uintptr_t g_last_outside = 0;
static uint32_t g_last_caller = 0;   // vuelta al ejecutable mas cercana en la pila (RVA)
static uintptr_t g_stack_copy[96];   // ultima direccion del hilo del juego fuera del ejecutable (DLL del sistema)   // para el perfil del codigo nativo (SuspendThread + GetThreadContext)
// Direccion nativa (RIP) del hilo del juego relativa al ejecutable, o 0 si no se puede leer.
static uint32_t GuestRva() {
  if (!g_guest_full) return 0;
  if (SuspendThread(g_guest_full) == (DWORD)-1) return 0;
  CONTEXT ctx; std::memset(&ctx, 0, sizeof(ctx)); ctx.ContextFlags = CONTEXT_CONTROL;
  const BOOL ok = GetThreadContext(g_guest_full, &ctx);
  // copia de la cima de la pila (para saber desde donde del ejecutable se llamo a la DLL)
  if (ok) { __try { std::memcpy(g_stack_copy, reinterpret_cast<const void*>(ctx.Rsp), sizeof(g_stack_copy)); } __except (1) { std::memset(g_stack_copy, 0, sizeof(g_stack_copy)); } }
  ResumeThread(g_guest_full);   // nada de reservar memoria ni bloquear con el hilo parado
  if (!ok) return 0;
  static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const uintptr_t rip = uintptr_t(ctx.Rip);
  if (rip >= base && rip - base < 0xFFFFFFF0u) return uint32_t(rip - base);
  g_last_outside = rip;
  g_last_caller = 0;
  for (size_t k = 0; k < sizeof(g_stack_copy) / 8; ++k) {   // primera direccion de vuelta dentro del ejecutable
    const uintptr_t v = g_stack_copy[k];
    if (v > base + 0x1000 && v - base < 0x08000000u) { g_last_caller = uint32_t(v - base); break; }
  }
  return 1;
}
// Nombre exportado mas cercano por debajo de 'addr' dentro de su modulo (p. ej. ntdll!RtlAcquireSRWLockExclusive+40)
static std::string ExportName(uintptr_t addr) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(addr), &m) || !m) return "?";
  const uintptr_t b = reinterpret_cast<uintptr_t>(m);
  char full[260]; const DWORD n = GetModuleFileNameA(m, full, sizeof(full));
  const char* mod = full; for (DWORD k = 0; k < n; ++k) if (full[k] == '\\' || full[k] == '/') mod = full + k + 1;
  std::string best = "?"; uint32_t bestRva = 0; const uint32_t rva = uint32_t(addr - b);
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + dos->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (dir.VirtualAddress) {
    auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(b + dir.VirtualAddress);
    auto* names = reinterpret_cast<const DWORD*>(b + ex->AddressOfNames);
    auto* ords = reinterpret_cast<const WORD*>(b + ex->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<const DWORD*>(b + ex->AddressOfFunctions);
    for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
      const uint32_t f = funcs[ords[i]];
      if (f <= rva && f >= bestRva) { bestRva = f; best = reinterpret_cast<const char*>(b + names[i]); }
    }
  }
  char out[200]; std::snprintf(out, sizeof(out), "%s!%s+%X", mod, best.c_str(), rva - bestRva);
  return out;
}

static double Ms(const FILETIME& k, const FILETIME& u) {
  const ULONGLONG a = (ULONGLONG(k.dwHighDateTime) << 32) | k.dwLowDateTime;
  const ULONGLONG b = (ULONGLONG(u.dwHighDateTime) << 32) | u.dwLowDateTime;
  return double(a + b) / 10000.0;   // unidades de 100 ns
}

// Mascaras de nucleos: el juego en el primer nucleo fisico (sus hilos logicos), el resto para los auxiliares.
struct Affinity { bool on = false; DWORD_PTR guest = 0, helpers = 0; int physical = 0; char info[160] = "sin reparto"; };
static const Affinity& Aff() {
  static Affinity a = [] {
    Affinity r;
    const char* e = std::getenv("NWII_AFFINITY");
    if (e && e[0] == '0') { std::snprintf(r.info, sizeof(r.info), "desactivado (NWII_AFFINITY=0)"); return r; }
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (!len) return r;
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> v(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!GetLogicalProcessorInformation(v.data(), &len)) return r;
    std::vector<DWORD_PTR> cores;
    for (auto& i : v) if (i.Relationship == RelationProcessorCore && i.ProcessorMask) cores.push_back(i.ProcessorMask);
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    r.physical = int(cores.size());
    if (cores.size() < 4) { std::snprintf(r.info, sizeof(r.info), "sin reparto (%d nucleos fisicos)", r.physical); return r; }
    // el ultimo nucleo fisico: el primero suele recibir mas interrupciones del sistema
    r.guest = cores.back() & proc;
    r.helpers = proc & ~r.guest;
    if (!r.guest || !r.helpers) return r;
    r.on = true;
    std::snprintf(r.info, sizeof(r.info), "%d nucleos fisicos: juego en 0x%llX, auxiliares en 0x%llX", r.physical,
                  (unsigned long long)r.guest, (unsigned long long)r.helpers);
    return r;
  }();
  return a;
}
const char* AffinityInfo() { return Aff().info; }
void PinHelper() { const Affinity& a = Aff(); if (a.on) SetThreadAffinityMask(GetCurrentThread(), a.helpers); }
void Mark(Thread t) {
  if (t < 0 || t >= kCount) return;
  const Affinity& a = Aff();
  if (a.on) SetThreadAffinityMask(GetCurrentThread(), t == kGuest ? a.guest : a.helpers);
  if (g_threads[t].load()) return;
  HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
  if (h) g_threads[t].store(h);
  if (t == kGuest) g_guest_full = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
}
double ThreadMs(Thread t) {
  if (t < 0 || t >= kCount) return 0;
  HANDLE h = g_threads[t].load();
  FILETIME c, e, k, u;
  return h && GetThreadTimes(h, &c, &e, &k, &u) ? Ms(k, u) : 0;
}
double ProcessMs() {
  FILETIME c, e, k, u;
  return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? Ms(k, u) : 0;
}
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
void SleepUs(uint32_t us) {
  if (!us) return;
  static thread_local HANDLE timer = [] {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!h) h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    return h;
  }();
  if (timer) {
    LARGE_INTEGER due; due.QuadPart = -LONGLONG(us) * 10;   // relativo, en 100 ns
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(timer, 50); return; }
  }
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}
#else
static uint32_t GuestRva() { return 0; }
const char* AffinityInfo() { return "sin reparto"; }
void PinHelper() {}
void Mark(Thread) {}
double ThreadMs(Thread) { return 0; }
double ProcessMs() { return 0; }
void SleepUs(uint32_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
#endif
bool g_count_regs = false;
static uint32_t g_reg_reads[0x10000 / 2];
static uint32_t g_reg_writes[0x10000 / 2];
void CountReg(uint32_t addr, bool write) {
  const uint32_t i = (addr & 0xFFFF) >> 1;
  if (write) ++g_reg_writes[i]; else ++g_reg_reads[i];
}
static void RegReport() {
  std::vector<std::pair<uint32_t, uint32_t>> top;
  for (uint32_t i = 0; i < 0x8000; ++i) {
    if (g_reg_reads[i]) top.push_back({g_reg_reads[i], i * 2});
    if (g_reg_writes[i]) top.push_back({g_reg_writes[i], 0x10000 | (i * 2)});
    g_reg_reads[i] = g_reg_writes[i] = 0;
  }
  std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::string s; char b[48];
  for (size_t i = 0; i < top.size() && i < 12; ++i) {
    std::snprintf(b, sizeof(b), " %s%04X:%u", (top[i].second & 0x10000) ? "W" : "R", top[i].second & 0xFFFF, top[i].first / 10);
    s += b;
  }
  std::printf("[Registros] por segundo:%s\n", s.c_str());
}
void StartSampler(const volatile uint32_t* pc) {
  static bool started = false;
  if (started || !pc) return;
  started = true;
  g_count_regs = true;
  std::thread([pc] {
    PinHelper();
    std::vector<uint32_t> bins(1 << 16, 0);
    std::unordered_map<uint32_t, uint32_t> host;   // RIP/16 del hilo del juego -> muestras
    std::unordered_map<std::string, uint32_t> outside;   // fuera del ejecutable: modulo+desplazamiento
    uint32_t total = 0, other = 0;
    auto last = std::chrono::steady_clock::now();
    for (;;) {
      SleepUs(1000);
      const uint32_t rva = GuestRva();
      if (rva) ++host[rva >> 8];
#ifdef _WIN32
      if (rva == 1 && g_last_outside) {
        char cb[24]; std::snprintf(cb, sizeof(cb), " <-%X", g_last_caller);
        ++outside[ExportName(g_last_outside) + cb];
      }
#endif
      const uint32_t p = *pc;
      if (p >= 0x80000000u && p < 0x81800000u) ++bins[(p >> 8) & 0xFFFF]; else ++other;
      ++total;
      const auto now = std::chrono::steady_clock::now();
      if (now - last >= std::chrono::seconds(10)) {
        last = now;
        std::vector<std::pair<uint32_t, uint32_t>> top;
        for (uint32_t i = 0; i < bins.size(); ++i) if (bins[i]) top.push_back({bins[i], i});
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::string s;
        char b[32];
        for (size_t i = 0; i < top.size() && i < 40; ++i) { std::snprintf(b, sizeof(b), " %04X:%u", top[i].second, top[i].first); s += b; }
        std::printf("[Muestreo] muestras=%u fuera=%u%s\n", total, other, s.c_str());
        RegReport();
        if (!host.empty()) {
          std::vector<std::pair<uint32_t, uint32_t>> ht(host.begin(), host.end());
          std::sort(ht.begin(), ht.end(), [](auto& a, auto& b) { return a.second > b.second; });
          std::string hs; char hb[32]; uint32_t ntot = 0;
          for (auto& e : ht) ntot += e.second;
          for (size_t i = 0; i < ht.size(); ++i) { std::snprintf(hb, sizeof(hb), " %X:%u", ht[i].first << 8, ht[i].second); hs += hb; }
          std::printf("[Nativo] muestras=%u%s\n", ntot, hs.c_str());
          host.clear();
        }
        if (!outside.empty()) {
          std::vector<std::pair<std::string, uint32_t>> ot(outside.begin(), outside.end());
          std::sort(ot.begin(), ot.end(), [](auto& x, auto& y) { return x.second > y.second; });
          std::string os;
          for (size_t i = 0; i < ot.size() && i < 40; ++i) os += " " + ot[i].first + ":" + std::to_string(ot[i].second);
          std::printf("[NativoFuera]%s\n", os.c_str());
          outside.clear();
        }
        std::fill(bins.begin(), bins.end(), 0u);
        total = other = 0;
      }
    }
  }).detach();
}
}  // namespace port_perf

// ---- Steam Deck: frecuencias, GPU y consumo (ver port_perf.h) ----
namespace {
std::string PortLeer(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
}
std::string port_perf::DeckInfo() {
  std::string out;
  char b[160];
  const std::string ci = PortLeer("Z:\\proc\\cpuinfo");
  if (!ci.empty()) {
    std::vector<double> mhz;
    size_t pos = 0;
    while ((pos = ci.find("cpu MHz", pos)) != std::string::npos) {
      pos = ci.find(':', pos);
      if (pos == std::string::npos) break;
      mhz.push_back(std::atof(ci.c_str() + pos + 1));
    }
    if (!mhz.empty()) {
      const auto mm = std::minmax_element(mhz.begin(), mhz.end());
      double suma = 0; for (double v : mhz) suma += v;
      // el hilo del juego va en el ultimo nucleo fisico (dos ultimos procesadores logicos)
      const double juego = mhz.size() >= 2 ? (mhz[mhz.size() - 1] + mhz[mhz.size() - 2]) / 2 : mhz.back();
      std::snprintf(b, sizeof b, "cpu_mhz=%.0f-%.0f media=%.0f nucleo_juego=%.0f", *mm.first, *mm.second, suma / mhz.size(), juego);
      out += b;
    }
  }
  for (int c = 0; c < 3; ++c) {
    const std::string d = "Z:\\sys\\class\\drm\\card" + std::to_string(c) + "\\device\\";
    const std::string busy = PortLeer(d + "gpu_busy_percent");
    if (busy.empty()) continue;
    std::snprintf(b, sizeof b, " gpu=%d%%", std::atoi(busy.c_str()));
    out += b;
    const std::string sclk = PortLeer(d + "pp_dpm_sclk");
    const size_t st = sclk.find('*');
    if (st != std::string::npos) {
      size_t ini = sclk.rfind('\n', st); ini = ini == std::string::npos ? 0 : ini + 1;
      const size_t dos = sclk.find(':', ini);
      if (dos != std::string::npos && dos < st) {
        std::snprintf(b, sizeof b, " gpu_mhz=%d", std::atoi(sclk.c_str() + dos + 1));
        out += b;
      }
    }
    break;
  }
  for (int h = 0; h < 12; ++h) {
    const std::string d = "Z:\\sys\\class\\hwmon\\hwmon" + std::to_string(h) + "\\";
    std::string nombre = PortLeer(d + "name");
    if (nombre.rfind("amdgpu", 0) != 0) continue;
    std::string pw = PortLeer(d + "power1_average");
    if (pw.empty()) pw = PortLeer(d + "power1_input");
    if (!pw.empty()) { std::snprintf(b, sizeof b, " apu_w=%.1f", std::atof(pw.c_str()) / 1e6); out += b; }
    const std::string t = PortLeer(d + "temp1_input");
    if (!t.empty()) { std::snprintf(b, sizeof b, " temp=%.0fC", std::atof(t.c_str()) / 1000); out += b; }
    break;
  }
  return out;
}
