#pragma once
// tools/gx-pipeline: texturas HD en formato de paquete de Dolphin.
// Los paquetes nombran cada textura como
//   tex1_<ancho>x<alto>[_m]_<hash>[_<hash paleta>|_$]_<formato>.png
// donde <hash> es XXH64 (semilla 0) de los datos del nivel base tal como estan
// en la memoria del juego y <hash paleta> el XXH64 del tramo de paleta usado.
// Se buscan en la carpeta texturas_hd (con subcarpetas) junto al proyecto.
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <memory>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace hd_textures {

// ------------------------------------------------------------------ XXH64
namespace detail {
constexpr uint64_t P1 = 11400714785074694791ULL, P2 = 14029467366897019727ULL, P3 = 1609587929392839161ULL,
                   P4 = 9650029242287828579ULL, P5 = 2870177450012600261ULL;
inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline uint64_t rd64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }  // x86: little-endian
inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t round(uint64_t acc, uint64_t in) { acc += in * P2; acc = rotl(acc, 31); acc *= P1; return acc; }
inline uint64_t merge(uint64_t acc, uint64_t v) { v = round(0, v); acc ^= v; acc = acc * P1 + P4; return acc; }
}  // namespace detail

inline uint64_t XXH64(const void* data, size_t len, uint64_t seed = 0) {
  using namespace detail;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  const uint8_t* end = p + len;
  uint64_t h;
  if (len >= 32) {
    uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
    const uint8_t* limit = end - 32;
    do {
      v1 = round(v1, rd64(p)); p += 8; v2 = round(v2, rd64(p)); p += 8;
      v3 = round(v3, rd64(p)); p += 8; v4 = round(v4, rd64(p)); p += 8;
    } while (p <= limit);
    h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
  } else {
    h = seed + P5;
  }
  h += uint64_t(len);
  while (p + 8 <= end) { h ^= round(0, rd64(p)); h = rotl(h, 27) * P1 + P4; p += 8; }
  if (p + 4 <= end) { h ^= uint64_t(rd32(p)) * P1; h = rotl(h, 23) * P2 + P3; p += 4; }
  while (p < end) { h ^= uint64_t(*p) * P5; h = rotl(h, 11) * P1; ++p; }
  h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
  return h;
}

