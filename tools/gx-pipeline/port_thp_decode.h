#pragma once
// tools/gx-pipeline: decodificador nativo de cuadros de video THP (JPEG de base 4:2:0
// sin relleno de bytes 0xFF) con salida en texturas I8 en mosaico de GX (8x4), como
// THPVideoDecode del SDK. Sin dependencias del runtime: se prueba aparte.
#include <cmath>
#include <cstdint>
#include <cstring>

namespace port_thp {

struct Huff {
  uint8_t vals[256] = {};
  int maxcode[18] = {}, valptr[17] = {}, mincode[17] = {};
  uint16_t look[512] = {};   // (longitud << 8) | valor para codigos de <= 9 bits; 0 = mas largo
  bool ok = false;
  void Build(const uint8_t counts[16], const uint8_t* symbols, int nsym) {
    std::memcpy(vals, symbols, size_t(nsym));
    std::memset(look, 0, sizeof(look));
    int code = 0, k = 0;
    for (int l = 1; l <= 16; ++l) {
      valptr[l] = k; mincode[l] = code;
      for (int i = 0; i < counts[l - 1]; ++i, ++k, ++code) {
        if (l <= 9) {
          const int shift = 9 - l;
          for (int f = 0; f < (1 << shift); ++f) look[(code << shift) | f] = uint16_t((l << 8) | vals[k]);
        }
      }
      maxcode[l] = counts[l - 1] ? code - 1 : -1;
      code <<= 1;
    }
    maxcode[17] = 0x7fffffff;
    ok = true;
  }
};

struct Bits {
  const uint8_t* p; const uint8_t* end; uint32_t acc = 0; int n = 0;
  void Fill() { while (n <= 24) { const uint32_t b = p < end ? *p++ : 0; acc |= b << (24 - n); n += 8; } }
  uint32_t Get(int k) { if (!k) return 0; Fill(); const uint32_t v = acc >> (32 - k); acc <<= k; n -= k; return v; }
  int Decode(const Huff& h) {
    Fill();
    const uint16_t e = h.look[acc >> 23];
    if (e) { const int l = e >> 8; acc <<= l; n -= l; return e & 0xFF; }
    for (int l = 10; l <= 16; ++l) {
      const int code = int(acc >> (32 - l));
      if (code <= h.maxcode[l]) { acc <<= l; n -= l; const int idx = h.valptr[l] + code - h.mincode[l]; return idx >= 0 && idx < 256 ? h.vals[idx] : 0; }
    }
    acc <<= 16; n -= 16; return 0;   // codigo invalido
  }
  // Para los marcadores de reinicio: descarta los bits de este byte y salta FF Dn.
  void Restart() {
    const int bytes_left = n / 8;   // bytes leidos por adelantado
    p -= bytes_left; acc = 0; n = 0;
    if (p + 1 < end && p[0] == 0xFF && p[1] >= 0xD0 && p[1] <= 0xD7) p += 2;
  }
};
inline int Extend(uint32_t v, int s) { return s && v < (1u << (s - 1)) ? int(v) - (1 << s) + 1 : int(v); }

static const uint8_t kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                                    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

inline const float* IdctTable() {
  static float t[64];
  static bool init = false;
  if (!init) {
    for (int x = 0; x < 8; ++x)
      for (int u = 0; u < 8; ++u) t[x * 8 + u] = float((u == 0 ? 1.0 / std::sqrt(2.0) : 1.0) * 0.5 * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
    init = true;
  }
  return t;
}
// Coeficientes (orden natural, ya descuantizados) -> 8x8 bytes con +128 y saturacion.
inline void Idct(const float in[64], uint8_t out[64]) {
  const float* c = IdctTable();
  float tmp[64];
  for (int v = 0; v < 8; ++v) {          // filas: frecuencia u -> x
    const float* r = in + v * 8;
    for (int x = 0; x < 8; ++x) {
      const float* cx = c + x * 8;
      tmp[v * 8 + x] = r[0] * cx[0] + r[1] * cx[1] + r[2] * cx[2] + r[3] * cx[3] + r[4] * cx[4] + r[5] * cx[5] + r[6] * cx[6] + r[7] * cx[7];
    }
  }
  for (int x = 0; x < 8; ++x) {          // columnas: frecuencia v -> y
    for (int y = 0; y < 8; ++y) {
      const float* cy = c + y * 8;
      float s = 0;
      for (int v = 0; v < 8; ++v) s += tmp[v * 8 + x] * cy[v];
      int px = int(std::lround(s + 128.0f));
      out[y * 8 + x] = uint8_t(px < 0 ? 0 : px > 255 ? 255 : px);
    }
  }
}
// Byte (x, y) de una textura I8 en mosaico de 8x4 con ancho w (multiplo de 8).
inline size_t TileOffset(int w, int x, int y) { return (size_t(y >> 2) * size_t(w >> 3) + size_t(x >> 3)) * 32 + size_t(y & 3) * 8 + size_t(x & 7); }

struct Frame { int w = 0, h = 0; };

// data: cuadro (desde FF D8). outY/outU/outV: texturas de w*h y (w/2)*(h/2) bytes.
// Devuelve false si el cuadro usa algo no soportado (el llamador usa el decodificador del juego).
// 'probe' solo lee la cabecera (w, h) sin decodificar.
inline bool DecodeFrame(const uint8_t* data, size_t size, uint8_t* outY, uint8_t* outU, uint8_t* outV, Frame* info, bool probe = false) {
  const uint8_t* p = data; const uint8_t* end = data + size;
  uint8_t q[4][64] = {};
  Huff dc[4], ac[4];
  int w = 0, h = 0, ncomp = 0, restart = 0;
  struct Comp { int id, hs, vs, tq, td, ta; } comp[3] = {};
  auto rd16 = [](const uint8_t* b) { return (int(b[0]) << 8) | b[1]; };
  for (;;) {
    while (p < end && *p != 0xFF) ++p;
    while (p < end && *p == 0xFF) ++p;
    if (p >= end) return false;
    const uint8_t m = *p++;
    if (m == 0xD8) continue;              // SOI
    if (m == 0xD9) return false;          // EOI sin imagen
    if (p + 2 > end) return false;
    const int len = rd16(p);
    const uint8_t* seg = p + 2; const uint8_t* segend = p + len;
    if (len < 2 || segend > end) return false;
    if (m == 0xDB) {                      // DQT
      while (seg < segend) {
        const int pq = *seg >> 4, tq = *seg & 15; ++seg;
        if (pq != 0 || tq > 3 || seg + 64 > segend) return false;
        std::memcpy(q[tq], seg, 64); seg += 64;
      }
    } else if (m == 0xC0) {               // SOF0
      if (len < 8 || seg[0] != 8) return false;
      h = rd16(seg + 1); w = rd16(seg + 3); ncomp = seg[5];
      if (ncomp != 3 || len < 8 + 3 * 3) return false;
      for (int i = 0; i < 3; ++i) { comp[i].id = seg[6 + i * 3]; comp[i].hs = seg[7 + i * 3] >> 4; comp[i].vs = seg[7 + i * 3] & 15; comp[i].tq = seg[8 + i * 3] & 3; }
      if (comp[0].hs != 2 || comp[0].vs != 2 || comp[1].hs != 1 || comp[1].vs != 1 || comp[2].hs != 1 || comp[2].vs != 1) return false;
      if (w <= 0 || h <= 0 || (w & 15) || (h & 15) || w > 1024 || h > 1024) return false;
      if (info) { info->w = w; info->h = h; }
      if (probe) return true;
    } else if (m == 0xC4) {               // DHT
      while (seg + 17 <= segend) {
        const int tc = *seg >> 4, th = *seg & 15; ++seg;
        uint8_t counts[16]; int total = 0;
        for (int i = 0; i < 16; ++i) { counts[i] = seg[i]; total += seg[i]; }
        seg += 16;
        if (th > 3 || tc > 1 || total > 256 || seg + total > segend) return false;
        (tc ? ac[th] : dc[th]).Build(counts, seg, total);
        seg += total;
      }
    } else if (m == 0xDD) {               // DRI
      if (len >= 4) restart = rd16(seg);
    } else if (m == 0xDA) {               // SOS: datos hasta el final (sin relleno de 0xFF)
      if (!w || seg[0] != 3) return false;
      for (int i = 0; i < 3; ++i) {
        const int id = seg[1 + i * 2];
        int k = 0; while (k < 3 && comp[k].id != id) ++k;
        if (k == 3) return false;
        comp[k].td = seg[2 + i * 2] >> 4; comp[k].ta = seg[2 + i * 2] & 15;
      }
      for (int k = 0; k < 3; ++k) if (comp[k].td > 3 || comp[k].ta > 3 || !dc[comp[k].td].ok || !ac[comp[k].ta].ok) return false;
      Bits b{segend, end};
      int pred[3] = {0, 0, 0};
      const int mw = w / 16, mh = h / 16, cw = w / 2;
      float coef[64]; uint8_t blk[64];
      int until_restart = restart;
      for (int my = 0; my < mh; ++my)
        for (int mx = 0; mx < mw; ++mx) {
          if (restart) {
            if (until_restart == 0) { b.Restart(); pred[0] = pred[1] = pred[2] = 0; until_restart = restart; }
            --until_restart;
          }
          for (int bi = 0; bi < 6; ++bi) {
            const int ci = bi < 4 ? 0 : bi - 3;
            const Comp& c = comp[ci];
            const uint8_t* qt = q[c.tq];
            std::memset(coef, 0, sizeof(coef));
            const int s = b.Decode(dc[c.td]);
            pred[ci] += Extend(b.Get(s), s);
            coef[0] = float(pred[ci] * qt[0]);
            for (int k = 1; k < 64;) {
              const int rs = b.Decode(ac[c.ta]);
              const int r = rs >> 4, sz = rs & 15;
              if (!sz) { if (r == 15) { k += 16; continue; } break; }
              k += r;
              if (k > 63) break;
              coef[kZigzag[k]] = float(Extend(b.Get(sz), sz) * qt[k]);
              ++k;
            }
            Idct(coef, blk);
            if (ci == 0) {
              const int x0 = mx * 16 + (bi & 1) * 8, y0 = my * 16 + (bi >> 1) * 8;
              for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) outY[TileOffset(w, x0 + x, y0 + y)] = blk[y * 8 + x];
            } else {
              uint8_t* o = ci == 1 ? outU : outV;
              const int x0 = mx * 8, y0 = my * 8;
              for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) o[TileOffset(cw, x0 + x, y0 + y)] = blk[y * 8 + x];
            }
          }
        }
      return true;
    }
    p = segend;
  }
}

}  // namespace port_thp
