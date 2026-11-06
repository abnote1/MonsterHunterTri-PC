// tools/gx-pipeline: red del Wii (IOS) para el modo online (la ciudad) contra un servidor propio.
//
// El juego habla con IOS por /dev/net/*: aqui se atienden esas peticiones con sockets del PC.
//   /dev/net/ip/top      sockets BSD del Wii (SOSocket, SOConnect, SORecvFrom, SOPoll...) y DNS
//   /dev/net/ssl         SSL de IOS. Sin cifrado: lo que el juego escribe va tal cual por el socket.
//                        El servidor es nuestro (servidor_wfc.py y MH3SP con UseSSL=OFF), asi que no
//                        hacen falta certificados ni parchear los del juego.
//   /dev/net/kd/request, /dev/net/kd/time, /dev/net/ncd/manage, /dev/net/wd/command
//                        lo minimo de WiiConnect24 y de la configuracion de red ("conexion por cable,
//                        prueba de conexion correcta").
// DNS: cualquier nombre (naswii.nintendowifi.net, gpcm.gs..., mmh-t1-opn01.mmh-service.capcom.co.jp...)
// se resuelve a la IP del servidor (online.ini: servidor=...). Las conexiones a los puertos 443 y 80
// (servicios web de Nintendo: NAS y prueba de conexion) van al puerto puerto_nas (9000 por defecto).
//
// Las operaciones que en el Wii bloquean (connect, recv, poll... en sockets sin O_NONBLOCK) quedan
// pendientes y se completan desde PortNetPoll (hilo del juego, en process_pending_callbacks):
// resultado en req+4 y respuesta IPC con ipc_post_reply, como las lecturas diferidas del mando.
// Los sockets del PC son siempre no bloqueantes.
//
// Registro: lineas "[Red] ..." en el log del juego. NWII_NET_TRACE=1 registra tambien cada envio
// y recepcion. Codigo propio (MIT); los numeros de ioctl y la disposicion de los buffers siguen lo
// documentado del IOS del Wii.
#include "runtime/devices.h"
#include "runtime/ios_kernel.h"
#include "runtime/hw/hw.h"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET port_sock_t;
static const port_sock_t kBadSock = INVALID_SOCKET;
static int NetErr() { return WSAGetLastError(); }
static void CloseSock(port_sock_t s) { closesocket(s); }
static void SetNonBlock(port_sock_t s) { u_long one = 1; ioctlsocket(s, FIONBIO, &one); }
#define PE(x) WSA##x
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int port_sock_t;
static const port_sock_t kBadSock = -1;
static int NetErr() { return errno; }
static void CloseSock(port_sock_t s) { close(s); }
static void SetNonBlock(port_sock_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#define PE(x) x
#endif

using nwii::runtime::CPUContext;
using nwii::runtime::IDevice;
using nwii::runtime::IpcRequest;
using nwii::runtime::IPC_OK;
using nwii::runtime::IPC_NO_REPLY;

int g_port_net_pending = 0;   // operaciones de red esperando (lo mira process_pending_callbacks)

namespace {

// ---------------------------------------------------------------- errores del Wii (SO_E*)
enum : int {
  SO_EADDRINUSE = 3, SO_EAFNOSUPPORT = 5, SO_EAGAIN = 6, SO_EALREADY = 7, SO_EBADF = 8,
  SO_ECONNABORTED = 13, SO_ECONNREFUSED = 14, SO_ECONNRESET = 15, SO_EHOSTUNREACH = 23,
  SO_EINPROGRESS = 26, SO_EINVAL = 28, SO_EISCONN = 30, SO_EMFILE = 33, SO_EMSGSIZE = 35,
  SO_ENETDOWN = 38, SO_ENETUNREACH = 40, SO_ENOBUFS = 42, SO_ENOTCONN = 56, SO_ENOTSOCK = 59,
  SO_EOPNOTSUPP = 63, SO_EPROTONOSUPPORT = 68, SO_ETIMEDOUT = 76,
};
enum : int32_t { SSL_OK = 0, SSL_FAILED = -1, SSL_RAGAIN = -2, SSL_WAGAIN = -3, SSL_SYSCALL = -5,
                 SSL_ZERO = -6, SSL_ID = -8 };

int MapErr(int e, bool rw) {
  switch (e) {
  case PE(EWOULDBLOCK): return rw ? SO_EAGAIN : SO_EINPROGRESS;
#ifndef _WIN32
#if EAGAIN != EWOULDBLOCK
  case EAGAIN: return rw ? SO_EAGAIN : SO_EINPROGRESS;
#endif
#endif
  case PE(EINPROGRESS): return SO_EINPROGRESS;
  case PE(EALREADY): return SO_EALREADY;
  case PE(EISCONN): return SO_EISCONN;
  case PE(ENOTCONN): return SO_ENOTCONN;
  case PE(ECONNREFUSED): return SO_ECONNREFUSED;
  case PE(ECONNRESET): return SO_ECONNRESET;
  case PE(ECONNABORTED): return SO_ECONNABORTED;
  case PE(ETIMEDOUT): return SO_ETIMEDOUT;
  case PE(EHOSTUNREACH): return SO_EHOSTUNREACH;
  case PE(ENETUNREACH): return SO_ENETUNREACH;
  case PE(ENETDOWN): return SO_ENETDOWN;
  case PE(EADDRINUSE): return SO_EADDRINUSE;
  case PE(EMSGSIZE): return SO_EMSGSIZE;
  case PE(ENOBUFS): return SO_ENOBUFS;
  case PE(ENOTSOCK): return SO_ENOTSOCK;
  case PE(EOPNOTSUPP): return SO_EOPNOTSUPP;
  default: return SO_EINVAL;
  }
}
bool IsWouldBlock(int e) {
#ifdef _WIN32
  return e == WSAEWOULDBLOCK;
#else
  return e == EWOULDBLOCK || e == EAGAIN;
#endif
}

// ---------------------------------------------------------------- memoria del invitado
// Las direcciones de los buffers de IOS son fisicas: MEM1 0x0..., MEM2 0x1....
uint32_t VA(uint32_t a) {
  if (a < 0x01800000u || (a >= 0x10000000u && a < 0x14000000u)) return a | 0x80000000u;
  return a;
}
uint32_t R32(CPUContext& c, uint32_t a) { return c.mmu.read32(VA(a)); }
void W32(CPUContext& c, uint32_t a, uint32_t v) { c.mmu.write32(VA(a), v); }
uint8_t R8(CPUContext& c, uint32_t a) { return c.mmu.read8(VA(a)); }
void W8(CPUContext& c, uint32_t a, uint8_t v) { c.mmu.write8(VA(a), v); }
void W16(CPUContext& c, uint32_t a, uint16_t v) { W8(c, a, uint8_t(v >> 8)); W8(c, a + 1, uint8_t(v)); }
void ReadBytes(CPUContext& c, uint32_t a, void* dst, uint32_t n) {
  uint8_t* d = static_cast<uint8_t*>(dst);
  for (uint32_t i = 0; i < n; ++i) d[i] = R8(c, a + i);
}
void WriteBytes(CPUContext& c, uint32_t a, const void* src, uint32_t n) {
  const uint8_t* s = static_cast<const uint8_t*>(src);
  for (uint32_t i = 0; i < n; ++i) W8(c, a + i, s[i]);
}
std::string ReadStr(CPUContext& c, uint32_t a, uint32_t max) {
  std::string s;
  if (!a) return s;
  for (uint32_t i = 0; i < max; ++i) { char ch = char(R8(c, a + i)); if (!ch) break; s += ch; }
  return s;
}
std::string Printable(const uint8_t* p, size_t n, size_t lim = 160) {
  std::string s;
  for (size_t i = 0; i < n && s.size() < lim; ++i) {
    const uint8_t ch = p[i];
    if (ch == '\r') continue;
    if (ch == '\n') s += " | ";
    else s += (ch >= 32 && ch < 127) ? char(ch) : '.';
  }
  return s;
}
std::string IpStr(uint32_t ip_host) {
  char b[24]; std::snprintf(b, sizeof b, "%u.%u.%u.%u", ip_host >> 24, (ip_host >> 16) & 255, (ip_host >> 8) & 255, ip_host & 255);
  return b;
}

// ---------------------------------------------------------------- ajustes (online.ini)
struct Config {
  bool loaded = false;
  std::string server = "127.0.0.1";
  uint16_t nas_port = 9000;
  uint32_t server_ip = 0x7F000001;   // orden del host
  uint32_t local_ip = 0xC0A80102;
  bool trace = false;
  bool autostart = true;   // arrancar los servidores locales si no estan abiertos
} g_cfg;

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
  size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

bool ParseIPv4(const std::string& s, uint32_t& out) {
  in_addr a{};
  if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
  out = ntohl(a.s_addr);
  return true;
}

void EnsureServers();

void LoadConfig() {
  if (g_cfg.loaded) return;
  g_cfg.loaded = true;
#ifdef _WIN32
  WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  std::ifstream f("online.ini");
  std::string line;
  while (std::getline(f, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
    if (k == "servidor" && !v.empty()) g_cfg.server = v;
    else if (k == "puerto_nas") g_cfg.nas_port = uint16_t(std::atoi(v.c_str()));
    else if (k == "registro_detallado") g_cfg.trace = v == "1";
    else if (k == "arrancar_servidores") g_cfg.autostart = v != "0";
  }
  if (const char* e = std::getenv("NWII_ONLINE_SERVER")) if (*e) g_cfg.server = e;
  if (const char* e = std::getenv("NWII_NET_TRACE")) g_cfg.trace = e[0] == '1';
  uint32_t ip = 0;
  if (!ParseIPv4(g_cfg.server, ip)) {
    addrinfo hints{}; hints.ai_family = AF_INET; addrinfo* res = nullptr;
    if (getaddrinfo(g_cfg.server.c_str(), nullptr, &hints, &res) == 0 && res) {
      ip = ntohl(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
      freeaddrinfo(res);
    } else {
      std::printf("[Red] no se pudo resolver el servidor '%s'; uso 127.0.0.1\n", g_cfg.server.c_str());
      ip = 0x7F000001;
    }
  }
  g_cfg.server_ip = ip;
  // IP propia: la de la interfaz que saldria hacia fuera (no se envia nada)
  port_sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s != kBadSock) {
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl((ip >> 24) == 127 ? 0x08080808u : ip);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
      sockaddr_in me{}; socklen_t ml = sizeof me;
      if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &ml) == 0 && me.sin_addr.s_addr)
        g_cfg.local_ip = ntohl(me.sin_addr.s_addr);
    }
    CloseSock(s);
  }
  std::printf("[Red] modo online: servidor %s (%s), servicios web de Nintendo -> puerto %u, IP propia %s\n",
              g_cfg.server.c_str(), IpStr(g_cfg.server_ip).c_str(), unsigned(g_cfg.nas_port), IpStr(g_cfg.local_ip).c_str());
  if (g_cfg.autostart && (g_cfg.server_ip >> 24) == 127) EnsureServers();
}