// ------------------------------------------------------------------ PNG
namespace png {
struct BitReader {
  const uint8_t* p; size_t n, pos = 0; uint64_t bitbuf = 0; int bitcnt = 0; bool err = false;
  BitReader(const uint8_t* d, size_t len) : p(d), n(len) {}
  void fill() { while (bitcnt <= 56) { const uint64_t b = pos < n ? p[pos] : 0; if (pos >= n + 8) { err = true; return; } ++pos; bitbuf |= b << bitcnt; bitcnt += 8; } }
  uint32_t peek(int need) { if (bitcnt < need) fill(); return uint32_t(bitbuf & ((1ull << need) - 1)); }
  void drop(int k) { bitbuf >>= k; bitcnt -= k; }
  uint32_t bits(int need) { if (!need) return 0; const uint32_t v = peek(need); drop(need); return v; }
  // bytes realmente consumidos (para bloques sin comprimir)
  void align_bytes() { const int extra = bitcnt & 7; drop(extra); pos -= size_t(bitcnt / 8); bitbuf = 0; bitcnt = 0; }
};
struct Huffman {
  static constexpr int kFast = 10;
  uint16_t counts[16]{}, symbols[320]{};
  uint16_t fast[1 << kFast]{};  // (longitud << 9) | simbolo; 0 = no esta en la tabla rapida
  bool build(const uint8_t* lengths, int n) {
    std::memset(counts, 0, sizeof(counts)); std::memset(fast, 0, sizeof(fast));
    for (int i = 0; i < n; ++i) counts[lengths[i]]++;
    counts[0] = 0;
    uint16_t offs[16]{};
    for (int i = 1; i < 16; ++i) offs[i] = offs[i - 1] + counts[i - 1];
    for (int i = 0; i < n; ++i) if (lengths[i]) symbols[offs[lengths[i]]++] = uint16_t(i);
    // codigos canonicos -> tabla rapida (indexada con los bits invertidos)
    int code = 0; uint16_t next_code[16]{};
    for (int len = 1; len < 16; ++len) { code = (code + counts[len - 1]) << 1; next_code[len] = uint16_t(code); }
    for (int i = 0; i < n; ++i) {
      const int len = lengths[i]; if (!len || len > kFast) continue;
      int c = next_code[len]++, rev = 0;
      for (int k = 0; k < len; ++k) { rev = (rev << 1) | (c & 1); c >>= 1; }
      for (int k = rev; k < (1 << kFast); k += (1 << len)) fast[k] = uint16_t((len << 9) | i);
    }
    return true;
  }
  int decode(BitReader& br) const {
    const uint32_t look = br.peek(kFast);
    const uint16_t e = fast[look];
    if (e) { br.drop(e >> 9); return e & 511; }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
      code |= int(br.bits(1));
      const int count = counts[len];
      if (code - count < first) return symbols[index + (code - first)];
      index += count; first += count; first <<= 1; code <<= 1;
      if (br.err) return -1;
    }
    return -1;
  }
};
// zlib -> datos sin comprimir
inline bool Inflate(const uint8_t* src, size_t len, std::vector<uint8_t>& out) {
  if (len < 2) return false;
  BitReader br(src + 2, len - 2);  // cabecera zlib
  static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
  static const uint8_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
  static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
  static const uint8_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
  int last = 0;
  while (!last) {
    last = int(br.bits(1));
    const int type = int(br.bits(2));
    if (br.err) return false;
    if (type == 0) {
      br.align_bytes();
      if (br.pos + 4 > br.n) return false;
      const uint16_t l = uint16_t(br.p[br.pos] | (br.p[br.pos + 1] << 8)); br.pos += 4;
      if (br.pos + l > br.n) return false;
      out.insert(out.end(), br.p + br.pos, br.p + br.pos + l); br.pos += l;
      continue;
    }
    Huffman lit, dist;
    if (type == 1) {
      uint8_t l[288];
      for (int i = 0; i < 144; ++i) l[i] = 8; for (int i = 144; i < 256; ++i) l[i] = 9;
      for (int i = 256; i < 280; ++i) l[i] = 7; for (int i = 280; i < 288; ++i) l[i] = 8;
      lit.build(l, 288);
      uint8_t d[30]; for (int i = 0; i < 30; ++i) d[i] = 5; dist.build(d, 30);
    } else if (type == 2) {
      const int hlit = int(br.bits(5)) + 257, hdist = int(br.bits(5)) + 1, hclen = int(br.bits(4)) + 4;
      static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
      uint8_t cl[19]{}; for (int i = 0; i < hclen; ++i) cl[order[i]] = uint8_t(br.bits(3));
      Huffman clh; clh.build(cl, 19);
      uint8_t lens[320]{}; int n = 0;
      while (n < hlit + hdist) {
        const int sym = clh.decode(br);
        if (sym < 0) return false;
        if (sym < 16) lens[n++] = uint8_t(sym);
        else if (sym == 16) { if (!n) return false; const int r = 3 + int(br.bits(2)); for (int i = 0; i < r && n < 320; ++i) { lens[n] = lens[n - 1]; ++n; } }
        else if (sym == 17) { const int r = 3 + int(br.bits(3)); for (int i = 0; i < r && n < 320; ++i) lens[n++] = 0; }
        else { const int r = 11 + int(br.bits(7)); for (int i = 0; i < r && n < 320; ++i) lens[n++] = 0; }
      }
      lit.build(lens, hlit); dist.build(lens + hlit, hdist);
    } else return false;
    size_t o = out.size();
    for (;;) {
      if (out.size() < o + 300) out.resize(std::max(out.size() * 2, o + 65536));
      const int sym = lit.decode(br);
      if (sym < 0 || br.err) { out.resize(o); return false; }
      if (sym < 256) { out[o++] = uint8_t(sym); continue; }
      if (sym == 256) break;
      const int li = sym - 257; if (li >= 29) { out.resize(o); return false; }
      const int length = lbase[li] + int(br.bits(lext[li]));
      const int ds = dist.decode(br); if (ds < 0 || ds >= 30) { out.resize(o); return false; }
      const size_t d = dbase[ds] + br.bits(dext[ds]);
      if (d > o) { out.resize(o); return false; }
      uint8_t* dst = out.data() + o; const uint8_t* src = dst - d;
      if (d >= size_t(length)) std::memcpy(dst, src, size_t(length));
      else for (int i = 0; i < length; ++i) dst[i] = src[i];
      o += size_t(length);
    }
    out.resize(o);
  }
  return true;
}
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
// Decodifica a RGBA8. Soporta tipos de color 0/2/3/4/6, profundidad 1-16, sin entrelazado.
inline bool Decode(const std::vector<uint8_t>& f, std::vector<uint8_t>& rgba, int& w, int& h) {
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (f.size() < 33 || std::memcmp(f.data(), sig, 8) != 0) return false;
  size_t pos = 8; int depth = 0, ctype = 0, interlace = 0;
  std::vector<uint8_t> idat, pal, trns;
  while (pos + 8 <= f.size()) {
    const uint32_t len = be32(&f[pos]); const char* type = reinterpret_cast<const char*>(&f[pos + 4]);
    if (pos + 12 + len > f.size()) return false;
    const uint8_t* d = &f[pos + 8];
    if (!std::memcmp(type, "IHDR", 4)) { w = int(be32(d)); h = int(be32(d + 4)); depth = d[8]; ctype = d[9]; interlace = d[12]; }
    else if (!std::memcmp(type, "PLTE", 4)) pal.assign(d, d + len);
    else if (!std::memcmp(type, "tRNS", 4)) trns.assign(d, d + len);
    else if (!std::memcmp(type, "IDAT", 4)) idat.insert(idat.end(), d, d + len);
    else if (!std::memcmp(type, "IEND", 4)) break;
    pos += 12 + len;
  }
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || interlace) return false;
  const int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
  if (!ch) return false;
  const size_t bpp_bits = size_t(ch) * depth, stride = (size_t(w) * bpp_bits + 7) / 8, bpp = std::max<size_t>(1, bpp_bits / 8);
  std::vector<uint8_t> raw; raw.reserve((stride + 1) * h);
  if (!Inflate(idat.data(), idat.size(), raw) || raw.size() < (stride + 1) * size_t(h)) return false;
  std::vector<uint8_t> img(stride * h);
  for (int y = 0; y < h; ++y) {
    const uint8_t ft = raw[y * (stride + 1)];
    const uint8_t* in = &raw[y * (stride + 1) + 1];
    uint8_t* o = &img[y * stride];
    const uint8_t* prev = y ? &img[(y - 1) * stride] : nullptr;
    if (ft == 0) { std::memcpy(o, in, stride); continue; }
    if (ft == 1) { for (size_t x = 0; x < stride; ++x) o[x] = uint8_t(in[x] + (x >= bpp ? o[x - bpp] : 0)); continue; }
    if (ft == 2) { if (prev) for (size_t x = 0; x < stride; ++x) o[x] = uint8_t(in[x] + prev[x]); else std::memcpy(o, in, stride); continue; }
    for (size_t x = 0; x < stride; ++x) {
      const int a = x >= bpp ? o[x - bpp] : 0, b = prev ? prev[x] : 0, c = (prev && x >= bpp) ? prev[x - bpp] : 0;
      int v = in[x];
      switch (ft) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: { const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                  v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); break; }
        default: return false;
      }
      o[x] = uint8_t(v);
    }
  }
  if (depth == 8 && (ctype == 6 || ctype == 2)) {  // caso habitual: copia directa
    rgba.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
      const uint8_t* row = &img[y * stride]; uint8_t* o = &rgba[size_t(y) * w * 4];
      if (ctype == 6) std::memcpy(o, row, size_t(w) * 4);
      else for (int x = 0; x < w; ++x) { o[x * 4] = row[x * 3]; o[x * 4 + 1] = row[x * 3 + 1]; o[x * 4 + 2] = row[x * 3 + 2]; o[x * 4 + 3] = 255; }
    }
    return true;
  }
  rgba.assign(size_t(w) * h * 4, 255);
  auto sample = [&](const uint8_t* row, int x, int c) -> int {  // canal c del pixel x, a 8 bits
    if (depth == 8) return row[x * ch + c];
    if (depth == 16) return row[(x * ch + c) * 2];
    const int bitpos = (x * ch + c) * depth;
    const int v = (row[bitpos >> 3] >> (8 - depth - (bitpos & 7))) & ((1 << depth) - 1);
    return ctype == 3 ? v : v * 255 / ((1 << depth) - 1);
  };
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = &img[y * stride];
    for (int x = 0; x < w; ++x) {
      uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
      if (ctype == 3) {
        const int i = sample(row, x, 0);
        if (size_t(i) * 3 + 2 < pal.size()) { o[0] = pal[i * 3]; o[1] = pal[i * 3 + 1]; o[2] = pal[i * 3 + 2]; }
        o[3] = size_t(i) < trns.size() ? trns[i] : 255;
      } else if (ctype == 0 || ctype == 4) {
        const int g = sample(row, x, 0); o[0] = o[1] = o[2] = uint8_t(g);
        o[3] = ctype == 4 ? uint8_t(sample(row, x, 1)) : 255;
      } else {
        o[0] = uint8_t(sample(row, x, 0)); o[1] = uint8_t(sample(row, x, 1)); o[2] = uint8_t(sample(row, x, 2));
        o[3] = ctype == 6 ? uint8_t(sample(row, x, 3)) : 255;
      }
    }
  }
  return true;
}
}  // namespace png

