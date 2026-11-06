#pragma once
// tools/gx-pipeline: menu del port. Se abre con la tecla P o con Select(Back)+Start
// del mando. Mientras esta abierto el juego queda en pausa (el invitado espera en
// process_pending_callbacks y el reloj del invitado no avanza), el sonido calla y
// el juego no recibe botones. Ajustes guardados en ajustes_port.ini (carpeta del
// proyecto). Solo lo incluye main.cpp.
#include "port_menu_state.h"
#include "port_menu_font.h"
#include "hd_textures.h"
#include "port_cloud.h"
#include "port_lang.h"
#include "runtime/presentation.h"
bool PortNetOnline();   // port_net.cpp: hay una conexion TCP abierta con el servidor
#include "runtime/config.h"
#include <SDL.h>
#include <glad/glad.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <filesystem>

namespace port_menu {

struct Settings {
  int scale = 2;            // resolucion interna 1x..5x (en vivo)
  bool bloom = false;       // resplandor
  bool vsync = true;
  bool fullscreen = false;
  int aspect = 1;           // 0 = 4:3, 1 = 16:9, 2 = 21:9, 3 = 32:9
  int tex_filter = 0;       // 0 original, 1 trilineal, 2..5 anisotropico 2x..16x
  bool fxaa = false;        // antialiasing FXAA
  bool hd = false;          // texturas HD (carpeta texturas_hd)
  bool keep_aspect = true;
  bool sharp = false;
  bool fps = true;
  int volume = 100;
  int lang = -1;            // idioma del juego (port_lang.h), -1 = sin cambiar
  bool cloud_auto = true;   // nube: subida automatica
};

inline Settings& settings() { static Settings s; return s; }
inline int& StartupLang() { static int l = -1; return l; }  // idioma con el que arranco el juego
// En la Steam Deck (Steam pone SteamDeck=1) se usan ajustes propios, para que la misma
// carpeta sirva en el PC y en la Deck con resolucion y pantalla distintas.
inline bool OnSteamDeck() { const char* v = std::getenv("SteamDeck"); return v && v[0] == '1'; }
inline const char* SettingsFile() { return OnSteamDeck() ? "ajustes_port_deck.ini" : "ajustes_port.ini"; }
#define kSettingsFile SettingsFile()

inline void SaveSettings() {
  const Settings& s = settings();
  std::ofstream f(kSettingsFile, std::ios::trunc);
  if (!f) return;
  f << "# Ajustes del menu del port (tecla P / Select+Start). Se pueden editar a mano.\n"
    << "escala=" << s.scale << "\n"
    << "resplandor=" << (s.bloom ? 1 : 0) << "\n"
    << "vsync=" << (s.vsync ? 1 : 0) << "\n"
    << "pantalla_completa=" << (s.fullscreen ? 1 : 0) << "\n"
    << "aspecto=" << s.aspect << "\n"
    << "filtrado=" << s.tex_filter << "\n"
    << "fxaa=" << (s.fxaa ? 1 : 0) << "\n"
    << "texturas_hd=" << (s.hd ? 1 : 0) << "\n"
    << "mantener_proporcion=" << (s.keep_aspect ? 1 : 0) << "\n"
    << "pixeles_nitidos=" << (s.sharp ? 1 : 0) << "\n"
    << "contador_fps=" << (s.fps ? 1 : 0) << "\n"
    << "volumen=" << s.volume << "\n"
    << "nube_auto=" << (s.cloud_auto ? 1 : 0) << "\n";
  if (s.lang >= 0) f << "idioma=" << port_lang::Code(s.lang) << "\n";
}

// Relacion de aspecto: 4:3 y 16:9 son nativas del juego; 21:9 y 32:9 ensanchan
// las proyecciones en perspectiva 16:9 (el juego sigue en 16:9 y la imagen se
// presenta mas ancha; el HUD se estira en horizontal).
inline float AspectRatio(int a) { return a == 0 ? 4.0f / 3.0f : a == 1 ? 16.0f / 9.0f : a == 2 ? 21.0f / 9.0f : 32.0f / 9.0f; }
inline void ApplyAspect() {
  const int a = settings().aspect;
  nwii::runtime::presentation_settings().widescreen = a != 0;
  g_hfov_scale.store(a >= 2 ? (16.0f / 9.0f) / AspectRatio(a) : 1.0f);
}
inline nwii::runtime::PresentationRect FitRect(int width, int height) {
  if (width <= 0 || height <= 0) return {0, 0, 0, 0};
  if (!settings().keep_aspect) return {0, 0, width, height};
  const float r = AspectRatio(settings().aspect);
  int w = width, h = height;
  if (float(width) / float(height) > r) w = std::max(1, int(height * r + 0.5f));
  else h = std::max(1, int(width / r + 0.5f));
  return {(width - w) / 2, (height - h) / 2, w, h};
}

// Escalado "nitido" (sin suavizar) solo si la escala es exacta: con 1344 lineas en una
// pantalla de 1440, una de cada 14 se duplicaria y se verian rayas horizontales.
inline bool UseNearest(const nwii::runtime::PresentationRect& s, const nwii::runtime::PresentationRect& d) {
  if (!settings().sharp || s.width <= 0 || s.height <= 0) return false;
  return d.width % s.width == 0 && d.height % s.height == 0;
}

// FXAA 3.11 (version de calidad reducida, una pasada) al presentar la imagen.
// Devuelve false si no esta activo o no se pudo preparar (se usa la copia normal).
inline bool PresentFXAA(GLuint efb_tex, int efb_w, int efb_h, const nwii::runtime::PresentationRect& src,
                        const nwii::runtime::PresentationRect& dst) {
  if (!settings().fxaa || !efb_tex || dst.width <= 0 || dst.height <= 0) return false;
  static GLuint prog = 0, vao = 0; static GLint u_rect = -1, u_rcp = -1; static bool tried = false;
  if (!tried) {
    tried = true;
    const char* vs = "#version 330 core\nuniform vec4 uRect; out vec2 uv;\n"
      "void main(){ vec2 p = vec2(float((gl_VertexID<<1)&2), float(gl_VertexID&2)); uv = uRect.xy + p * uRect.zw; gl_Position = vec4(p*2.0-1.0, 0.0, 1.0); }\n";
    const char* fs = "#version 330 core\nuniform sampler2D t; uniform vec2 uRcp; in vec2 uv; out vec4 o;\n"
      "float L(vec3 c){ return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
      "void main(){\n"
      " vec3 rgbM = texture(t, uv).rgb;\n"
      " vec3 rgbNW = texture(t, uv + vec2(-1.0,-1.0)*uRcp).rgb; vec3 rgbNE = texture(t, uv + vec2(1.0,-1.0)*uRcp).rgb;\n"
      " vec3 rgbSW = texture(t, uv + vec2(-1.0, 1.0)*uRcp).rgb; vec3 rgbSE = texture(t, uv + vec2(1.0, 1.0)*uRcp).rgb;\n"
      " float lM=L(rgbM), lNW=L(rgbNW), lNE=L(rgbNE), lSW=L(rgbSW), lSE=L(rgbSE);\n"
      " float lMin=min(lM,min(min(lNW,lNE),min(lSW,lSE))), lMax=max(lM,max(max(lNW,lNE),max(lSW,lSE)));\n"
      " vec2 dir = vec2(-((lNW+lNE)-(lSW+lSE)), ((lNW+lSW)-(lNE+lSE)));\n"
      " float red = max((lNW+lNE+lSW+lSE)*0.03125, 1.0/128.0);\n"
      " float rcpMin = 1.0/(min(abs(dir.x),abs(dir.y))+red);\n"
      " dir = clamp(dir*rcpMin, vec2(-8.0), vec2(8.0)) * uRcp;\n"
      " vec3 A = 0.5*(texture(t, uv+dir*(1.0/3.0-0.5)).rgb + texture(t, uv+dir*(2.0/3.0-0.5)).rgb);\n"
      " vec3 B = A*0.5 + 0.25*(texture(t, uv+dir*-0.5).rgb + texture(t, uv+dir*0.5).rgb);\n"
      " float lB = L(B);\n"
      " o = vec4((lB<lMin || lB>lMax) ? A : B, 1.0);\n"
      "}\n";
    auto sh = [](GLenum ty, const char* src_) { GLuint id = glCreateShader(ty); glShaderSource(id, 1, &src_, nullptr); glCompileShader(id);
      GLint ok = 0; glGetShaderiv(id, GL_COMPILE_STATUS, &ok); if (!ok) { glDeleteShader(id); return GLuint(0); } return id; };
    GLuint v = sh(GL_VERTEX_SHADER, vs), f = sh(GL_FRAGMENT_SHADER, fs);
    if (v && f) {
      prog = glCreateProgram(); glAttachShader(prog, v); glAttachShader(prog, f); glLinkProgram(prog);
      GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok); if (!ok) { glDeleteProgram(prog); prog = 0; }
    }
    if (v) glDeleteShader(v); if (f) glDeleteShader(f);
    if (!prog) { std::puts("[Menu] FXAA no disponible"); return false; }
    u_rect = glGetUniformLocation(prog, "uRect"); u_rcp = glGetUniformLocation(prog, "uRcp");
    glUseProgram(prog); glUniform1i(glGetUniformLocation(prog, "t"), 0);
    glGenVertexArrays(1, &vao);
  }
  if (!prog) return false;
  GLint old_prog = 0, old_vao = 0, old_tex = 0, old_active = 0, vp[4];
  glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog); glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active); glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
  GLint old_sampler = 0; glGetIntegerv(GL_SAMPLER_BINDING, &old_sampler); glBindSampler(0, 0);  // ver Renderer::Flush
  glGetIntegerv(GL_VIEWPORT, vp);
  const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE);
  glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glViewport(dst.x, dst.y, dst.width, dst.height);
  glUseProgram(prog);
  glUniform4f(u_rect, float(src.x) / efb_w, float(src.y) / efb_h, float(src.width) / efb_w, float(src.height) / efb_h);
  glUniform2f(u_rcp, 1.0f / efb_w, 1.0f / efb_h);
  glBindTexture(GL_TEXTURE_2D, efb_tex);
  glBindVertexArray(vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindSampler(0, GLuint(old_sampler));
  glBindTexture(GL_TEXTURE_2D, GLuint(old_tex)); glActiveTexture(GLenum(old_active));
  glViewport(vp[0], vp[1], vp[2], vp[3]);
  if (blend) glEnable(GL_BLEND); if (depth) glEnable(GL_DEPTH_TEST); if (cull) glEnable(GL_CULL_FACE);
  glUseProgram(GLuint(old_prog)); glBindVertexArray(GLuint(old_vao));
  return true;
}