// ---------------------------------------------------------------- arranque de los servidores locales
// Si el servidor es este equipo (127.x) y no hay nada escuchando, el port abre el mismo los dos
// servidores al entrar en el online: servidor\servidor_wfc.py (servicios de Nintendo) y
// servidor\MH3SP\master_server.py, con el Python que va en servidor\python (Windows embebido; en la
// Steam Deck corre dentro de Proton como el juego) o, si no esta, el del sistema. Van sin ventana,
// con su salida en servidor\servidores.log, y se cierran al cerrar el juego.
// online.ini: arrancar_servidores=0 lo desactiva (para abrirlos a mano con "Servidor online.cmd").
bool TcpListening(uint32_t ip, uint16_t port, int timeout_ms) {
  port_sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kBadSock) return false;
  SetNonBlock(s);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(ip);
  connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
  fd_set wf, ef; FD_ZERO(&wf); FD_ZERO(&ef); FD_SET(s, &wf); FD_SET(s, &ef);
  timeval tv{0, timeout_ms * 1000};
  bool ok = false;
  if (select(int(s) + 1, nullptr, &wf, &ef, &tv) > 0 && FD_ISSET(s, &wf) && !FD_ISSET(s, &ef)) {
    int so = 0; socklen_t sl = sizeof so;
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &sl);
    ok = so == 0;
  }
  CloseSock(s);
  return ok;
}

#ifdef _WIN32
std::wstring ExeDir() {
  std::vector<wchar_t> b(4096);
  const DWORD n = GetModuleFileNameW(nullptr, b.data(), DWORD(b.size()));
  std::wstring p(b.data(), n);
  const size_t k = p.find_last_of(L"\\/");
  return k == std::wstring::npos ? std::wstring(L".") : p.substr(0, k);
}
bool FileExists(const std::wstring& p) {
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
std::string Narrow(const std::wstring& w) {
  std::string r; for (wchar_t c : w) r += (c < 128) ? char(c) : '?'; return r;
}
HANDLE g_srv_job = nullptr;
std::vector<HANDLE> g_srv_procs;
void KillServers() {
  for (HANDLE h : g_srv_procs) { TerminateProcess(h, 0); CloseHandle(h); }
  g_srv_procs.clear();
}
bool SpawnHidden(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, HANDLE out) {
  std::wstring cmd = L"\"" + exe + L"\" " + args;
  std::vector<wchar_t> cl(cmd.begin(), cmd.end()); cl.push_back(0);
  STARTUPINFOW si{}; si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = nullptr; si.hStdOutput = out; si.hStdError = out;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                      nullptr, cwd.c_str(), &si, &pi)) {
    std::printf("[Red] no se pudo abrir %s (error %lu)\n", Narrow(exe).c_str(), GetLastError());
    return false;
  }
  if (g_srv_job) AssignProcessToJobObject(g_srv_job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  g_srv_procs.push_back(pi.hProcess);
  return true;
}
#endif