// ------------------------------------------------------------------ indice del paquete
class Pack {
  std::unordered_map<std::string, std::string> files_;  // "WxH_hash[_tlut|_$]_fmt" -> ruta
  bool scanned_ = false;
 public:
  static Pack& get() { static Pack p; return p; }
  size_t size() const { return files_.size(); }
  void Scan(const char* dir = "texturas_hd") {
    if (scanned_) return;
    scanned_ = true;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) { std::printf("[TexturasHD] no hay carpeta %s\n", dir); return; }
    size_t dds = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file(ec)) continue;
      std::string name = it->path().filename().string();
      std::string lower = name; std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
      const bool is_png = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".png") == 0;
      if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".dds") == 0) { ++dds; continue; }
      if (!is_png || lower.rfind("tex1_", 0) != 0) continue;
      std::string stem = lower.substr(5, lower.size() - 9);  // sin "tex1_" ni ".png"
      if (stem.find("_mip") != std::string::npos) continue;   // niveles extra: se generan
      // quitar la marca "_m" de mipmaps (justo detras de WxH)
      const auto u = stem.find('_');
      if (u != std::string::npos && stem.compare(u, 3, "_m_") == 0) stem.erase(u, 2);
      files_[stem] = it->path().string();
    }
    std::printf("[TexturasHD] %zu texturas PNG en %s%s\n", files_.size(), dir, dds ? " (hay archivos DDS: aun no se cargan)" : "");
  }
  // Nombre del juego -> ruta (o vacio).
  std::string Find(uint32_t w, uint32_t h, uint64_t tex_hash, bool paletted, uint64_t tlut_hash, uint32_t fmt) const {
    if (files_.empty()) return {};
    char key[96];
    if (paletted) {
      std::snprintf(key, sizeof(key), "%ux%u_%016llx_%016llx_%u", w, h, (unsigned long long)tex_hash, (unsigned long long)tlut_hash, fmt);
      auto it = files_.find(key); if (it != files_.end()) return it->second;
      std::snprintf(key, sizeof(key), "%ux%u_%016llx_$_%u", w, h, (unsigned long long)tex_hash, fmt);
      it = files_.find(key); if (it != files_.end()) return it->second;
      return {};
    }
    std::snprintf(key, sizeof(key), "%ux%u_%016llx_%u", w, h, (unsigned long long)tex_hash, fmt);
    auto it = files_.find(key); return it != files_.end() ? it->second : std::string();
  }
};