// Tras cargar la configuracion (toml): toma sus valores como base y aplica encima
// lo guardado en ajustes_port.ini, si existe.
inline void LoadSettings() {
  Settings& s = settings();
  auto& pres = nwii::runtime::presentation_settings();
  auto& cfg = nwii::runtime::Config::get();
  s.scale = std::clamp(pres.internal_scale, 1, 5);
  s.aspect = pres.widescreen ? 1 : 0; s.keep_aspect = pres.keep_aspect; s.sharp = pres.sharp_pixels;
  s.vsync = cfg.enable_vsync;
  const char* nb = std::getenv("NWII_FX_NO_BLOOM");
  s.bloom = nb && nb[0] == '0';  // por defecto sin resplandor (preferencia del usuario)
  if (OnSteamDeck() && !std::filesystem::exists(kSettingsFile)) {
    // Primera vez en la Deck: pantalla 1280x800, resolucion interna 2x, pantalla completa.
    s.scale = 2; s.fullscreen = true; s.vsync = true; s.tex_filter = 0; s.fxaa = false;
    std::puts("[Menu] Steam Deck: ajustes iniciales (2x, pantalla completa) en ajustes_port_deck.ini");
  }
  std::ifstream f(kSettingsFile);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq);
    const int v = std::atoi(line.c_str() + eq + 1);
    if (k == "escala") s.scale = std::clamp(v, 1, 5);
    else if (k == "resplandor") s.bloom = v != 0;
    else if (k == "vsync") s.vsync = v != 0;
    else if (k == "pantalla_completa") s.fullscreen = v != 0;
    else if (k == "panoramico") s.aspect = v ? std::max(s.aspect, 1) : 0;
    else if (k == "aspecto") s.aspect = std::clamp(v, 0, 3);
    else if (k == "filtrado") s.tex_filter = std::clamp(v, 0, 5);
    else if (k == "fxaa") s.fxaa = v != 0;
    else if (k == "texturas_hd") s.hd = v != 0;
    else if (k == "mantener_proporcion") s.keep_aspect = v != 0;
    else if (k == "pixeles_nitidos") s.sharp = v != 0;
    else if (k == "contador_fps") s.fps = v != 0;
    else if (k == "volumen") s.volume = std::clamp(v, 0, 100);
    else if (k == "nube_auto") s.cloud_auto = v != 0;
    else if (k == "idioma") s.lang = port_lang::FromCode(line.c_str() + eq + 1);
  }
  pres.internal_scale = s.scale;
  pres.widescreen = s.aspect != 0; pres.keep_aspect = s.keep_aspect; pres.sharp_pixels = s.sharp;
  cfg.enable_vsync = s.vsync;
  g_no_bloom.store(!s.bloom);
  g_volume.store(s.volume);
  ApplyAspect();
  g_tex_filter.store(s.tex_filter);
  g_hd_textures.store(s.hd);
  g_lang.store(s.lang);
  StartupLang() = s.lang;
  port_cloud::SetAuto(s.cloud_auto);
  std::printf("[Menu] ajustes: escala=%d resplandor=%d vsync=%d completa=%d aspecto=%d volumen=%d idioma=%s nube_auto=%d\n",
              s.scale, int(s.bloom), int(s.vsync), int(s.fullscreen), s.aspect, s.volume, s.lang >= 0 ? port_lang::Code(s.lang) : "-", int(s.cloud_auto));
}

