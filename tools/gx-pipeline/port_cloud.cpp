// tools/gx-pipeline: guardado en la nube (Dropbox). Ver port_cloud.h.
//
// Que se sube (carpeta de la aplicacion en el Dropbox del usuario, Aplicaciones/<nombre>):
//   partida.mhtnube   archivos de la NAND del titulo (/title/00010000/<id>/...) juntos
//   ranuraN.sav       ranuras del guardado rapido
// La app de Dropbox la crea el usuario con acceso "App folder": el juego solo ve esa carpeta.
// Permiso con OAuth 2 + PKCE (sin secreto), redireccion fija a http://localhost:53682/.
//
// Sincronizacion: en nube_estado.ini se recuerda el hash de cada archivo la ultima vez
// que local y nube coincidieron (content_hash de Dropbox). Al arrancar el juego:
//   - solo cambio la nube  -> se baja (copia de lo local en nube_respaldos)
//   - solo cambio lo local -> se sube
//   - cambiaron los dos    -> gana el mas reciente; el perdedor queda en nube_respaldos
//     (si pierde la nube, Dropbox conserva la version anterior en su historial)
// Durante el juego solo se sube (la partida en memoria es la buena); "Descargar" deja la
// partida preparada para el siguiente arranque y las ranuras quedan listas al momento.
#include "port_cloud.h"
#include "runtime/config.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
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
#include <winhttp.h>
#include <wincrypt.h>
#include <shellapi.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace fs = std::filesystem;

namespace port_cloud {
namespace detail {

// ------------------------------------------------------------------ utilidades
static std::string Hex(const uint8_t* p, size_t n) {
  static const char* h = "0123456789abcdef";
  std::string s; s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) { s += h[p[i] >> 4]; s += h[p[i] & 15]; }
  return s;
}

// SHA-256 (desafio PKCE de OAuth y hash de contenido de Dropbox).
std::string Sha256Raw(const void* data, size_t len) {
  static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  const uint8_t* src = static_cast<const uint8_t*>(data);
  const uint64_t bits = uint64_t(len) * 8;
  const size_t full = len / 64, rem = len % 64;
  uint8_t tail[128] = {0};
  if (rem) std::memcpy(tail, src + full * 64, rem);
  tail[rem] = 0x80;
  const size_t tail_blocks = rem < 56 ? 1 : 2;
  for (int i = 0; i < 8; ++i) tail[tail_blocks * 64 - 1 - i] = uint8_t(bits >> (8 * i));
  auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  for (size_t blk = 0; blk < full + tail_blocks; ++blk) {
    const uint8_t* m = blk < full ? src + blk * 64 : tail + (blk - full) * 64;
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = uint32_t(m[i * 4]) << 24 | uint32_t(m[i * 4 + 1]) << 16 | uint32_t(m[i * 4 + 2]) << 8 | uint32_t(m[i * 4 + 3]);
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g);
      const uint32_t t1 = hh + S1 + ch + k[i] + w[i];
      const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = S0 + mj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  std::string out(32, '\0');
  for (int i = 0; i < 8; ++i) for (int j = 0; j < 4; ++j) out[i * 4 + j] = char(uint8_t(h[i] >> (24 - 8 * j)));
  return out;
}
std::string Sha256Raw(const std::string& s) { return Sha256Raw(s.data(), s.size()); }

// content_hash de Dropbox: SHA-256 de la concatenacion de los SHA-256 de cada bloque de 4 MiB.
std::string ContentHash(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  std::string cat;
  for (size_t off = 0; off < len; off += 4u << 20) cat += Sha256Raw(p + off, (std::min)(size_t(4u << 20), len - off));
  const std::string h = Sha256Raw(cat);
  return Hex(reinterpret_cast<const uint8_t*>(h.data()), h.size());
}

std::string Base64Url(const std::string& in) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string o; size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8 | uint8_t(in[i + 2]);
    o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
  }
  if (in.size() - i == 1) { const uint32_t v = uint32_t(uint8_t(in[i])) << 16; o += t[v >> 18]; o += t[(v >> 12) & 63]; }
  else if (in.size() - i == 2) { const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8; o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; }
  return o;
}
std::string Base64UrlDecode(const std::string& in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '-' || c == '+') return 62; if (c == '_' || c == '/') return 63; return -1; };
  std::string o; uint32_t acc = 0; int n = 0;
  for (char c : in) { const int v = val(c); if (v < 0) continue; acc = (acc << 6) | uint32_t(v); n += 6; if (n >= 8) { n -= 8; o += char(uint8_t(acc >> n)); } }
  return o;
}

std::string UrlEncode(const std::string& s) {
  static const char* h = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
    else { o += '%'; o += h[c >> 4]; o += h[c & 15]; }
  }
  return o;
}
std::string UrlDecode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') o += ' ';
    else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
      o += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; }
    else o += s[i];
  }
  return o;
}
std::map<std::string, std::string> ParseQuery(const std::string& q) {
  std::map<std::string, std::string> m; size_t i = 0;
  while (i <= q.size()) {
    size_t amp = q.find('&', i); if (amp == std::string::npos) amp = q.size();
    const std::string kv = q.substr(i, amp - i); const size_t eq = kv.find('=');
    if (!kv.empty()) m[UrlDecode(kv.substr(0, eq))] = eq == std::string::npos ? "" : UrlDecode(kv.substr(eq + 1));
    i = amp + 1;
  }
  return m;
}
std::string JsonEscape(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += char(c);
  }
  return o + "\"";
}