void EnsureServers() {
  const uint32_t lo = 0x7F000001;
  bool wfc = TcpListening(lo, g_cfg.nas_port, 300), mh = TcpListening(lo, 8200, 300);
  if (wfc && mh) { std::printf("[Red] servidores ya abiertos\n"); return; }
#ifdef _WIN32
  const std::wstring dir = ExeDir(), srv = dir + L"\\servidor";
  if (!FileExists(srv + L"\\servidor_wfc.py") || !FileExists(srv + L"\\MH3SP\\master_server.py")) {
    std::printf("[Red] no hay servidores en %s\\servidor: abre \"Servidor online.cmd\" a mano\n", Narrow(dir).c_str());
    return;
  }
  std::wstring py = srv + L"\\python\\python.exe", pyarg;
  if (!FileExists(py)) {
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"python.exe", nullptr, MAX_PATH, found, nullptr)) py = found;
    else if (SearchPathW(nullptr, L"py.exe", nullptr, MAX_PATH, found, nullptr)) { py = found; pyarg = L"-3 "; }
    else { std::printf("[Red] falta Python: ni servidor\\python ni Python instalado\n"); return; }
  }
  if (!g_srv_job) {
    g_srv_job = CreateJobObjectW(nullptr, nullptr);
    if (g_srv_job) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
      li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(g_srv_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    std::atexit(KillServers);
  }
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE log = CreateFileW((srv + L"\\servidores.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
  SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
  std::printf("[Red] abriendo los servidores locales con %s\n", Narrow(py).c_str());
  if (!wfc) SpawnHidden(py, pyarg + L"servidor_wfc.py", srv, log);
  if (!mh) SpawnHidden(py, pyarg + L"master_server.py", srv + L"\\MH3SP", log);
  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(25)) {
    wfc = wfc || TcpListening(lo, g_cfg.nas_port, 200);
    mh = mh || TcpListening(lo, 8200, 200);
    if (wfc && mh) break;
    Sleep(200);
  }
  const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  if (wfc && mh) std::printf("[Red] servidores listos en %ld ms\n", ms);
  else std::printf("[Red] los servidores no responden (nintendo=%d capcom=%d); mira servidor\\servidores.log\n", int(wfc), int(mh));
#else
  std::printf("[Red] no hay servidores abiertos en este equipo\n");
#endif
}

// Nombre -> IP (orden del host). Las IP escritas se respetan; cualquier nombre va al servidor.
uint32_t Resolve(const std::string& name) {
  uint32_t ip;
  if (ParseIPv4(name, ip)) return ip;
  std::printf("[Red] DNS %s -> %s\n", name.c_str(), IpStr(g_cfg.server_ip).c_str());
  return g_cfg.server_ip;
}

const uint8_t kMac[6] = {0x00, 0x17, 0xAB, 0x4D, 0x48, 0x33};

// ---------------------------------------------------------------- sockets del Wii
constexpr int kMaxSock = 24;
struct WSock {
  bool used = false;
  port_sock_t s = kBadSock;
  bool tcp = true;
  bool nonblock = false;
  int conn = 0;        // 0 nada, 1 conectando, 2 conectado, 3 error
  int conn_err = 0;    // error del Wii (positivo) si conn == 3
  uint32_t peer_ip = 0; uint16_t peer_port = 0;
  uint64_t rx = 0, tx = 0;
};
WSock g_socks[kMaxSock];

WSock* Sock(uint32_t fd) { return (fd < uint32_t(kMaxSock) && g_socks[fd].used) ? &g_socks[fd] : nullptr; }

int AddSock(port_sock_t s, bool tcp) {
  for (int i = 0; i < kMaxSock; ++i)
    if (!g_socks[i].used) {
      g_socks[i] = WSock{}; g_socks[i].used = true; g_socks[i].s = s; g_socks[i].tcp = tcp;
      SetNonBlock(s);
      if (tcp) { int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one); }
      return i;
    }
  return -1;
}

// sockaddr del Wii {len, family, port BE, addr BE} <-> sockaddr_in
sockaddr_in ReadAddr(CPUContext& c, uint32_t a) {
  sockaddr_in o{}; o.sin_family = AF_INET;
  uint8_t b[8]; ReadBytes(c, a, b, 8);
  std::memcpy(&o.sin_port, b + 2, 2); std::memcpy(&o.sin_addr, b + 4, 4);
  return o;
}
void WriteAddr(CPUContext& c, uint32_t a, uint32_t len, const sockaddr_in& in) {
  if (!a || len < 8) return;
  uint8_t b[8] = {8, 2, 0, 0, 0, 0, 0, 0};
  std::memcpy(b + 2, &in.sin_port, 2); std::memcpy(b + 4, &in.sin_addr, 4);
  WriteBytes(c, a, b, 8);
}
// 443/80 de los servicios de Nintendo (NAS por https, prueba de conexion) -> nuestro servidor web
void Remap(sockaddr_in& a) {
  const uint16_t p = ntohs(a.sin_port);
  if ((p == 443 || p == 80) && ntohl(a.sin_addr.s_addr) == g_cfg.server_ip) a.sin_port = htons(g_cfg.nas_port);
}

int ConnState(WSock& w) {
  if (w.conn != 1) return w.conn;
  fd_set wf, ef; FD_ZERO(&wf); FD_ZERO(&ef); FD_SET(w.s, &wf); FD_SET(w.s, &ef);
  timeval tv{0, 0};
  const int n = select(int(w.s) + 1, nullptr, &wf, &ef, &tv);
  if (n <= 0) return 1;
  int so = 0; socklen_t sl = sizeof so;
  getsockopt(w.s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so), &sl);
  if (FD_ISSET(w.s, &ef) || so != 0) {
    w.conn = 3; w.conn_err = MapErr(so ? so : PE(ECONNREFUSED), false);
    std::printf("[Red] conexion a %s:%u fallida (error %d)\n", IpStr(w.peer_ip).c_str(), unsigned(w.peer_port), w.conn_err);
  } else if (FD_ISSET(w.s, &wf)) {
    w.conn = 2;
    std::printf("[Red] conectado a %s:%u\n", IpStr(w.peer_ip).c_str(), unsigned(w.peer_port));
  }
  return w.conn;
}

// ---------------------------------------------------------------- SSL (sin cifrado)
struct SslSlot { bool used = false; int fd = -1; std::string host; };
SslSlot g_ssl[4];
SslSlot* Ssl(uint32_t id1) { const uint32_t i = id1 - 1; return (i < 4 && g_ssl[i].used) ? &g_ssl[i] : nullptr; }

// ---------------------------------------------------------------- operaciones pendientes
enum Kind { K_CONNECT, K_ACCEPT, K_RECV, K_SEND, K_POLL, K_SSL_HS, K_SSL_READ, K_SSL_WRITE };
struct Op {
  IpcRequest r;
  Kind kind;
  int fd = -1;
  bool started = false;
  bool has_deadline = false;
  std::chrono::steady_clock::time_point deadline;
};
std::vector<Op> g_ops;

uint32_t VecAddr(const IpcRequest& r, bool io, uint32_t i) {
  const uint32_t idx = io ? r.arg_cnt_in + i : i;
  return idx < r.ioctlv_vecs.size() && (io ? i < r.arg_cnt_out : i < r.arg_cnt_in) ? r.ioctlv_vecs[idx].addr : 0;
}
uint32_t VecLen(const IpcRequest& r, bool io, uint32_t i) {
  const uint32_t idx = io ? r.arg_cnt_in + i : i;
  return idx < r.ioctlv_vecs.size() && (io ? i < r.arg_cnt_out : i < r.arg_cnt_in) ? r.ioctlv_vecs[idx].len : 0;
}

struct Stats { uint64_t recv = 0, send = 0, rxb = 0, txb = 0, polls = 0; } g_st;