inline void ApplyFullscreen(SDL_Window* w) {
  if (w) SDL_SetWindowFullscreen(w, settings().fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
inline void ApplyStartup(SDL_Window* w) {
  SDL_GL_SetSwapInterval(settings().vsync ? 1 : 0);
  if (settings().fullscreen) ApplyFullscreen(w);
}

// Salida de audio: silencio en pausa, volumen aplicado al resto.
template <class Pull>
inline void AudioFilter(int16_t* out, size_t frames, Pull pull) {
  if (g_paused.load(std::memory_order_relaxed)) { std::memset(out, 0, frames * 4); return; }
  pull(out, frames);
  const int vol = g_volume.load(std::memory_order_relaxed);
  if (vol >= 100) return;
  for (size_t i = 0; i < frames * 2; ++i) out[i] = int16_t(int(out[i]) * vol / 100);
}

// ---------------------------------------------------------------- dibujo
class Renderer {
  struct V { float x, y, u, v, r, g, b, a; };
  std::vector<V> verts_;
  GLuint prog_ = 0, vao_ = 0, vbo_ = 0, tex_ = 0; GLint u_screen_ = -1; bool tried_ = false;
  float sw_ = 1, sh_ = 1;
  const port_menu_font::Glyph* Find(uint32_t cp) const {
    for (const auto& g : port_menu_font::kGlyphs) if (g.cp == cp) return &g;
    return nullptr;
  }
  static uint32_t NextCp(const char*& p) {
    const unsigned char c = (unsigned char)*p++;
    if (c < 0x80) return c;
    if ((c >> 5) == 6 && *p) { uint32_t r = ((c & 31) << 6) | ((unsigned char)*p++ & 63); return r; }
    if ((c >> 4) == 14 && p[0] && p[1]) { uint32_t r = ((c & 15) << 12) | (((unsigned char)p[0] & 63) << 6) | ((unsigned char)p[1] & 63); p += 2; return r; }
    return '?';
  }
 public:
  bool Init() {
    if (tried_) return prog_ != 0;
    tried_ = true;
    const char* vs = "#version 330 core\nlayout(location=0) in vec2 p; layout(location=1) in vec2 t; layout(location=2) in vec4 c;"
                     "uniform vec2 uScreen; out vec2 vt; out vec4 vc;"
                     "void main(){ vt=t; vc=c; gl_Position=vec4(p.x/uScreen.x*2.0-1.0, 1.0-p.y/uScreen.y*2.0, 0.0, 1.0); }";
    const char* fs = "#version 330 core\nin vec2 vt; in vec4 vc; uniform sampler2D uAtlas; out vec4 o;"
                     "void main(){ o = vec4(vc.rgb, vc.a * texture(uAtlas, vt).r); }";
    auto sh = [](GLenum t, const char* s) { GLuint id = glCreateShader(t); glShaderSource(id, 1, &s, nullptr); glCompileShader(id);
      GLint ok = 0; glGetShaderiv(id, GL_COMPILE_STATUS, &ok); if (!ok) { glDeleteShader(id); return GLuint(0); } return id; };
    GLuint v = sh(GL_VERTEX_SHADER, vs), f = sh(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) { std::puts("[Menu] no se pudo compilar el sombreador del menu"); return false; }
    prog_ = glCreateProgram(); glAttachShader(prog_, v); glAttachShader(prog_, f); glLinkProgram(prog_);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0; glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(prog_); prog_ = 0; return false; }
    u_screen_ = glGetUniformLocation(prog_, "uScreen");
    glUseProgram(prog_); glUniform1i(glGetUniformLocation(prog_, "uAtlas"), 0);
    std::vector<uint8_t> px(size_t(port_menu_font::kAtlasW) * port_menu_font::kAtlasH);
    size_t o = 0;
    for (uint32_t i = 0; i + 1 < port_menu_font::kRleSize && o < px.size(); i += 2)
      for (int n = 0; n < port_menu_font::kAtlasRle[i + 1] && o < px.size(); ++n) px[o++] = port_menu_font::kAtlasRle[i];
    glGenTextures(1, &tex_); glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, port_menu_font::kAtlasW, port_menu_font::kAtlasH, 0, GL_RED, GL_UNSIGNED_BYTE, px.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenVertexArrays(1, &vao_); glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_); glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(V), (void*)(4 * sizeof(float)));
    return true;
  }
  void Begin(int w, int h) { verts_.clear(); sw_ = float(w); sh_ = float(h); }
  void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, const float c[4]) {
    const V q[6] = {{x0, y0, u0, v0, c[0], c[1], c[2], c[3]}, {x0, y1, u0, v1, c[0], c[1], c[2], c[3]}, {x1, y1, u1, v1, c[0], c[1], c[2], c[3]},
                    {x0, y0, u0, v0, c[0], c[1], c[2], c[3]}, {x1, y1, u1, v1, c[0], c[1], c[2], c[3]}, {x1, y0, u1, v0, c[0], c[1], c[2], c[3]}};
    verts_.insert(verts_.end(), q, q + 6);
  }
  void Rect(float x, float y, float w, float h, const float c[4]) {
    const float u = 3.0f / port_menu_font::kAtlasW, v = 3.0f / port_menu_font::kAtlasH;  // bloque blanco
    Quad(x, y, x + w, y + h, u, v, u, v, c);
  }
  float TextWidth(const std::string& s, float scale) const {
    float w = 0; const char* p = s.c_str();
    while (*p) { const auto* g = Find(NextCp(p)); if (g) w += g->adv * scale; }
    return w;
  }
  // y = parte superior de la linea
  void Text(float x, float y, const std::string& s, float scale, const float c[4]) {
    const char* p = s.c_str();
    while (*p) {
      const auto* g = Find(NextCp(p));
      if (!g) continue;
      if (g->w && g->h) {
        const float gx = x + g->ox * scale, gy = y + g->oy * scale;
        Quad(gx, gy, gx + g->w * scale, gy + g->h * scale,
             float(g->x) / port_menu_font::kAtlasW, float(g->y) / port_menu_font::kAtlasH,
             float(g->x + g->w) / port_menu_font::kAtlasW, float(g->y + g->h) / port_menu_font::kAtlasH, c);
      }
      x += g->adv * scale;
    }
  }
  void Flush() {
    if (!Init() || verts_.empty()) return;
    GLint old_prog = 0, old_vao = 0, old_buf = 0, old_tex = 0, old_active = 0, src = 0, dst = 0, srca = 0, dsta = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog); glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buf); glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
    glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
    // El renderizador deja objetos sampler en las unidades (filtrado con mipmaps): sin
    // quitarlo, el atlas (sin mipmaps) queda incompleto y el menu no se ve.
    GLint old_sampler = 0; glGetIntegerv(GL_SAMPLER_BINDING, &old_sampler); glBindSampler(0, 0);
    glGetIntegerv(GL_BLEND_SRC_RGB, &src); glGetIntegerv(GL_BLEND_DST_RGB, &dst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &srca); glGetIntegerv(GL_BLEND_DST_ALPHA, &dsta);
    const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), cull = glIsEnabled(GL_CULL_FACE), sc = glIsEnabled(GL_SCISSOR_TEST);
    glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(prog_); glUniform2f(u_screen_, sw_, sh_);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glBindVertexArray(vao_); glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, verts_.size() * sizeof(V), verts_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(verts_.size()));
    glBindSampler(0, GLuint(old_sampler));
    glBindTexture(GL_TEXTURE_2D, GLuint(old_tex)); glActiveTexture(GLenum(old_active));
    glBlendFuncSeparate(GLenum(src), GLenum(dst), GLenum(srca), GLenum(dsta));
    if (!blend) glDisable(GL_BLEND); if (depth) glEnable(GL_DEPTH_TEST); if (cull) glEnable(GL_CULL_FACE); if (sc) glEnable(GL_SCISSOR_TEST);
    glUseProgram(GLuint(old_prog)); glBindVertexArray(GLuint(old_vao)); glBindBuffer(GL_ARRAY_BUFFER, GLuint(old_buf));
  }
};