// ------------------------------------------------------------------ JSON minimo
struct Json {
  enum Type { kNull, kBool, kNum, kStr, kArr, kObj } t = kNull;
  bool b = false; double n = 0; std::string s;
  std::vector<Json> a;                 // arreglo, o valores del objeto
  std::vector<std::string> keys;       // claves del objeto (mismo orden que a)
  const Json& operator[](const std::string& k) const {
    static const Json none;
    if (t == kObj) for (size_t i = 0; i < keys.size(); ++i) if (keys[i] == k) return a[i];
    return none;
  }
  std::string str() const { return t == kStr ? s : std::string(); }
};
struct JsonParser {
  const char* p; const char* e; bool ok = true;
  void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
  static void Utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
    else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 63)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
  }
  uint32_t hex4() {
    if (e - p < 4) { ok = false; return 0; }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = *p++; v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0'); else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10); else ok = false;
    }
    return v;
  }
  std::string str() {
    std::string o; ++p;
    while (p < e && *p != '"') {
      if (*p != '\\') { o += *p++; continue; }
      if (++p >= e) break;
      const char c = *p++;
      switch (c) {
        case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': o += '\r'; break;
        case 'b': o += '\b'; break; case 'f': o += '\f'; break;
        case 'u': {
          uint32_t cp = hex4();
          if (cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') { p += 2; const uint32_t lo = hex4(); cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); }
          Utf8(o, cp); break;
        }
        default: o += c;
      }
    }
    if (p < e) ++p; else ok = false;
    return o;
  }
  Json value(int depth = 0) {
    Json j; ws();
    if (p >= e || depth > 64) { ok = false; return j; }
    if (*p == '{') {
      j.t = Json::kObj; ++p; ws();
      if (p < e && *p == '}') { ++p; return j; }
      while (ok && p < e) {
        ws(); if (p >= e || *p != '"') { ok = false; break; }
        std::string k = str(); ws();
        if (p >= e || *p != ':') { ok = false; break; }
        ++p; j.keys.push_back(k); j.a.push_back(value(depth + 1)); ws();
        if (p < e && *p == ',') { ++p; continue; }
        if (p < e && *p == '}') { ++p; break; }
        ok = false;
      }
    } else if (*p == '[') {
      j.t = Json::kArr; ++p; ws();
      if (p < e && *p == ']') { ++p; return j; }
      while (ok && p < e) {
        j.a.push_back(value(depth + 1)); ws();
        if (p < e && *p == ',') { ++p; continue; }
        if (p < e && *p == ']') { ++p; break; }
        ok = false;
      }
    } else if (*p == '"') { j.t = Json::kStr; j.s = str(); }
    else if (e - p >= 4 && !std::strncmp(p, "true", 4)) { j.t = Json::kBool; j.b = true; p += 4; }
    else if (e - p >= 5 && !std::strncmp(p, "false", 5)) { j.t = Json::kBool; p += 5; }
    else if (e - p >= 4 && !std::strncmp(p, "null", 4)) { p += 4; }
    else {
      const char* st = p;
      while (p < e && (std::isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) ++p;
      if (p == st) { ok = false; return j; }
      j.t = Json::kNum; j.n = std::strtod(std::string(st, p).c_str(), nullptr);
    }
    return j;
  }
};
Json ParseJson(const std::string& s, bool* ok = nullptr) {
  JsonParser jp{s.data(), s.data() + s.size()};
  Json j = jp.value();
  if (ok) *ok = jp.ok;
  return j;
}

// ------------------------------------------------------------------ archivos locales
static bool ReadFile(const fs::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end); const auto n = f.tellg(); f.seekg(0);
  if (n < 0) return false;
  out.resize(size_t(n));
  if (n > 0) f.read(&out[0], n);
  return bool(f);
}
static bool WriteFileAtomic(const fs::path& p, const std::string& data) {
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  fs::path tmp = p; tmp += ".tmp";
  { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return false; f.write(data.data(), std::streamsize(data.size())); if (!f) return false; }
  fs::rename(tmp, p, ec);
  if (ec) { fs::remove(p, ec); ec.clear(); fs::rename(tmp, p, ec); }
  return !ec;
}
// Fecha UTC en el formato ISO de la nube (sin fracciones): 2026-10-11T13:05:00
static std::string IsoUtc(std::time_t t) {
  std::tm g{};
#ifdef _WIN32
  gmtime_s(&g, &t);
#else
  gmtime_r(&t, &g);
#endif
  char b[32]; std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S", &g);
  return b;
}
static std::string FileTimeIso(const fs::path& p) {
  std::error_code ec; const auto ft = fs::last_write_time(p, ec);
  if (ec) return "";
  const auto sc = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  return IsoUtc(std::chrono::system_clock::to_time_t(sc));
}
static std::string LocalStamp() {
  const std::time_t t = std::time(nullptr); std::tm l{};
#ifdef _WIN32
  localtime_s(&l, &t);
#else
  localtime_r(&t, &l);
#endif
  char b[32]; std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &l);
  return b;
}

static fs::path NandDir() {
  if (const char* d = std::getenv("NWII_NAND_DIR")) return fs::path(d);
  const auto& gd = nwii::runtime::Config::get().game_dir;
  if (gd.empty()) return fs::path("nand");
  return fs::path(gd).parent_path().parent_path() / "nand";
}
// Prefijo (codificado en hexadecimal como los nombres .vnand) de la carpeta del titulo.
static std::string TitlePrefixHex() {
  std::string id = nwii::runtime::Config::get().game_id;
  if (id.size() < 4) id += "    ";
  char path[64];
  std::snprintf(path, sizeof(path), "/title/00010000/%02x%02x%02x%02x/", (unsigned char)id[0], (unsigned char)id[1], (unsigned char)id[2], (unsigned char)id[3]);
  return Hex(reinterpret_cast<const uint8_t*>(path), std::strlen(path));
}
static std::vector<fs::path> TitleFiles() {
  std::vector<fs::path> v; std::error_code ec;
  const std::string pre = TitlePrefixHex();
  for (const auto& e : fs::directory_iterator(NandDir(), ec)) {
    if (e.path().extension() != ".vnand") continue;
    const std::string stem = e.path().stem().string();
    if (stem.compare(0, pre.size(), pre) == 0) v.push_back(e.path());
  }
  std::sort(v.begin(), v.end());
  return v;
}