// Pila de llamadas del invitado (cadena de marcos r1 -> LR guardado): para localizar en el
// codigo del juego quien decide algo (p. ej. abandonar el servidor). Solo con registro detallado.
void GuestBacktrace(CPUContext& c, const char* why) {
  std::printf("[Red] pila (%s): pc=%08X lr=%08X", why, c.pc, c.lr);
  uint32_t sp = c.gpr[1];
  for (int i = 0; i < 24; ++i) {
    if (sp < 0x80000000u || sp >= 0x94000000u || (sp & 3)) break;
    const uint32_t next = c.mmu.read32(sp);
    if (next < 0x80000000u || next >= 0x94000000u || next <= sp) break;
    std::printf(" %08X", c.mmu.read32(next + 4));
    sp = next;
  }
  std::printf("\n");
}
uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// Buffer reutilizable para envios y recepciones (sin reservar ni poner a cero en cada intento).
std::vector<uint8_t>& Scratch(uint32_t len) {
  static std::vector<uint8_t> b;
  if (b.size() < std::max<uint32_t>(len, 1)) b.resize(std::max<uint32_t>(len, 1));
  return b;
}

// Devuelve true si la operacion termino (res = valor de la respuesta IPC).
bool TryOp(CPUContext& c, Op& op, int32_t& res) {
  const IpcRequest& r = op.r;
  switch (op.kind) {
  case K_CONNECT: {
    WSock* w = Sock(op.fd);
    if (!w) { res = -SO_EBADF; return true; }
    if (!op.started) {
      op.started = true;
      if (w->conn == 2) { res = -SO_EISCONN; return true; }
      if (w->conn == 1) {
        const int st = ConnState(*w);
        res = st == 2 ? -SO_EISCONN : st == 3 ? -w->conn_err : -SO_EALREADY;
        if (st == 1 && !w->nonblock) return false;
        if (st == 3) w->conn = 0;
        return true;
      }
      sockaddr_in a = ReadAddr(c, r.in_buf + 8);
      const uint16_t orig_port = ntohs(a.sin_port);
      Remap(a);
      w->peer_ip = ntohl(a.sin_addr.s_addr); w->peer_port = ntohs(a.sin_port);
      std::printf("[Red] connect fd=%d %s:%u%s%s\n", op.fd, IpStr(w->peer_ip).c_str(), unsigned(orig_port),
                  orig_port != w->peer_port ? (" -> puerto " + std::to_string(w->peer_port)).c_str() : "",
                  w->nonblock ? " (no bloqueante)" : "");
      if (connect(w->s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) { w->conn = 2; res = 0; return true; }
      const int e = NetErr();
      if (IsWouldBlock(e) || e == PE(EINPROGRESS)) {
        w->conn = 1;
        if (!w->tcp) { w->conn = 2; res = 0; return true; }
        if (w->nonblock) { res = -SO_EINPROGRESS; return true; }
        return false;
      }
      w->conn = 0; res = -MapErr(e, false);
      std::printf("[Red] connect fd=%d error %d\n", op.fd, res);
      return true;
    }
    const int st = ConnState(*w);
    if (st == 2) { res = 0; return true; }
    if (st == 3) { res = -w->conn_err; w->conn = 0; return true; }
    return false;
  }
  case K_ACCEPT: {
    WSock* w = Sock(op.fd);
    if (!w) { res = -SO_EBADF; return true; }
    sockaddr_in from{}; socklen_t fl = sizeof from;
    const port_sock_t ns = accept(w->s, reinterpret_cast<sockaddr*>(&from), &fl);
    if (ns == kBadSock) {
      const int e = NetErr();
      if (IsWouldBlock(e)) { if (w->nonblock) { res = -SO_EAGAIN; return true; } return false; }
      res = -MapErr(e, false); return true;
    }
    const int nfd = AddSock(ns, true);
    if (nfd < 0) { CloseSock(ns); res = -SO_EMFILE; return true; }
    g_socks[nfd].conn = 2;
    WriteAddr(c, r.out_buf, r.out_size, from);
    std::printf("[Red] accept fd=%d -> fd=%d\n", op.fd, nfd);
    res = nfd; return true;
  }
  case K_RECV: {
    // in0 = {fd, flags}, io0 = datos, io1 = direccion de origen
    const uint32_t p = VecAddr(r, false, 0);
    const uint32_t flags = R32(c, p + 4);
    WSock* w = Sock(op.fd);
    if (!w) { res = -SO_EBADF; return true; }
    const bool nb = w->nonblock || (flags & 4);
    if (w->tcp && w->conn == 1 && ConnState(*w) == 1) { if (nb) { res = -SO_EAGAIN; return true; } return false; }
    const uint32_t dst = VecAddr(r, true, 0), len = VecLen(r, true, 0);
    const uint32_t aaddr = VecAddr(r, true, 1), alen = VecLen(r, true, 1);
    std::vector<uint8_t>& buf = Scratch(len);
    sockaddr_in from{}; socklen_t fl = sizeof from;
    const int n = int(recvfrom(w->s, reinterpret_cast<char*>(buf.data()), int(len), int(flags & 3),
                               alen ? reinterpret_cast<sockaddr*>(&from) : nullptr, alen ? &fl : nullptr));
    if (n < 0) {
      const int e = NetErr();
      if (IsWouldBlock(e)) { if (nb) { res = -SO_EAGAIN; return true; } return false; }
      res = -MapErr(e, true);
      std::printf("[Red] recv fd=%d error %d\n", op.fd, res);
      return true;
    }
    if (n > 0) WriteBytes(c, dst, buf.data(), uint32_t(n));
    if (alen) WriteAddr(c, aaddr, alen, from);
    ++g_st.recv; g_st.rxb += uint64_t(n); w->rx += uint64_t(n);
    if (g_cfg.trace || (n > 0 && w->rx == uint64_t(n)))
      std::printf("[Red] recv fd=%d %d bytes: %s\n", op.fd, n, Printable(buf.data(), size_t(n)).c_str());
    if (n == 0 && w->tcp) std::printf("[Red] fd=%d cerrado por el otro extremo\n", op.fd);
    res = n; return true;
  }
  case K_SEND: {
    // in0 = datos, in1 = {fd, flags@4, has_dest@8, sockaddr@0xC}
    const uint32_t data = VecAddr(r, false, 0), len = VecLen(r, false, 0), p = VecAddr(r, false, 1);
    const uint32_t flags = R32(c, p + 4), has_dest = R32(c, p + 8);
    WSock* w = Sock(op.fd);
    if (!w) { res = -SO_EBADF; return true; }
    const bool nb = w->nonblock || (flags & 4);
    if (w->tcp && w->conn == 1 && ConnState(*w) == 1) { if (nb) { res = -SO_EAGAIN; return true; } return false; }
    std::vector<uint8_t>& buf = Scratch(len);
    ReadBytes(c, data, buf.data(), len);
    sockaddr_in to{};
    if (has_dest) { to = ReadAddr(c, p + 0xC); Remap(to); }
    const int n = int(sendto(w->s, reinterpret_cast<const char*>(buf.data()), int(len), int(flags & 1),
                             has_dest ? reinterpret_cast<sockaddr*>(&to) : nullptr, has_dest ? sizeof to : 0));
    if (n < 0) {
      const int e = NetErr();
      if (IsWouldBlock(e)) { if (nb) { res = -SO_EAGAIN; return true; } return false; }
      res = -MapErr(e, true);
      std::printf("[Red] send fd=%d error %d\n", op.fd, res);
      return true;
    }
    ++g_st.send; g_st.txb += uint64_t(n);
    if (g_cfg.trace && w->tcp && n >= 8) {
      // paquetes PAT de Capcom: {tam16, seq16, id32}. Salir de la capa / cerrar sesion.
      const uint32_t id = BE32(buf.data() + 4);
      if (id == 0x64020100u) GuestBacktrace(c, "ReqLayerEnd");
      else if (id == 0x60100100u) GuestBacktrace(c, "ReqShut");
    }
    if (g_cfg.trace && !w->tcp && has_dest && ntohs(to.sin_port) == 27900) GuestBacktrace(c, "disponibilidad");
    if (g_cfg.trace || w->tx == 0)
      std::printf("[Red] send fd=%d%s %d bytes: %s\n", op.fd,
                  has_dest ? (" a " + IpStr(ntohl(to.sin_addr.s_addr)) + ":" + std::to_string(ntohs(to.sin_port))).c_str() : "",
                  n, Printable(buf.data(), size_t(n)).c_str());
    w->tx += uint64_t(n);
    res = n; return true;
  }
  case K_POLL: {
    // in = s64 timeout (ms, negativo = sin limite); out = {fd, events, revents} x N
    const uint32_t n = std::min<uint32_t>(r.out_size / 12, kMaxSock);
    if (!op.started) {
      op.started = true; ++g_st.polls;
      const int64_t to = int64_t((uint64_t(R32(c, r.in_buf)) << 32) | R32(c, r.in_buf + 4));
      if (to >= 0) { op.has_deadline = true; op.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(to); }
    }
    fd_set rf, wf, ef; FD_ZERO(&rf); FD_ZERO(&wf); FD_ZERO(&ef);
    int maxs = 0; bool any = false;
    for (uint32_t i = 0; i < n; ++i) {
      WSock* w = Sock(R32(c, r.out_buf + i * 12));
      if (!w) continue;
      if (w->conn == 1) ConnState(*w);
      FD_SET(w->s, &rf); FD_SET(w->s, &wf); FD_SET(w->s, &ef);
      maxs = std::max(maxs, int(w->s)); any = true;
    }
    if (any) { timeval tv{0, 0}; select(maxs + 1, &rf, &wf, &ef, &tv); }
    int ready = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t e = r.out_buf + i * 12;
      const uint32_t fd = R32(c, e), ev = R32(c, e + 4);
      uint32_t rev = 0;
      WSock* w = Sock(fd);
      if (!w) rev = 0x80;
      else {
        const bool connecting = w->tcp && w->conn == 1;
        if (!connecting && FD_ISSET(w->s, &rf)) rev |= ev & 0x3;
        if (!connecting && FD_ISSET(w->s, &wf)) rev |= ev & 0x18;
        if (FD_ISSET(w->s, &ef) || w->conn == 3) rev |= 0x20;
      }
      W32(c, e + 8, rev);
      if (rev) ++ready;
    }
    if (ready) { res = ready; return true; }
    if (op.has_deadline && std::chrono::steady_clock::now() >= op.deadline) { res = 0; return true; }
    return false;
  }
  case K_SSL_HS: case K_SSL_READ: case K_SSL_WRITE: {
    // Buffers (orden de IOS): in0 = resultado SSL, in1 = datos de lectura, io0 = id SSL, io1 = datos a escribir
    const uint32_t out_res = VecAddr(r, false, 0);
    res = IPC_OK;
    SslSlot* sl = Ssl(R32(c, VecAddr(r, true, 0)));
    WSock* w = sl ? Sock(uint32_t(sl->fd)) : nullptr;
    if (!sl) { W32(c, out_res, uint32_t(SSL_ID)); return true; }
    if (!w) { W32(c, out_res, uint32_t(SSL_SYSCALL)); return true; }
    const int st = w->tcp ? ConnState(*w) : 2;
    if (st == 1) { W32(c, out_res, uint32_t(SSL_RAGAIN)); return w->nonblock; }
    if (st != 2) { W32(c, out_res, uint32_t(SSL_SYSCALL)); return true; }
    if (op.kind == K_SSL_HS) {
      std::printf("[Red] SSL %s: conexion lista (sin cifrado)\n", sl->host.c_str());
      W32(c, out_res, SSL_OK); return true;
    }
    if (op.kind == K_SSL_WRITE) {
      const uint32_t data = VecAddr(r, true, 1), len = VecLen(r, true, 1);
      std::vector<uint8_t>& buf = Scratch(len);
      ReadBytes(c, data, buf.data(), len);
      const int n = int(send(w->s, reinterpret_cast<const char*>(buf.data()), int(len), 0));
      if (n < 0) {
        const int e = NetErr();
        if (IsWouldBlock(e)) { W32(c, out_res, uint32_t(SSL_WAGAIN)); return w->nonblock; }
        std::printf("[Red] SSL %s: error al escribir (%d)\n", sl->host.c_str(), MapErr(e, true));
        W32(c, out_res, uint32_t(SSL_FAILED)); return true;
      }
      ++g_st.send; g_st.txb += uint64_t(n);
      if (g_cfg.trace || w->tx == 0)
        std::printf("[Red] SSL %s -> %d bytes: %s\n", sl->host.c_str(), n, Printable(buf.data(), size_t(n), 240).c_str());
      w->tx += uint64_t(n);
      W32(c, out_res, n ? uint32_t(n) : uint32_t(SSL_ZERO)); return true;
    }
    const uint32_t dst = VecAddr(r, false, 1), len = VecLen(r, false, 1);
    std::vector<uint8_t>& buf = Scratch(len);
    const int n = int(recv(w->s, reinterpret_cast<char*>(buf.data()), int(len), 0));
    if (n < 0) {
      const int e = NetErr();
      if (IsWouldBlock(e)) { W32(c, out_res, uint32_t(SSL_RAGAIN)); return w->nonblock; }
      std::printf("[Red] SSL %s: error al leer (%d)\n", sl->host.c_str(), MapErr(e, true));
      W32(c, out_res, uint32_t(SSL_FAILED)); return true;
    }
    if (n > 0) WriteBytes(c, dst, buf.data(), uint32_t(n));
    ++g_st.recv; g_st.rxb += uint64_t(n);
    if (g_cfg.trace || (n > 0 && w->rx == 0))
      std::printf("[Red] SSL %s <- %d bytes: %s\n", sl->host.c_str(), n, Printable(buf.data(), size_t(n), 240).c_str());
    if (n == 0) std::printf("[Red] SSL %s: cerrado por el servidor\n", sl->host.c_str());
    w->rx += uint64_t(n);
    W32(c, out_res, n ? uint32_t(n) : uint32_t(SSL_ZERO)); return true;
  }
  }
  res = -SO_EINVAL; return true;
}

