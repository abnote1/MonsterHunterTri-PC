
// tools/gx-pipeline (make_patch): volcado de un cuadro para investigar el brillo. Se pide con F10.
// En logs\volcado-<hora>\ deja cuadro.txt (cada dibujo del cuadro en orden: 2D/3D, mezcla, formato
// del EFB, TEV, texturas; el estado completo de registros cada vez que cambia) e imagenes del EFB
// (640x480, a la izquierda el color y a la derecha el alfa en gris) al pasar de 3D a 2D, tras cada
// dibujo 2D, tras cada copia del EFB y al final. Solo afecta al cuadro pedido.
static bool s_dump = false;
static FILE* s_dump_f = nullptr;
static std::string s_dump_dir;
static int s_dump_img = 0, s_dump_idx = 0;
static bool s_dump_prev3d = false;
static std::unordered_map<uint64_t, int> s_dump_states;

static void DumpWriteBmp(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const int ow = w * 2, row = ((ow * 3 + 3) / 4) * 4, size = 54 + row * h;
  uint8_t hd[54] = {'B', 'M'};
  auto le32 = [&](int o, uint32_t v) { hd[o] = uint8_t(v); hd[o + 1] = uint8_t(v >> 8); hd[o + 2] = uint8_t(v >> 16); hd[o + 3] = uint8_t(v >> 24); };
  le32(2, uint32_t(size)); le32(10, 54); le32(14, 40); le32(18, uint32_t(ow)); le32(22, uint32_t(h));
  hd[26] = 1; hd[28] = 24; le32(34, uint32_t(row * h));
  std::fwrite(hd, 1, 54, f);
  std::vector<uint8_t> line(size_t(row), 0);
  for (int y = 0; y < h; ++y) {   // GL y BMP van de abajo arriba
    for (int x = 0; x < w; ++x) {
      const uint8_t* p = &rgba[size_t(y * w + x) * 4];
      uint8_t* c = &line[size_t(x) * 3];   c[0] = p[2]; c[1] = p[1]; c[2] = p[0];
      uint8_t* a = &line[size_t(w + x) * 3]; a[0] = a[1] = a[2] = p[3];
    }
    std::fwrite(line.data(), 1, line.size(), f);
  }
  std::fclose(f);
}

static void DumpSnap(const char* tag) {
  if (!s_dump || s_dump_img >= 60) return;
  // el lote pendiente ya lo ha dibujado quien llama (Render)
  const int W = 640 * EFBScale(), H = 480 * EFBScale();
  static GLuint fbo = 0, rb = 0;
  if (!fbo) {
    glGenFramebuffers(1, &fbo); glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 640, 480);
  }
  GLint rd = 0, dr = 0; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rd); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dr);
  GLboolean cmask[4]; glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
  const GLboolean sc = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_SCISSOR_TEST); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(dr));
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
  glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
  glBlitFramebuffer(0, 0, W, H, 0, 0, 640, 480, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  std::vector<uint8_t> px(size_t(640) * 480 * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, 640, 480, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(rd)); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(dr));
  glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
  if (sc) glEnable(GL_SCISSOR_TEST);
  char name[64]; std::snprintf(name, sizeof name, "img%02d.bmp", s_dump_img);
  DumpWriteBmp(s_dump_dir + "/" + name, px, 640, 480);
  std::fprintf(s_dump_f, "  >> %s  (%s, tras el dibujo %d)\n", name, tag, s_dump_idx);
  ++s_dump_img;
}

static void DumpBegin() {
  char stamp[32]; const std::time_t t = std::time(nullptr); std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&t));
  s_dump_dir = std::string("logs/volcado-") + stamp;
#ifdef _WIN32
  _mkdir("logs"); _mkdir(s_dump_dir.c_str());
#else
  mkdir("logs", 0755); mkdir(s_dump_dir.c_str(), 0755);
#endif
  s_dump_f = std::fopen((s_dump_dir + "/cuadro.txt").c_str(), "w");
  if (!s_dump_f) { s_dump = false; return; }
  s_dump_img = 0; s_dump_idx = 0; s_dump_prev3d = false; s_dump_states.clear();
  std::fprintf(s_dump_f, "Volcado de un cuadro. Escala %d. bloom_omitido=%d\n", EFBScale(), int(port_menu::g_no_bloom.load()));
  std::printf("[Volcado] guardando un cuadro en %s\n", s_dump_dir.c_str());
}

static void DumpEnd() {
  if (!s_dump) return;
  DumpSnap("final del cuadro");
  std::fprintf(s_dump_f, "Fin: %d dibujos, %d imagenes, %zu estados distintos\n", s_dump_idx, s_dump_img, s_dump_states.size());
  std::fclose(s_dump_f); s_dump_f = nullptr; s_dump = false;
  std::printf("[Volcado] listo\n");
}