// Paquete de la partida: "MHTN", version, numero de archivos y por cada uno
// nombre + tamano + contenido (en orden, para que el MD5 sea estable).
static bool BuildBundle(std::string& out, std::string& newest) {
  out.assign("MHTN", 4);
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out += char(uint8_t(v >> (8 * i))); };
  const auto files = TitleFiles();
  u32(1); u32(uint32_t(files.size()));
  newest.clear();
  for (const auto& p : files) {
    std::string data;
    if (!ReadFile(p, data)) return false;
    const std::string name = p.filename().string();
    out += char(uint8_t(name.size())); out += char(uint8_t(name.size() >> 8));
    out += name; u32(uint32_t(data.size())); out += data;
    newest = (std::max)(newest, FileTimeIso(p));
  }
  return !files.empty();
}
static bool ParseBundle(const std::string& b, std::vector<std::pair<std::string, std::string>>& files) {
  size_t pos = 0;
  auto u32 = [&](uint32_t& v) { if (pos + 4 > b.size()) return false; v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(b[pos + i])) << (8 * i); pos += 4; return true; };
  if (b.size() < 12 || b.compare(0, 4, "MHTN") != 0) return false;
  pos = 4; uint32_t ver = 0, n = 0;
  if (!u32(ver) || ver != 1 || !u32(n) || n > 4096) return false;
  const std::string pre = TitlePrefixHex();
  for (uint32_t i = 0; i < n; ++i) {
    if (pos + 2 > b.size()) return false;
    const size_t len = size_t(uint8_t(b[pos])) | size_t(uint8_t(b[pos + 1])) << 8; pos += 2;
    if (pos + len > b.size()) return false;
    std::string name = b.substr(pos, len); pos += len;
    uint32_t sz = 0; if (!u32(sz) || pos + sz > b.size()) return false;
    // solo archivos .vnand de este titulo, sin rutas
    if (name.find_first_of("/\\:") != std::string::npos || name.compare(0, pre.size(), pre) != 0 ||
        name.size() < 7 || name.compare(name.size() - 6, 6, ".vnand") != 0) return false;
    files.emplace_back(std::move(name), b.substr(pos, sz)); pos += sz;
  }
  return pos == b.size();
}
// Sustituye la partida local por la del paquete; la actual se mueve a nube_respaldos\.
static bool ApplyBundle(const std::string& bundle, std::string& err) {
  std::vector<std::pair<std::string, std::string>> files;
  if (!ParseBundle(bundle, files) || files.empty()) { err = "el archivo de la nube no es válido"; return false; }
  const fs::path backup = fs::path("nube_respaldos") / (LocalStamp() + "-partida");
  std::error_code ec; fs::create_directories(backup, ec);
  for (const auto& p : TitleFiles()) {
    fs::copy_file(p, backup / p.filename(), fs::copy_options::overwrite_existing, ec);
    if (ec) { err = "no se pudo hacer la copia de seguridad"; return false; }
  }
  for (const auto& p : TitleFiles()) {
    bool keep = false;
    for (const auto& f : files) if (f.first == p.filename().string()) keep = true;
    if (!keep) { fs::remove(p, ec); }  // ya esta copiado en el respaldo
  }
  for (const auto& f : files)
    if (!WriteFileAtomic(NandDir() / f.first, f.second)) { err = "no se pudo escribir la partida"; return false; }
  std::printf("[Nube] partida de la nube aplicada (copia anterior en %s)\n", backup.string().c_str());
  return true;
}

static fs::path SlotPath(int s) { return fs::path("guardados_rapidos") / ("ranura" + std::to_string(s) + ".sav"); }
static const char* kPending = "nube_pendiente.mhtnube";
static const char* kStateFile = "nube_estado.ini";
static const char* kTokenFile = "nube_dropbox.dat";
static const char* kKeyFile = "dropbox_clave.txt";
[[maybe_unused]] static constexpr int kRedirectPort = 53682;
[[maybe_unused]] static const char* kRedirect = "http://localhost:53682/";

// ------------------------------------------------------------------ estado compartido
struct Shared {
  std::mutex m;
  std::condition_variable cv;
  std::deque<int> jobs;               // Job
  std::string status = "Sin conectar.";
  std::string account;
  std::deque<std::string> messages;
  std::map<std::string, std::string> synced;  // nombre -> content_hash la ultima vez que coincidieron
  std::string app_key, refresh_token, access_token;
  std::chrono::steady_clock::time_point access_until{};
  std::chrono::steady_clock::time_point nand_due{};
  bool nand_dirty = false;
  bool slot_dirty[4] = {false, false, false, false};
  bool stop = false;
  bool running_job = false;
  std::thread worker;
};
static Shared& S() { static Shared s; return s; }
static std::atomic<bool> g_auto{true};
static std::atomic<bool> g_busy{false};
static std::atomic<bool> g_game_started{false};
enum Job { kConnect = 1, kDisconnect, kUploadAll, kDownloadAll, kStartSlots };

static void SetStatus(const std::string& s) { std::lock_guard<std::mutex> l(S().m); S().status = s; std::printf("[Nube] %s\n", s.c_str()); }
static void Message(const std::string& s) { std::lock_guard<std::mutex> l(S().m); S().messages.push_back(s); if (S().messages.size() > 8) S().messages.pop_front(); }

static void LoadState() {
  std::ifstream f(kStateFile); std::string line;
  while (std::getline(f, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos || line[0] == '#') continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    S().synced[k] = v;
  }
}
static void SaveState() {
  std::string o = "# Estado de la sincronizacion con Dropbox (no editar).\n";
  { std::lock_guard<std::mutex> l(S().m);
    for (const auto& kv : S().synced) o += kv.first + "=" + kv.second + "\n"; }
  WriteFileAtomic(kStateFile, o);
}

// ------------------------------------------------------------------ red (Windows)
struct HttpResp { int status = 0; std::string body, location, err; };

