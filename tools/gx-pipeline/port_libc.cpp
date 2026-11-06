// tools/gx-pipeline: funciones de la biblioteca C del juego que el recompilador sustituyo mal.
//
// sscanf (0x8045F37C) quedo convertido en una llamada a OSReport: imprimia la cadena y devolvia
// basura. Lo usan el codigo de red del juego ("%d" de un parametro del servidor; con el fallo, el
// juego abandonaba el servidor con el error 0x800A0003) y DWC ("%llu" del userid que da el NAS,
// fecha y hora de la respuesta). Aqui va un sscanf propio que lee y escribe la memoria del juego.
// OSReport (os_hle_stubs.cpp) lo llama cuando ctx.pc == 0x8045F37C.
//
// Convenio PPC EABI: r3 = cadena, r4 = formato, punteros en r5..r10 y luego en la pila (r1+8...).
// Conversiones: d i u x X o s c [ ] n p % f e g a, con anchura, '*' y modificadores hh h l ll L j z t q.
#include "runtime/cpu_context.h"
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using nwii::runtime::CPUContext;

namespace {

struct Guest {
  CPUContext& c;
  uint32_t s;          // posicion en la cadena de entrada
  int argi = 0;
  int peek() { return c.mmu.read8(s); }
  int get() { const int ch = c.mmu.read8(s); if (ch) ++s; return ch; }
  uint32_t next_ptr() {
    const int i = argi++;
    if (i < 6) return c.gpr[5 + i];
    return c.mmu.read32(c.gpr[1] + 8 + uint32_t(i - 6) * 4);
  }
};

void StoreInt(CPUContext& c, uint32_t p, uint64_t v, int len) {
  switch (len) {
  case 1: c.mmu.write8(p, uint8_t(v)); break;          // hh
  case 2: c.mmu.write16(p, uint16_t(v)); break;        // h
  case 8: c.mmu.write32(p, uint32_t(v >> 32)); c.mmu.write32(p + 4, uint32_t(v)); break;   // ll
  default: c.mmu.write32(p, uint32_t(v)); break;       // int, long (32 bits en el Wii)
  }
}

}  // namespace