static void DumpEvent(const char* what, uint32_t a, uint32_t b, uint32_t c) {
  if (!s_dump) return;
  std::fprintf(s_dump_f, "== %s %08X %u x %u (tras el dibujo %d) 52=%06X 53=%06X 54=%06X 01=%06X 02=%06X 03=%06X 04=%06X 49=%06X 4A=%06X 4F=%06X 50=%06X 51=%06X\n", what, a, b, c, s_dump_idx,
               g_state.bp[0x52], g_state.bp[0x53], g_state.bp[0x54], g_state.bp[0x01], g_state.bp[0x02], g_state.bp[0x03], g_state.bp[0x04], g_state.bp[0x49], g_state.bp[0x4A], g_state.bp[0x4F], g_state.bp[0x50], g_state.bp[0x51]);
}

static void DumpBeforeDraw(const GXCommand& cmd) {
  if (!s_dump) return;
  const bool p3d = CurrentIsPerspective();
  if (!p3d && s_dump_prev3d) DumpSnap("paso de 3D a 2D");
  s_dump_prev3d = p3d;
  // estado: registros de pixel, TEV, texturas, indirectas, niebla, canales
  static const int regs[] = {0x00, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
                             0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x27,
                             0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x40, 0x41, 0x42, 0x43,
                             0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xF3, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD};
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
  for (int r : regs) mix(g_state.bp[r]);
  for (int r = 0xC0; r < 0xC0 + 2 * 16; ++r) mix(g_state.bp[r]);
  mix(g_state.numTevStages); mix(g_state.numTexGens); mix(g_state.numChans);
  for (int r = 0x9; r <= 0x12; ++r) mix(g_state.xf[r]);
  for (int i = 0; i < 8; ++i) { mix(g_state.texStages[i].base_addr); mix(g_state.texStages[i].format); }
  for (int i = 0; i < 8; ++i) { mix(g_state.tevKonstWords[i]); mix(g_state.tevColorWords[i]); }
  auto it = s_dump_states.find(h);
  const bool fresh = it == s_dump_states.end();
  const int sid = fresh ? int(s_dump_states.size()) : it->second;
  if (fresh) s_dump_states[h] = sid;
  ++s_dump_idx;
  std::fprintf(s_dump_f, "D%d %s v=%zu estado=%d mezcla=%06X alfa_const=%06X efb_fmt=%u z=%06X\n", s_dump_idx, p3d ? "3D" : "2D",
               cmd.vertices.size(), sid, g_state.bp[0x41], g_state.bp[0x42], g_state.bp[0x43] & 7, g_state.bp[0x40]);
  if (!fresh) return;
  std::fprintf(s_dump_f, "  ESTADO %d: etapas=%u gens=%u canales=%u\n", sid, unsigned(g_state.numTevStages), unsigned(g_state.numTexGens), unsigned(g_state.numChans));
  std::fprintf(s_dump_f, "   bp:");
  for (int r : regs) std::fprintf(s_dump_f, " %02X=%06X", r, g_state.bp[r]);
  std::fprintf(s_dump_f, "\n   tev:");
  for (unsigned i = 0; i < g_state.numTevStages && i < 16; ++i)
    std::fprintf(s_dump_f, " [%u C=%06X A=%06X]", i, g_state.bp[0xC0 + 2 * i], g_state.bp[0xC1 + 2 * i]);
  std::fprintf(s_dump_f, "\n   xf09-12:");
  for (int r = 0x9; r <= 0x12; ++r) std::fprintf(s_dump_f, " %08X", g_state.xf[r]);
  std::fprintf(s_dump_f, "\n   konst:");
  for (int i = 0; i < 8; ++i) std::fprintf(s_dump_f, " %06X", g_state.tevKonstWords[i]);
  std::fprintf(s_dump_f, " regs:");
  for (int i = 0; i < 8; ++i) std::fprintf(s_dump_f, " %06X", g_state.tevColorWords[i]);
  std::fprintf(s_dump_f, "\n");
  for (int i = 0; i < 8; ++i) {
    const auto& ts = g_state.texStages[i];
    if (!ts.base_addr) continue;
    std::fprintf(s_dump_f, "   tex%d %08X %ux%u fmt=%u copia_efb=%d\n", i, ts.base_addr, unsigned(ts.width), unsigned(ts.height), unsigned(ts.format),
                 int(nwii::runtime::hle::TextureCache::get().find_efb_texture(ts.base_addr) != 0));
  }
  if (!cmd.vertices.empty()) {
    const auto& v = cmd.vertices[0];
    std::fprintf(s_dump_f, "   v0 col=%.3f,%.3f,%.3f,%.3f uv=%.3f,%.3f\n", v.color[0], v.color[1], v.color[2], v.color[3], v.tex[0][0], v.tex[0][1]);
  }
}

static void DumpAfterDraw() {
  if (!s_dump) return;
  if (!CurrentIsPerspective()) DumpSnap("tras dibujo 2D");
}