#ifdef _WIN32
static std::wstring W(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
  return w;
}
static std::string U8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}
static HINTERNET Session() {
  static HINTERNET h = [] {
    HINTERNET s = WinHttpOpen(L"MHTriPort/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"MHTriPort/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    return s;
  }();
  return h;
}
static HttpResp Http(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
                     const void* data = nullptr, size_t size = 0) {
  HttpResp res;
  const std::wstring wurl = W(url);
  URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
  uc.dwSchemeLength = DWORD(-1); uc.dwHostNameLength = DWORD(-1); uc.dwUrlPathLength = DWORD(-1); uc.dwExtraInfoLength = DWORD(-1);
  if (!Session() || !WinHttpCrackUrl(wurl.c_str(), DWORD(wurl.size()), 0, &uc)) { res.err = "dirección no válida"; return res; }
  const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (uc.lpszExtraInfo) path += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  HINTERNET c = WinHttpConnect(Session(), host.c_str(), uc.nPort, 0);
  if (!c) { res.err = "sin conexión (" + std::to_string(GetLastError()) + ")"; return res; }
  HINTERNET r = WinHttpOpenRequest(c, W(method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!r) { WinHttpCloseHandle(c); res.err = "sin conexión (" + std::to_string(GetLastError()) + ")"; return res; }
  WinHttpSetTimeouts(r, 10000, 10000, 120000, 120000);
  std::wstring hdr;
  for (const auto& h : headers) hdr += W(h) + L"\r\n";
  BOOL ok = WinHttpSendRequest(r, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), hdr.empty() ? 0 : DWORD(-1),
                               const_cast<void*>(data), DWORD(size), DWORD(size), 0);
  if (ok) ok = WinHttpReceiveResponse(r, nullptr);
  if (!ok) {
    const DWORD e = GetLastError();
    res.err = (e == ERROR_WINHTTP_TIMEOUT) ? "tiempo de espera agotado" : (e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT) ? "sin conexión a internet" : "error de red " + std::to_string(e);
    WinHttpCloseHandle(r); WinHttpCloseHandle(c); return res;
  }
  DWORD st = 0, sl = sizeof(st);
  WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sl, WINHTTP_NO_HEADER_INDEX);
  res.status = int(st);
  DWORD ll = 0;
  WinHttpQueryHeaders(r, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &ll, WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && ll > 0) {
    std::wstring loc(ll / sizeof(wchar_t) + 1, L'\0');
    if (WinHttpQueryHeaders(r, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, &loc[0], &ll, WINHTTP_NO_HEADER_INDEX)) {
      loc.resize(ll / sizeof(wchar_t)); res.location = U8(loc);
    }
  }
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
    const size_t o = res.body.size(); res.body.resize(o + avail);
    DWORD got = 0;
    if (!WinHttpReadData(r, &res.body[o], avail, &got)) { res.body.resize(o); res.err = "conexión cortada"; break; }
    res.body.resize(o + got);
  }
  WinHttpCloseHandle(r); WinHttpCloseHandle(c);
  return res;
}
static bool Protect(const std::string& in, std::string& out) {
  DATA_BLOB a{DWORD(in.size()), reinterpret_cast<BYTE*>(const_cast<char*>(in.data()))}, b{};
  if (!CryptProtectData(&a, L"MHTri Dropbox", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &b)) return false;
  out.assign(reinterpret_cast<char*>(b.pbData), b.cbData); LocalFree(b.pbData); return true;
}
static bool Unprotect(const std::string& in, std::string& out) {
  DATA_BLOB a{DWORD(in.size()), reinterpret_cast<BYTE*>(const_cast<char*>(in.data()))}, b{};
  if (!CryptUnprotectData(&a, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &b)) return false;
  out.assign(reinterpret_cast<char*>(b.pbData), b.cbData); LocalFree(b.pbData); return true;
}
#else
static HttpResp Http(const std::string&, const std::string&, const std::vector<std::string>&, const void* = nullptr, size_t = 0) {
  HttpResp r; r.err = "la nube solo funciona en Windows"; return r;
}
static bool Protect(const std::string& in, std::string& out) { out = in; return true; }
static bool Unprotect(const std::string& in, std::string& out) { out = in; return true; }
#endif

static std::string ApiError(const HttpResp& r) {
  if (!r.err.empty()) return r.err;
  std::printf("[Nube] respuesta %d de Dropbox: %.600s\n", r.status, r.body.c_str());
  // Dropbox responde en texto plano cuando a la app le falta un permiso en la consola.
  const size_t sc = r.body.find("required scope '");
  if (sc != std::string::npos) {
    const size_t a = sc + 16, b = r.body.find('\'', a);
    return "a la app de Dropbox le falta el permiso " + r.body.substr(a, b == std::string::npos ? std::string::npos : b - a) +
           " (pestaña Permissions, marcar y Submit) y después desconecta y vuelve a conectar";
  }
  bool ok = false; Json j = ParseJson(r.body, &ok);
  std::string m = j["error_summary"].str();
  if (m.empty()) m = j["error_description"].str();
  if (m.empty()) m = j["error"].str();
  if (m.empty() && r.body.size() < 200) m = r.body;
  if (m.find("missing_scope") != std::string::npos)
    return "el permiso guardado no incluye todo lo necesario: desconecta y vuelve a conectar (con los permisos marcados en la app)";
  if (r.status == 401) return "el permiso de Dropbox caducó o se retiró (vuelve a conectar)";
  if (m.find("insufficient_space") != std::string::npos) return "no queda espacio en tu Dropbox";
  return "Dropbox respondió " + std::to_string(r.status) + (m.empty() ? "" : ": " + m);
}