// Imagenes HD decodificadas. Se decodifican en hilos aparte (los PNG del paquete
// llegan a 8192x2048 y tardan mas de un segundo): mientras tanto el juego muestra la
// textura original y la HD se pone cuando esta lista. Se reducen a 4 veces el tamano
// original como mucho (y 4096 de lado): a mas no se nota y pesan mucho.
struct Decoded { std::vector<uint8_t> rgba; int w = 0, h = 0; };

namespace detail {
struct AsyncState {
  std::mutex m; std::condition_variable cv;
  std::unordered_map<std::string, std::shared_ptr<const Decoded>> cache;
  std::vector<std::string> order;   // mas antiguo primero
  size_t bytes = 0;
  std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> pending;   // ruta -> tamano original
  std::vector<std::string> queue;
  std::unordered_map<std::string, bool> failed;
  bool started = false;
};
inline AsyncState& A() { static AsyncState* a = new AsyncState; return *a; }   // nunca se destruye: los hilos siguen esperando al salir
inline void Downscale(Decoded& d, uint32_t ow, uint32_t oh) {
  while ((uint32_t(d.w) > ow * 4 || d.w > 4096 || uint32_t(d.h) > oh * 4 || d.h > 4096) && d.w >= 2 && d.h >= 2 && d.w % 2 == 0 && d.h % 2 == 0) {
    const int nw = d.w / 2, nh = d.h / 2;
    std::vector<uint8_t> o(size_t(nw) * nh * 4);
    for (int y = 0; y < nh; ++y) {
      const uint8_t* r0 = d.rgba.data() + size_t(2 * y) * d.w * 4; const uint8_t* r1 = r0 + size_t(d.w) * 4;
      uint8_t* q = o.data() + size_t(y) * nw * 4;
      for (int x = 0; x < nw; ++x, q += 4)
        for (int c = 0; c < 4; ++c) q[c] = uint8_t((r0[8 * x + c] + r0[8 * x + 4 + c] + r1[8 * x + c] + r1[8 * x + 4 + c] + 2) >> 2);
    }
    d.rgba.swap(o); d.w = nw; d.h = nh;
  }
}
inline void Worker() {
  auto& a = A();
  for (;;) {
    std::string path; std::pair<uint32_t, uint32_t> orig;
    { std::unique_lock<std::mutex> l(a.m);
      a.cv.wait(l, [&a] { return !a.queue.empty(); });
      path = a.queue.back(); a.queue.pop_back();   // lo ultimo pedido, primero (lo que se ve ahora)
      orig = a.pending[path]; }
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto d = std::make_shared<Decoded>();
    const bool ok = png::Decode(file, d->rgba, d->w, d->h);
    if (ok) Downscale(*d, orig.first, orig.second);
    std::lock_guard<std::mutex> l(a.m);
    a.pending.erase(path);
    if (!ok) { a.failed[path] = true; std::printf("[TexturasHD] no se pudo leer %s\n", path.c_str()); continue; }
    a.bytes += d->rgba.size();
    while (a.bytes > (size_t(1) << 30) && !a.order.empty()) {
      auto old = a.cache.find(a.order.front());
      if (old != a.cache.end()) { a.bytes -= old->second->rgba.size(); a.cache.erase(old); }
      a.order.erase(a.order.begin());
    }
    a.cache[path] = d; a.order.push_back(path);
  }
}
}  // namespace detail