void PortGuestSscanf(CPUContext& c) {
  Guest g{c, c.gpr[3]};
  uint32_t f = c.gpr[4];
  int assigned = 0;
  bool any_input = false;   // se ha intentado alguna conversion con entrada
  bool failed = false;
  bool eof = false;        // se acabo la entrada antes de una conversion
  for (;;) {
    int fc = c.mmu.read8(f++);
    if (!fc) break;
    if (std::isspace(fc)) { while (std::isspace(g.peek())) g.get(); continue; }
    if (fc != '%') {
      if (g.peek() != fc) { failed = true; break; }
      g.get(); continue;
    }
    fc = c.mmu.read8(f++);
    if (fc == '%') { while (std::isspace(g.peek())) g.get(); if (g.peek() != '%') { failed = true; break; } g.get(); continue; }
    bool suppress = false;
    if (fc == '*') { suppress = true; fc = c.mmu.read8(f++); }
    int width = 0;
    while (fc >= '0' && fc <= '9') { width = width * 10 + (fc - '0'); fc = c.mmu.read8(f++); }
    int len = 4; bool dbl = false;
    if (fc == 'h') { fc = c.mmu.read8(f++); len = 2; if (fc == 'h') { fc = c.mmu.read8(f++); len = 1; } }
    else if (fc == 'l') { fc = c.mmu.read8(f++); dbl = true; if (fc == 'l') { fc = c.mmu.read8(f++); len = 8; } }
    else if (fc == 'L' || fc == 'q') { fc = c.mmu.read8(f++); len = 8; dbl = true; }
    else if (fc == 'j') { fc = c.mmu.read8(f++); len = 8; }
    else if (fc == 'z' || fc == 't') { fc = c.mmu.read8(f++); }
    if (!fc) break;
    if (width <= 0) width = 0x7FFFFFFF;

    if (fc == 'n') { if (!suppress) StoreInt(c, g.next_ptr(), g.s - c.gpr[3], len); continue; }

    if (fc == 'c') {
      if (width == 0x7FFFFFFF) width = 1;
      const uint32_t p = suppress ? 0 : g.next_ptr();
      int n = 0;
      while (n < width && g.peek()) { const int ch = g.get(); if (p) c.mmu.write8(p + uint32_t(n), uint8_t(ch)); ++n; }
      any_input = true;
      if (n < width) { failed = true; break; }
      if (!suppress) ++assigned;
      continue;
    }

    if (fc == '[') {
      bool set[256] = {};
      bool neg = false;
      int ch = c.mmu.read8(f++);
      if (ch == '^') { neg = true; ch = c.mmu.read8(f++); }
      if (ch == ']') { set[']'] = true; ch = c.mmu.read8(f++); }
      int prev = -1;
      while (ch && ch != ']') {
        if (ch == '-' && prev >= 0 && c.mmu.read8(f) && c.mmu.read8(f) != ']') {
          const int hi = c.mmu.read8(f++);
          for (int k = prev; k <= hi; ++k) set[k & 255] = true;
          prev = -1;
        } else { set[ch] = true; prev = ch; }
        ch = c.mmu.read8(f++);
      }
      const uint32_t p = suppress ? 0 : g.next_ptr();
      int n = 0;
      any_input = true;
      while (n < width && g.peek() && (set[g.peek()] != neg)) { const int k = g.get(); if (p) c.mmu.write8(p + uint32_t(n), uint8_t(k)); ++n; }
      if (!n) { failed = true; break; }
      if (p) c.mmu.write8(p + uint32_t(n), 0);
      if (!suppress) ++assigned;
      continue;
    }

    while (std::isspace(g.peek())) g.get();
    if (!g.peek()) { eof = true; break; }
    any_input = true;

    if (fc == 's') {
      const uint32_t p = suppress ? 0 : g.next_ptr();
      int n = 0;
      while (n < width && g.peek() && !std::isspace(g.peek())) { const int k = g.get(); if (p) c.mmu.write8(p + uint32_t(n), uint8_t(k)); ++n; }
      if (p) c.mmu.write8(p + uint32_t(n), 0);
      if (!suppress) ++assigned;
      continue;
    }

    // numeros: se copia el trozo que cabe en la anchura y se convierte con la libreria del PC
    std::string tok;
    while (int(tok.size()) < width && g.peek()) {
      const int ch = g.peek();
      bool ok;
      if (fc == 'f' || fc == 'e' || fc == 'g' || fc == 'E' || fc == 'G' || fc == 'a')
        ok = std::isdigit(ch) || ch == '.' || ch == 'e' || ch == 'E' || ch == 'x' || ch == 'X' ||
             (std::isxdigit(ch) && tok.find_first_of("xX") != std::string::npos) ||
             ((ch == '+' || ch == '-') && (tok.empty() || tok.back() == 'e' || tok.back() == 'E' || tok.back() == 'p' || tok.back() == 'P')) ||
             ((ch == 'p' || ch == 'P') && tok.find_first_of("xX") != std::string::npos);
      else if (fc == 'x' || fc == 'X' || fc == 'p')
        ok = std::isxdigit(ch) || ((ch == '+' || ch == '-') && tok.empty()) ||
             ((ch == 'x' || ch == 'X') && (tok == "0" || tok == "+0" || tok == "-0"));
      else if (fc == 'o') ok = (ch >= '0' && ch <= '7') || ((ch == '+' || ch == '-') && tok.empty());
      else if (fc == 'i')
        ok = std::isxdigit(ch) || ((ch == '+' || ch == '-') && tok.empty()) ||
             ((ch == 'x' || ch == 'X') && (tok == "0" || tok == "+0" || tok == "-0"));
      else ok = std::isdigit(ch) || ((ch == '+' || ch == '-') && tok.empty());
      if (!ok) break;
      tok += char(g.get());
    }
    if (tok.empty() || tok == "+" || tok == "-") { failed = true; break; }
    if (suppress) continue;
    const uint32_t p = g.next_ptr();
    if (fc == 'f' || fc == 'e' || fc == 'g' || fc == 'E' || fc == 'G' || fc == 'a') {
      const double v = std::strtod(tok.c_str(), nullptr);
      if (dbl) { uint64_t b; std::memcpy(&b, &v, 8); c.mmu.write32(p, uint32_t(b >> 32)); c.mmu.write32(p + 4, uint32_t(b)); }
      else { const float fv = float(v); uint32_t b; std::memcpy(&b, &fv, 4); c.mmu.write32(p, b); }
    } else {
      const int base = (fc == 'x' || fc == 'X' || fc == 'p') ? 16 : fc == 'o' ? 8 : fc == 'i' ? 0 : 10;
      uint64_t v;
      if (fc == 'd' || fc == 'i') v = uint64_t(std::strtoll(tok.c_str(), nullptr, base));
      else v = std::strtoull(tok.c_str(), nullptr, base);
      StoreInt(c, p, v, len);
    }
    ++assigned;
  }
  (void)any_input; (void)failed;
  const int32_t ret = (eof && assigned == 0) ? -1 : assigned;
  static int shown = 0;
  if (shown < 40) {
    ++shown;
    std::string fmt, in;
    for (uint32_t i = 0; i < 32; ++i) { const int ch = c.mmu.read8(c.gpr[4] + i); if (!ch) break; fmt += char(ch); }
    for (uint32_t i = 0; i < 32; ++i) { const int ch = c.mmu.read8(c.gpr[3] + i); if (!ch) break; in += char(ch); }
    std::printf("[libc] sscanf(\"%s\", \"%s\") -> %d\n", in.c_str(), fmt.c_str(), ret);
  }
  c.gpr[3] = uint32_t(ret);
}