int32_t Run(CPUContext& c, const IpcRequest& r, Kind k, int fd) {
  Op op; op.r = r; op.kind = k; op.fd = fd;
  int32_t res = 0;
  if (TryOp(c, op, res)) return res;
  if (r.req_addr == 0) return k >= K_SSL_HS ? IPC_OK : -SO_EAGAIN;
  g_ops.push_back(std::move(op));
  g_port_net_pending = int(g_ops.size());
  return IPC_NO_REPLY;
}

// Cierra un socket: lo que esperaba en el responde "no conectado".
void AbortOpsOn(CPUContext& c, int fd) {
  for (size_t i = 0; i < g_ops.size();) {
    Op& op = g_ops[i];
    int ofd = op.fd;
    if (op.kind >= K_SSL_HS) { SslSlot* sl = Ssl(R32(c, VecAddr(op.r, true, 0))); ofd = sl ? sl->fd : -1; }
    if (ofd == fd && op.kind != K_POLL) {
      int32_t res = -SO_ENOTCONN;
      if (op.kind >= K_SSL_HS) { W32(c, VecAddr(op.r, false, 0), uint32_t(SSL_SYSCALL)); res = IPC_OK; }
      c.mmu.write32(op.r.req_addr + 4, uint32_t(res));
      nwii::runtime::hw::ipc_post_reply(op.r.req_addr);
      g_ops.erase(g_ops.begin() + std::ptrdiff_t(i));
    } else ++i;
  }
  g_port_net_pending = int(g_ops.size());
}