// Lista (o null) sin esperar; si no esta, se pide a los hilos de decodificacion.
inline std::shared_ptr<const Decoded> GetOrRequest(const std::string& path, uint32_t ow, uint32_t oh) {
  auto& a = detail::A();
  std::lock_guard<std::mutex> l(a.m);
  auto it = a.cache.find(path);
  if (it != a.cache.end()) return it->second;
  if (a.failed.count(path) || a.pending.count(path)) return nullptr;
  if (!a.started) {
    a.started = true;
    const unsigned n = std::max(1u, std::min(3u, std::thread::hardware_concurrency() / 4));
    for (unsigned i = 0; i < n; ++i) std::thread(detail::Worker).detach();
  }
  a.pending[path] = {ow, oh}; a.queue.push_back(path);
  a.cv.notify_one();
  return nullptr;
}
inline bool Failed(const std::string& path) { auto& a = detail::A(); std::lock_guard<std::mutex> l(a.m); return a.failed.count(path) != 0; }

// Calcula el nombre como Dolphin y busca la textura HD. Devuelve la imagen si ya esta
// decodificada; si no, null y *pending = ruta (se esta decodificando).
// data/size: nivel base en memoria del juego. tlut: paleta (big-endian) o null.
inline std::shared_ptr<const Decoded> Load(const uint8_t* data, size_t size, const uint8_t* tlut, size_t tlut_size,
                                           uint32_t w, uint32_t h, uint32_t fmt, std::string* pending = nullptr) {
  Pack& pack = Pack::get();
  pack.Scan();
  if (!pack.size() || !data || !size) return nullptr;
  uint64_t tlut_hash = 0;
  const bool paletted = tlut && tlut_size;
  if (paletted) {
    uint32_t mn = 0xffff, mx = 0;
    if (tlut_size == 16 * 2) { for (size_t i = 0; i < size; ++i) { const uint32_t lo = data[i] & 15, hi = data[i] >> 4; mn = std::min({mn, lo, hi}); mx = std::max({mx, lo, hi}); } }
    else if (tlut_size == 256 * 2) { for (size_t i = 0; i < size; ++i) { mn = std::min<uint32_t>(mn, data[i]); mx = std::max<uint32_t>(mx, data[i]); } }
    else { for (size_t i = 0; i + 1 < size; i += 2) { const uint32_t v = ((uint32_t(data[i]) << 8) | data[i + 1]) & 0x3fff; mn = std::min(mn, v); mx = std::max(mx, v); } }
    if (mx >= mn) {
      const size_t off = size_t(mn) * 2, n = size_t(mx + 1 - mn) * 2;
      tlut_hash = (off + n <= tlut_size) ? XXH64(tlut + off, n) : XXH64(tlut, tlut_size);
    }
  }
  const uint64_t tex_hash = XXH64(data, size);
  const std::string path = pack.Find(w, h, tex_hash, paletted, tlut_hash, fmt);
  if (path.empty()) return nullptr;
  auto d = GetOrRequest(path, w, h);
  if (!d) { if (pending && !Failed(path)) *pending = path; return nullptr; }
  static int logged = 0;
  if (logged < 20) { ++logged; std::printf("[TexturasHD] %ux%u fmt %u -> %s (%dx%d)\n", w, h, fmt, path.c_str(), d->w, d->h); }
  return d;
}

}  // namespace hd_textures