// ---------------------------------------------------------------- menu
struct Item {
  std::string label;
  std::function<std::string()> value;      // vacio = accion
  std::function<void(int)> change;         // -1 / +1 (izquierda / derecha)
  std::function<void()> activate;          // Intro / A
  std::string note;                        // linea de ayuda bajo el menu
};

class Menu {
  enum Page { kMain, kGraphics, kSound, kControls, kQuit, kSaves, kCloud };
  bool open_ = false, quit_ = false, wait_release_ = false;
  Page page_ = kMain; int sel_ = 0;
  SDL_Window* window_ = nullptr; Renderer r_;
  bool start_held_ = false, back_held_ = false;
  int stick_dir_ = 0;  // -1 arriba, 1 abajo, 0 centro
  static std::string YesNo(bool b) { return b ? "Sí" : "No"; }
  static int& SaveSlot() { static int slot = 1; return slot; }
  static std::string SlotInfo(int slot) {
    char path[64]; std::snprintf(path, sizeof(path), "guardados_rapidos/ranura%d.sav", slot);
    std::error_code ec; auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return "Ranura vacía.";
    const auto st = std::chrono::time_point_cast<std::chrono::system_clock::duration>(t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t tt = std::chrono::system_clock::to_time_t(st);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char b[96]; std::snprintf(b, sizeof(b), "Guardada el %02d/%02d a las %02d:%02d.", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_hour, tmv.tm_min);
    return b;
  }
  std::string toast_; std::chrono::steady_clock::time_point toast_until_{};
  uint64_t toast_serial_ = 0;
  void Toast(const std::string& t) { toast_ = t; toast_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500); }
  void RequestSave() { g_save_request.store(SaveSlot()); Toast("Guardando en la ranura " + std::to_string(SaveSlot()) + "…"); }
  void RequestLoad() { g_load_request.store(SaveSlot()); Toast("Cargando la ranura " + std::to_string(SaveSlot()) + "…"); }
  void PollResult() {
    const uint64_t serial = g_result_serial.load();
    if (serial == toast_serial_) return;
    toast_serial_ = serial;
    const int r = g_last_result.load();
    if (r > 0 && r < 100) Toast("Guardado en la ranura " + std::to_string(r));
    else if (r < 0) Toast("Ranura " + std::to_string(-r) + " cargada");
    else if (r >= 400) Toast("No se pudo guardar ahora: inténtalo otra vez");
    else if (r >= 300) Toast("La ranura " + std::to_string(r - 300) + " no es válida");
    else if (r >= 200) Toast("La ranura " + std::to_string(r - 200) + " está vacía");
    else if (r >= 100) Toast("Error al escribir la ranura " + std::to_string(r - 100));
  }