void CloseAll(CPUContext& c) {
  for (int i = 0; i < kMaxSock; ++i)
    if (g_socks[i].used) { AbortOpsOn(c, i); CloseSock(g_socks[i].s); g_socks[i] = WSock{}; }
  for (auto& s : g_ssl) s = SslSlot{};
}

// ================================================================ /dev/net/ip/top
class IpTop : public IDevice {
public:
  const char* get_name() const override { return "/dev/net/ip/top"; }
  int32_t open(CPUContext&, const std::string&, uint32_t) override { LoadConfig(); return 0; }

  int32_t ioctl(CPUContext& c, const IpcRequest& r) override {
    LoadConfig();
    switch (r.ioctl_cmd) {
    case 0x0F: {   // SOCKET
      const uint32_t af = R32(c, r.in_buf), type = R32(c, r.in_buf + 4), prot = R32(c, r.in_buf + 8);
      if (af != 2) return -SO_EAFNOSUPPORT;
      if (type != 1 && type != 2) return -SO_EPROTONOSUPPORT;
      const port_sock_t s = socket(AF_INET, type == 1 ? SOCK_STREAM : SOCK_DGRAM, type == 1 ? IPPROTO_TCP : IPPROTO_UDP);
      if (s == kBadSock) return -MapErr(NetErr(), false);
      const int fd = AddSock(s, type == 1);
      if (fd < 0) { CloseSock(s); return -SO_EMFILE; }
      std::printf("[Red] socket %s -> fd=%d\n", type == 1 ? "TCP" : "UDP", fd);
      (void)prot;
      return fd;
    }
    case 0x30: return -SO_EPROTONOSUPPORT;   // ICMPSOCKET (ping): no
    case 0x03: case 0x33: {   // CLOSE / ICMPCLOSE
      const uint32_t fd = R32(c, r.in_buf);
      WSock* w = Sock(fd);
      if (!w) return -SO_EBADF;
      AbortOpsOn(c, int(fd));
      for (auto& sl : g_ssl) if (sl.used && sl.fd == int(fd)) sl.fd = -1;
      std::printf("[Red] close fd=%u (enviados %llu, recibidos %llu bytes)\n", fd,
                  (unsigned long long)w->tx, (unsigned long long)w->rx);
      CloseSock(w->s); *w = WSock{};
      return 0;
    }
    case 0x04: return Run(c, r, K_CONNECT, int(R32(c, r.in_buf)));   // CONNECT
    case 0x01: return Run(c, r, K_ACCEPT, int(R32(c, r.in_buf)));    // ACCEPT
    case 0x02: {   // BIND
      WSock* w = Sock(R32(c, r.in_buf));
      if (!w) return -SO_EBADF;
      sockaddr_in a = ReadAddr(c, r.in_buf + 8);
      int one = 1; setsockopt(w->s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
      const int rc = bind(w->s, reinterpret_cast<sockaddr*>(&a), sizeof a);
      std::printf("[Red] bind fd=%u puerto %u -> %d\n", R32(c, r.in_buf), unsigned(ntohs(a.sin_port)), rc);
      return rc == 0 ? 0 : -MapErr(NetErr(), false);
    }
    case 0x0A: {   // LISTEN
      WSock* w = Sock(R32(c, r.in_buf));
      if (!w) return -SO_EBADF;
      return listen(w->s, int(R32(c, r.in_buf + 4))) == 0 ? 0 : -MapErr(NetErr(), false);
    }
    case 0x0E: {   // SHUTDOWN
      WSock* w = Sock(R32(c, r.in_buf));
      if (!w) return -SO_EBADF;
      shutdown(w->s, int(R32(c, r.in_buf + 4)));
      return 0;
    }
    case 0x05: {   // FCNTL
      WSock* w = Sock(R32(c, r.in_buf));
      if (!w) return -SO_EBADF;
      const uint32_t cmd = R32(c, r.in_buf + 4), arg = R32(c, r.in_buf + 8);
      if (cmd == 3) return w->nonblock ? 4 : 0;
      if (cmd == 4) { w->nonblock = (arg & 4) != 0; return 0; }
      return 0;
    }
    case 0x06: case 0x07: {   // GETPEERNAME / GETSOCKNAME
      WSock* w = Sock(R32(c, r.in_buf));
      if (!w) return -SO_EBADF;
      sockaddr_in a{}; socklen_t al = sizeof a;
      const int rc = r.ioctl_cmd == 6 ? getpeername(w->s, reinterpret_cast<sockaddr*>(&a), &al)
                                      : getsockname(w->s, reinterpret_cast<sockaddr*>(&a), &al);
      if (r.ioctl_cmd == 7 && a.sin_addr.s_addr == 0) a.sin_addr.s_addr = htonl(g_cfg.local_ip);
      WriteAddr(c, r.out_buf, r.out_size, a);
      return rc == 0 ? 0 : -MapErr(NetErr(), false);
    }
    case 0x08: {   // GETSOCKOPT: out = {fd, level, optname, optlen, optval...}
      WSock* w = Sock(R32(c, r.out_buf));
      const uint32_t opt = R32(c, r.out_buf + 8);
      uint32_t val = 0;
      if (w && opt == 0x1009) {   // SO_ERROR
        if (w->conn == 1) ConnState(*w);
        val = w->conn == 3 ? uint32_t(w->conn_err) : 0;
      } else if (w && opt == 0x1008) val = w->tcp ? 1 : 2;   // SO_TYPE
      else if (opt == 0x1001 || opt == 0x1002) val = 0x8000;   // SNDBUF / RCVBUF
      W32(c, r.out_buf + 0xC, 4); W32(c, r.out_buf + 0x10, val);
      return w ? 0 : -SO_EBADF;
    }
    case 0x09: return Sock(R32(c, r.in_buf)) ? 0 : -SO_EBADF;   // SETSOCKOPT: se acepta sin mas
    case 0x0B: return Run(c, r, K_POLL, -1);   // POLL
    case 0x10: return int32_t(g_cfg.local_ip);   // GETHOSTID
    case 0x11: {   // GETHOSTBYNAME
      const std::string name = ReadStr(c, r.in_buf, std::max<uint32_t>(r.in_size, 1));
      const uint32_t ip = Resolve(name);
      const uint32_t o = r.out_buf;
      if (r.out_size < 0x460) return -1;
      for (uint32_t i = 0; i < 0x460; i += 4) W32(c, o + i, 0);
      const std::string nm = name.substr(0, 0xFF);
      WriteBytes(c, o + 0x10, nm.c_str(), uint32_t(nm.size() + 1));
      W32(c, o + 0x0, o + 0x10);        // h_name
      W16(c, o + 0x8, 2); W16(c, o + 0xA, 4);   // AF_INET, 4 bytes
      W32(c, o + 0xC, o + 0x340);       // h_addr_list
      W32(c, o + 0x110, ip);            // la direccion (orden de red = big endian)
      W32(c, o + 0x340, o + 0x110);
      W32(c, o + 0x344, 0);
      W32(c, o + 0x4, o + 0x344);       // h_aliases (vacia)
      return 0;
    }
    case 0x15: {   // INETATON
      const std::string name = ReadStr(c, r.in_buf, std::max<uint32_t>(r.in_size, 1));
      W32(c, r.out_buf, Resolve(name));
      return 1;
    }
    case 0x16: {   // INETPTON
      uint32_t ip;
      if (!ParseIPv4(ReadStr(c, r.in_buf, std::max<uint32_t>(r.in_size, 1)), ip)) return 0;
      W32(c, r.out_buf + 4, ip);
      return 1;
    }
    case 0x17: {   // INETNTOP
      char b[16]; std::snprintf(b, sizeof b, "%u.%u.%u.%u", R8(c, r.in_buf + 8), R8(c, r.in_buf + 9), R8(c, r.in_buf + 10), R8(c, r.in_buf + 11));
      WriteBytes(c, r.out_buf, b, uint32_t(std::strlen(b) + 1));
      return 0;
    }
    case 0x1F: case 0x1E: return 0;   // INITINTERFACE / SETINTERFACE
    case 0x32: return 0;              // ICMPCANCEL
    default:
      std::printf("[Red] ip/top ioctl 0x%X sin atender\n", r.ioctl_cmd);
      return 0;
    }
  }

  int32_t ioctlv(CPUContext& c, const IpcRequest& r) override {
    LoadConfig();
    switch (r.ioctl_cmd) {
    case 0x0C: return Run(c, r, K_RECV, int(R32(c, VecAddr(r, false, 0))));   // RECVFROM
    case 0x0D: return Run(c, r, K_SEND, int(R32(c, VecAddr(r, false, 1))));   // SENDTO
    case 0x1C: {   // GETINTERFACEOPT: in0 = {0xfffe, opcion}, io0 = valor, io1 = longitud
      const uint32_t in0 = VecAddr(r, false, 0), o = VecAddr(r, true, 0), ol = VecAddr(r, true, 1);
      const uint32_t param = R32(c, in0), opt = R32(c, in0 + 4);
      if (param != 0xFFFE) return -0x70;
      switch (opt) {
      case 0xB003: W32(c, o, g_cfg.server_ip); W32(c, o + 4, g_cfg.server_ip); break;   // DNS
      case 0x1003: W32(c, o, 0); break;           // error
      case 0x1004: WriteBytes(c, o, kMac, 6); break;   // MAC
      case 0x1005: W32(c, o, 1); break;           // enlace activo
      case 0x3001: W32(c, o, 0x10); break;
      case 0x4002: W32(c, o, 1); break;
      case 0x4003:                                // IP, mascara, difusion
        if (ol) W32(c, ol, 0xC);
        W32(c, o, g_cfg.local_ip); W32(c, o + 4, 0xFFFFFF00u); W32(c, o + 8, g_cfg.local_ip | 0xFFu);
        break;
      case 0x4005: W32(c, o, 0x20); break;
      case 0x4006:                                // tabla de rutas: una, por defecto
        W32(c, o, 0); W32(c, o + 4, 0); W32(c, o + 8, (g_cfg.local_ip & 0xFFFFFF00u) | 1u); W32(c, o + 12, 1);
        W32(c, o + 16, 0); W32(c, o + 20, 0);
        if (ol) W32(c, ol, 24);
        break;
      case 0x6003: case 0x600A: case 0x600C: W32(c, o, 0x80); break;
      case 0xB002: W32(c, o, 2); break;
      default: std::printf("[Red] GETINTERFACEOPT 0x%X sin atender\n", opt); break;
      }
      return 0;
    }
    case 0x18: {   // GETADDRINFO: in0 = nombre, in1 = servicio, in2 = pistas; io0 = resultado
      const std::string node = ReadStr(c, VecAddr(r, false, 0), std::max<uint32_t>(VecLen(r, false, 0), 1));
      const std::string serv = ReadStr(c, VecAddr(r, false, 1), std::max<uint32_t>(VecLen(r, false, 1), 1));
      uint32_t socktype = 1;
      if (VecLen(r, false, 2) >= 12) { const uint32_t t = R32(c, VecAddr(r, false, 2) + 8); if (t) socktype = t; }
      const uint32_t ip = node.empty() ? g_cfg.local_ip : Resolve(node);
      const uint32_t o = VecAddr(r, true, 0);
      const uint32_t sa = o + 0x460;
      W32(c, o + 0x00, 0); W32(c, o + 0x04, 2); W32(c, o + 0x08, socktype); W32(c, o + 0x0C, 0);
      W32(c, o + 0x10, 8); W32(c, o + 0x14, 0); W32(c, o + 0x18, sa); W32(c, o + 0x1C, 0);
      W8(c, sa, 8); W8(c, sa + 1, 2); W16(c, sa + 2, uint16_t(std::atoi(serv.c_str()))); W32(c, sa + 4, ip);
      return 0;
    }
    case 0x1A: case 0x1B: case 0x1D: return 0;   // STARTUP / CLEANUP / SETINTERFACEOPT
    case 0x31: return -SO_EPROTONOSUPPORT;       // ICMPPING
    default:
      std::printf("[Red] ip/top ioctlv 0x%X sin atender\n", r.ioctl_cmd);
      return 0;
    }
  }
};

// ================================================================ /dev/net/ssl
class SslDev : public IDevice {
public:
  const char* get_name() const override { return "/dev/net/ssl"; }
  int32_t open(CPUContext&, const std::string&, uint32_t) override { LoadConfig(); return 0; }
  int32_t ioctlv(CPUContext& c, const IpcRequest& r) override {
    LoadConfig();
    const uint32_t out_res = VecAddr(r, false, 0);
    switch (r.ioctl_cmd) {
    case 0x01: {   // NEW: io0 = verificacion, io1 = nombre del servidor
      const std::string host = ReadStr(c, VecAddr(r, true, 1), std::max<uint32_t>(VecLen(r, true, 1), 1));
      for (int i = 0; i < 4; ++i)
        if (!g_ssl[i].used) {
          g_ssl[i] = SslSlot{}; g_ssl[i].used = true; g_ssl[i].host = host;
          std::printf("[Red] SSL nuevo #%d para %s\n", i + 1, host.c_str());
          W32(c, out_res, uint32_t(i + 1));
          return IPC_OK;
        }
      W32(c, out_res, uint32_t(SSL_FAILED));
      return IPC_OK;
    }
    case 0x02: {   // CONNECT: io0 = id, io1 = socket
      SslSlot* sl = Ssl(R32(c, VecAddr(r, true, 0)));
      if (!sl) { W32(c, out_res, uint32_t(SSL_ID)); return IPC_OK; }
      sl->fd = int(R32(c, VecAddr(r, true, 1)));
      W32(c, out_res, SSL_OK);
      return IPC_OK;
    }
    case 0x03: case 0x0C: return Run(c, r, K_SSL_HS, -1);   // DOHANDSHAKE (EX)
    case 0x04: return Run(c, r, K_SSL_READ, -1);
    case 0x05: return Run(c, r, K_SSL_WRITE, -1);
    case 0x06: {   // SHUTDOWN
      SslSlot* sl = Ssl(R32(c, VecAddr(r, true, 0)));
      if (!sl) { W32(c, out_res, uint32_t(SSL_ID)); return IPC_OK; }
      std::printf("[Red] SSL cierre %s\n", sl->host.c_str());
      *sl = SslSlot{};
      W32(c, out_res, SSL_OK);
      return IPC_OK;
    }
    default:   // certificados (SETROOTCA, SETCLIENTCERT, BUILTIN..., DISABLEVERIFY...): sin cifrado, todo vale
      if (out_res) W32(c, out_res, SSL_OK);
      return IPC_OK;
    }
  }
};

// ================================================================ WiiConnect24 / configuracion de red
uint64_t WiiTimeNow() {   // segundos desde el 1 de enero de 2000 (UTC)
  return uint64_t(std::time(nullptr)) - 946684800ull;
}

class KdRequest : public IDevice {
public:
  const char* get_name() const override { return "/dev/net/kd/request"; }
  int32_t ioctl(CPUContext& c, const IpcRequest& r) override {
    const uint32_t o = r.out_buf;
    switch (r.ioctl_cmd) {
    case 0x06:   // STARTUP_SOCKET
      if (r.out_size >= 8) { W32(c, o, 0); W32(c, o + 4, 0); }
      return 0;
    case 0x07: case 0x28:   // CLEANUP_SOCKET / REQUEST_SHUTDOWN
      CloseAll(c);
      if (r.out_size >= 4) W32(c, o, 0);
      return 0;
    case 0x0F:   // REQUEST_GENERATED_USER_ID: id ya generado
      if (r.out_size >= 0x10) {
        W32(c, o, 0); W32(c, o + 4, 0x00000F42); W32(c, o + 8, 0x4D483331u); W32(c, o + 0xC, 1);
      }
      return 0;
    case 0x04:   // KD_GET_TIME_TRIGGERS
      if (r.out_size >= 12) { W32(c, o, 0); W32(c, o + 4, 0); W32(c, o + 8, 0); }
      return 0;
    default:
      if (r.out_size >= 4) W32(c, o, 0);
      std::printf("[Red] kd/request ioctl 0x%X\n", r.ioctl_cmd);
      return 0;
    }
  }
};

class KdTime : public IDevice {
  uint32_t m_rtc = 0;
public:
  const char* get_name() const override { return "/dev/net/kd/time"; }
  int32_t ioctl(CPUContext& c, const IpcRequest& r) override {
    const uint32_t o = r.out_buf;
    int32_t result = 0;
    switch (r.ioctl_cmd) {
    case 0x14: { const uint64_t t = WiiTimeNow(); W32(c, o + 4, uint32_t(t >> 32)); W32(c, o + 8, uint32_t(t)); break; }
    case 0x15: break;
    case 0x17: m_rtc = R32(c, r.in_buf); break;
    case 0x18: { const uint64_t t = WiiTimeNow() - m_rtc; W32(c, o + 4, uint32_t(t >> 32)); W32(c, o + 8, uint32_t(t)); break; }
    case 0x16: result = -9; break;
    default: std::printf("[Red] kd/time ioctl 0x%X\n", r.ioctl_cmd); break;
    }
    if (o && r.out_size >= 4) W32(c, o, uint32_t(result));
    return 0;
  }
};

class NcdManage : public IDevice {
  uint32_t m_lock = 0;
public:
  const char* get_name() const override { return "/dev/net/ncd/manage"; }
  int32_t ioctlv(CPUContext& c, const IpcRequest& r) override {
    uint32_t res = 0, vec = 0;
    switch (r.ioctl_cmd) {
    case 0x1:   // LOCKWIRELESSDRIVER
      if (m_lock) res = uint32_t(-4); else { m_lock = r.fd + 1; W32(c, VecAddr(r, true, 0) + 4, m_lock); }
      break;
    case 0x2:   // UNLOCKWIRELESSDRIVER
      if (R32(c, VecAddr(r, false, 0)) == m_lock) m_lock = 0; else res = uint32_t(-3);
      break;
    case 0x3: case 0x5: {   // GETCONFIG / READCONFIG: cable, DHCP, prueba de conexion correcta
      const uint32_t o = VecAddr(r, true, 0), n = std::min<uint32_t>(VecLen(r, true, 0), 0x1B5C);
      for (uint32_t i = 0; i < n; ++i) W8(c, o + i, 0);
      if (n > 8) { W8(c, o + 4, 2); W8(c, o + 8, 0xA7); }
      if (r.ioctl_cmd == 0x3) vec = 1;
      std::printf("[Red] ncd: configuracion de red (por cable)\n");
      break;
    }
    case 0x4: case 0x6: break;   // SETCONFIG / WRITECONFIG
    case 0x7: W32(c, VecAddr(r, true, 0) + 4, 3); break;   // GETLINKSTATUS: cable
    case 0x8: WriteBytes(c, VecAddr(r, true, 1), kMac, 6); break;
    default: std::printf("[Red] ncd ioctlv 0x%X\n", r.ioctl_cmd); break;
    }
    const uint32_t ra = VecAddr(r, true, vec);
    if (ra) { W32(c, ra, res); if (vec == 1) W32(c, ra + 4, res); }
    return 0;
  }
};

class WdCommand : public IDevice {
public:
  const char* get_name() const override { return "/dev/net/wd/command"; }
  int32_t ioctlv(CPUContext&, const IpcRequest& r) override {
    std::printf("[Red] wd/command ioctlv 0x%X\n", r.ioctl_cmd);
    return 0;
  }
};

}  // namespace