// ------------------------------------------------------------------ credenciales y permiso
static bool LoadCredentials() {
  std::string text;
  if (!ReadFile(kKeyFile, text)) return false;
  std::string key;
  for (char c : text) if (std::isalnum((unsigned char)c)) key += c; else if (!key.empty()) break;   // primera palabra
  std::lock_guard<std::mutex> l(S().m);
  S().app_key = key;
  return !key.empty();
}
static void LoadToken() {
  std::string enc, plain;
  if (!ReadFile(kTokenFile, enc) || !Unprotect(enc, plain)) return;
  Json j = ParseJson(plain);
  std::lock_guard<std::mutex> l(S().m);
  S().refresh_token = j["refresh_token"].str(); S().account = j["email"].str();
}
static bool SaveToken(const std::string& refresh, const std::string& email) {
  std::string enc;
  if (!Protect("{\"refresh_token\":" + JsonEscape(refresh) + ",\"email\":" + JsonEscape(email) + "}", enc)) return false;
  return WriteFileAtomic(kTokenFile, enc);
}
static bool EnsureAccess(std::string& err) {
  std::string key, rt;
  { std::lock_guard<std::mutex> l(S().m);
    if (!S().access_token.empty() && std::chrono::steady_clock::now() < S().access_until) return true;
    key = S().app_key; rt = S().refresh_token; }
  if (rt.empty()) { err = "no hay ninguna cuenta conectada"; return false; }
  if (key.empty()) { err = "falta dropbox_clave.txt en la carpeta del juego"; return false; }
  const std::string body = "client_id=" + UrlEncode(key) + "&refresh_token=" + UrlEncode(rt) + "&grant_type=refresh_token";
  HttpResp r = Http("POST", "https://api.dropboxapi.com/oauth2/token", {"Content-Type: application/x-www-form-urlencoded"}, body.data(), body.size());
  if (r.status != 200) {
    err = ApiError(r);
    if (r.status == 400) err += " (vuelve a conectar la cuenta)";
    return false;
  }
  Json j = ParseJson(r.body);
  std::lock_guard<std::mutex> l(S().m);
  S().access_token = j["access_token"].str();
  const int secs = j["expires_in"].t == Json::kNum ? int(j["expires_in"].n) : 3600;
  S().access_until = std::chrono::steady_clock::now() + std::chrono::seconds((std::max)(60, secs - 120));
  return !S().access_token.empty();
}
static std::string Bearer() { std::lock_guard<std::mutex> l(S().m); return "Authorization: Bearer " + S().access_token; }

// ------------------------------------------------------------------ Dropbox
struct CloudFile { std::string id, md5, modified; };   // md5 = content_hash de Dropbox

static bool ListCloud(std::map<std::string, CloudFile>& out, std::string& err) {
  const std::string body = "{\"path\":\"\",\"recursive\":false}";
  HttpResp r = Http("POST", "https://api.dropboxapi.com/2/files/list_folder", {Bearer(), "Content-Type: application/json"}, body.data(), body.size());
  if (r.status != 200) { err = ApiError(r); return false; }
  Json j = ParseJson(r.body);
  for (const auto& f : j["entries"].a) {
    if (f[".tag"].str() != "file") continue;
    std::string mod = f["client_modified"].str();
    if (mod.empty()) mod = f["server_modified"].str();
    out[f["name"].str()] = {f["path_lower"].str(), f["content_hash"].str(), mod.substr(0, 19)};
  }
  return true;
}
static bool Upload(const std::string& name, const std::string& data, const std::string& /*existing*/, const std::string& mtime, CloudFile& out, std::string& err) {
  std::string arg = "{\"path\":" + JsonEscape("/" + name) + ",\"mode\":\"overwrite\",\"autorename\":false,\"mute\":true";
  if (!mtime.empty()) arg += ",\"client_modified\":" + JsonEscape(mtime + "Z");
  arg += "}";
  HttpResp r = Http("POST", "https://content.dropboxapi.com/2/files/upload",
                    {Bearer(), "Content-Type: application/octet-stream", "Dropbox-API-Arg: " + arg}, data.data(), data.size());
  if (r.status != 200) { err = ApiError(r); return false; }
  Json j = ParseJson(r.body);
  out = {j["path_lower"].str(), j["content_hash"].str(), j["client_modified"].str().substr(0, 19)};
  if (out.md5.empty()) out.md5 = ContentHash(data.data(), data.size());
  return true;
}
static bool Download(const std::string& path, std::string& data, std::string& err) {
  HttpResp r = Http("POST", "https://content.dropboxapi.com/2/files/download", {Bearer(), "Dropbox-API-Arg: {\"path\":" + JsonEscape(path) + "}"});
  if (r.status != 200 || !r.err.empty()) { err = ApiError(r); return false; }
  data.swap(r.body);
  return true;
}

// ------------------------------------------------------------------ sincronizacion
enum class Dir { kAuto, kUp, kDown };