  std::vector<Item> Items() {
    Settings& s = settings();
    std::vector<Item> it;
    auto toggle = [&](const char* label, bool* v, std::function<void()> apply, const char* note) {
      it.push_back({label, [v] { return YesNo(*v); }, [v, apply](int) { *v = !*v; if (apply) apply(); SaveSettings(); },
                    [v, apply] { *v = !*v; if (apply) apply(); SaveSettings(); }, note});
    };
    switch (page_) {
      case kMain:
        it.push_back({"Reanudar", nullptr, nullptr, [this] { Close(); }, ""});
        it.push_back({"Guardado rápido", nullptr, nullptr, [this] { Go(kSaves); }, "F5 guarda, F8 carga y F6 cambia de ranura sin abrir el menú."});
        it.push_back({"Nube (Dropbox)", nullptr, nullptr, [this] { Go(kCloud); },
                      port_cloud::Connected() ? port_cloud::Status() : std::string("Guarda la partida y las ranuras en tu Dropbox.")});
        it.push_back({"Gráficos", nullptr, nullptr, [this] { Go(kGraphics); }, ""});
        it.push_back({"Sonido", nullptr, nullptr, [this] { Go(kSound); }, ""});
        it.push_back({"Idioma del juego", [&s] { return std::string(port_lang::Name(s.lang < 0 ? 0 : s.lang)); },
                      [&s](int d) { s.lang = ((s.lang < 0 ? 0 : s.lang) + d + 5) % 5; g_lang.store(s.lang); SaveSettings(); },
                      [&s] { s.lang = ((s.lang < 0 ? 0 : s.lang) + 1) % 5; g_lang.store(s.lang); SaveSettings(); },
                      (s.lang != StartupLang() && !(s.lang == 0 && StartupLang() < 0))
                          ? std::string("Reinicia el juego para aplicar el cambio.")
                          : std::string(s.lang == 3 || s.lang == 4 ? "Alemán e italiano: experimental en esta versión del juego." : "Textos y voces del menú del juego. Se aplica al reiniciar.")});
        it.push_back({"Controles", nullptr, nullptr, [this] { Go(kControls); }, ""});
        it.push_back({"Salir del juego", nullptr, nullptr, [this] { Go(kQuit); }, ""});
        break;
      case kGraphics:
        it.push_back({"Resolución interna", [&s] {
                        static const char* n[] = {"", "1x (480p)", "2x (960p, ~1080p)", "3x (1440p)", "4x (1920p)", "5x (2400p, ~4K)"};
                        return std::string(n[std::clamp(s.scale, 1, 5)]); },
                      [&s](int d) { s.scale = std::clamp(s.scale + d, 1, 5); SaveSettings(); },
                      [&s] { s.scale = s.scale % 5 + 1; SaveSettings(); },
                      "Más alta = más nítido y más lento. 4x y 5x piden mucha tarjeta gráfica."});
        it.push_back({"Texturas", [&s] { return std::string(s.hd ? "HD" : "Original"); },
                      [&s](int) { s.hd = !s.hd; g_hd_textures.store(s.hd); g_tex_reload.store(true); SaveSettings(); },
                      [&s] { s.hd = !s.hd; g_hd_textures.store(s.hd); g_tex_reload.store(true); SaveSettings(); },
                      [] { auto& pk = hd_textures::Pack::get(); pk.Scan();
                           return pk.size() ? "Paquete en texturas_hd: " + std::to_string(pk.size()) + " texturas (formato Dolphin)."
                                            : std::string("No hay paquete: copia sus texturas en la carpeta texturas_hd."); }()});
        it.push_back({"Filtrado de texturas", [&s] {
                        static const char* n[] = {"Original", "Trilineal", "Anisotrópico 2x", "Anisotrópico 4x", "Anisotrópico 8x", "Anisotrópico 16x"};
                        return std::string(n[std::clamp(s.tex_filter, 0, 5)]); },
                      [&s](int d) { const int o = s.tex_filter; s.tex_filter = std::clamp(s.tex_filter + d, 0, 5); g_tex_filter.store(s.tex_filter); if ((o > 0) != (s.tex_filter > 0)) g_tex_reload.store(true); SaveSettings(); },
                      [&s] { const int o = s.tex_filter; s.tex_filter = (s.tex_filter + 1) % 6; g_tex_filter.store(s.tex_filter); if ((o > 0) != (s.tex_filter > 0)) g_tex_reload.store(true); SaveSettings(); },
                      "Texturas lejanas y en ángulo más nítidas y sin parpadeo."});
        it.push_back({"Antialiasing", [&s] { return std::string(s.fxaa ? "FXAA" : "No"); },
                      [&s](int) { s.fxaa = !s.fxaa; SaveSettings(); }, [&s] { s.fxaa = !s.fxaa; SaveSettings(); },
                      "Suaviza los bordes dentados. Con resolución alta hace menos falta."});
        toggle("Resplandor (bloom)", &s.bloom, [&s] { g_no_bloom.store(!s.bloom); }, "Brillo alrededor de las zonas claras.");
        toggle("Sincronización vertical", &s.vsync, [&s] { SDL_GL_SetSwapInterval(s.vsync ? 1 : 0); }, "Evita el desgarro de imagen.");
        toggle("Pantalla completa", &s.fullscreen, [this] { ApplyFullscreen(window_); }, "");
        it.push_back({"Relación de aspecto", [&s] { static const char* n[] = {"4:3", "16:9", "21:9", "32:9"}; return std::string(n[std::clamp(s.aspect, 0, 3)]); },
                      [&s](int d) { s.aspect = std::clamp(s.aspect + d, 0, 3); ApplyAspect(); SaveSettings(); },
                      [&s] { s.aspect = (s.aspect + 1) % 4; ApplyAspect(); SaveSettings(); },
                      "21:9 y 32:9 amplían la vista; el HUD se estira. Al pasar de 4:3 a panorámico, reinicia."});
        toggle("Mantener proporción", &s.keep_aspect, [&s] { nwii::runtime::presentation_settings().keep_aspect = s.keep_aspect; }, "No: la imagen ocupa toda la ventana.");
        toggle("Píxeles nítidos", &s.sharp, [&s] { nwii::runtime::presentation_settings().sharp_pixels = s.sharp; }, "Sin suavizar, solo si la escala es exacta (si no, saldrían rayas).");
        toggle("Contador de FPS", &s.fps, nullptr, "");
        it.push_back({"Volver", nullptr, nullptr, [this] { Go(kMain, 3); }, ""});
        break;
      case kSound:
        it.push_back({"Volumen", [&s] { return std::to_string(s.volume) + " %"; },
                      [&s](int d) { s.volume = std::clamp(s.volume + d * 10, 0, 100); g_volume.store(s.volume); SaveSettings(); },
                      nullptr, "Izquierda / derecha para cambiar."});
        it.push_back({"Volver", nullptr, nullptr, [this] { Go(kMain, 4); }, ""});
        break;
      case kControls:
        it.push_back({"Volver", nullptr, nullptr, [this] { Go(kMain, 6); }, ""});
        break;
      case kQuit:
        it.push_back({"No, volver al menú", nullptr, nullptr, [this] { Go(kMain, 7); }, ""});
        it.push_back({"Sí, salir", nullptr, nullptr, [this] { quit_ = true; Close(); }, ""});
        break;
      case kSaves: {
        it.push_back({"Ranura", [] { return std::to_string(SaveSlot()); },
                      [](int d) { SaveSlot() = (SaveSlot() + d + 2) % 3 + 1; }, [] { SaveSlot() = SaveSlot() % 3 + 1; },
                      SlotInfo(SaveSlot())});
        it.push_back({"Guardar ahora", nullptr, nullptr, [this] { RequestSave(); Close(); },
                      "Guarda todo tal como está. No sustituye al guardado del propio juego."});
        it.push_back({"Cargar", nullptr, nullptr, [this] { RequestLoad(); Close(); },
                      "Vuelve al momento guardado en esta ranura."});
        it.push_back({"Volver", nullptr, nullptr, [this] { Go(kMain, 1); }, ""});
        break;
      }
      case kCloud: {
        const bool creds = port_cloud::HasCredentials(), conn = port_cloud::Connected(), busy = port_cloud::Busy();
        std::string st = port_cloud::Status();
        if (st.size() > 90) st = st.substr(0, 87) + "...";
        if (!conn) {
          it.push_back({"Conectar con Dropbox", [busy] { return std::string(busy ? "Esperando…" : ""); }, nullptr, [] { port_cloud::RequestConnect(); },
                        creds ? "Se abre el navegador para dar permiso (Alt+Tab si no lo ves). " + st
                              : std::string("Falta dropbox_clave.txt en la carpeta del juego (ver CONTROLES.md).")});
        } else {
          const std::string acc = port_cloud::Account();
          it.push_back({"Cuenta", [acc] { return acc.empty() ? std::string("Conectada") : acc; }, nullptr, nullptr, st});
          it.push_back({"Subida automática", [&s] { return YesNo(s.cloud_auto); },
                        [&s](int) { s.cloud_auto = !s.cloud_auto; port_cloud::SetAuto(s.cloud_auto); SaveSettings(); },
                        [&s] { s.cloud_auto = !s.cloud_auto; port_cloud::SetAuto(s.cloud_auto); SaveSettings(); },
                        "Sube la partida al guardar en el juego y las ranuras al pulsar F5. Al arrancar baja lo más reciente."});
          it.push_back({"Subir ahora", [busy] { return std::string(busy ? "Trabajando…" : ""); }, nullptr, [this] { port_cloud::RequestUpload(); Toast("Nube: subiendo…"); },
                        "La nube queda igual que este PC (Dropbox guarda las versiones anteriores)."});
          it.push_back({"Descargar de la nube", [busy] { return std::string(busy ? "Trabajando…" : ""); }, nullptr, [this] { port_cloud::RequestDownload(); Toast("Nube: descargando…"); },
                        "Las ranuras se bajan al momento; la partida se usará al reiniciar el juego."});
          it.push_back({"Desconectar la cuenta", nullptr, nullptr, [] { port_cloud::RequestDisconnect(); }, "Quita el permiso de este PC. No borra nada de Dropbox."});
        }
        it.push_back({"Volver", nullptr, nullptr, [this] { Go(kMain, 2); }, ""});
        break;
      }
    }
    return it;
  }
  void Go(Page p, int sel = 0) { page_ = p; sel_ = sel; }
  void Back() {
    if (page_ == kMain) Close();
    else if (page_ == kSaves) Go(kMain, 1);
    else if (page_ == kCloud) Go(kMain, 2);
    else if (page_ == kGraphics) Go(kMain, 3);
    else if (page_ == kSound) Go(kMain, 4);
    else if (page_ == kControls) Go(kMain, 6);
    else Go(kMain, 7);
  }
  void Move(int d) {
    const int n = int(Items().size());
    if (n > 0) sel_ = (sel_ + d + n) % n;
  }
  // En Graficos, cada cambio vuelve a dibujar el ultimo cuadro para verlo al momento
  // aunque el juego este en pausa.
  void Change(int d) { auto it = Items(); if (sel_ < int(it.size()) && it[sel_].change) { it[sel_].change(d); if (page_ == kGraphics) g_rerender.store(true); } }
  void Activate() { auto it = Items(); if (sel_ < int(it.size()) && it[sel_].activate) { const Page before = page_; it[sel_].activate(); if (before == kGraphics && page_ == kGraphics) g_rerender.store(true); } }

