// tools/gx-pipeline (make_patch): mezcla de GX completa, como la consola. El original solo
// hacia la mezcla normal y la resta. Faltaban tres cosas del PE que pueden aclarar la imagen:
//  - operaciones logicas (BP 0x41 bit 1, modo en bits 12-15; GX_BM_LOGIC): se pintaba copiando;
//  - alfa constante (BP 0x42 bit 8, valor en bits 0-7): el alfa que se guarda es esa constante,
//    no el del sombreador; lo que mezcla despues con el alfa de destino salia mal;
//  - formatos del EFB sin alfa (BP 0x43: solo RGBA6_Z24 tiene alfa): el alfa de destino vale 1
//    y no se escribe; el port guardaba y leia un alfa que la consola no tiene.
// NWII_BLEND_LEGACY=1 vuelve a la mezcla original. Cada 5 s, si algo de esto aparece, se
// registra "[Mezcla] ...".
static uint64_t s_mix_logic = 0, s_mix_const = 0, s_mix_dsta = 0, s_mix_draws = 0;
static uint32_t s_mix_logic_modes = 0, s_mix_fmts = 0;
static void PortMixReport() {
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(5)) return;
  last = now;
  if (s_mix_logic | s_mix_const | s_mix_dsta)
    std::printf("[Mezcla] dibujos=%llu logicas=%llu (modos %04X) alfa_constante=%llu alfa_destino_sin_alfa=%llu formatos_efb=%02X\n",
                (unsigned long long)s_mix_draws, (unsigned long long)s_mix_logic, s_mix_logic_modes,
                (unsigned long long)s_mix_const, (unsigned long long)s_mix_dsta, s_mix_fmts);
  s_mix_logic = s_mix_const = s_mix_dsta = s_mix_draws = 0; s_mix_logic_modes = 0; s_mix_fmts = 0;
}
static void ApplyBlend() {
  static const bool legacy = std::getenv("NWII_BLEND_LEGACY") != nullptr;
  const uint32_t cm = g_state.bp[0x41];
  const uint32_t fmt = g_state.bp[0x43] & 7;
  const bool has_alpha = legacy || fmt == 1;
  const uint32_t cm1 = g_state.bp[0x42];
  bool enable = cm & 1;
  const bool subtract = (cm >> 11) & 1;
  const bool logic = !legacy && !enable && (cm & 2);
  const bool const_alpha = !legacy && !logic && has_alpha && (cm & 0x10) && (cm1 & 0x100);
  int dstF = (cm >> 5) & 7;
  int srcF = (cm >> 8) & 7;
  ++s_mix_draws; s_mix_fmts |= 1u << fmt;
  PortMixReport();
  const GLboolean color = (cm & 8) ? GL_TRUE : GL_FALSE;
  glColorMask(color, color, color, ((cm & 0x10) && has_alpha) ? GL_TRUE : GL_FALSE);
  if (logic) {
    static const GLenum ops[16] = {
        GL_CLEAR, GL_AND, GL_AND_REVERSE, GL_COPY, GL_AND_INVERTED, GL_NOOP, GL_XOR, GL_OR,
        GL_NOR, GL_EQUIV, GL_INVERT, GL_OR_REVERSE, GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND, GL_SET};
    const uint32_t op = (cm >> 12) & 15;
    ++s_mix_logic; s_mix_logic_modes |= 1u << op;
    glDisable(GL_BLEND);
    glEnable(GL_COLOR_LOGIC_OP);
    glLogicOp(ops[op]);
    return;
  }
  glDisable(GL_COLOR_LOGIC_OP);
  if (!enable && !const_alpha) { glDisable(GL_BLEND); return; }
  static const GLenum srcMap[8] = {
      GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
      GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
  static const GLenum dstMap[8] = {
      GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
      GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
  GLenum s = GL_ONE, d = GL_ZERO, eq = GL_FUNC_ADD;
  if (enable && subtract) { eq = GL_FUNC_REVERSE_SUBTRACT; s = GL_ONE; d = GL_ONE; }
  else if (enable) {
    s = srcMap[srcF]; d = dstMap[dstF];
    if (!has_alpha) {   // sin alfa en el EFB el alfa de destino vale 1
      if (srcF >= 6 || dstF >= 6) ++s_mix_dsta;
      if (srcF == 6) s = GL_ONE; else if (srcF == 7) s = GL_ZERO;
      if (dstF == 6) d = GL_ONE; else if (dstF == 7) d = GL_ZERO;
    }
  }
  glEnable(GL_BLEND);
  if (const_alpha) {
    ++s_mix_const;
    glBlendColor(0.f, 0.f, 0.f, float(cm1 & 0xFF) / 255.f);
    glBlendEquationSeparate(eq, GL_FUNC_ADD);
    glBlendFuncSeparate(s, d, GL_CONSTANT_ALPHA, GL_ZERO);
  } else {
    glBlendEquation(eq);
    glBlendFunc(s, d);
  }
}