struct Item {
  std::string name;   // nombre en la nube
  int slot = 0;       // 0 = partida (paquete de la NAND)
};
static bool ReadLocal(const Item& it, std::string& data, std::string& mtime) {
  if (it.slot == 0) return BuildBundle(data, mtime);
  const fs::path p = SlotPath(it.slot);
  mtime = FileTimeIso(p);
  return ReadFile(p, data);
}
// Devuelve false solo si hubo un error (no si no habia nada que hacer).
static bool SyncItem(const Item& it, const std::map<std::string, CloudFile>& cloud, Dir dir, std::string& err, std::string& done) {
  std::string local, lmtime;
  const bool has_local = ReadLocal(it, local, lmtime);
  const std::string lhash = has_local ? ContentHash(local.data(), local.size()) : "";
  const auto cf = cloud.find(it.name);
  const bool has_cloud = cf != cloud.end();
  std::string last;
  { std::lock_guard<std::mutex> l(S().m); auto s = S().synced.find(it.name); if (s != S().synced.end()) last = s->second; }
  const bool in_game = g_game_started.load();
  enum { kNone, kUpload, kDownload } act = kNone;
  if (has_local && has_cloud && lhash == cf->second.md5) act = kNone;
  else if (dir == Dir::kUp) act = has_local ? kUpload : kNone;
  else if (dir == Dir::kDown) act = has_cloud ? kDownload : kNone;
  else if (has_local && !has_cloud) act = kUpload;
  else if (!has_local && has_cloud) act = kDownload;
  else if (has_local && has_cloud) {
    if (lhash == last) act = kDownload;
    else if (cf->second.md5 == last) act = kUpload;
    else act = (lmtime >= cf->second.modified) ? kUpload : kDownload;   // cambiaron los dos: gana el mas reciente
  }
  // Con el juego en marcha la partida en memoria es la buena: nunca se pisa en caliente.
  if (act == kDownload && it.slot == 0 && in_game && dir != Dir::kDown) act = kUpload;
  if (act == kNone) {
    if (has_local && has_cloud && lhash == cf->second.md5) { std::lock_guard<std::mutex> l(S().m); S().synced[it.name] = lhash; }
    return true;
  }
  if (act == kUpload) {
    CloudFile out;
    if (!Upload(it.name, local, has_cloud ? cf->second.id : "", lmtime, out, err)) { err = it.name + ": " + err; return false; }
    { std::lock_guard<std::mutex> l(S().m); S().synced[it.name] = out.md5.empty() ? lhash : out.md5; }
    std::printf("[Nube] subido %s (%.1f MB)\n", it.name.c_str(), local.size() / 1048576.0);
    done += done.empty() ? "subido " : ", ";
    done += it.slot ? "ranura " + std::to_string(it.slot) : "partida";
    return true;
  }
  std::string data;
  if (!Download(cf->second.id, data, err)) { err = it.name + ": " + err; return false; }
  if (ContentHash(data.data(), data.size()) != cf->second.md5 && !cf->second.md5.empty()) { err = it.name + ": la descarga llegó incompleta"; return false; }
  if (it.slot == 0) {
    if (in_game) {
      if (!WriteFileAtomic(kPending, data)) { err = "no se pudo guardar la partida descargada"; return false; }
      done += done.empty() ? "" : ", ";
      done += "partida lista para el próximo arranque";
      return true;   // se aplica (y se anota) al arrancar
    }
    if (!ApplyBundle(data, err)) return false;
  } else {
    const fs::path p = SlotPath(it.slot);
    std::error_code ec;
    if (has_local) {
      const fs::path backup = fs::path("nube_respaldos") / (LocalStamp() + "-ranura" + std::to_string(it.slot) + ".sav");
      fs::create_directories(backup.parent_path(), ec);
      fs::copy_file(p, backup, fs::copy_options::overwrite_existing, ec);
    }
    if (!WriteFileAtomic(p, data)) { err = "no se pudo escribir la ranura " + std::to_string(it.slot); return false; }
  }
  { std::lock_guard<std::mutex> l(S().m); S().synced[it.name] = cf->second.md5; }
  std::printf("[Nube] bajado %s (%.1f MB)\n", it.name.c_str(), data.size() / 1048576.0);
  done += done.empty() ? "bajado " : ", ";
  done += it.slot ? "ranura " + std::to_string(it.slot) : "partida";
  return true;
}
static std::vector<Item> AllItems() { return {{"partida.mhtnube", 0}, {"ranura1.sav", 1}, {"ranura2.sav", 2}, {"ranura3.sav", 3}}; }

static bool RunSync(const std::vector<Item>& items, Dir dir, bool quiet_if_nothing) {
  std::string err, done;
  g_busy.store(true);
  SetStatus("Sincronizando con Dropbox…");
  std::map<std::string, CloudFile> cloud;
  bool ok = EnsureAccess(err) && ListCloud(cloud, err);
  for (size_t i = 0; ok && i < items.size(); ++i) ok = SyncItem(items[i], cloud, dir, err, done);
  SaveState();
  g_busy.store(false);
  char when[16]; const std::time_t t = std::time(nullptr); std::tm l{};
#ifdef _WIN32
  localtime_s(&l, &t);
#else
  localtime_r(&t, &l);
#endif
  std::strftime(when, sizeof(when), "%H:%M", &l);
  if (!ok) { SetStatus("Error: " + err); Message("Nube: " + err); return false; }
  if (done.empty()) { SetStatus(std::string("Al día (") + when + ")."); if (!quiet_if_nothing) Message("Nube: todo estaba al día"); }
  else { SetStatus("Al día (" + std::string(when) + "): " + done + "."); Message("Nube: " + done); }
  return true;
}