  static bool AnyInputHeld() {
    int n = 0; const Uint8* k = SDL_GetKeyboardState(&n);
    for (int i = 0; i < n; ++i) if (k[i]) return true;
    for (int j = 0; j < SDL_NumJoysticks(); ++j) {
      SDL_GameController* c = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(j));
      if (!c) continue;
      for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) if (SDL_GameControllerGetButton(c, SDL_GameControllerButton(b))) return true;
    }
    return false;
  }

 public:
  bool IsOpen() const { return open_; }
  bool QuitRequested() const { return quit_; }
  void SetWindow(SDL_Window* w) { window_ = w; }
  void Open() {
    if (open_) return;
    open_ = true; wait_release_ = false; page_ = kMain; sel_ = 0;
    // Conectado al online no se pausa: el servidor cortaria la conexion al no recibir
    // respuesta a sus comprobaciones de linea (ReqLineCheck cada 30 s).
    const bool pause = !::PortNetOnline();
    g_input_blocked.store(true); g_paused.store(pause);
    std::puts(pause ? "[Menu] abierto (juego en pausa)" : "[Menu] abierto (online: el juego sigue)");
  }
  void Close() {
    if (!open_) return;
    open_ = false; wait_release_ = true;  // el juego vuelve cuando se suelten las teclas/botones
    g_paused.store(false);
    std::puts("[Menu] cerrado");
  }
  // Por vuelta del bucle principal.
  void Update() {
    if (quit_) { g_paused.store(false); g_input_blocked.store(false); return; }
    if (!open_ && wait_release_ && !AnyInputHeld()) { wait_release_ = false; g_input_blocked.store(false); }
  }
  // Devuelve true si el evento es del menu (no se pasa al juego).
  bool HandleEvent(const SDL_Event& e) {
    if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_F10) {
      g_dump_request.store(true);   // volcado del siguiente cuadro (logs\\volcado-*)
      return true;
    }
    if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_P) {
      if (open_) Close(); else Open();
      return true;
    }
    if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
      const bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
      const int b = e.cbutton.button;
      bool combo = false;
      if (b == SDL_CONTROLLER_BUTTON_START) { if (down && back_held_) combo = true; start_held_ = down; }
      if (b == SDL_CONTROLLER_BUTTON_BACK) { if (down && start_held_) combo = true; back_held_ = down; }
      if (combo) { if (open_) Close(); else Open(); return true; }
    }
    if (!open_ && e.type == SDL_KEYDOWN && !e.key.repeat) {
      const auto sc = e.key.keysym.scancode;
      if (sc == SDL_SCANCODE_F5) { RequestSave(); return true; }
      if (sc == SDL_SCANCODE_F8) { RequestLoad(); return true; }
      if (sc == SDL_SCANCODE_F6) { SaveSlot() = SaveSlot() % 3 + 1; Toast("Ranura " + std::to_string(SaveSlot())); return true; }
    }
    if (!open_) return false;
    // ---- navegacion con el menu abierto
    if (e.type == SDL_KEYDOWN) {
      switch (e.key.keysym.scancode) {
        case SDL_SCANCODE_UP: case SDL_SCANCODE_W: Move(-1); break;
        case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: Move(1); break;
        case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: Change(-1); break;
        case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: Change(1); break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: Activate(); break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: Back(); break;
        default: break;
      }
      return true;
    }
    if (e.type == SDL_CONTROLLERBUTTONDOWN) {
      switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: Move(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: Move(1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: Change(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: Change(1); break;
        case SDL_CONTROLLER_BUTTON_A: Activate(); break;
        case SDL_CONTROLLER_BUTTON_B: Back(); break;
        case SDL_CONTROLLER_BUTTON_START: if (!back_held_) Close(); break;
        default: break;
      }
      return true;
    }
    if (e.type == SDL_CONTROLLERAXISMOTION) {
      if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
        const int d = e.caxis.value < -20000 ? -1 : (e.caxis.value > 20000 ? 1 : 0);
        if (d != 0 && d != stick_dir_) Move(d);
        if (std::abs(e.caxis.value) < 12000 || d != 0) stick_dir_ = d;
      } else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
        static int xdir = 0;
        const int d = e.caxis.value < -20000 ? -1 : (e.caxis.value > 20000 ? 1 : 0);
        if (d != 0 && d != xdir) Change(d);
        if (std::abs(e.caxis.value) < 12000 || d != 0) xdir = d;
      }
      return true;
    }
    if (e.type == SDL_KEYUP || e.type == SDL_CONTROLLERBUTTONUP || e.type == SDL_MOUSEBUTTONDOWN ||
        e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEWHEEL)
      return true;
    return false;
  }

  void DrawToast(int w, int h) {
    PollResult();
    if (std::chrono::steady_clock::now() > toast_until_) { std::string m; if (port_cloud::PopMessage(m)) Toast(m); }
    if (toast_.empty() || std::chrono::steady_clock::now() > toast_until_ || w <= 0 || h <= 0) return;
    const float s = std::max(0.5f, h / 720.0f) * 0.62f;
    const float bg[4] = {0.07f, 0.08f, 0.10f, 0.88f}, accent[4] = {0.85f, 0.55f, 0.15f, 1.0f}, white[4] = {1, 1, 1, 1};
    const float tw = r_.TextWidth(toast_, s), th = port_menu_font::kLineH * s;
    const float x = w - tw - 40 * s, y = h - th - 40 * s;
    r_.Begin(w, h);
    r_.Rect(x - 16 * s, y - 10 * s, tw + 32 * s, th + 20 * s, bg);
    r_.Rect(x - 16 * s, y - 10 * s, 4 * s, th + 20 * s, accent);
    r_.Text(x, y, toast_, s, white);
    r_.Flush();
  }
  void Draw(int w, int h) {
    DrawToast(w, h);
    if (!open_ || w <= 0 || h <= 0) return;
    const float s = std::max(0.5f, h / 720.0f) * 0.78f;   // tamano de letra (32 px * s)
    const float line = port_menu_font::kLineH * s * 1.45f;
    const float dim[4] = {0, 0, 0, 0.62f}, panel[4] = {0.07f, 0.08f, 0.10f, 0.92f}, accent[4] = {0.85f, 0.55f, 0.15f, 1.0f};
    const float white[4] = {1, 1, 1, 1}, grey[4] = {0.72f, 0.74f, 0.78f, 1}, dark[4] = {0.07f, 0.06f, 0.05f, 1};
    r_.Begin(w, h);
    { float d[4] = {0, 0, 0, page_ == kGraphics ? 0.25f : dim[3]}; r_.Rect(0, 0, float(w), float(h), d); }  // en Graficos se ve mejor el cambio
    std::vector<Item> it = Items();
    sel_ = std::clamp(sel_, 0, std::max(0, int(it.size()) - 1));
    const char* titles[] = {"Menú del port", "Gráficos", "Sonido", "Controles", "¿Salir del juego?", "Guardado rápido", "Nube (Dropbox)"};
    std::vector<std::pair<std::string, std::string>> rows;  // pagina de controles
    if (page_ == kControls) {
      rows = {{"Moverse", "WASD  ·  stick izquierdo"}, {"Cámara", "Flechas  ·  stick derecho"},
              {"Confirmar / atacar (A Wii)", "Espacio / clic izq.  ·  B"}, {"Cancelar / esquivar (B Wii)", "Clic derecho  ·  A"},
              {"Sacar arma (botón +)", "Intro  ·  Y"}, {"Usar objeto", "1  ·  X"}, {"Menú del juego", "Escape o 2  ·  Start"},
              {"Correr", "Mayús o E  ·  RB"}, {"Centrar cámara / objetos", "Ctrl o Q  ·  LB"}, {"Patada (botón -)", "Retroceso  ·  Back"},
              {"ZL / ZR", "—  ·  LT / RT"}, {"Este menú", "P  ·  Back + Start"}};
    }
    float pw = std::min(float(w) - 40.0f, (page_ == kControls ? 980.0f : 760.0f) * s / 0.78f);
    const float content_lines = float(it.size()) + (page_ == kControls ? float(rows.size()) * 0.82f + 0.4f : 0.0f) + (page_ == kQuit ? 1.2f : 0.0f);
    bool has_note = false; for (auto& x : it) if (!x.note.empty()) has_note = true;
    float ph = line * (content_lines + 3.4f + (has_note ? 0.9f : 0.0f));
    const float px = (w - pw) / 2, py = std::max(10.0f, (h - ph) / 2);
    r_.Rect(px, py, pw, ph, panel);
    r_.Rect(px, py, pw, 4 * s, accent);
    const float pad = 34 * s;
    float y = py + line * 0.55f;
    r_.Text(px + pad, y, titles[page_], s * 1.25f, accent);
    y += line * 1.5f;
    if (page_ == kQuit) { r_.Text(px + pad, y, "Lo que no hayas guardado en el juego se perderá.", s * 0.8f, grey); y += line * 1.2f; }
    if (page_ == kControls) {
      const float colw = pw * 0.46f;
      for (auto& rw : rows) {
        r_.Text(px + pad, y, rw.first, s * 0.78f, grey);
        r_.Text(px + pad + colw, y, rw.second, s * 0.78f, white);
        y += line * 0.82f;
      }
      y += line * 0.4f;
    }
    for (int i = 0; i < int(it.size()); ++i) {
      const bool on = i == sel_;
      if (on) r_.Rect(px + pad * 0.5f, y - line * 0.12f, pw - pad, line * 0.92f, accent);
      const float* tc = on ? dark : white;
      r_.Text(px + pad, y, it[i].label, s, tc);
      if (it[i].value) {
        std::string v = it[i].value();
        if (it[i].change) v = "<  " + v + "  >";
        r_.Text(px + pw - pad - r_.TextWidth(v, s), y, v, s, tc);
      }
      y += line;
    }
    const std::string note = (sel_ < int(it.size())) ? it[sel_].note : "";
    if (!note.empty()) r_.Text(px + pad, y + line * 0.1f, note, s * 0.7f, grey);
    const std::string help = "↑↓ elegir   ←→ cambiar   Intro / A aceptar   Esc / B volver   P / Back+Start cerrar";
    const float hs = s * 0.62f;
    r_.Text(px + (pw - r_.TextWidth(help, hs)) / 2, py + ph - line * 0.85f, help, hs, grey);
    r_.Flush();
  }
};

inline Menu& menu() { static Menu m; return m; }

}  // namespace port_menu