// Lo llama process_pending_callbacks (hilo del juego) mientras haya operaciones esperando.
// force: el juego esta ocioso (bucle de espera del SO), probablemente esperando a la red.
void PortNetPoll(CPUContext& c, bool force) {
  static uint32_t tick = 0;
  if (!force && (++tick & 255) != 0) return;
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::microseconds(500)) return;
  last = now;
  for (size_t i = 0; i < g_ops.size();) {
    int32_t res = 0;
    if (TryOp(c, g_ops[i], res)) {
      const uint32_t req = g_ops[i].r.req_addr;
      g_ops.erase(g_ops.begin() + std::ptrdiff_t(i));
      c.mmu.write32(req + 4, uint32_t(res));
      nwii::runtime::hw::ipc_post_reply(req);
    } else ++i;
  }
  g_port_net_pending = int(g_ops.size());
  static auto report = now;
  if (now - report >= std::chrono::seconds(10) && (g_st.recv | g_st.send)) {
    report = now;
    std::printf("[Red] trafico: %llu envios (%llu bytes), %llu recepciones (%llu bytes), %llu poll, %zu esperando\n",
                (unsigned long long)g_st.send, (unsigned long long)g_st.txb, (unsigned long long)g_st.recv,
                (unsigned long long)g_st.rxb, (unsigned long long)g_st.polls, g_ops.size());
  }
}

// Hay alguna conexion TCP abierta (el menu del port no pausa el juego mientras dure).
bool PortNetOnline() {
  for (const auto& w : g_socks) if (w.used && w.tcp && w.conn == 2) return true;
  return false;
}

void PortNetRegister() {
  auto& k = nwii::runtime::IOSKernel::get();
  k.register_device(std::make_unique<IpTop>());
  k.register_device(std::make_unique<SslDev>());
  k.register_device(std::make_unique<KdRequest>());
  k.register_device(std::make_unique<KdTime>());
  k.register_device(std::make_unique<NcdManage>());
  k.register_device(std::make_unique<WdCommand>());
}