// ------------------------------------------------------------------ conexion (OAuth 2 con PKCE y redireccion local fija)
static bool Connect(std::string& err) {
#ifdef _WIN32
  std::string key;
  { std::lock_guard<std::mutex> l(S().m); key = S().app_key; }
  if (key.empty() && !LoadCredentials()) { err = "falta dropbox_clave.txt en la carpeta del juego"; return false; }
  { std::lock_guard<std::mutex> l(S().m); key = S().app_key; }
  WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
  // "localhost" puede resolverse a 127.0.0.1 o a ::1: se escucha en los dos.
  SOCKET ls[2] = {INVALID_SOCKET, INVALID_SOCKET};
  {
    SOCKET s4 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(kRedirectPort);
    if (s4 != INVALID_SOCKET && bind(s4, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && listen(s4, 4) == 0) ls[0] = s4;
    else if (s4 != INVALID_SOCKET) closesocket(s4);
    SOCKET s6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in6 a6{}; a6.sin6_family = AF_INET6; a6.sin6_addr.s6_addr[15] = 1; a6.sin6_port = htons(kRedirectPort);  // ::1
    if (s6 != INVALID_SOCKET && bind(s6, reinterpret_cast<sockaddr*>(&a6), sizeof(a6)) == 0 && listen(s6, 4) == 0) ls[1] = s6;
    else if (s6 != INVALID_SOCKET) closesocket(s6);
  }
  if (ls[0] == INVALID_SOCKET && ls[1] == INVALID_SOCKET) { err = "el puerto 53682 está ocupado por otro programa"; return false; }
  std::random_device rd; std::string rnd;
  for (int i = 0; i < 48; ++i) rnd += char(rd() & 0xFF);
  const std::string verifier = Base64Url(rnd), challenge = Base64Url(Sha256Raw(verifier));
  std::string st; for (int i = 0; i < 16; ++i) st += char(rd() & 0xFF);
  const std::string state = Base64Url(st);
  const std::string url = "https://www.dropbox.com/oauth2/authorize?client_id=" + UrlEncode(key) +
      "&response_type=code&token_access_type=offline&redirect_uri=" + UrlEncode(kRedirect) +
      "&scope=" + UrlEncode("account_info.read files.metadata.read files.content.read files.content.write") +
      "&code_challenge=" + challenge + "&code_challenge_method=S256&state=" + state;
  ShellExecuteW(nullptr, L"open", W(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  SetStatus("Esperando el permiso en el navegador…");
  Message("Nube: acepta el permiso en el navegador (Alt+Tab si no lo ves)");
  std::string code;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(4);
  while (code.empty() && err.empty() && std::chrono::steady_clock::now() < deadline) {
    { std::lock_guard<std::mutex> l(S().m); if (S().stop) break; }
    fd_set rs; FD_ZERO(&rs);
    for (SOCKET x : ls) if (x != INVALID_SOCKET) FD_SET(x, &rs);
    timeval tv{1, 0};
    if (select(0, &rs, nullptr, nullptr, &tv) <= 0) continue;
    for (SOCKET x : ls) {
      if (x == INVALID_SOCKET || !FD_ISSET(x, &rs) || !code.empty() || !err.empty()) continue;
      SOCKET c = accept(x, nullptr, nullptr);
      if (c == INVALID_SOCKET) continue;
      DWORD to = 5000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
      std::string req; char buf[2048];
      while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) { const int n = recv(c, buf, sizeof(buf), 0); if (n <= 0) break; req.append(buf, size_t(n)); }
      const std::string line = req.substr(0, req.find("\r\n"));
      std::string query; const size_t q = line.find('?');
      if (q != std::string::npos) { const size_t sp = line.find(' ', q); query = line.substr(q + 1, sp == std::string::npos ? std::string::npos : sp - q - 1); }
      auto params = ParseQuery(query);
      std::string html;
      if (params.count("code") && params["state"] == state) { code = params["code"]; html = "Listo: Monster Hunter Tri ya puede guardar en tu Dropbox. Puedes cerrar esta pestaña y volver al juego."; }
      else if (params.count("error")) { err = "permiso denegado (" + params["error"] + ")"; html = "No se concedió el permiso. Puedes cerrar esta pestaña."; }
      std::string resp;
      if (html.empty()) resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      else {
        html = "<!doctype html><meta charset=\"utf-8\"><title>Monster Hunter Tri</title><body style=\"font:18px sans-serif;margin:3em\"><p>" + html + "</p></body>";
        resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(html.size()) + "\r\nConnection: close\r\n\r\n" + html;
      }
      send(c, resp.data(), int(resp.size()), 0);
      closesocket(c);
    }
  }
  for (SOCKET x : ls) if (x != INVALID_SOCKET) closesocket(x);
  if (code.empty()) { if (err.empty()) err = "no llegó el permiso (tiempo agotado)"; return false; }
  const std::string body = "code=" + UrlEncode(code) + "&grant_type=authorization_code&client_id=" + UrlEncode(key) +
                           "&redirect_uri=" + UrlEncode(kRedirect) + "&code_verifier=" + verifier;
  HttpResp r = Http("POST", "https://api.dropboxapi.com/oauth2/token", {"Content-Type: application/x-www-form-urlencoded"}, body.data(), body.size());
  if (r.status != 200) { err = ApiError(r); return false; }
  Json j = ParseJson(r.body);
  const std::string refresh = j["refresh_token"].str();
  if (refresh.empty()) { err = "Dropbox no devolvió un permiso permanente"; return false; }
  {
    std::lock_guard<std::mutex> l(S().m);
    S().refresh_token = refresh; S().access_token = j["access_token"].str();
    const int secs = j["expires_in"].t == Json::kNum ? int(j["expires_in"].n) : 3600;
    S().access_until = std::chrono::steady_clock::now() + std::chrono::seconds((std::max)(60, secs - 120));
  }
  // Correo de la cuenta (solo para mostrarlo en el menu).
  std::string email;
  { const std::string nb = "null";
    HttpResp a = Http("POST", "https://api.dropboxapi.com/2/users/get_current_account", {Bearer(), "Content-Type: application/json"}, nb.data(), nb.size());
    if (a.status == 200) email = ParseJson(a.body)["email"].str(); }
  { std::lock_guard<std::mutex> l(S().m); S().account = email; }
  if (!SaveToken(refresh, email)) Message("Nube: no se pudo guardar el permiso en disco");
  SaveState();
  SetStatus("Conectado" + (email.empty() ? std::string(".") : " como " + email + "."));
  Message("Nube: Dropbox conectado" + (email.empty() ? std::string() : " (" + email + ")"));
  return true;
#else
  err = "la nube solo funciona en Windows";
  return false;
#endif
}
static void Disconnect() {
  std::string e;
  if (EnsureAccess(e)) Http("POST", "https://api.dropboxapi.com/2/auth/token/revoke", {Bearer()});
  { std::lock_guard<std::mutex> l(S().m); S().refresh_token.clear(); S().access_token.clear(); S().account.clear(); S().synced.clear(); }
  std::error_code ec; fs::remove(kTokenFile, ec); fs::remove(kStateFile, ec);
  SetStatus("Sin conectar.");
  Message("Nube: cuenta desconectada");
}

// ------------------------------------------------------------------ hilo de la nube
static void Worker() {
  for (;;) {
    int job = 0; bool nand = false; std::vector<Item> slots;
    {
      std::unique_lock<std::mutex> l(S().m);
      S().cv.wait_for(l, std::chrono::seconds(1), [] { return S().stop || !S().jobs.empty(); });
      if (!S().jobs.empty()) { job = S().jobs.front(); S().jobs.pop_front(); }
      if (!job && g_auto.load() && !S().refresh_token.empty()) {
        if (S().nand_dirty && std::chrono::steady_clock::now() >= S().nand_due) { S().nand_dirty = false; nand = true; }
        for (int s = 1; s <= 3; ++s) if (S().slot_dirty[s]) { S().slot_dirty[s] = false; slots.push_back({"ranura" + std::to_string(s) + ".sav", s}); }
      }
      if (S().stop && !job && !nand && slots.empty()) return;
      S().running_job = job || nand || !slots.empty();
    }
    std::string err;
    switch (job) {
      case kConnect:
        if (Connect(err)) RunSync(AllItems(), Dir::kAuto, true);
        else { SetStatus("Error: " + err); Message("Nube: " + err); }
        break;
      case kDisconnect: Disconnect(); break;
      case kUploadAll: RunSync(AllItems(), Dir::kUp, false); break;
      case kDownloadAll: RunSync(AllItems(), Dir::kDown, false); break;
      case kStartSlots: RunSync({{"ranura1.sav", 1}, {"ranura2.sav", 2}, {"ranura3.sav", 3}}, Dir::kAuto, true); break;
      default: break;
    }
    if (nand || !slots.empty()) {
      std::vector<Item> items;
      if (nand) items.push_back({"partida.mhtnube", 0});
      items.insert(items.end(), slots.begin(), slots.end());
      RunSync(items, Dir::kAuto, true);
    }
    { std::lock_guard<std::mutex> l(S().m); S().running_job = false; }
    S().cv.notify_all();
  }
}
static void Enqueue(int job) {
  { std::lock_guard<std::mutex> l(S().m); for (int j : S().jobs) if (j == job) return; S().jobs.push_back(job); }
  S().cv.notify_all();
}

}  // namespace detail

using namespace detail;

void Init() {
  static bool done = false;
  if (done) return;
  done = true;
  LoadCredentials();
  LoadToken();
  LoadState();
  {
    std::lock_guard<std::mutex> l(S().m);
    if (!S().refresh_token.empty()) S().status = "Conectado" + (S().account.empty() ? std::string(".") : " como " + S().account + ".");
    else if (S().app_key.empty()) S().status = "Falta dropbox_clave.txt (ver CONTROLES.md).";
    S().worker = std::thread(Worker);
  }
}

void StartupSync() {
  Init();
  // Partida bajada durante la ultima sesion con "Descargar": se aplica ahora.
  std::string pending;
  if (ReadFile(kPending, pending)) {
    std::string err;
    if (ApplyBundle(pending, err)) {
      std::lock_guard<std::mutex> l(S().m); S().synced["partida.mhtnube"] = ContentHash(pending.data(), pending.size());
      Message("Nube: partida descargada aplicada");
    } else Message("Nube: " + err);
    std::error_code ec; fs::remove(kPending, ec);
    SaveState();
  }
  bool connected;
  { std::lock_guard<std::mutex> l(S().m); connected = !S().refresh_token.empty(); }
  // La partida se comprueba antes de arrancar (el juego la lee al empezar); las
  // ranuras, ya en marcha y en segundo plano (solo se usan al pulsar F8).
  if (connected && g_auto.load()) {
    std::printf("[Nube] comprobando la partida en Dropbox antes de arrancar…\n");
    const auto t0 = std::chrono::steady_clock::now();
    RunSync({{"partida.mhtnube", 0}}, Dir::kAuto, true);
    std::printf("[Nube] comprobacion terminada en %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  }
  g_game_started.store(true);
  if (connected && g_auto.load()) Enqueue(kStartSlots);
}

void Shutdown() {
  bool connected;
  {
    std::lock_guard<std::mutex> l(S().m);
    connected = !S().refresh_token.empty();
    if (connected && g_auto.load()) S().nand_due = std::chrono::steady_clock::now();   // lo pendiente, ya
    S().stop = true;
  }
  S().cv.notify_all();
  if (!S().worker.joinable()) return;
  // Espera a que termine (subida de la partida o de una ranura) como mucho 30 s.
  auto& sh = S();
  std::thread waiter([&sh] { if (sh.worker.joinable()) sh.worker.join(); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  for (;;) {
    { std::lock_guard<std::mutex> l(S().m);
      const bool pending = S().running_job || !S().jobs.empty() || (g_auto.load() && connected && (S().nand_dirty || S().slot_dirty[1] || S().slot_dirty[2] || S().slot_dirty[3]));
      if (!pending) break; }
    if (std::chrono::steady_clock::now() > deadline) { std::printf("[Nube] salida sin terminar la subida\n"); waiter.detach(); return; }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  waiter.join();
}

void NotifyNandSaved(const std::string& nand_path) {
  char pre[64]; std::string id = nwii::runtime::Config::get().game_id; if (id.size() < 4) id += "    ";
  std::snprintf(pre, sizeof(pre), "/title/00010000/%02x%02x%02x%02x/", (unsigned char)id[0], (unsigned char)id[1], (unsigned char)id[2], (unsigned char)id[3]);
  if (nand_path.compare(0, std::strlen(pre), pre) != 0) return;
  std::lock_guard<std::mutex> l(S().m);
  S().nand_dirty = true;
  S().nand_due = std::chrono::steady_clock::now() + std::chrono::seconds(8);   // el juego escribe varios archivos seguidos
}
void NotifySlotSaved(int slot) {
  if (slot < 1 || slot > 3) return;
  { std::lock_guard<std::mutex> l(S().m); S().slot_dirty[slot] = true; }
  S().cv.notify_all();
}

void SetAuto(bool on) { g_auto.store(on); }
bool Auto() { return g_auto.load(); }
bool HasCredentials() { std::lock_guard<std::mutex> l(S().m); return !S().app_key.empty(); }
bool Connected() { std::lock_guard<std::mutex> l(S().m); return !S().refresh_token.empty(); }
bool Busy() { return g_busy.load(); }
std::string Account() { std::lock_guard<std::mutex> l(S().m); return S().account; }
std::string Status() { std::lock_guard<std::mutex> l(S().m); return S().status; }
void RequestConnect() { if (!HasCredentials()) LoadCredentials(); Enqueue(kConnect); }
void RequestDisconnect() { Enqueue(kDisconnect); }
void RequestUpload() { Enqueue(kUploadAll); }
void RequestDownload() { Enqueue(kDownloadAll); }
bool PopMessage(std::string& out) {
  std::lock_guard<std::mutex> l(S().m);
  if (S().messages.empty()) return false;
  out = S().messages.front(); S().messages.pop_front();
  return true;
}

}  // namespace port_cloud
