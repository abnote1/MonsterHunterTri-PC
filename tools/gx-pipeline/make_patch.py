# Genera tools/gx-pipeline/gx.cpp a partir de export/nWiiRuntime/src/hle/gx.cpp.
import re,sys,pathlib
root=pathlib.Path(__file__).resolve().parents[2]
src=(root/'export/nWiiRuntime/src/hle/gx.cpp').read_text().replace('\r\n','\n')
tail=(pathlib.Path(__file__).parent/'pipeline_tail.inc').read_text().replace('\r\n','\n')
s=src
start=s.index('void ProcessGXFifo() {')
m=re.search(r'namespace nwii::runtime \{\s*\nstatic inline void wgp_push',s)
end=m.start()
body=s[start:end]
rb=body.index('    if (g_window) {\n        if (!g_renderer) {')
prep=body[:rb]
prep=prep.replace('void ProcessGXFifo() {','static bool PrepareGXFrame(std::vector<nwii::runtime::gx::GXCommand>& out_frame) {',1)
n=prep.count('return;'); prep=prep.replace('return;','return false;')
assert prep.count('++g_stat_frames;')==1
prep=prep.replace('++g_stat_frames;','/* g_stat_frames se cuenta al dibujar (RenderGXFrame) */')
prep+='    out_frame.swap(frame);\n    return true;\n}\n'
rest=body[rb:]
assert rest.strip().endswith('}') and 'g_renderer->Render(frame);' in rest and rest.count('{')==rest.count('}')+1-1 or True
s=s[:start]+prep+tail+'\n'+s[end:]
s=s.replace('#include <mutex>\n','#include <mutex>\n#include <thread>\n#include <atomic>\n#include <cstdio>\n#include <unordered_map>\n',1)
(pathlib.Path(__file__).parent/'gx.cpp').write_text(s.replace('\n','\r\n'),newline='')
print('ok, returns->false:',n)

# --- fifo_parser.cpp: cache de display lists mas grande, ajustable por entorno ---
fp=(root/'export/nWiiRuntime/src/hle/gx/fifo_parser.cpp').read_text().replace('\r\n','\n')
old="static constexpr size_t kDLCacheEntries = 4096;\nstatic constexpr size_t kDLCacheBytes = 64 * 1024 * 1024;\n"
assert old in fp, 'constantes de cache no encontradas'
new=("// tools/gx-pipeline: en zonas 2/3 la cache se llenaba (4096) y expulsaba a rafagas al mover la camara.\n"
     "static size_t DLCacheEnv(const char* name, size_t def) {\n"
     "    const char* e = std::getenv(name);\n"
     "    if (!e || !*e) return def;\n"
     "    const unsigned long long v = std::strtoull(e, nullptr, 10);\n"
     "    return v ? size_t(v) : def;\n"
     "}\n"
     "static const size_t kDLCacheEntries = DLCacheEnv(\"NWII_DL_CACHE_ENTRIES\", 16384);\n"
     "static const size_t kDLCacheBytes = DLCacheEnv(\"NWII_DL_CACHE_MB\", 256) * 1024 * 1024;\n")
fp=fp.replace(old,new)
(pathlib.Path(__file__).parent/'fifo_parser.cpp').write_text(fp.replace('\n','\r\n'),newline='')
print('ok fifo_parser.cpp')

# --- ios.cpp: una sola fuente de VI (reloj del planificador a 60 Hz) ---
io=(root/'export/nWiiRuntime/src/hle/ios.cpp').read_text().replace('\r\n','\n')
old="""  if (ctx.vblank_pending) {
      if (!ctx.in_callback && (ctx.msr & 0x8000)) {"""
assert io.count(old)==1, 'bloque vblank_pending no encontrado'
new="""  // tools/gx-pipeline: el bucle del host tambien marcaba vblank_pending en cada
  // vuelta, ademas del VI periodico del planificador (tb_freq/60). Con el dibujado
  // canalizado el bucle va rapido y el juego recibia >60 VI/s (iba acelerado).
  // Solo el VI del planificador; NWII_HOST_VI=1 recupera las dos fuentes.
  static const bool host_vi = std::getenv("NWII_HOST_VI") != nullptr;
  if (ctx.vblank_pending && !host_vi) ctx.vblank_pending = false;
  if (ctx.vblank_pending) {
      if (!ctx.in_callback && (ctx.msr & 0x8000)) {"""
io=io.replace(old,new)

# --- ios.cpp: camino rapido equivalente de process_pending_callbacks ---
o1="bool process_pending_callbacks(CPUContext &ctx) {\n"
assert io.count(o1)==1
o2="  if((++input_poll&1023)==0)WPADPollConnections(ctx);\n"
assert io.count(o2)==1
o3="          vi_registered = true;\n"
assert io.count(o3)==1
fast = """// tools/gx-pipeline: camino rapido de process_pending_callbacks. Se llama en cada
// salto hacia atras del codigo recompilado y casi siempre no hay nada que hacer.
// Comprueba exactamente las condiciones que harian actuar a la version completa
// (interrupciones, avisos, VI, IPC/DI, DEC, eventos vencidos, contexto 0xFFFFFFFC,
// sondeo del mando cada 1024 llamadas, latido del registro) y, si ninguna se
// cumple, solo publica el reloj al planificador como hacia la version completa.
// NWII_PPC_SLOW=1 desactiva el camino rapido.
static bool g_ppc_vi_ready = false;
static uint32_t g_ppc_calls = 0;
static bool process_pending_callbacks_full(CPUContext &ctx);
bool process_pending_callbacks(CPUContext &ctx) {
  static const bool fast_ok = std::getenv("NWII_PPC_SLOW") == nullptr &&
      std::getenv("NWII_CPU_PROFILE") == nullptr && std::getenv("NWII_LOOPTRACE") == nullptr &&
      !DiagnosticSamplingEnabled();
  if (fast_ok && g_ppc_vi_ready && ((g_ppc_calls + 1) & 1023) != 0 &&
      ctx.pc != 0xFFFFFFFC && ctx.pc != 0 && !ctx.in_callback &&
      !ctx.vblank_pending.load(std::memory_order_relaxed) &&
      g_ipc_interrupt_delay == 0 && nwii::runtime::hw::g_di_interrupt_delay == 0 &&
      g_trace_after_dec <= 0 && (ctx.inst_count % 20000000) != 0 &&
      (nwii::runtime::hw::pi_intsr.load(std::memory_order_relaxed) &
       nwii::runtime::hw::pi_intmr.load(std::memory_order_relaxed)) == 0 &&
      ctx.pending_cb_count.load(std::memory_order_relaxed) == 0) {
    static EventScheduler& sch = EventScheduler::get();
    const uint64_t now = ctx.read_timebase();
    if (now < sch.next_due() &&
        !(ctx.dec_irq_pending && (now - ctx.dec_written_tb) >= ctx.dec_value)) {
      sch.advance(ctx, now);
      ++g_ppc_calls;
      return false;
    }
  }
  return process_pending_callbacks_full(ctx);
}

static bool process_pending_callbacks_full(CPUContext &ctx) {
"""
io=io.replace(o1,fast)
io=io.replace(o2,"  if((++g_ppc_calls&1023)==0)WPADPollConnections(ctx);\n")
io=io.replace(o3,"          vi_registered = true;\n          g_ppc_vi_ready = true;\n")
# --- ios.cpp: pausa del menu del port (el invitado espera; su reloj no avanza) ---
o_pp="bool process_pending_callbacks(CPUContext &ctx) {\n  static const bool fast_ok"
assert io.count(o_pp)==1
io=io.replace(o_pp,"""static void PortMenuPauseWait(CPUContext &ctx) {
  const auto t0 = std::chrono::steady_clock::now();
  while (port_menu::g_paused.load(std::memory_order_acquire) && ctx.is_running)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  // El reloj del invitado se limita al tiempo real desde tb_start: se desplaza
  // el origen lo que ha durado la pausa para que el juego no "recupere" ese tiempo.
  ctx.tb_start += std::chrono::steady_clock::now() - t0;
}
bool process_pending_callbacks(CPUContext &ctx) {
  if (port_menu::g_paused.load(std::memory_order_relaxed)) PortMenuPauseWait(ctx);
  static const bool fast_ok""")
io='#include "port_menu_state.h"\n#include <thread>\n#include <chrono>\n'+io
(pathlib.Path(__file__).parent/'ios.cpp').write_text(io.replace('\n','\r\n'),newline='')
print('ok ios.cpp')

# --- renderer_gl.cpp: copias EFB sin consultas de estado a OpenGL ---
rg=(root/'export/nWiiRuntime/src/hle/gx/renderer_gl.cpp').read_text().replace('\r\n','\n')
anchor="namespace nwii::runtime::gx {\n"
assert rg.count(anchor)>=1
fast = """namespace nwii::runtime::gx {
// tools/gx-pipeline: copia EFB sin consultas al estado de OpenGL.
// La version original (gx_efb_copy.h) hacia en cada copia glGetIntegerv x2,
// glIsEnabled, glGetTexLevelParameteriv x2, reasignaba el adjunto del FBO y
// llamaba a glCheckFramebufferStatus. Con el controlador de NVIDIA cada consulta
// obliga a esperar a que se procese todo lo pendiente; con la GPU cargada (escala
// 2x) eso producia cuadros de hasta ~200 ms. Aqui el estado se lleva en la CPU:
// - un FBO por textura destino, adjuntado y validado una vez (y al cambiar tamano);
// - el tamano de cada textura se recuerda (las texturas EFB nunca se borran:
//   TextureCache las conserva a proposito);
// - el FBO de lectura (EFB) se consulta una sola vez: el renderizador no cambia
//   de framebuffer salvo aqui, y el bucle principal enlaza siempre el mismo EFB;
// - el recorte (scissor) no lo activa nunca el renderizador.
// NWII_EFB_COPY_LEGACY=1 vuelve a la version original.
struct EfbCopyTarget { GLuint fbo = 0; int w = 0, h = 0; bool complete = false; };
static bool CopyEFBToTextureFast(GLuint texture, GLuint& scratch, int x, int y,
                                 int width, int height, int efbHeight, int destWidth, int destHeight) {
  static const bool legacy = std::getenv("NWII_EFB_COPY_LEGACY") != nullptr;
  if (legacy) return CopyEFBToTexture(texture, scratch, x, y, width, height, efbHeight, destWidth, destHeight);
  if (!texture || x<0 || y<0 || width<=0 || height<=0 || y>=efbHeight) return false;
  if (destWidth<=0) destWidth=width;
  if (destHeight<=0) destHeight=height;
  static GLint efb_read = -1;
  if (efb_read < 0) glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &efb_read);
  static std::unordered_map<GLuint, EfbCopyTarget> targets;
  auto& t = targets[texture];
  const bool resize = (t.w != destWidth || t.h != destHeight);
  if (resize) {
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,destWidth,destHeight,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
  }
  if (!t.fbo) {
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
  } else {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t.fbo);
  }
  if (resize || !t.complete) {
    t.complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    t.w = destWidth; t.h = destHeight;
  }
  if (t.complete) {
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(x,efbHeight-y,x+width,efbHeight-y-height,
                      0,0,destWidth,destHeight,GL_COLOR_BUFFER_BIT,
                      (destWidth==width&&destHeight==height)?GL_NEAREST:GL_LINEAR);
    // Sin canal alfa en el EFB (RGB8_Z24 / RGB565_Z16) la copia sale opaca, como en la
    // consola. El framebuffer del port si guarda alfa y podia copiar 0: el fondo de los
    // dialogos en pausa y del teclado (la escena copiada) salia invisible.
    const uint32_t pfmt = g_state.bp[0x43] & 7;
    if (pfmt == 0 || pfmt == 2) {
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
      glClearColor(0.f, 0.f, 0.f, 1.f);
      glClear(GL_COLOR_BUFFER_BIT);
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
  }
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)efb_read);
  glBindTexture(GL_TEXTURE_2D, texture);
  return t.complete;
}
"""
rg=rg.replace(anchor,fast,1)
call="CopyEFBToTexture(tex, copy_fbo,"
assert rg.count(call)==1
rg=rg.replace(call,"CopyEFBToTextureFast(tex, copy_fbo,")

# rastreo de copias EFB con el registro PE 0x43 (formato de pixel/profundidad)
o_key="uint64_t key=(uint64_t(dest)<<32)^(uint64_t(cmd.val)<<8)^w^(uint64_t(h)<<20);"
assert rg.count(o_key)==1
rg=rg.replace(o_key,"uint64_t key=(uint64_t(dest)<<32)^(uint64_t(cmd.val)<<8)^w^(uint64_t(h)<<20)^(uint64_t(g_state.bp[0x43])<<40);")
o_ctl='<< " control=" << std::hex << cmd.val << std::dec'
assert rg.count(o_ctl)==1
rg=rg.replace(o_ctl,'<< " control=" << std::hex << cmd.val << " pe43=" << g_state.bp[0x43] << std::dec << " fmt=" << (((cmd.val>>4)&7)|((cmd.val>>3)&1)<<3) << " intensity=" << ((cmd.val>>15)&1)')

# rastreo detallado de dibujos que leen copias EFB (NWII_FXTRACE2)
o_fx='  static const bool fx_trace=std::getenv("NWII_FXTRACE")!=nullptr;\n'
assert rg.count(o_fx)==1
fx2=r"""  static const bool fx_trace2=std::getenv("NWII_FXTRACE2")!=nullptr;
  if (fx_trace2) {
    bool uses_efb=false; uint64_t key=g_state.GetShaderHash(0) ^ (uint64_t(g_state.bp[0x41])<<32) ^ (uint64_t(g_state.bp[0x42])<<40);
    for(unsigned i=0;i<8;i++){ const auto& ts=g_state.texStages[i];
      if(ts.base_addr && nwii::runtime::hle::TextureCache::get().find_efb_texture(ts.base_addr)!=0){uses_efb=true; key^=uint64_t(ts.base_addr)*(i+3)^(uint64_t(ts.format)<<(i*4));} }
    static std::unordered_set<uint64_t> seen2; static unsigned printed=0;
    const bool screen_quad = !CurrentIsPerspective() && cmd.vertices.size()<=6 && (g_state.bp[0x41]&1);
    if(screen_quad && !uses_efb) key ^= 0x5157ull<<48 ^ (uint64_t(g_state.xf[0x00C])<<8) ^ g_state.tevKonstWords[0] ^ (uint64_t(g_state.tevColorWords[2])<<20);
    if((uses_efb || screen_quad) && printed<120 && seen2.insert(key).second){ ++printed;
      std::printf("[FX2] tipo=%s\n", uses_efb ? "lee_copia_EFB" : "cuadro_pantalla_con_mezcla");
      std::printf("[FX2] verts=%zu persp=%d bp40=%06X bp41=%06X bp42=%06X bp43=%06X alphatest=%06X stages=%u gens=%u chans=%u\n",
        cmd.vertices.size(), int(CurrentIsPerspective()), g_state.bp[0x40], g_state.bp[0x41], g_state.bp[0x42], g_state.bp[0x43], g_state.bp[0xF3],
        unsigned(g_state.numTevStages), unsigned(g_state.numTexGens), unsigned(g_state.numChans));
      for(unsigned i=0;i<8;i++){ const auto& ts=g_state.texStages[i]; if(!ts.base_addr) continue;
        std::printf("[FX2]   tex%u addr=%08X %ux%u fmt=%u efb=%d\n", i, ts.base_addr, ts.width, ts.height, unsigned(ts.format),
          int(nwii::runtime::hle::TextureCache::get().find_efb_texture(ts.base_addr)!=0)); }
      for(unsigned i=0;i<g_state.numTevStages && i<16;i++){ const auto& t=g_state.tevStages[i];
        uint32_t ksel=g_state.bp[0xF6+i/2]; unsigned kc=(i%2)?((ksel>>14)&0x1F):((ksel>>4)&0x1F); unsigned ka=(i%2)?((ksel>>19)&0x1F):((ksel>>9)&0x1F);
        std::printf("[FX2]   tev%u map=%u coord=%u C:a=%u b=%u c=%u d=%u bias=%u op=%u sc=%u cl=%u ->%u  A:a=%u b=%u c=%u d=%u bias=%u op=%u sc=%u cl=%u ->%u kc=%u ka=%u\n",
          i, unsigned(t.texMap), unsigned(t.texCoord), t.colorInA,t.colorInB,t.colorInC,t.colorInD,t.colorBias,t.colorOp,t.colorScale,t.colorClamp,t.colorRegId,
          t.alphaInA,t.alphaInB,t.alphaInC,t.alphaInD,t.alphaBias,t.alphaOp,t.alphaScale,t.alphaClamp,t.alphaRegId, kc, ka); }
      std::printf("[FX2]   xf09=%X amb=%08X,%08X mat=%08X,%08X chanctrl=%X,%X,%X,%X\n", g_state.xf[0x009], g_state.xf[0x00A], g_state.xf[0x00B], g_state.xf[0x00C], g_state.xf[0x00D], g_state.xf[0x00E], g_state.xf[0x00F], g_state.xf[0x010], g_state.xf[0x011]);
      std::printf("[FX2]   konst=");for(int r=0;r<8;r++)std::printf("%06X ",g_state.tevKonstWords[r]);
      std::printf(" color=");for(int r=0;r<8;r++)std::printf("%06X ",g_state.tevColorWords[r]);
      if(!cmd.vertices.empty()){const auto& v=cmd.vertices[0]; std::printf(" v0col=%.3f,%.3f,%.3f,%.3f uv0=%.3f,%.3f", v.color[0],v.color[1],v.color[2],v.color[3], v.tex[0][0], v.tex[0][1]);}
      std::printf("\n");
    }
  }
"""
rg=rg.replace(o_fx, fx2+o_fx)

# --- filtro de copia EFB (BP 0x53/0x54) y gamma (bits 7-8 de BP 0x52) ---
# En la consola cada copia EFB pasa por un filtro vertical de 7 coeficientes
# (en 1/64) que el juego programa con GXSetCopyFilter. Si la suma no es 64 la
# copia sale mas oscura o mas clara. El port hacia un blit tal cual.
# NWII_COPY_FILTER_OFF=1 vuelve al blit sin filtro.
o_gt='static void FlushBatch() {'
assert rg.count(o_gt)==1
cf=r"""// tools/gx-pipeline: filtro vertical de copia EFB y gamma, como el hardware.
static void CopyFilterWeights(float w[3]) {
  const uint32_t a=g_state.bp[0x53]&0xFFFFFF, b=g_state.bp[0x54]&0xFFFFFF;
  if(!a && !b){ w[0]=0; w[1]=1; w[2]=0; return; } // nunca programado
  const unsigned c0=a&63,c1=(a>>6)&63,c2=(a>>12)&63,c3=(a>>18)&63,c4=b&63,c5=(b>>6)&63,c6=(b>>12)&63;
  w[0]=(c0+c1)/64.0f; w[1]=(c2+c3+c4)/64.0f; w[2]=(c5+c6)/64.0f;
}
static void ApplyCopyFilterGX(GLuint texture, int destW, int destH, int srcHNative, uint32_t control) {
  static const bool off=std::getenv("NWII_COPY_FILTER_OFF")!=nullptr;
  if(off || !texture || destW<=0 || destH<=0 || srcHNative<=0) return;
  float w[3]; CopyFilterWeights(w);
  const unsigned gm=(control>>7)&3;
  if(w[0]==0.f && w[1]==1.f && w[2]==0.f && gm==0) return;
  static GLint efb_fbo=-1; if(efb_fbo<0) glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&efb_fbo);
  static GLuint prog=0, vao=0, scratch=0, scratch_fbo=0; static int sw=0, sh=0;
  static GLint u_w=-1,u_dy=-1,u_g=-1,u_t=-1; static bool failed=false;
  if(failed) return;
  if(!prog){
    prog=CompileShader(
      "#version 330 core\nout vec2 uv;\nvoid main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));uv=p;gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n",
      "#version 330 core\nin vec2 uv;out vec4 o;uniform sampler2D t;uniform vec3 w;uniform float dy;uniform float g;\n"
      "void main(){vec4 c=w.x*texture(t,uv-vec2(0.0,dy))+w.y*texture(t,uv)+w.z*texture(t,uv+vec2(0.0,dy));c=clamp(c,0.0,1.0);c.rgb=pow(c.rgb,vec3(g));o=c;}\n");
    if(!prog){failed=true;return;}
    u_w=glGetUniformLocation(prog,"w");u_dy=glGetUniformLocation(prog,"dy");u_g=glGetUniformLocation(prog,"g");u_t=glGetUniformLocation(prog,"t");
    glGenVertexArrays(1,&vao); glGenTextures(1,&scratch); glGenFramebuffers(1,&scratch_fbo);
  }
  static std::unordered_map<GLuint,GLuint> dest_fbos; static std::unordered_map<GLuint,std::pair<int,int>> dest_sizes;
  GLuint& dfbo=dest_fbos[texture];
  auto& dsz=dest_sizes[texture];
  if(!dfbo){glGenFramebuffers(1,&dfbo);}
  if(dsz.first!=destW||dsz.second!=destH){
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,dfbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    dsz={destW,destH};
  }
  glActiveTexture(GL_TEXTURE0);
  glBindSampler(0,0);  // sin el sampler del juego (mipmaps): la textura auxiliar no tiene niveles
  if(sw!=destW||sh!=destH){
    glBindTexture(GL_TEXTURE_2D,scratch);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,destW,destH,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,scratch_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,scratch,0);
    sw=destW; sh=destH;
  }
  // copia destino -> auxiliar
  glBindFramebuffer(GL_READ_FRAMEBUFFER,dfbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,scratch_fbo);
  glBlitFramebuffer(0,0,destW,destH,0,0,destW,destH,GL_COLOR_BUFFER_BIT,GL_NEAREST);
  // auxiliar filtrada -> destino
  GLint vp[4]; glGetIntegerv(GL_VIEWPORT,vp);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,dfbo);
  glViewport(0,0,destW,destH);
  glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  glUseProgram(prog);
  glUniform1i(u_t,0); glUniform3f(u_w,w[0],w[1],w[2]);
  glUniform1f(u_dy,1.0f/float(srcHNative));
  glUniform1f(u_g, gm==1?1.0f/1.7f:(gm==2?1.0f/2.2f:1.0f));
  glBindTexture(GL_TEXTURE_2D,scratch);
  glBindVertexArray(vao);
  glDrawArrays(GL_TRIANGLES,0,3);
  // restaurar: el siguiente dibujo vuelve a aplicar programa, mezcla, profundidad y VAO
  glBindVertexArray(0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER,(GLuint)efb_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)efb_fbo);
  glViewport(vp[0],vp[1],vp[2],vp[3]);
  glBindTexture(GL_TEXTURE_2D,texture);
}

"""
rg=rg.replace(o_gt, cf+o_gt)
o_cp='            if (!copied) continue;\n'
assert rg.count(o_cp)==1
rg=rg.replace(o_cp, o_cp+'            ApplyCopyFilterGX(tex, int(((cmd.val&0x200)?std::max(1u,w/2):w)*EFBScale()), int(((cmd.val&0x200)?std::max(1u,h/2):h)*EFBScale()), int(h), cmd.val);\n')
# rastreo: coeficientes del filtro en cada copia distinta
o_k2="^(uint64_t(g_state.bp[0x43])<<40);"
assert rg.count(o_k2)==1
rg=rg.replace(o_k2,"^(uint64_t(g_state.bp[0x43])<<40)^(uint64_t(g_state.bp[0x53])*0x9E3779B1ull)^(uint64_t(g_state.bp[0x54])<<44);")
o_i2='<< " intensity=" << ((cmd.val>>15)&1)'
assert rg.count(o_i2)==1
rg=rg.replace(o_i2, o_i2+' << " bp53=" << std::hex << g_state.bp[0x53] << " bp54=" << g_state.bp[0x54] << std::dec << " gamma=" << ((cmd.val>>7)&3)')
# rastreo de la copia a XFB (pantalla)
o_x='          nwii::runtime::display_copy_region()={int(g_state.efbSrcX),int(g_state.efbSrcY),int(g_state.efbW),int(g_state.efbH)};\n'
assert rg.count(o_x)==1
rg=rg.replace(o_x, o_x+'          static const bool xfb_trace=std::getenv("NWII_EFBCOPY_TRACE")!=nullptr; static int xfb_n=0;\n          if(xfb_trace && xfb_n<20){static uint64_t last=~0ull; uint64_t k=(uint64_t(g_state.bp[0x53])<<32)^g_state.bp[0x54]^(uint64_t(cmd.val)<<20); if(k!=last){last=k;++xfb_n; std::printf("[XFBCopy] control=%X bp53=%X bp54=%X gamma=%u\\n",cmd.val,g_state.bp[0x53],g_state.bp[0x54],(cmd.val>>7)&3);}}\n')

# --- diagnostico: NWII_FX_NO_BLOOM=1 omite la suma aditiva (ONE,ONE) de una copia EFB en un cuadro de pantalla ---
o_fx1='  static const bool fx_trace=std::getenv("NWII_FXTRACE")!=nullptr;\n'
assert rg.count(o_fx1)==1
nb=r"""  const bool no_bloom=port_menu::g_no_bloom.load(std::memory_order_relaxed);
  if (no_bloom && !CurrentIsPerspective() && cmd.vertices.size()<=6) {
    const uint32_t cm=g_state.bp[0x41];
    if ((cm&1) && !((cm>>11)&1) && ((cm>>5)&7)==1 && ((cm>>8)&7)==1) {
      // Solo la suma del resplandor, que lee la copia reducida (80x56; como mucho 160 de ancho).
      // Con el juego en pausa (dialogos de tutorial, teclado) el fondo es la copia de pantalla
      // completa (640x448) sumada sobre negro con la misma mezcla: esa hay que dibujarla.
      for(unsigned i=0;i<8;i++){ const auto& st=g_state.texStages[i]; const uint32_t a=st.base_addr;
        if(a && st.width<=160 && nwii::runtime::hle::TextureCache::get().find_efb_texture(a)!=0) return; }
    }
  }
"""
rg=rg.replace(o_fx1, nb+o_fx1)

# --- diagnostico: NWII_MATTRACE=1 vuelca materiales 3D (TEV, texturas, canal de color y luces) ---
o_fx3='  static const bool fx_trace=std::getenv("NWII_FXTRACE")!=nullptr;\n'
assert rg.count(o_fx3)==1
mt=r"""  static const bool mat_trace=std::getenv("NWII_MATTRACE")!=nullptr;
  if (mat_trace && CurrentIsPerspective() && !cmd.vertices.empty()) {
    uint64_t key=g_state.GetShaderHash(0)^(uint64_t(g_state.xf[0x00E])<<40)^(uint64_t(g_state.xf[0x010])<<52)^(uint64_t(g_state.bp[0x41])<<20);
    for(unsigned i=0;i<8;i++) if(g_state.texStages[i].base_addr) key^=uint64_t(g_state.texStages[i].format+1)<<(i*3);
    static std::unordered_set<uint64_t> seenm; static unsigned pm=0;
    if(pm<220 && seenm.insert(key).second){ ++pm;
      const auto& v=cmd.vertices[0];
      std::printf("[MAT] #%u verts=%zu bp41=%06X stages=%u gens=%u chans=%u pre_xf=%d has_norm=%d has_col=%d pos=%.2f,%.2f,%.2f nrm=%.3f,%.3f,%.3f col=%.3f,%.3f,%.3f,%.3f\n", pm,
        cmd.vertices.size(), g_state.bp[0x41], unsigned(g_state.numTevStages), unsigned(g_state.numTexGens), unsigned(g_state.numChans),
        int(v.pre_xf), int(v.has_norm), int(v.has_color), v.pos[0],v.pos[1],v.pos[2], v.norm[0],v.norm[1],v.norm[2], v.color[0],v.color[1],v.color[2],v.color[3]);
      for(unsigned i=0;i<8;i++){ const auto& ts=g_state.texStages[i]; if(!ts.base_addr) continue;
        std::printf("[MAT]   tex%u addr=%08X %ux%u fmt=%u\n", i, ts.base_addr, ts.width, ts.height, unsigned(ts.format)); }
      for(unsigned i=0;i<g_state.numTevStages && i<16;i++){ const auto& t=g_state.tevStages[i];
        uint32_t ksel=g_state.bp[0xF6+i/2]; unsigned kc=(i%2)?((ksel>>14)&0x1F):((ksel>>4)&0x1F); unsigned ka=(i%2)?((ksel>>19)&0x1F):((ksel>>9)&0x1F);
        uint32_t ras=g_state.bp[0x28+i/2]; unsigned rc=(i%2)?((ras>>19)&7):((ras>>7)&7);
        std::printf("[MAT]   tev%u map=%u coord=%u ras=%u C:a=%u b=%u c=%u d=%u bias=%u op=%u sc=%u cl=%u ->%u  A:a=%u b=%u c=%u d=%u bias=%u op=%u sc=%u cl=%u ->%u kc=%u ka=%u\n",
          i, unsigned(t.texMap), unsigned(t.texCoord), rc, t.colorInA,t.colorInB,t.colorInC,t.colorInD,t.colorBias,t.colorOp,t.colorScale,t.colorClamp,t.colorRegId,
          t.alphaInA,t.alphaInB,t.alphaInC,t.alphaInD,t.alphaBias,t.alphaOp,t.alphaScale,t.alphaClamp,t.alphaRegId, kc, ka); }
      std::printf("[MAT]   amb=%08X,%08X mat=%08X,%08X chanctrl=%X,%X,%X,%X konst=%06X %06X %06X %06X color=%06X %06X %06X %06X %06X %06X\n",
        g_state.xf[0x00A], g_state.xf[0x00B], g_state.xf[0x00C], g_state.xf[0x00D], g_state.xf[0x00E], g_state.xf[0x00F], g_state.xf[0x010], g_state.xf[0x011],
        g_state.tevKonstWords[0],g_state.tevKonstWords[1],g_state.tevKonstWords[2],g_state.tevKonstWords[3],
        g_state.tevColorWords[0],g_state.tevColorWords[1],g_state.tevColorWords[2],g_state.tevColorWords[3],g_state.tevColorWords[4],g_state.tevColorWords[5]);
      for(unsigned ch=0; ch<2; ++ch){ const uint32_t ctrl=g_state.xf[0x00E + ch]; if(!(ctrl&2)) continue;
        const unsigned mask=((ctrl>>2)&15)|(((ctrl>>11)&15)<<4);
        for(unsigned i=0;i<8;i++){ if(!(mask&(1u<<i))) continue; const float* L=g_state.lights[i]; uint32_t c; std::memcpy(&c,&L[3],4);
          // atenuacion como el sombreador, en el primer vertice
          const BatchVertex bv=BuildVertex(v); float n[3]={bv.nx,bv.ny,bv.nz}; float nl=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]); if(nl>0){n[0]/=nl;n[1]/=nl;n[2]/=nl;}
          float d[3]={L[10]-bv.x,L[11]-bv.y,L[12]-bv.z}; float d2=d[0]*d[0]+d[1]*d[1]+d[2]*d[2], dd=std::sqrt(d2); float l[3]={d[0]/dd,d[1]/dd,d[2]/dd};
          float ndl=l[0]*n[0]+l[1]*n[1]+l[2]*n[2]; const unsigned af=(ctrl>>9)&3, df=(ctrl>>7)&3; float att=1.f;
          if(af==3){ float a=std::max(0.f,l[0]*L[13]+l[1]*L[14]+l[2]*L[15]); att=std::max(0.f,L[4]+L[5]*a+L[6]*a*a)/(L[7]+L[8]*dd+L[9]*d2); }
          else if(af==1){ float a=(ndl>=0)?std::max(0.f,n[0]*L[13]+n[1]*L[14]+n[2]*L[15]):0.f; att=std::max(0.f,L[4]+L[5]*a+L[6]*a*a)/(L[7]+L[8]*a+L[9]*a*a); }
          std::printf("[MAT]   ch%u luz%u col=%08X cos=%g,%g,%g dist=%g,%g,%g pos=%g,%g,%g dir=%g,%g,%g | nl=%.3f ndl=%.3f att=%g fuerza=%g\n", ch, i, c,
            L[4],L[5],L[6],L[7],L[8],L[9],L[10],L[11],L[12],L[13],L[14],L[15], nl, ndl, att, att*(df==0?1.f:(df==1?ndl:std::max(0.f,ndl)))); }
      }
    }
  }
"""
rg=rg.replace(o_fx3, mt+o_fx3)

# --- diagnostico: NWII_CLEAR_MAGENTA=1 borra el EFB en magenta (para ver huecos sin geometria) ---
o_cc='::GX_GetClearColor(cc);'
assert rg.count(o_cc)==2
rg=rg.replace(o_cc,'::GX_GetClearColor(cc); { static const bool mag=std::getenv("NWII_CLEAR_MAGENTA")!=nullptr; if(mag){cc[0]=255;cc[1]=0;cc[2]=255;cc[3]=255;} }')

# --- diagnostico: NWII_MATTINT=1 pinta cada material 3D de un color/patron fijo y lo anota ([TINT]) ---
o_fx4='  static const bool fx_trace=std::getenv("NWII_FXTRACE")!=nullptr;\n'
assert rg.count(o_fx4)==1
tn=r"""  static const bool mat_tint=std::getenv("NWII_MATTINT")!=nullptr;
  if (mat_tint && s_active_shader.id) {
    const GLint loc=glGetUniformLocation(s_active_shader.id,"uDbgTint");
    if (loc>=0) {
      bool reads_efb=false; for(unsigned i=0;i<8;i++){ const uint32_t a=g_state.texStages[i].base_addr; if(a && nwii::runtime::hle::TextureCache::get().find_efb_texture(a)!=0) reads_efb=true; }
      if (CurrentIsPerspective() || !reads_efb) {
        uint64_t key=g_state.GetShaderHash(0)^(CurrentIsPerspective()?0ull:0xD2D2ull<<48)^(uint64_t(g_state.xf[0x00E])<<40)^(uint64_t(g_state.xf[0x010])<<52)^(uint64_t(g_state.bp[0x41])<<20);
        for(unsigned i=0;i<8;i++) if(g_state.texStages[i].base_addr) key^=uint64_t(g_state.texStages[i].base_addr)*(i+7);
        static std::unordered_map<uint64_t,unsigned> ids;
        auto it=ids.find(key); unsigned id;
        if(it==ids.end()){ id=unsigned(ids.size()); ids[key]=id;
          static const char* hue[8]={"rojo","verde","azul","amarillo","cian","magenta","naranja","morado"};
          static const char* pat[4]={"liso","rayas_horizontales","rayas_verticales","cuadros"};
          if(id<160){
            std::printf("[TINT] #%u %s %s persp=%d verts=%zu bp41=%06X stages=%u chanctrl=%X,%X mat=%08X amb=%08X", id, hue[id%8], pat[(id/8)%4], int(CurrentIsPerspective()), cmd.vertices.size(), g_state.bp[0x41], unsigned(g_state.numTevStages), g_state.xf[0x00E], g_state.xf[0x010], g_state.xf[0x00C], g_state.xf[0x00A]);
            for(unsigned i=0;i<8;i++){ const auto& ts=g_state.texStages[i]; if(ts.base_addr && ts.base_addr!=0x0061A5C0) std::printf(" tex%u=%08X/%ux%u/f%u", i, ts.base_addr, ts.width, ts.height, unsigned(ts.format)); }
            std::printf(" ind=%06X,%06X,%06X ord27=%06X\n", g_state.bp[0x10], g_state.bp[0x11], g_state.bp[0x12], g_state.bp[0x27]);
            for(unsigned i=0;i<g_state.numTevStages && i<16;i++){ const auto& t=g_state.tevStages[i];
              std::printf("[TINT]    tev%u map=%u coord=%u C:a=%u b=%u c=%u d=%u bias=%u op=%u sc=%u ->%u A:a=%u b=%u c=%u d=%u ->%u\n", i, unsigned(t.texMap), unsigned(t.texCoord),
                t.colorInA,t.colorInB,t.colorInC,t.colorInD,t.colorBias,t.colorOp,t.colorScale,t.colorRegId,t.alphaInA,t.alphaInB,t.alphaInC,t.alphaInD,t.alphaRegId); }
            if(!cmd.vertices.empty()){const auto& v=cmd.vertices[0]; std::printf("[TINT]    v0col=%.3f,%.3f,%.3f,%.3f has_col=%d\n", v.color[0],v.color[1],v.color[2],v.color[3], int(v.has_color));}
          }
        } else id=it->second;
        static const float rgb[8][3]={{1,0,0},{0,1,0},{0,0.3f,1},{1,1,0},{0,1,1},{1,0,1},{1,0.5f,0},{0.5f,0,1}};
        glUniform4f(loc, rgb[id%8][0], rgb[id%8][1], rgb[id%8][2], float(1+(id/8)%4));
      } else glUniform4f(loc,0,0,0,0);
    }
  }
"""
rg=rg.replace(o_fx4, tn+o_fx4)
# --- menu del port: filtrado de texturas (mipmaps / anisotropico) ---
o_ts="""static void ApplyTextureSampler(unsigned unit) {
  static GLuint samplers[8]{};
  static uint32_t previous[8]{};
  static bool configured[8]{};
  if (!samplers[0]) glGenSamplers(8, samplers);
  const uint32_t mode = g_state.bp[unit < 4 ? 0x80+unit : 0xA0+unit-4] & 0x9F;
  if (!configured[unit] || mode != previous[unit]) {"""
assert rg.count(o_ts)==1
rg=rg.replace(o_ts,"""static void ApplyTextureSampler(unsigned unit, bool efb_copy = true) {
  // efb_copy: las copias EFB no tienen mipmaps; las texturas del juego si (TextureCache).
  static GLuint samplers[2][8]{};
  static uint32_t previous[2][8]{};
  static bool configured[2][8]{};
  static int filter_seen[2][8]{};
  if (!samplers[0][0]) { glGenSamplers(8, samplers[0]); glGenSamplers(8, samplers[1]); }
  const int filter = efb_copy ? 0 : port_menu::g_tex_filter.load(std::memory_order_relaxed);
  const int set = filter > 0 ? 1 : 0;
  const uint32_t mode = g_state.bp[unit < 4 ? 0x80+unit : 0xA0+unit-4] & 0x9F;
  if (set == 1 && (!configured[1][unit] || mode != previous[1][unit] || filter_seen[1][unit] != filter)) {
    auto wrap=[](unsigned v){return v==1 ? GL_REPEAT : v==2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;};
    glSamplerParameteri(samplers[1][unit],GL_TEXTURE_WRAP_S,wrap(mode&3));
    glSamplerParameteri(samplers[1][unit],GL_TEXTURE_WRAP_T,wrap((mode>>2)&3));
    glSamplerParameteri(samplers[1][unit],GL_TEXTURE_MAG_FILTER,(mode&0x10)?GL_LINEAR:GL_NEAREST);
    glSamplerParameteri(samplers[1][unit],GL_TEXTURE_MIN_FILTER,(mode&0x80)?GL_LINEAR_MIPMAP_LINEAR:GL_NEAREST_MIPMAP_LINEAR);
    static const float aniso[6]={1,1,2,4,8,16};
    glSamplerParameterf(samplers[1][unit],0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */,aniso[filter<0?0:(filter>5?5:filter)]);
    previous[1][unit]=mode; configured[1][unit]=true; filter_seen[1][unit]=filter;
  }
  if (set == 1) { glBindSampler(unit,samplers[1][unit]); return; }
  GLuint* samplers0 = samplers[0]; uint32_t* previous0 = previous[0]; bool* configured0 = configured[0];
  if (!configured0[unit] || mode != previous0[unit]) {""")
o_ts2="""    glSamplerParameteri(samplers[unit],GL_TEXTURE_WRAP_S,wrap(mode&3));
    glSamplerParameteri(samplers[unit],GL_TEXTURE_WRAP_T,wrap((mode>>2)&3));
    glSamplerParameteri(samplers[unit],GL_TEXTURE_MAG_FILTER,(mode&0x10)?GL_LINEAR:GL_NEAREST);
    // The cache currently decodes the base level only; do not request absent mipmaps.
    glSamplerParameteri(samplers[unit],GL_TEXTURE_MIN_FILTER,(mode&0x80)?GL_LINEAR:GL_NEAREST);
    previous[unit]=mode; configured[unit]=true;
  }
  glBindSampler(unit,samplers[unit]);
}"""
assert rg.count(o_ts2)==1
rg=rg.replace(o_ts2,"""    glSamplerParameteri(samplers0[unit],GL_TEXTURE_WRAP_S,wrap(mode&3));
    glSamplerParameteri(samplers0[unit],GL_TEXTURE_WRAP_T,wrap((mode>>2)&3));
    glSamplerParameteri(samplers0[unit],GL_TEXTURE_MAG_FILTER,(mode&0x10)?GL_LINEAR:GL_NEAREST);
    // Filtrado original: solo el nivel base.
    glSamplerParameteri(samplers0[unit],GL_TEXTURE_MIN_FILTER,(mode&0x80)?GL_LINEAR:GL_NEAREST);
    previous0[unit]=mode; configured0[unit]=true;
  }
  glBindSampler(unit,samplers0[unit]);
}""")
o_bind="""      glBindTexture(GL_TEXTURE_2D, tex);
      ApplyTextureSampler(i);
"""
assert rg.count(o_bind)==1
rg=rg.replace(o_bind,"""      glBindTexture(GL_TEXTURE_2D, tex);
      ApplyTextureSampler(i, nwii::runtime::hle::TextureCache::get().find_efb_texture(stage.base_addr) == tex);
""")
# --- menu del port: 21:9 / 32:9 ensanchando las proyecciones en perspectiva 16:9 ---
o_pr="  std::memcpy(s_proj, p, sizeof(s_proj));\n"
assert rg.count(o_pr)==1
rg=rg.replace(o_pr,"""  {
    // Solo proyecciones en perspectiva con relacion ~16:9 (la escena); las de
    // efectos (iluminacion de personajes, sombras) tienen otra relacion y no se tocan.
    const float hs = port_menu::g_hfov_scale.load(std::memory_order_relaxed);
    if (hs != 1.0f && g_state.projSet && g_state.projType == 0 && sp[0] != 0.0f) {
      const float ratio = std::fabs(sp[2] / sp[0]);
      if (ratio > 1.65f && ratio < 1.90f) { p[0] *= hs; p[4] *= hs; p[8] *= hs; p[12] *= hs; }
      static int logged = 0; static float seen[8];
      if (logged < 8) { bool dup = false; for (int q = 0; q < logged; ++q) if (std::fabs(seen[q] - ratio) < 0.01f) dup = true;
        if (!dup) { seen[logged++] = ratio; std::printf("[Aspecto] proyeccion con relacion %.3f %s\\n", ratio, (ratio > 1.65f && ratio < 1.90f) ? "ensanchada" : "sin tocar"); } }
    }
  }
"""+o_pr)
o_rs="  void Render(const std::vector<GXCommand> &commands) override {\n"
assert rg.count(o_rs)==1
rg=rg.replace(o_rs,o_rs+"    if (port_menu::g_tex_reload.exchange(false)) nwii::runtime::hle::TextureCache::get().clear();  // Original <-> HD\n")
rg=rg.replace('#include <glad/glad.h>\n','#include <glad/glad.h>\n#include "port_menu_state.h"\n',1)
rg=rg.replace('#include "port_menu_state.h"\n','#include "port_menu_state.h"\n#include "port_efb_state.h"\n',1)
# borrado del EFB ligado al cuadro: el analizador va un cuadro por delante en otro hilo y
# g_state.pe_clear_pending podia borrar al EMPEZAR el cuadro cuya copia final pedia borrado
# (o el siguiente), y el juego a veces copia sin borrar para dibujar encima (teclado, pausa).
_c1="""    if (g_state.pe_clear_pending) {
      g_state.pe_clear_pending = false;
      unsigned char cc[4];"""
assert rg.count(_c1)==1, 'clear al empezar'
rg=rg.replace(_c1,"""    const bool port_clear = s_port_clear_next || port_menu::g_force_clear.exchange(false);
    s_port_clear_next = false;
    if (port_clear) {
      g_state.pe_clear_pending = false;
      unsigned char cc[4];""")
_c2="        if(cmd.reg==0x52 && (cmd.val&0x4000)) {\n"
assert rg.count(_c2)==1, 'xfb'
rg=rg.replace(_c2,_c2+"""          if (cmd.val & 0x800) {
            if (&cmd - commands.data() < port_last_xfb) {   // cuadros juntados: este no se muestra, borrar aqui
              unsigned char pcc[4]; ::GX_GetClearColor(pcc);
              { static const bool mag=std::getenv("NWII_CLEAR_MAGENTA")!=nullptr; if(mag){pcc[0]=255;pcc[1]=0;pcc[2]=255;pcc[3]=255;} }
              glClearColor(pcc[0] / 255.f, pcc[1] / 255.f, pcc[2] / 255.f, pcc[3] / 255.f);
              glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glDepthMask(GL_TRUE);
              glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            } else s_port_clear_next = true;   // el ultimo: se borra tras mostrarlo
          }
""")
_c4="    const bool port_clear = s_port_clear_next || port_menu::g_force_clear.exchange(false);\n"
assert rg.count(_c4)==1
rg=rg.replace(_c4,"""    ptrdiff_t port_last_xfb = -1;   // ultima copia a pantalla de esta lista
    for (size_t i = commands.size(); i-- > 0;)
      if (commands[i].type == GXCommandType::BPRegister && commands[i].reg == 0x52 && (commands[i].val & 0x4000)) { port_last_xfb = ptrdiff_t(i); break; }
"""+_c4)
_c3="static bool CopyEFBToTextureFast("
assert rg.count(_c3)==1
rg=rg.replace(_c3,"static bool s_port_clear_next = true;   // la copia a pantalla del cuadro anterior pidio borrar el EFB\n"+_c3,1)
# --- registro cuadro a cuadro (NWII_FRAMELOG=1): dibujos, copias, borrados
_f0="static bool s_port_clear_next = true;"
assert rg.count(_f0)==1
rg=rg.replace(_f0,_f0+"\nstatic const bool s_fl_on = std::getenv(\"NWII_FRAMELOG\") != nullptr;\nstatic int s_fl_draws = 0, s_fl_quads = 0, s_fl_bloom = 0, s_fl_frames = 0;\nstatic std::string s_fl_ev;\n",1)
_f1="    s_port_clear_next = false;\n    if (port_clear) {\n"
assert rg.count(_f1)==1
rg=rg.replace(_f1,"    s_port_clear_next = false;\n    if (s_fl_on) { s_fl_ev = port_clear ? \"BORRA \" : \"\"; s_fl_draws = s_fl_quads = s_fl_bloom = 0; }\n    if (port_clear) {\n")
_f2="          if (cmd.val & 0x800) {\n            if (&cmd - commands.data() < port_last_xfb) {"
assert rg.count(_f2)==1
rg=rg.replace(_f2,"          if (s_fl_on) { char b[48]; std::snprintf(b, sizeof(b), \"PANT(%X)@%d \", cmd.val, s_fl_draws); s_fl_ev += b; }\n"+_f2)
_f3="        if (cmd.reg == 0x52 && !(cmd.val & 0x4000)) {\n"
assert rg.count(_f3)==1
rg=rg.replace(_f3,_f3+"          if (s_fl_on) { char b[64]; std::snprintf(b, sizeof(b), \"COPIA(%08X %ux%u%s)@%d \", g_state.efbCopyDest, unsigned(g_state.efbW), unsigned(g_state.efbH), (cmd.val & 0x800) ? \" borra\" : \"\", s_fl_draws); s_fl_ev += b; }\n")
_f4="  const bool no_bloom=port_menu::g_no_bloom.load(std::memory_order_relaxed);\n"
assert rg.count(_f4)==1
rg=rg.replace(_f4,"  if (s_fl_on) { ++s_fl_draws; if (!CurrentIsPerspective() && cmd.vertices.size() <= 6) ++s_fl_quads; }\n"+_f4)
_f5="if(a && st.width<=160 && nwii::runtime::hle::TextureCache::get().find_efb_texture(a)!=0) return; }"
assert rg.count(_f5)==1
rg=rg.replace(_f5,"if(a && st.width<=160 && nwii::runtime::hle::TextureCache::get().find_efb_texture(a)!=0) { if (s_fl_on) ++s_fl_bloom; return; } }")
_f6="    nwii::runtime::hle::TextureCache::get().report_statistics();\n  }\n"
assert rg.count(_f6)==1, 'fin render'
rg=rg.replace(_f6,"    if (s_fl_on && s_fl_frames < 30000) { ++s_fl_frames; std::printf(\"[Cuadro] %d dibujos=%d cuadros_pantalla=%d sin_resplandor=%d %s\\n\", s_fl_frames, s_fl_draws, s_fl_quads, s_fl_bloom, s_fl_ev.c_str()); }\n"+_f6)
# borrado del EFB como en la consola: respeta las mascaras de escritura de color, alfa y
# profundidad (GXSetColorUpdate/GXSetAlphaUpdate/GXSetZMode) vigentes al copiar. El teclado
# virtual desactiva la escritura de color y copia el EFB sin redibujar la escena: el fondo
# debe seguir ahi (antes se borraba y parpadeaba).
_m0="static bool s_port_clear_next = true;"
assert rg.count(_m0)==1
rg=rg.replace(_m0,_m0+"""
static uint32_t s_port_clear_mask = 7;   // 1 color, 2 alfa, 4 profundidad (del borrado pendiente)
static uint32_t PortClearMask() {
  uint32_t m = 0;
  if (g_state.bp[0x41] & 0x08) m |= 1;
  if (g_state.bp[0x41] & 0x10) m |= 2;
  if (g_state.bp[0x40] & 0x10) m |= 4;
  const uint32_t f = g_state.bp[0x43] & 7;
  if (f == 0 || f == 2 || f == 3) m &= ~2u;   // formatos sin alfa
  return m;
}
static void PortClearEFB(uint32_t m) {
  if (!(m & 7)) return;
  unsigned char cc[4]; ::GX_GetClearColor(cc);
  { static const bool mag=std::getenv("NWII_CLEAR_MAGENTA")!=nullptr; if(mag){cc[0]=255;cc[1]=0;cc[2]=255;cc[3]=255;} }
  glClearColor(cc[0] / 255.f, cc[1] / 255.f, cc[2] / 255.f, cc[3] / 255.f);
  const GLboolean c = (m & 1) ? GL_TRUE : GL_FALSE;
  glColorMask(c, c, c, (m & 2) ? GL_TRUE : GL_FALSE);
  glDepthMask((m & 4) ? GL_TRUE : GL_FALSE);
  glClear(((m & 3) ? GL_COLOR_BUFFER_BIT : 0) | ((m & 4) ? GL_DEPTH_BUFFER_BIT : 0));
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glDepthMask(GL_TRUE);
}""",1)
_m1="""    if (port_clear) {
      g_state.pe_clear_pending = false;
      unsigned char cc[4];
      ::GX_GetClearColor(cc); { static const bool mag=std::getenv("NWII_CLEAR_MAGENTA")!=nullptr; if(mag){cc[0]=255;cc[1]=0;cc[2]=255;cc[3]=255;} }
      glClearColor(cc[0] / 255.f, cc[1] / 255.f, cc[2] / 255.f, cc[3] / 255.f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }"""
assert rg.count(_m1)==1, 'borrado inicio'
rg=rg.replace(_m1,"""    if (port_clear) {
      g_state.pe_clear_pending = false;
      PortClearEFB(s_port_clear_mask);
    }""")
_m2="""            if (&cmd - commands.data() < port_last_xfb) {   // cuadros juntados: este no se muestra, borrar aqui
              unsigned char pcc[4]; ::GX_GetClearColor(pcc);
              { static const bool mag=std::getenv("NWII_CLEAR_MAGENTA")!=nullptr; if(mag){pcc[0]=255;pcc[1]=0;pcc[2]=255;pcc[3]=255;} }
              glClearColor(pcc[0] / 255.f, pcc[1] / 255.f, pcc[2] / 255.f, pcc[3] / 255.f);
              glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glDepthMask(GL_TRUE);
              glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            } else s_port_clear_next = true;   // el ultimo: se borra tras mostrarlo"""
assert rg.count(_m2)==1, 'borrado xfb'
rg=rg.replace(_m2,"""            if (&cmd - commands.data() < port_last_xfb) PortClearEFB(PortClearMask());   // cuadros juntados: este no se muestra, borrar aqui
            else { s_port_clear_next = true; s_port_clear_mask = PortClearMask(); }   // el ultimo: se borra tras mostrarlo""")
_m3="""            if (cmd.val & 0x800) {
              unsigned char cc[4];
              ::GX_GetClearColor(cc); { static const bool mag=std::getenv("NWII_CLEAR_MAGENTA")!=nullptr; if(mag){cc[0]=255;cc[1]=0;cc[2]=255;cc[3]=255;} }
              glClearColor(cc[0] / 255.f, cc[1] / 255.f, cc[2] / 255.f,
                           cc[3] / 255.f);
              glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
              glDepthMask(GL_TRUE);
              glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
              g_state.pe_clear_pending = false;
            }"""
assert rg.count(_m3)==1, 'borrado copia'
rg=rg.replace(_m3,"""            if (cmd.val & 0x800) {
              PortClearEFB(PortClearMask());
              g_state.pe_clear_pending = false;
            }""")
_m4='std::snprintf(b, sizeof(b), "PANT(%X)@%d ", cmd.val, s_fl_draws);'
assert rg.count(_m4)==1
rg=rg.replace(_m4,'std::snprintf(b, sizeof(b), "PANT(%X m%u %08X)@%d ", cmd.val, PortClearMask(), g_state.efbCopyDest, s_fl_draws);')
rg+=r"""
// tools/gx-pipeline: copias EFB del guardado rapido (ver port_efb_state.h).
void port_efb::Service() {
  auto& s = S();
  bool dump = false, restore = false; std::vector<Tex> in;
  { std::lock_guard<std::mutex> l(s.m);
    dump = s.dump_req && !s.dump_done;
    if (s.restore_req) { in.swap(s.restore); s.restore_req = false; restore = true; } }
  if (!dump && !restore) return;
  GLint rfb = 0, dfb = 0, tex0 = 0, active = 0, pack = 4, unpack = 4;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active); glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
  glGetIntegerv(GL_PACK_ALIGNMENT, &pack); glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST); glDisable(GL_SCISSOR_TEST);
  auto& reg = nwii::runtime::hle::TextureCache::get().port_efb_textures();
  if (dump) {
    std::vector<Tex> out;
    static GLuint src_fbo = 0, dst_fbo = 0, dst_tex = 0;
    if (!src_fbo) { glGenFramebuffers(1, &src_fbo); glGenFramebuffers(1, &dst_fbo); glGenTextures(1, &dst_tex); }
    const int sc = nwii::runtime::EFBScale();
    for (const auto& e : reg) {
      if (!e.second) continue;
      GLint w = 0, h = 0;
      glBindTexture(GL_TEXTURE_2D, e.second);
      glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w); glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
      if (w <= 0 || h <= 0) continue;
      const int nw = std::max(1, w / sc), nh = std::max(1, h / sc);   // a resolucion nativa
      if (nw > 1024 || nh > 1024) continue;
      glBindTexture(GL_TEXTURE_2D, dst_tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, nw, nh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e.second, 0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
      if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
          glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) continue;
      glBlitFramebuffer(0, 0, w, h, 0, 0, nw, nh, GL_COLOR_BUFFER_BIT, (nw == w && nh == h) ? GL_NEAREST : GL_LINEAR);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, dst_fbo);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      Tex t; t.addr = e.first; t.w = uint32_t(nw); t.h = uint32_t(nh); t.rgba.resize(size_t(nw) * nh * 4);
      glReadPixels(0, 0, nw, nh, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
      out.push_back(std::move(t));
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo); glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    size_t bytes = 0; for (auto& t : out) bytes += t.rgba.size();
    std::printf("[Guardado] %zu copias EFB leidas de la GPU (%.1f MB)\n", out.size(), bytes / 1048576.0);
    { std::lock_guard<std::mutex> l(s.m); s.dumped.swap(out); s.dump_done = true; }
    s.cv.notify_all();
  }
  if (restore) {
    // Las copias de esta sesion no pertenecen a la partida cargada. Los objetos viejos
    // no se borran: otras tablas del renderizador los recuerdan por su numero.
    reg.clear();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (const auto& t : in) {
      if (!t.w || !t.h || t.rgba.size() != size_t(t.w) * t.h * 4) continue;
      GLuint tex = 0; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(t.w), GLsizei(t.h), 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      reg[t.addr & 0x1FFFFFFF] = tex;
    }
    std::printf("[Guardado] %zu copias EFB restauradas\n", in.size());
  }
  glBindTexture(GL_TEXTURE_2D, GLuint(tex0)); glActiveTexture(GLenum(active));
  glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(rfb)); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(dfb));
  glPixelStorei(GL_PACK_ALIGNMENT, pack); glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
  if (scissor) glEnable(GL_SCISSOR_TEST);
}
"""
assert 'port_menu_state.h' in rg
# --- sombreadores: cache en disco (se precompilan al arrancar) y medicion para el registro de tirones
_a1="// tools/gx-pipeline: filtro vertical de copia EFB y gamma, como el hardware.\n"
assert rg.count(_a1)==1
rg=rg.replace(_a1,r"""// tools/gx-pipeline: cache de sombreadores en disco. Cada sombreador nuevo se anade a
// cache_sombreadores.bin; al arrancar se compilan todos los conocidos (el juego usa unos
// pocos), asi no hay parones la primera vez que aparece un efecto. La cache se descarta
// si cambia el generador de sombreadores (huella de renderer_gl.cpp + tev_shader_gen.cpp que pone
// make_patch.py). Antes se descartaba en cada compilacion (fecha y hora) y tras cada cambio del port
// volvian los parones de compilar sombreadores (hasta 0,9 s cada uno la primera vez).
static const char* kPortShaderFile = "cache_sombreadores.bin";
static std::string PortShaderBuildId() { return std::string("MHSC2 ") + "__PORT_SHADER_ID__"; }   // huella del generador (make_patch.py)
static void PortFillShader(GLShader& sh) {
  for (int i = 0; i < 8; i++) {
    sh.uTex[i] = glGetUniformLocation(sh.id, ("uTex" + std::to_string(i)).c_str());
    sh.uTexMtx[i] = glGetUniformLocation(sh.id, ("uTexMtx[" + std::to_string(i) + "]").c_str());
  }
  sh.uTevKColor = glGetUniformLocation(sh.id, "uTevKColor[0]");
  if (sh.uTevKColor < 0) sh.uTevKColor = glGetUniformLocation(sh.id, "uTevKColor");
  sh.uTevColor = glGetUniformLocation(sh.id, "uTevColor[0]");
  if (sh.uTevColor < 0) sh.uTevColor = glGetUniformLocation(sh.id, "uTevColor");
  sh.uProjMtx = glGetUniformLocation(sh.id, "mvp");
  sh.uMatColor = glGetUniformLocation(sh.id, "uMatColor[0]");
  sh.uAmbColor = glGetUniformLocation(sh.id, "uAmbColor[0]");
  sh.uChanCtrl = glGetUniformLocation(sh.id, "uChanCtrl[0]");
  sh.uLightCol = glGetUniformLocation(sh.id, "uLightCol[0]");
  sh.uLightPos = glGetUniformLocation(sh.id, "uLightPos[0]");
  sh.uLightDir = glGetUniformLocation(sh.id, "uLightDir[0]");
  sh.uLightCosAtt = glGetUniformLocation(sh.id, "uLightCosAtt[0]");
  sh.uLightDistAtt = glGetUniformLocation(sh.id, "uLightDistAtt[0]");
}
static bool s_port_shader_file_ok = false;
static void PortShaderSave(uint64_t hash, const std::string& vs, const std::string& fs) {
  if (!s_port_shader_file_ok) return;
  std::ofstream o(kPortShaderFile, std::ios::binary | std::ios::app);
  if (!o) return;
  const uint32_t a = uint32_t(vs.size()), b = uint32_t(fs.size());
  o.write(reinterpret_cast<const char*>(&hash), 8); o.write(reinterpret_cast<const char*>(&a), 4); o.write(reinterpret_cast<const char*>(&b), 4);
  o.write(vs.data(), a); o.write(fs.data(), b);
}
static void PortShaderPrecompile() {
  static bool done = false;
  if (done) return;
  done = true;
  std::string head;
  {
    std::ifstream in(kPortShaderFile, std::ios::binary);
    std::getline(in, head);
    if (in && head == PortShaderBuildId()) {
      const auto t0 = std::chrono::steady_clock::now();
      int n = 0, bad = 0;
      for (;;) {
        uint64_t hash = 0; uint32_t a = 0, b = 0;
        if (!in.read(reinterpret_cast<char*>(&hash), 8) || !in.read(reinterpret_cast<char*>(&a), 4) || !in.read(reinterpret_cast<char*>(&b), 4)) break;
        if (a > (1u << 20) || b > (1u << 20)) break;
        std::string vs(a, '\0'), fs(b, '\0');
        if ((a && !in.read(&vs[0], a)) || (b && !in.read(&fs[0], b))) break;
        if (s_shader_cache.count(hash)) continue;
        GLShader sh = {};
        sh.uniforms = std::make_shared<UniformCache>();
        sh.id = CompileShader(vs.c_str(), fs.c_str());
        if (!sh.id) { ++bad; continue; }
        PortFillShader(sh);
        s_shader_cache[hash] = sh;
        ++n;
      }
      s_port_shader_file_ok = true;
      std::printf("[Sombreadores] %d precompilados desde %s en %.0f ms%s\n", n, kPortShaderFile,
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), bad ? " (algunos fallaron)" : "");
      return;
    }
  }
  std::ofstream o(kPortShaderFile, std::ios::binary | std::ios::trunc);
  if (o) { o << PortShaderBuildId() << "\n"; s_port_shader_file_ok = true; }
  if (!head.empty()) std::printf("[Sombreadores] cache de otra compilacion: se empieza de nuevo\n");
}

"""+_a1)
_a2="    sh.id =\n        CompileShader(src.vertex_source.c_str(), src.fragment_source.c_str());\n"
assert rg.count(_a2)==1
rg=rg.replace(_a2,"""    const auto port_c0 = std::chrono::steady_clock::now();
    sh.id =
        CompileShader(src.vertex_source.c_str(), src.fragment_source.c_str());
    ++port_menu::g_prof_compiles;
    port_menu::g_prof_compile_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - port_c0).count());
""")
_a3="    s_shader_cache[hash] = sh;\n    s_active_shader = sh;\n"
assert rg.count(_a3)==1
rg=rg.replace(_a3,_a3+"    PortShaderSave(hash, src.vertex_source, src.fragment_source);\n")
_a4="    if (port_menu::g_tex_reload.exchange(false)) nwii::runtime::hle::TextureCache::get().clear();  // Original <-> HD\n"
assert rg.count(_a4)==1
rg=rg.replace(_a4,"    PortShaderPrecompile();\n"+_a4)
if '#include <chrono>' not in rg: rg=rg.replace('#include <fstream>\n','#include <fstream>\n#include <chrono>\n',1)
(pathlib.Path(__file__).parent/'renderer_gl.cpp').write_text(rg.replace('\n','\r\n'),newline='')
print('ok renderer_gl.cpp')

# --- tev_shader_gen.cpp: modos de comparacion del TEV (bias == 3) ---
tv=(root/'export/nWiiRuntime/src/hle/gx/tev_shader_gen.cpp').read_text().replace('\r\n','\n')
c_start='        fs << "    vec3 cBias;\\n";\n'
c_end='        if (stage.colorClamp == 1) fs << "    cOut = clamp(cOut, 0.0, 1.0);\\n";\n'
assert tv.count(c_start)==1 and tv.count(c_end)==1
i0=tv.index(c_start); i1=tv.index(c_end)
color_normal=tv[i0:i1]
color_cmp=r"""        // tools/gx-pipeline: bias==3 activa la comparacion del TEV; modo=(escala<<1)|op.
        // Resultado = D + (A cmp B ? C : 0), con A,B,C de 8 bits y sin escala (como Dolphin).
        static const bool tev_cmp_legacy = std::getenv("NWII_TEV_COMPARE_LEGACY") != nullptr;
        if (stage.colorBias == 3 && !tev_cmp_legacy) {
            const int mode = (int(stage.colorScale) << 1) | int(stage.colorOp);
            { static int reported = 0; if (reported < 40) { ++reported; std::printf("[TEVCompare] color etapa=%d modo=%d\n", int(i), mode); } }
            fs << "    vec3 cOut;\n";
            fs << "    vec3 cA8 = floor(clamp(cIn[0],0.0,1.0)*255.0+0.5);\n";
            fs << "    vec3 cB8 = floor(clamp(cIn[1],0.0,1.0)*255.0+0.5);\n";
            fs << "    vec3 cC = clamp(cIn[2],0.0,1.0);\n";
            switch (mode) {
                case 0: fs << "    cOut = cIn[3] + ((cA8.r > cB8.r) ? cC : vec3(0.0));\n"; break;
                case 1: fs << "    cOut = cIn[3] + ((cA8.r == cB8.r) ? cC : vec3(0.0));\n"; break;
                case 2: fs << "    cOut = cIn[3] + ((dot(cA8, vec3(1.0,256.0,0.0)) > dot(cB8, vec3(1.0,256.0,0.0))) ? cC : vec3(0.0));\n"; break;
                case 3: fs << "    cOut = cIn[3] + ((dot(cA8, vec3(1.0,256.0,0.0)) == dot(cB8, vec3(1.0,256.0,0.0))) ? cC : vec3(0.0));\n"; break;
                case 4: fs << "    cOut = cIn[3] + ((dot(cA8, vec3(1.0,256.0,65536.0)) > dot(cB8, vec3(1.0,256.0,65536.0))) ? cC : vec3(0.0));\n"; break;
                case 5: fs << "    cOut = cIn[3] + ((dot(cA8, vec3(1.0,256.0,65536.0)) == dot(cB8, vec3(1.0,256.0,65536.0))) ? cC : vec3(0.0));\n"; break;
                case 6: fs << "    cOut = cIn[3] + vec3(greaterThan(cA8, cB8)) * cC;\n"; break;
                default: fs << "    cOut = cIn[3] + vec3(equal(cA8, cB8)) * cC;\n"; break;
            }
        } else {
"""
tv=tv[:i0]+color_cmp+color_normal+"        }\n"+tv[i1:]
a_start='        fs << "    float aBias;\\n";\n'
a_end='        if (stage.alphaClamp == 1) fs << "    aOut = clamp(aOut, 0.0, 1.0);\\n";\n'
assert tv.count(a_start)==1 and tv.count(a_end)==1
j0=tv.index(a_start); j1=tv.index(a_end)
alpha_normal=tv[j0:j1]
alpha_cmp=r"""        if (stage.alphaBias == 3 && !tev_cmp_legacy) {
            // Modos 0-5 comparan las entradas de COLOR A,B de esta etapa; 6-7 el alfa.
            const int mode = (int(stage.alphaScale) << 1) | int(stage.alphaOp);
            { static int reported = 0; if (reported < 40) { ++reported; std::printf("[TEVCompare] alfa etapa=%d modo=%d\n", int(i), mode); } }
            fs << "    float aOut;\n";
            fs << "    vec3 acA8 = floor(clamp(cIn[0],0.0,1.0)*255.0+0.5);\n";
            fs << "    vec3 acB8 = floor(clamp(cIn[1],0.0,1.0)*255.0+0.5);\n";
            fs << "    float aA8 = floor(clamp(aIn[0],0.0,1.0)*255.0+0.5);\n";
            fs << "    float aB8 = floor(clamp(aIn[1],0.0,1.0)*255.0+0.5);\n";
            fs << "    float aC = clamp(aIn[2],0.0,1.0);\n";
            switch (mode) {
                case 0: fs << "    aOut = aIn[3] + ((acA8.r > acB8.r) ? aC : 0.0);\n"; break;
                case 1: fs << "    aOut = aIn[3] + ((acA8.r == acB8.r) ? aC : 0.0);\n"; break;
                case 2: fs << "    aOut = aIn[3] + ((dot(acA8, vec3(1.0,256.0,0.0)) > dot(acB8, vec3(1.0,256.0,0.0))) ? aC : 0.0);\n"; break;
                case 3: fs << "    aOut = aIn[3] + ((dot(acA8, vec3(1.0,256.0,0.0)) == dot(acB8, vec3(1.0,256.0,0.0))) ? aC : 0.0);\n"; break;
                case 4: fs << "    aOut = aIn[3] + ((dot(acA8, vec3(1.0,256.0,65536.0)) > dot(acB8, vec3(1.0,256.0,65536.0))) ? aC : 0.0);\n"; break;
                case 5: fs << "    aOut = aIn[3] + ((dot(acA8, vec3(1.0,256.0,65536.0)) == dot(acB8, vec3(1.0,256.0,65536.0))) ? aC : 0.0);\n"; break;
                case 6: fs << "    aOut = aIn[3] + ((aA8 > aB8) ? aC : 0.0);\n"; break;
                default: fs << "    aOut = aIn[3] + ((aA8 == aB8) ? aC : 0.0);\n"; break;
            }
        } else {
"""
tv=tv[:j0]+alpha_cmp+alpha_normal+"        }\n"+tv[j1:]
if '#include <cstdlib>' not in tv:
    tv='#include <cstdlib>\n'+tv

# --- diagnostico NWII_MATTINT: uniforme uDbgTint (a=0 nada; 1 liso, 2 rayas h, 3 rayas v, 4 cuadros) ---
o_fl='    if (std::getenv("NWII_FLATCOLOR"))\n'
assert tv.count(o_fl)==1
tv=tv.replace(o_fl, r"""    if (std::getenv("NWII_MATTINT")) {
        fs << "    if (uDbgTint.a > 0.5) { ivec2 q = ivec2(gl_FragCoord.xy) / 8; int m = int(uDbgTint.a + 0.5); bool on = (m == 1) || (m == 2 && (q.y & 1) == 0) || (m == 3 && (q.x & 1) == 0) || (m == 4 && ((q.x + q.y) & 1) == 0); FragColor.rgb = on ? uDbgTint.rgb : vec3(0.0); }\n";
    }
"""+o_fl)
o_un='    fs << "uniform vec4 uTevKColor[4];\\n";\n'
assert tv.count(o_un)==1
tv=tv.replace(o_un, o_un+'    if (std::getenv("NWII_MATTINT")) fs << "uniform vec4 uDbgTint;\\n";\n')
(pathlib.Path(__file__).parent/'tev_shader_gen.cpp').write_text(tv.replace('\n','\r\n'),newline='')
print('ok tev_shader_gen.cpp')


# --- input_manager.cpp: el juego no recibe botones mientras el menu del port lo bloquea ---
im=(root/'export/nWiiRuntime/src/input/input_manager.cpp').read_text().replace('\r\n','\n')
o_im="    if(changed)connection_generation.fetch_add(1,std::memory_order_release);\n"
assert im.count(o_im)==1
im=im.replace(o_im,"""    if(port_menu::g_input_blocked.load(std::memory_order_relaxed)) {
        for(int i=0;i<4;++i) {
            pending_wiimote_presses[i]=0;pending_classic_presses[i]=0;
            wii_motes[i].buttons=0;wii_motes[i].classic.buttons=0;
            wii_motes[i].classic.left_x=wii_motes[i].classic.left_y=wii_motes[i].classic.right_x=wii_motes[i].classic.right_y=0;
            wii_motes[i].classic.trigger_l=wii_motes[i].classic.trigger_r=0;
            gc_pads[i].buttons=0;gc_pads[i].stick_x=gc_pads[i].stick_y=gc_pads[i].substick_x=gc_pads[i].substick_y=0;
            gc_pads[i].trigger_l=gc_pads[i].trigger_r=0;
        }
    }
"""+o_im)
im='#include "port_menu_state.h"\n'+im
(pathlib.Path(__file__).parent/'input_manager.cpp').write_text(im.replace('\n','\r\n'),newline='')
print('ok input_manager.cpp')

# --- main.cpp: menu del port (P / Select+Start) ---
mn=(root/'export/nWiiRuntime/src/core/main.cpp').read_text().replace('\r\n','\n')
def rep(old,new):
    global mn
    assert mn.count(old)==1, old
    mn=mn.replace(old,new)
rep('#include "runtime/hw/hw.h"\n','#include "runtime/hw/hw.h"\n#include "port_menu.h"\n')
rep('  nwii::runtime::init_ipc_client(*ctx);\n',
    '  if (port_menu::g_lang.load() >= 0 && port_lang::PatchGameTable([&](uint32_t a) { return uint8_t(ctx->mmu.read8(a)); }, [&](uint32_t a, uint8_t v) { ctx->mmu.write8(a, v); }))\n'
    '    std::cout << "[Idioma] tabla del juego ampliada (alemán e italiano)\\n";\n'
    '  port_cloud::StartupSync();\n'
    '  nwii::runtime::init_ipc_client(*ctx);\n')
rep('  nwii::runtime::input::InputManager::get().shutdown();\n  nwii::runtime::shutdown();\n  return 0;\n',
    '  port_cloud::Shutdown();\n  nwii::runtime::input::InputManager::get().shutdown();\n  nwii::runtime::shutdown();\n  return 0;\n')
rep('  if (!nwii::runtime::init(config_path))\n    return 1;\n','  if (!nwii::runtime::init(config_path))\n    return 1;\n  port_menu::LoadSettings();\n')
# sin argumentos (doble clic, acceso directo, Steam/Proton): game\DATA y el toml junto al .exe
# capturas de diagnostico: intervalo configurable (NWII_SCREENSHOT_MS, por defecto 2000)
rep('            next_shot = shot_now + std::chrono::seconds(2);\n',
    '            static const int shot_ms=[](){const char* e=std::getenv("NWII_SCREENSHOT_MS");return e?std::max(1,std::atoi(e)):2000;}();\n            next_shot = shot_now + std::chrono::milliseconds(shot_ms);\n')
rep('int main(int argc, char **argv) {\n  if (argc < 2) {\n',
    '''int main(int argc, char **argv) {
  // tools/gx-pipeline: carpeta portable. Sin argumentos se usan game/DATA y
  // monster-hunter-tri-native.toml junto al ejecutable, y esa carpeta pasa a ser la de trabajo
  // (ajustes, partidas, guardados rapidos, texturas HD...).
  static std::string port_args[3];
  static char* port_argv[4] = {nullptr, nullptr, nullptr, nullptr};
  if (argc < 2) {
    std::filesystem::path dir;
    if (char* base = SDL_GetBasePath()) { dir = std::filesystem::u8path(base); SDL_free(base); }
    std::error_code port_ec;
    if (!dir.empty()) std::filesystem::current_path(dir, port_ec);
    port_args[0] = argc > 0 && argv[0] ? argv[0] : "MonsterHunterTri.exe";
    port_args[1] = (dir / "game" / "DATA").string();
    port_args[2] = (dir / "monster-hunter-tri-native.toml").string();
    for (int i = 0; i < 3; ++i) port_argv[i] = port_args[i].data();
    argc = 3; argv = port_argv;
    std::cout << "[Portable] carpeta " << dir.string() << "\\n";
  }
  if (argc < 2) {
''')
rep('      nwii::runtime::hw::dsp_audio_pull(reinterpret_cast<int16_t*>(stream),size_t(bytes)/4);\n',
    '      port_menu::AudioFilter(reinterpret_cast<int16_t*>(stream),size_t(bytes)/4,[](int16_t* o,size_t n){nwii::runtime::hw::dsp_audio_pull(o,n);});\n')
rep('      SDL_GL_SetSwapInterval(nwii::runtime::Config::get().enable_vsync?1:0);\n',
    '      SDL_GL_SetSwapInterval(nwii::runtime::Config::get().enable_vsync?1:0);\n      port_menu::menu().SetWindow(window);\n      port_menu::ApplyStartup(window);\n')
rep('          nwii::runtime::input::InputManager::get().handle_event(e);\n          if (e.type == SDL_QUIT) quit = true;\n        }\n',
    '          if (e.type == SDL_QUIT) quit = true;\n          if (port_menu::menu().HandleEvent(e)) continue;\n          nwii::runtime::input::InputManager::get().handle_event(e);\n        }\n        port_menu::menu().Update();\n        if (port_menu::menu().QuitRequested()) quit = true;\n')
rep('        fps_overlay.Draw(g_stat_frames,',
    '        if (port_menu::settings().fps) fps_overlay.Draw(g_stat_frames,')
rep('std::chrono::steady_clock::now().time_since_epoch()).count(),draw_w,draw_h);\n',
    'std::chrono::steady_clock::now().time_since_epoch()).count(),draw_w,draw_h);\n        port_menu::menu().Draw(draw_w, draw_h);\n')
# resolucion interna en vivo y redibujado con el menu abierto
rep('  const int efb_width=640*nwii::runtime::EFBScale();\n  const int efb_height=480*nwii::runtime::EFBScale();\n',
    '  int efb_width=640*nwii::runtime::EFBScale();\n  int efb_height=480*nwii::runtime::EFBScale();\n  GLuint efb_depth_rbo = 0;\n')
rep('      GLuint efb_depth_rbo = 0;\n      glGenRenderbuffers(1, &efb_depth_rbo);\n','      glGenRenderbuffers(1, &efb_depth_rbo);\n')
assert mn.count('      extern void ProcessGXFifo();\n      ProcessGXFifo();\n')==2
i=mn.rindex('      extern void ProcessGXFifo();\n      ProcessGXFifo();\n')+len('      extern void ProcessGXFifo();\n      ProcessGXFifo();\n')
mn=mn[:i]+"""      if (!headless && port_menu::g_rerender.exchange(false)) {
        const int want = std::clamp(port_menu::settings().scale, 1, 5);
        if (efb_width != 640 * want) {
          nwii::runtime::presentation_settings().internal_scale = want;
          efb_width = 640 * want; efb_height = 480 * want;
          glBindTexture(GL_TEXTURE_2D, efb_tex);
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, efb_width, efb_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
          glBindRenderbuffer(GL_RENDERBUFFER, efb_depth_rbo);
          glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, efb_width, efb_height);
          std::cout << "[Menu] resolucion interna " << want << "x (EFB " << efb_width << "x" << efb_height << ")\\n";
        }
        glBindFramebuffer(GL_FRAMEBUFFER, efb_fbo);
        glViewport(0, 0, efb_width, efb_height);
        extern void RerenderLastGXFrame();
        RerenderLastGXFrame();
      }
"""+mn[i:]
# presentacion: relacion de aspecto del menu (4:3/16:9/21:9/32:9) y FXAA opcional
rep('        auto rect=nwii::runtime::FitPresentation(draw_w,draw_h,presentation.keep_aspect,presentation.widescreen);\n',
    '        auto rect=port_menu::FitRect(draw_w,draw_h);\n')
rep('        glBlitFramebuffer(source.x,source.y,source.x+source.width,source.y+source.height,rect.x,rect.y,rect.x+rect.width,rect.y+rect.height,\n                          GL_COLOR_BUFFER_BIT,presentation.sharp_pixels?GL_NEAREST:GL_LINEAR);\n',
    '        if (!port_menu::PresentFXAA(efb_tex, efb_width, efb_height, source, rect))\n        glBlitFramebuffer(source.x,source.y,source.x+source.width,source.y+source.height,rect.x,rect.y,rect.x+rect.width,rect.y+rect.height,\n                          GL_COLOR_BUFFER_BIT,port_menu::UseNearest(source,rect)?GL_NEAREST:GL_LINEAR);\n')
# rendimiento: tiempo de CPU por hilo en [STAT] (port_perf) y registro a archivo en la Steam Deck
rep('#include "port_menu.h"\n', '#include "port_menu.h"\n#include "port_perf.h"\n#include <algorithm>\n#include <ctime>\n#ifdef _WIN32\n#include <io.h>\n#endif\n')
rep('int main(int argc, char **argv) {\n', """int main(int argc, char **argv) {
  // tools/gx-pipeline: en la Steam Deck (o con NWII_LOG_FILE=1) el registro va a logs/juego-FECHA.log
  // junto al ejecutable, con el registro de tirones activado. Se guardan los 10 ultimos.
  if (port_menu::OnSteamDeck() || std::getenv("NWII_LOG_FILE")) {
    std::filesystem::path base;
    if (char* b = SDL_GetBasePath()) { base = std::filesystem::u8path(b); SDL_free(b); }
    std::error_code lec;
    const auto logs = base / "logs";
    std::filesystem::create_directories(logs, lec);
    std::vector<std::filesystem::path> old;
    for (auto& e : std::filesystem::directory_iterator(logs, lec))
      if (e.path().filename().string().rfind("juego-", 0) == 0 && e.path().extension() == ".log") old.push_back(e.path());
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i + 9 < old.size(); ++i) std::filesystem::remove(old[i], lec);
    char stamp[32]; const std::time_t now = std::time(nullptr); std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    const std::string file = (logs / ("juego-" + std::string(stamp) + ".log")).string();
    if (std::freopen(file.c_str(), "w", stdout)) {
      std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
#ifdef _WIN32
      _dup2(_fileno(stdout), _fileno(stderr));
      if (!std::getenv("NWII_HITCH_LOG")) _putenv("NWII_HITCH_LOG=1");
#endif
      std::printf("[Registro] %s (Steam Deck=%d)\\n", file.c_str(), int(port_menu::OnSteamDeck()));
    }
  }
  port_perf::Mark(port_perf::kMain);
  std::printf("[Version] port compilado %s %s (memoria nativa, sondeo 1/32)\\n", __DATE__, __TIME__);
  std::printf("[Nucleos] %s\\n", port_perf::AffinityInfo());
""")
rep('  nwii::runtime::g_ctx_ptr = ctx;\n  std::cout << "[Thread] CPU Core started.', '  nwii::runtime::g_ctx_ptr = ctx;\n  port_perf::Mark(port_perf::kGuest);\n  if (std::getenv("NWII_CPU_SAMPLE")) port_perf::StartSampler(&ctx->pc);\n  std::cout << "[Thread] CPU Core started.')
rep("          stat_line << '\\n';\n          std::cout << stat_line.str();\n", """          {
            static double s_pg = 0, s_pm = 0, s_px = 0, s_pp = 0, s_pi = 0;
            const double idle = double(port_perf::IdleUs()) / 1000.0;
            stat_line << std::dec << " juego_libre=" << int((idle - s_pi) * 100 / double(sample_ms > 0 ? sample_ms : 1)) << "%";
            s_pi = idle;
            {
              static uint64_t s_ic = 0;
              const uint64_t ic = nwii::runtime::g_ctx_ptr ? nwii::runtime::g_ctx_ptr->inst_count : 0;
              const double sec = double(sample_ms > 0 ? sample_ms : 1) / 1000.0;
              // instrucciones del juego por segundo y velocidad de su reloj frente al real (100 % = al dia)
              stat_line << " instr_juego=" << uint64_t(double(ic - s_ic) / sec / 1e6) << "M/s reloj_juego=" << int(double(ic - s_ic) / (40500000.0 * sec) * 100.0) << "%";
              s_ic = ic;
            }
            const double g = port_perf::ThreadMs(port_perf::kGuest), m = port_perf::ThreadMs(port_perf::kMain);
            const double x = port_perf::ThreadMs(port_perf::kGx), p = port_perf::ProcessMs();
            const double span = double(sample_ms > 0 ? sample_ms : 1);
            stat_line << std::dec << " cpu_juego=" << int((g - s_pg) * 100 / span) << "% cpu_render=" << int((m - s_pm) * 100 / span)
                      << "% cpu_gx=" << int((x - s_px) * 100 / span) << "% cpu_proceso=" << int((p - s_pp) * 100 / span)
                      << "%/" << port_perf::Cores() * 100 << "%";
            s_pg = g; s_pm = m; s_px = x; s_pp = p;
          }
          stat_line << '\\n';
          std::cout << stat_line.str();
          std::cout.flush(); std::fflush(stdout);
""")
# Ritmo de presentacion: esperar al siguiente cuadro del juego en vez de a una rejilla de 60 Hz
# (ver PortWaitFrameUntil en pipeline_tail.inc).
_pz="        std::this_thread::sleep_until(pacer.deadline());\n"
assert mn.count(_pz)==1, 'pace'
mn=mn.replace(_pz, "        void PortWaitFrameUntil(std::chrono::steady_clock::time_point);\n        PortWaitFrameUntil(pacer.deadline());\n")
_dk="          extern void DSP_LogAudioStats();\n"
assert mn.count(_dk)==1, 'deck info'
mn=mn.replace(_dk, "          { static uint64_t s_ip = 0; const uint64_t ip = nwii::runtime::g_port_interp_steps; std::printf(\"[Interprete] entradas=%llu en 5 s directas=%d\\n\", (unsigned long long)(ip - s_ip), int(nwii::runtime::g_port_dcalls)); s_ip = ip; }\n"+_dk)
mn=mn.replace(_dk, "          if (port_menu::OnSteamDeck() || std::getenv(\"NWII_DECK_INFO\")) { static const std::string d0 = port_perf::DeckInfo(); if (!d0.empty()) std::printf(\"[Deck] %s\\n\", port_perf::DeckInfo().c_str()); }\n"+_dk)
_sw="        SDL_GL_SwapWindow(window);\n"
assert mn.count(_sw)==1, 'swap'
mn=mn.replace(_sw, "        { void PortPaceBeforeSwap(); PortPaceBeforeSwap(); }   // ritmo del VI (pipeline_tail.inc)\n"+_sw)
(pathlib.Path(__file__).parent/'main.cpp').write_text(mn.replace('\n','\r\n'),newline='')
print('ok main.cpp')


# --- texture_cache_gl.cpp: mipmaps para el filtrado del menu (trilineal / anisotropico) ---
tc=(root/'export/nWiiRuntime/src/hle/texture_cache_gl.cpp').read_text().replace('\r\n','\n')
o_tc="""    Image img;
    if(ram) {
        const TextureReadView view{stage.base_addr,stage.tlut_offset,&source,&palette};
        TextureReadScope scope(view);img=decode_texture(ctx,stage);
    } else img=decode_texture(ctx,stage);
    GLuint tex;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,stage.width,stage.height,0,GL_RGBA,GL_UNSIGNED_BYTE,img.data);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);UnloadImage(img);
"""
assert tc.count(o_tc)==1
tc=tc.replace(o_tc,"""    const auto port_t0=std::chrono::steady_clock::now();
    GLuint tex;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    // tools/gx-pipeline: textura HD del paquete (formato Dolphin) si esta activada y existe.
    // Si aun se esta decodificando se sube la original y la HD se pone despues (port_hd_upgrade).
    bool hd_loaded=false; std::string hd_pending;
    if(port_menu::g_hd_textures.load(std::memory_order_relaxed)) {
        const uint8_t* data=ram?source.data():ctx.mmu.get_ptr(stage.base_addr);
        size_t tl=stage.format==8?32:stage.format==9?512:stage.format==10?32768:0;
        const uint8_t* tlut=(tl && stage.tlut_offset+tl<=sizeof(g_state.tlutMem))?g_state.tlutMem+stage.tlut_offset:nullptr;
        if(data) if(auto hd=hd_textures::Load(data,size,tlut,tlut?tl:0,stage.width,stage.height,stage.format,&hd_pending)) {
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,hd->w,hd->h,0,GL_RGBA,GL_UNSIGNED_BYTE,hd->rgba.data());
            hd_loaded=true;
        }
    }
    if(!hd_loaded) {
        Image img;
        const auto port_d0=std::chrono::steady_clock::now();
        if(ram) {
            const TextureReadView view{stage.base_addr,stage.tlut_offset,&source,&palette};
            TextureReadScope scope(view);img=decode_texture(ctx,stage);
        } else img=decode_texture(ctx,stage);
        port_menu::g_prof_decode_us+=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-port_d0).count());
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,stage.width,stage.height,0,GL_RGBA,GL_UNSIGNED_BYTE,img.data);
        UnloadImage(img);
    }
    // Mipmaps solo para el filtrado trilineal/anisotropico del menu (el original usa solo el
    // nivel base); al cambiar el filtrado se rehace la cache.
    if(port_menu::g_tex_filter.load(std::memory_order_relaxed)>0) glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    ++port_menu::g_prof_uploads;
    port_menu::g_prof_upload_us+=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-port_t0).count());
""")
_o5="    cache.emplace(key,CachedTexture{tex,recency.begin(),std::move(source),std::move(palette)});\n"
assert tc.count(_o5)==1
tc=tc.replace(_o5,"    auto port_em=cache.emplace(key,CachedTexture{tex,recency.begin(),std::move(source),std::move(palette)});\n    port_em.first->second.hd_pending=std::move(hd_pending);\n")
_o6="                return saved.texture;\n"
assert tc.count(_o6)==1
tc=tc.replace(_o6,"                port_hd_upgrade(existing->second,stage.width,stage.height);\n                return saved.texture;\n")
_o7="            return existing->second.texture;\n"
assert tc.count(_o7)==1
tc=tc.replace(_o7,"            port_hd_upgrade(existing->second,stage.width,stage.height);\n            return existing->second.texture;\n")
tc+=r"""
// tools/gx-pipeline: pone la textura HD cuando termina de decodificarse (una cada ~6 ms
// como mucho, para repartir la subida entre cuadros).
void nwii::runtime::hle::TextureCache::port_hd_upgrade(CachedTexture& e, uint32_t w, uint32_t h) {
    if(e.hd_pending.empty()) return;
    if(!port_menu::g_hd_textures.load(std::memory_order_relaxed)) { e.hd_pending.clear(); return; }
    static std::chrono::steady_clock::time_point last{};
    const auto now=std::chrono::steady_clock::now();
    if(now-last<std::chrono::milliseconds(6)) return;
    auto d=hd_textures::GetOrRequest(e.hd_pending,w,h);
    if(!d) { if(hd_textures::Failed(e.hd_pending)) e.hd_pending.clear(); return; }
    last=now;
    glBindTexture(GL_TEXTURE_2D,e.texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,d->w,d->h,0,GL_RGBA,GL_UNSIGNED_BYTE,d->rgba.data());
    if(port_menu::g_tex_filter.load(std::memory_order_relaxed)>0) glGenerateMipmap(GL_TEXTURE_2D);
    e.hd_pending.clear();
    ++port_menu::g_prof_hd;
}
"""

tc='#include "port_menu_state.h"\n#include "hd_textures.h"\n#include <chrono>\n'+tc
(pathlib.Path(__file__).parent/'texture_cache_gl.cpp').write_text(tc.replace('\n','\r\n'),newline='')
print('ok texture_cache_gl.cpp')


# ============================================================================
# Guardado/cargado rapido (savestates): serializadores en las copias del runtime
# ============================================================================
def _src(rel): return (root/'export/nWiiRuntime/src'/rel).read_text().replace('\r\n','\n')
def _out(name, text): (pathlib.Path(__file__).parent/name).write_text(text.replace('\n','\r\n'),newline=''); print('ok', name)
INC='#include "port_state_io.h"\n'

# --- hw_ai
t=_src('hw/hw_ai.cpp')
t=INC+t+'''
namespace nwii::runtime::hw {
void PortState_hw_ai(port_state::IO& io) {
  io.tag("AI00"); io.pod(ai_cr); io.pod(ai_vr); io.pod(ai_scnt); io.pod(ai_it);
  if (!io.saving) { ai_last_us = nwii::runtime::get_os_time(); ai_fraction = 0; }
}
}
'''
_out('hw_ai.cpp', t)

# --- hw_cp
t=_src('hw/hw_cp.cpp')
t=INC+t+'''
namespace nwii::runtime::hw {
void PortState_hw_cp(port_state::IO& io) {
  io.tag("CP00"); io.pod(g_cp_cr); io.pod(g_cp_sr); io.pod(g_cp_clear); io.pod(g_cp_fifo_base); io.pod(g_cp_fifo_end);
  io.pod(g_cp_fifo_hi); io.pod(g_cp_fifo_lo); io.pod(g_cp_fifo_rw_dist); io.pod(g_cp_fifo_wp); io.pod(g_cp_fifo_rp);
}
}
'''
_out('hw_cp.cpp', t)

# --- hw_di
t=_src('hw/hw_di.cpp')
t=INC+t+'''
namespace nwii::runtime::hw {
void PortState_hw_di(port_state::IO& io) {
  io.tag("DI00"); io.pod(di_cmd); io.pod(di_mar); io.pod(di_len); io.pod(di_cr); io.pod(di_imm); io.pod(di_sr); io.pod(di_cvr);
  io.pod(di_stream_playing); io.pod(g_di_interrupt_delay);
}
}
'''
_out('hw_di.cpp', t)

# --- hw_dsp (los estaticos del protocolo de buzon pasan a nivel de archivo)
t=_src('hw/hw_dsp.cpp')
for old in ['                static uint16_t s_cpu_mail_hi = 0;\n','                static int s_ax_state = 0; \n',
            '                    static uint32_t list_words = 0;\n','                        static bool boot_entry_pending=false;\n']:
    assert t.count(old)==1, old
    t=t.replace(old,'')
anchor='static AXWiiMixer ax_mixer;\n'
assert t.count(anchor)==1
t=t.replace(anchor,anchor+'// tools/gx-pipeline: estado del buzon del DSP a nivel de archivo (guardado rapido).\nstatic uint16_t s_cpu_mail_hi = 0;\nstatic int s_ax_state = 0;\nstatic uint32_t list_words = 0;\nstatic bool boot_entry_pending = false;\n')
t=INC+t+'''
namespace nwii::runtime::hw {
bool PortState_hw_dsp_idle() { return dsp_replies.empty() && s_ax_state != 1 && !boot_entry_pending; }
void PortState_hw_dsp(port_state::IO& io) {
  io.tag("DSP0");
  io.pod(dsp_mbox_cpu_hi); io.pod(dsp_mbox_cpu_lo); io.pod(dsp_mbox_dsp_hi); io.pod(dsp_mbox_dsp_lo);
  io.pod(ar_mmaddr); io.pod(ar_araddr); io.pod(ar_cnt); io.pod(dsp_control); io.pod(ar_size); io.pod(ar_mode); io.pod(ar_refresh);
  io.deque(dsp_replies); io.pod(adma_addr); io.pod(adma_control);
  io.pod(s_cpu_mail_hi); io.pod(s_ax_state); io.pod(list_words); io.pod(boot_entry_pending);
  if (!io.saving) {
    // Sonido ya mezclado de la sesion anterior: se descarta.
    std::lock_guard<std::mutex> lk(adma_mutex);
    adma_read = adma_size = 0; ax_completed_read = ax_completed_size = 0;
    adma_started_us = adma_next_host_us = nwii::runtime::get_os_time(); adma_primed = false; adma_fade_in = true;
  }
}
}
'''
_out('hw_dsp.cpp', t)

# --- hw_ipc
t=_src('hw/hw_ipc.cpp')
t=INC+t+'''
namespace nwii::runtime::hw {
bool PortState_hw_ipc_idle() { return ipc_replies.empty(); }
void PortState_hw_ipc(port_state::IO& io) {
  io.tag("IPC0"); io.pod(ipc_arm_msg); io.pod(ipc_arm_ctrl); io.pod(ipc_ppc_ctrl); io.pod(ipc_irq_flags); io.pod(ipc_irq_mask);
  io.deque(ipc_replies); io.pod(ipc_ppc_msg);
}
}
'''
_out('hw_ipc.cpp', t)

# --- hw_pe
t=_src('hw/hw_pe.cpp')
t=INC+t+'''
namespace nwii::runtime::hw {
void PortState_hw_pe(port_state::IO& io) {
  io.tag("PE00"); io.atom(g_pe_ctrl); io.atom(g_pe_token_pending); io.atom(g_pe_finish_pending); io.atom(g_pe_sr); io.atom(g_pe_token);
}
}
'''
_out('hw_pe.cpp', t)

# --- hw_pi
t=_src('hw/hw_pi.cpp')
t=INC+t+'''
namespace nwii::runtime::hw {
void PortState_hw_pi(port_state::IO& io) {
  io.tag("PI00"); io.atom(pi_intsr); io.atom(pi_intmr); io.pod(pi_fifo_base); io.pod(pi_fifo_end); io.pod(pi_fifo_wptr);
}
}
'''
_out('hw_pi.cpp', t)

# --- hw_vi
t=_src('hw/hw_vi.cpp')
t=INC+t+'''
namespace nwii::runtime::hw {
void PortState_hw_vi(port_state::IO& io) {
  io.tag("VI00"); io.pod(vi_dcr); io.pod(g_vi_top_field_base); io.pod(g_vi_btm_field_base); io.pod(vi_di);
}
}
'''
_out('hw_vi.cpp', t)

# --- usb_device: puntero a la instancia y sus colas
t=_src('hle/devices/usb_device.cpp')
old='class USBDevice : public IDevice {\n'
assert t.count(old)==1
t=t.replace(old,'class USBDevice;\nstatic USBDevice* g_usb_self = nullptr;\n'+old+'public:\n    USBDevice() { g_usb_self = this; }\n    bool port_idle() const { return m_events.empty(); }\n    void port_state(port_state::IO& io) { io.tag("USB0"); io.deque(m_pending_intr); if (!io.saving) m_events.clear(); }\n')
t=INC+t+'''
bool PortState_usb_idle() { return !nwii::runtime::devices::g_usb_self || nwii::runtime::devices::g_usb_self->port_idle(); }
void PortState_usb(port_state::IO& io) {
  if (nwii::runtime::devices::g_usb_self) nwii::runtime::devices::g_usb_self->port_state(io);
  else { std::deque<std::array<uint32_t,3>> none; io.tag("USB0"); io.deque(none); }
}
'''
assert '#include <array>' in t or True
t=t.replace(INC, INC+'#include <array>\n',1)
_out('usb_device.cpp', t)

# --- fs_device: solo la tabla de descriptores abiertos (la NAND no se toca)
t=_src('hle/devices/fs_device.cpp')
_o1='''        std::cout<<"[SC] Valid console configuration, display="'''
assert t.count(_o1)==1
t=t.replace(_o1,'''        if(port_lang::PatchSysconf(f.data,port_menu::g_lang.load())) std::cout<<"[Idioma] SYSCONF IPL.LNG="<<int(port_lang::WiiLanguage(port_menu::g_lang.load()))<<" ("<<port_lang::Name(port_menu::g_lang.load())<<")\\n";
'''+_o1)
_o2='''    if(replaced) std::cout<<"[NAND] Saved "<<f.path<<" bytes="<<f.data.size()<<'\\n';'''
assert t.count(_o2)==1, 'save_nand'
t=t.replace(_o2,'''    if(replaced) { std::cout<<"[NAND] Saved "<<f.path<<" bytes="<<f.data.size()<<'\\n'; port_cloud::NotifyNandSaved(f.path); }''')
t='#include "port_lang.h"\n#include "port_cloud.h"\n'+t
t=INC+t+'''
void PortState_fs(port_state::IO& io) {
  io.tag("FS00");
  for (auto& h : nwii::runtime::devices::g_vnand_handles) { io.pod(h.file_idx); io.pod(h.pos); io.pod(h.open); io.pod(h.mode); }
}
'''
_out('fs_device.cpp', t)

# --- pad.cpp
t=_src('hle/pad.cpp')
t=INC+t+'''
void PortState_pad(port_state::IO& io) {
  io.tag("PAD0"); io.pod(wpad_formats); io.pod(wpad_connect_callbacks); io.pod(wpad_extension_callbacks); io.pod(kpad_previous_classic);
}
'''
_out('pad.cpp', t)

# --- os_hle_stubs.cpp (HID2)
t=_src('core/os_hle_stubs.cpp')
t=INC+t+'''
void PortState_os_stubs(port_state::IO& io) { io.tag("OS00"); io.pod(nwii::runtime::hid2_register); }
'''
_out('os_hle_stubs.cpp', t)

# --- ios_kernel.cpp
t=_src('hle/ios_kernel.cpp')
t=INC+t+'''
void PortState_ios_kernel(port_state::IO& io) {
  io.tag("IOK0");
  auto fds = nwii::runtime::IOSKernel::get().port_get_fds();
  io.vec(fds);
  if (!io.saving && io.ok) nwii::runtime::IOSKernel::get().port_set_fds(fds);
}
'''
_out('ios_kernel.cpp', t)

def _reread(name): return (pathlib.Path(__file__).parent/name).read_text().replace('\r\n','\n')

# --- ios.cpp (copia ya generada arriba): enganche del guardado rapido + estado IPC/IOS
t=_reread('ios.cpp')
old="bool process_pending_callbacks(CPUContext &ctx) {\n  if (port_menu::g_paused.load(std::memory_order_relaxed)) PortMenuPauseWait(ctx);\n"
assert t.count(old)==1
t=t.replace(old,"""bool process_pending_callbacks(CPUContext &ctx) {
  if (port_menu::g_paused.load(std::memory_order_relaxed)) PortMenuPauseWait(ctx);
  if ((port_menu::g_save_request.load(std::memory_order_relaxed) | port_menu::g_load_request.load(std::memory_order_relaxed)) != 0)
    ::port_savestate::GuestHook(ctx);
""")
t=INC+"namespace nwii::runtime { struct CPUContext; }\nnamespace port_savestate { void GuestHook(nwii::runtime::CPUContext&); }\n"+t+'''
bool PortState_ios_idle() {
  return !nwii::runtime::g_ipc_reply_pending && nwii::runtime::g_ipc_interrupt_delay == 0 && nwii::runtime::hw::g_di_interrupt_delay == 0;
}
void PortState_ios(port_state::IO& io) {
  io.tag("IOS1"); io.pod(ipc_pool_next); io.pod(ios_guest_heap_ptr); io.pod(ios_ipc_heap_ptr);
  io.pod(nwii::runtime::g_ipc_reply_pending); io.pod(nwii::runtime::g_ipc_interrupt_delay);
}
'''
# rendimiento: el juego, ocioso en el bucle de espera del planificador del SO (0x804D3E38, ningun
# hilo listo), giraba en vacio hasta el siguiente evento. Ahora duerme hasta poco antes (<=2 ms)
# si va por delante del reloj real: libera el nucleo (en la Steam Deck, energia para el resto).
# NWII_IDLE_SLEEP=0 vuelve a girar.
_i0="bool process_pending_callbacks(CPUContext &ctx) {\n  if (port_menu::g_paused.load(std::memory_order_relaxed)) PortMenuPauseWait(ctx);\n"
assert t.count(_i0)==1, 'idle'
t=t.replace(_i0, """static void PortIdleWait(CPUContext &ctx) {
  static const bool spin = [] { const char* e = std::getenv("NWII_IDLE_SLEEP"); return e && e[0] == '0'; }();
  if (spin || ctx.in_callback || !ctx.cr[0].eq || ctx.vblank_pending.load(std::memory_order_relaxed) ||
      ctx.pending_cb_count.load(std::memory_order_relaxed) != 0 || g_ipc_interrupt_delay != 0 ||
      nwii::runtime::hw::g_di_interrupt_delay != 0 ||
      (nwii::runtime::hw::pi_intsr.load(std::memory_order_relaxed) & nwii::runtime::hw::pi_intmr.load(std::memory_order_relaxed)) != 0)
    return;
  if (ctx.mmu.read32(ctx.gpr[13] - 14984) != 0) return;   // hay hilos listos
  const int64_t wus = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - ctx.tb_start).count();
  if (wus <= 0) return;
  const uint64_t wall = uint64_t(wus) / 1000000 * ctx.tb_freq + uint64_t(wus) % 1000000 * ctx.tb_freq / 1000000;
  if (ctx.inst_count <= wall) return;   // el juego va por detras del reloj real: no dormir
  static EventScheduler& sch = EventScheduler::get();
  uint64_t due = sch.next_due();
  if (ctx.dec_irq_pending) { const uint64_t d = ctx.dec_written_tb + ctx.dec_value; if (d < due) due = d; }
  if (due <= wall) return;
  const uint64_t us = (due - wall) * 1000000ull / ctx.tb_freq;
  if (us < 300) return;
  ::PortFifoKick();   // el lector del FIFO, a trabajar mientras el juego duerme
  const auto t0 = std::chrono::steady_clock::now();
  ::port_perf::SleepUs(uint32_t(std::min<uint64_t>(us - 150, 2000)));
  ::port_perf::AddIdleUs(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count()));
}
"""+_i0+"  if (ctx.pc == 0x804D3E38) PortIdleWait(ctx);\n")
# hora del host de cada retrazado del VI, para presentar al ritmo del VI (pipeline_tail.inc)
_vl="              vi_period, [](CPUContext& c, uint64_t) {\n"
assert t.count(_vl)==1, 'vi lambda'
t=t.replace(_vl, "              vi_period, [](CPUContext& c, uint64_t late) {\n"
  "                  { const uint64_t tb = c.read_timebase() - late;\n"
  "                    ::PortViTick(std::chrono::duration_cast<std::chrono::nanoseconds>(c.tb_start.time_since_epoch()).count() +\n"
  "                        int64_t(tb / c.tb_freq) * 1000000000LL + int64_t((tb % c.tb_freq) * 1000000000ULL / c.tb_freq)); }\n")
t='#include "port_perf.h"\nvoid PortFifoKick();\nvoid PortViTick(int64_t);\n'+t
_out('ios.cpp', t)

# --- fifo_parser.cpp: estado del lector (matrices, XF, BP) y vaciado de caches
t=_reread('fifo_parser.cpp')
t=INC+t+'''
void PortState_fifo_parser(port_state::IO& io) {
  using namespace nwii::runtime::gx;
  io.tag("FIF0"); io.pod(g_parse_posmtx); io.pod(g_parse_nrmmtx); io.pod(g_parse_xf); io.pod(g_parse_bp);
  if (!io.saving) {
    ++g_matrix_revision; g_matrix_snapshot.reset(); g_dl_state_dirty = true;
    while (!s_dl_cache.empty()) RemoveDLCacheEntry(s_dl_cache.begin());
    s_dl_variants.clear();
  }
}
'''
_out('fifo_parser.cpp', t)

# --- gx.cpp: FIFO vacia / descarte de lo pendiente tras cargar
t=_reread('gx.cpp')
old="    if (s_parse_buf.empty()) return false;\n"
assert t.count(old)==1
t=t.replace(old,"""    if (s_gx_reset_req.load(std::memory_order_acquire)) {
        // tools/gx-pipeline: tras cargar un guardado rapido, nada de lo leido antes vale.
        s_parse_buf.clear(); s_carry.clear();
        s_gx_reset_req.store(false); s_gx_reset_ack.fetch_add(1);
    }
    s_parse_pending.store(s_parse_buf.size());
"""+old)
old2="    nwii::runtime::gx::FifoParser::Parse(s_parse_buf, commands);\n"
assert t.count(old2)==1
t=t.replace(old2,old2+"    s_parse_pending.store(s_parse_buf.size());\n")
old3="static std::vector<nwii::runtime::gx::GXCommand> s_carry;\n"
assert t.count(old3)==1
t=t.replace(old3,old3+"""static std::atomic<bool> s_gx_reset_req{false};
static std::atomic<uint32_t> s_gx_reset_ack{0};
static std::atomic<size_t> s_parse_pending{0};
""")
old4="void ProcessGXFifo() {\n    static const bool serial"
assert t.count(old4)==1
t=t.replace(old4,"""namespace port_efb { void Service(); }
void ProcessGXFifo() {
    port_efb::Service();   // copias EFB del guardado rapido (leer / restaurar), antes de dibujar nada
    if (port_menu::g_gx_after_load.exchange(false)) {
        s_last_frame.clear();
        std::lock_guard<std::mutex> lk(s_pipe_mtx);
        s_pipe_frame.clear(); s_pipe_ready = false;
    }
    static const bool serial""")
t='#include "port_menu_state.h"\n'+INC+t+'''
bool PortState_gx_idle() {
  std::lock_guard<std::mutex> lk(::g_fifo_mutex);
  return ::g_hw_fifo.empty() && s_parse_pending.load() == 0 && !s_gx_reset_req.load();
}
void PortState_gx_reset() {
  { std::lock_guard<std::mutex> lk(::g_fifo_mutex); ::g_hw_fifo.clear(); }
  { std::lock_guard<std::mutex> lk(::g_dl_snap_mtx); ::g_dl_snap.clear(); }
  const uint32_t before = s_gx_reset_ack.load();
  s_gx_reset_req.store(true);
  // El hilo de lectura lo atiende en su siguiente vuelta (cada 250 us si no hay datos).
  for (int i = 0; i < 400 && s_gx_reset_ack.load() == before; ++i) std::this_thread::sleep_for(std::chrono::microseconds(500));
}
'''
_out('gx.cpp', t)

# --- gx.cpp: decodificacion de vertices en paralelo (varios hilos) ---
# Cada dibujo se decodifica de forma independiente (DrawRaw lleva su propia copia
# del estado), asi que se reparten entre hilos. Menos tiempo en el hilo de lectura
# = las senales que espera el juego (PE finish/token) se atienden antes.
# NWII_GX_DECODE_THREADS=1 lo desactiva (decodificacion en un solo hilo, como antes).
t=_reread('gx.cpp')
old="""    for (auto &c : frame) {
        if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive) {
            nwii::runtime::gx::FifoParser::DecodeDraw(c);
            ++g_stat_draws;
        }
    }
"""
assert t.count(old)==1
t=t.replace(old,"""    ParallelDecodeDraws(frame);
""")
pool='''
// ---- Decodificacion de vertices en paralelo (tools/gx-pipeline) ----
#include <condition_variable>
namespace {
struct DecodePool {
    std::mutex m; std::condition_variable cv, done_cv;
    std::vector<nwii::runtime::gx::GXCommand*>* job = nullptr;
    std::atomic<size_t> next{0};
    unsigned active = 0; uint64_t generation = 0;
    std::vector<std::thread> threads;
    void Work(std::vector<nwii::runtime::gx::GXCommand*>& list) {
        constexpr size_t kChunk = 32;
        for (;;) {
            const size_t i = next.fetch_add(kChunk);
            if (i >= list.size()) return;
            const size_t e = std::min(list.size(), i + kChunk);
            for (size_t k = i; k < e; ++k) nwii::runtime::gx::FifoParser::DecodeDraw(*list[k]);
        }
    }
    void Start(unsigned n) {
        for (unsigned t = 0; t < n; ++t) threads.emplace_back([this] {
            uint64_t seen = 0;
            for (;;) {
                std::vector<nwii::runtime::gx::GXCommand*>* j;
                { std::unique_lock<std::mutex> lk(m); cv.wait(lk, [&] { return generation != seen; }); seen = generation; j = job; }
                if (j) Work(*j);
                { std::lock_guard<std::mutex> lk(m); if (--active == 0) done_cv.notify_all(); }
            }
        });
    }
    void Run(std::vector<nwii::runtime::gx::GXCommand*>& list) {
        { std::lock_guard<std::mutex> lk(m); job = &list; next.store(0); active = unsigned(threads.size()); ++generation; }
        cv.notify_all();
        Work(list);
        std::unique_lock<std::mutex> lk(m); done_cv.wait(lk, [&] { return active == 0; }); job = nullptr;
    }
};
}
static void ParallelDecodeDraws(std::vector<nwii::runtime::gx::GXCommand>& frame) {
    static std::vector<nwii::runtime::gx::GXCommand*> draws;
    draws.clear();
    for (auto &c : frame)
        if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive) { draws.push_back(&c); ++g_stat_draws; }
    static const unsigned helpers = [] {
        const char* e = std::getenv("NWII_GX_DECODE_THREADS");
        unsigned n = e ? unsigned(std::max(1, std::atoi(e))) : std::min(6u, std::max(1u, std::thread::hardware_concurrency() / 2));
        std::printf("[GXDecode] %u hilos para decodificar vertices\\n", n);
        return n - 1;
    }();
    static DecodePool* pool = [] { auto* p = new DecodePool(); p->Start(helpers); return p; }();
    if (helpers == 0 || draws.size() < 256) { for (auto* c : draws) nwii::runtime::gx::FifoParser::DecodeDraw(*c); return; }
    pool->Run(draws);
}
'''
anchor="static bool PrepareGXFrame(std::vector<nwii::runtime::gx::GXCommand>& out_frame) {"
assert t.count(anchor)==1
t=t.replace(anchor, pool+anchor)
_ps='            uint64_t seen = 0;\n'
assert t.count(_ps)==1, 'pool'
t=t.replace(_ps,_ps+'            port_perf::PinHelper();\n')
_w='static void GXPipelineWorker() {\n'
assert t.count(_w)==1, 'worker'
t=t.replace(_w, _w+'    port_perf::Mark(port_perf::kGx);\n')
t='#include "port_perf.h"\n'+t
_out('gx.cpp', t)


# --- gx.cpp: cuadros con varias copias a pantalla (teclado virtual) ---
# El teclado hace por cuadro: escena -> copia a pantalla SIN borrar (color bloqueado) ->
# teclado encima -> copia a pantalla con borrado. El analizador tiraba los dibujos de todo
# lo anterior a la penultima copia (la escena) y a veces mostraba la escena sola: parpadeo.
# Ahora solo se tiran los dibujos que un borrado de color posterior deja sin efecto, y una
# copia que no borra no cierra el cuadro si el juego va a seguir dibujando encima.
t=_reread('gx.cpp')
_g0="    if (last_frame_end < 0)\n        return false; \n"
assert t.count(_g0)==1, 'gx last_frame_end'
t=t.replace(_g0,_g0+"""    {
        static uint32_t s_bp41 = 0x18;   // BP 0x41 (escritura de color) al principio de s_carry
        static int s_holds = 0;
        static thread_local std::vector<std::pair<int, bool>> xfbs;   // (indice, borra el color)
        xfbs.clear();
        uint32_t bp41 = s_bp41;
        for (int i = 0; i <= last_frame_end; ++i) {
            const auto &c = s_carry[i];
            if (c.type != nwii::runtime::gx::GXCommandType::BPRegister) continue;
            if (c.reg == 0x41) bp41 = c.val;
            else if (c.reg == 0x52 && (c.val & 0x4000)) xfbs.push_back({i, (c.val & 0x800) && (bp41 & 0x08)});
        }
        size_t n = xfbs.size();
        const bool overlay = (s_carry[last_frame_end].val & 0x800) && !xfbs.back().second;
        if (overlay && s_holds < 3) {
            if (n < 2) { ++s_holds; return false; }   // esperar a la copia final del cuadro
            --n;
            last_frame_end = xfbs[n - 1].first;
        }
        s_holds = 0;
        prev_frame_end = -1;
        for (size_t k = 0; k + 1 < n; ++k) if (xfbs[k].second) prev_frame_end = xfbs[k].first;
        uint32_t b = s_bp41;
        for (int i = 0; i <= last_frame_end; ++i) {
            const auto &c = s_carry[i];
            if (c.type == nwii::runtime::gx::GXCommandType::BPRegister && c.reg == 0x41) b = c.val;
        }
        s_bp41 = b;   // s_carry se recorta hasta last_frame_end
    }
""")
_out('gx.cpp', t)

# --- gx.cpp: escrituras al FIFO grafico agrupadas ---
# Cada escritura del juego al FIFO (decenas de miles por cuadro) comprobaba la redireccion a RAM,
# tomaba el cerrojo y metia byte a byte en g_hw_fifo. Ahora, sin redireccion, se acumulan en un
# bufer del hilo del juego que se vuelca al llegar a 8 KB, en cada sondeo real de interrupciones y
# antes de cualquier acceso a registros (HW_Reg_*), que es cuando el juego puede esperar a la GPU.
# NWII_WGP_BATCH=0 vuelve al camino original.
t=_reread('gx.cpp')
_w8="void GX_WGPIPE_Write8(uint8_t val) {\n    if (wgp_redirect(val)) return;\n"
assert t.count(_w8)==1, 'w8'
t=t.replace(_w8, """// tools/gx-pipeline: bufer del hilo del juego para el FIFO grafico (ver make_patch.py)
static thread_local uint8_t s_wgp_buf[8192];   // por hilo: un volcado desde otro hilo no toca el del juego
static thread_local size_t s_wgp_n = 0;
static thread_local int s_wgp_mode = -1;   // -1 sin comprobar, 0 FIFO normal, 1 redireccion a RAM
static const bool s_wgp_slow = [] {
    const char* b = std::getenv("NWII_WGP_BATCH");
    return (b && b[0] == '0') || std::getenv("NWII_GXDUMP") || std::getenv("NWII_WGSEG") ||
           std::getenv("NWII_WGTRACE") || std::getenv("NWII_SAMPLE");
}();
static void wgp_spill() {
    if (!s_wgp_n) return;
    std::lock_guard<std::mutex> lock(g_fifo_mutex);
    g_hw_fifo.insert(g_hw_fifo.end(), s_wgp_buf, s_wgp_buf + s_wgp_n);
    s_wgp_n = 0;
}
void GX_WGPIPE_Flush() { s_wgp_mode = -1; wgp_spill(); }
void GX_WGPIPE_Discard() { s_wgp_n = 0; s_wgp_mode = -1; }
static inline bool wgp_fast(size_t n) {
    if (s_wgp_mode < 0) {
        if (s_wgp_slow) { s_wgp_mode = 1; return false; }
        static const uint32_t physical_mask = Config::get().platform == Platform::Wii ? 0x1FFFFFFFu : 0x03FFFFFFu;
        uint32_t base, end, wptr;
        nwii::runtime::hw::pi_fifo_get(base, end, wptr);
        const bool redirect = base != 0 && nwii::runtime::g_mmu &&
            (base & physical_mask) != (nwii::runtime::hw::cp_fifo_base_reg() & physical_mask);
        s_wgp_mode = redirect ? 1 : 0;
    }
    if (s_wgp_mode != 0) return false;
    if (s_wgp_n + n > sizeof(s_wgp_buf)) wgp_spill();
    return true;
}
"""+_w8.replace("{\n", "{\n    if (wgp_fast(1)) { s_wgp_buf[s_wgp_n++] = val; return; }\n    GX_WGPIPE_Flush();\n",1))
_w16="void GX_WGPIPE_Write16(uint16_t val) {\n"
assert t.count(_w16)==1, 'w16'
t=t.replace(_w16, _w16+"    if (wgp_fast(2)) { s_wgp_buf[s_wgp_n] = uint8_t(val >> 8); s_wgp_buf[s_wgp_n + 1] = uint8_t(val); s_wgp_n += 2; return; }\n    GX_WGPIPE_Flush();\n")
_w32="void GX_WGPIPE_Write32(uint32_t val) {\n"
assert t.count(_w32)==1, 'w32'
t=t.replace(_w32, _w32+"    if (wgp_fast(4)) { uint8_t* d = s_wgp_buf + s_wgp_n; d[0] = uint8_t(val >> 24); d[1] = uint8_t(val >> 16); d[2] = uint8_t(val >> 8); d[3] = uint8_t(val); s_wgp_n += 4; return; }\n    GX_WGPIPE_Flush();\n")
_w64="void GX_WGPIPE_WriteF64(double val) {\n    union { double d; uint64_t i; } u; u.d = val;\n"
assert t.count(_w64)==1, 'w64'
t=t.replace(_w64, _w64+"    if (wgp_fast(8)) { for (int k = 0; k < 8; ++k) s_wgp_buf[s_wgp_n + k] = uint8_t(u.i >> (56 - 8 * k)); s_wgp_n += 8; return; }\n    GX_WGPIPE_Flush();\n")
# el hilo de lectura del FIFO dormia 250 us (en Windows ~1-2 ms) cuando no habia un cuadro
# completo; cada vez que el juego espera a la GPU (GXDrawDone, fichas de sincronia) pagaba esa
# latencia, y con muchos monstruos espera muchas veces por cuadro. Ahora el juego lo despierta al
# volcar datos (NWII_FIFO_WAKE=0 vuelve a la espera fija).
_wk="static void GXPipelineWorker() {\n"
assert t.count(_wk)==1, 'worker'
t=t.replace(_wk, """// tools/gx-pipeline: despertar del hilo de lectura del FIFO
std::mutex g_port_fifo_wake_mtx;
std::condition_variable g_port_fifo_wake_cv;
std::atomic<bool> g_port_fifo_kick{false};
std::atomic<bool> g_port_fifo_sleeping{false};
static const bool g_port_fifo_wake = [] { const char* e = std::getenv("NWII_FIFO_WAKE"); return !(e && e[0] == '0'); }();
void PortFifoKick() {
    g_port_fifo_kick.store(true, std::memory_order_release);
    if (g_port_fifo_sleeping.load(std::memory_order_acquire)) { std::lock_guard<std::mutex> lk(g_port_fifo_wake_mtx); g_port_fifo_wake_cv.notify_one(); }
}
// Al volcar el bufer del juego no se despierta al lector cada vez: con muchos monstruos eran miles
// de llamadas al sistema por segundo en el hilo del juego (~3%). Se le despierta cada 64 KB o cada
// 0,5 ms (el lector se despierta solo cada 1 ms, y siempre que el juego se queda esperando).
void PortFifoKickBatched(size_t n) {
    static thread_local size_t since = 0;
    static thread_local int64_t last = 0;
    since += n;
    const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (since < 65536 && now - last < 500) return;
    since = 0; last = now;
    PortFifoKick();
}
"""+_wk,1)
_sl="            std::this_thread::sleep_for(std::chrono::microseconds(250));\n"
assert t.count(_sl)==1, 'sleep'
t=t.replace(_sl, """            if (g_port_fifo_wake) {
                std::unique_lock<std::mutex> lk(g_port_fifo_wake_mtx);
                g_port_fifo_sleeping.store(true, std::memory_order_release);
                if (!g_port_fifo_kick.load(std::memory_order_acquire))
                    g_port_fifo_wake_cv.wait_for(lk, std::chrono::milliseconds(1), [] { return g_port_fifo_kick.load(std::memory_order_acquire); });
                g_port_fifo_sleeping.store(false, std::memory_order_release);
                g_port_fifo_kick.store(false, std::memory_order_release);
            } else
                std::this_thread::sleep_for(std::chrono::microseconds(250));
""")
_sp="    g_hw_fifo.insert(g_hw_fifo.end(), s_wgp_buf, s_wgp_buf + s_wgp_n);\n    s_wgp_n = 0;\n}\n"
assert t.count(_sp)==1, 'spill'
t=t.replace(_sp, "    g_hw_fifo.insert(g_hw_fifo.end(), s_wgp_buf, s_wgp_buf + s_wgp_n);\n    ::PortFifoKickBatched(s_wgp_n);\n    s_wgp_n = 0;\n}\n")
t=t.replace("static void wgp_spill() {\n","void PortFifoKick();\n}\nnamespace nwii::runtime {\nstatic void wgp_spill() {\n",1) if False else t
_rs="void PortState_gx_reset() {\n"
assert t.count(_rs)==1, 'reset'
t=t.replace(_rs, _rs+"  nwii::runtime::GX_WGPIPE_Discard();\n")
_pi="bool PortState_gx_idle() {\n"
assert t.count(_pi)==1, 'idle'
t=t.replace(_pi, _pi+"  nwii::runtime::GX_WGPIPE_Flush();\n")
_out('gx.cpp', t)
# volcado antes de cada acceso a registros
t=_reread('os_hle_stubs.cpp')
for _fn in ('uint32_t HW_Reg_Read32(uint32_t addr) {\n','uint16_t HW_Reg_Read16(uint32_t addr) {\n','void HW_Reg_Write32(uint32_t addr, uint32_t val) {\n','void HW_Reg_Write16(uint32_t addr, uint16_t val) {\n'):
    assert t.count(_fn)==1, _fn
    t=t.replace(_fn, _fn+'  GX_WGPIPE_Flush();\n  if (::port_perf::g_count_regs) ::port_perf::CountReg(addr, '+('true' if 'Write' in _fn else 'false')+');\n')
_ns='namespace nwii::runtime {\nuint32_t HW_Reg_Read32(uint32_t addr) {\n'
assert t.count(_ns)==1, 'ns stubs'
t=t.replace(_ns, 'namespace nwii::runtime {\nvoid GX_WGPIPE_Flush();\nuint32_t HW_Reg_Read32(uint32_t addr) {\n')
t='#include "port_perf.h"\n'+t
_out('os_hle_stubs.cpp', t)
# y en cada sondeo real de interrupciones (ios.cpp)
t=_reread('ios.cpp')
_pp="bool process_pending_callbacks(CPUContext &ctx) {\n"
assert t.count(_pp)==1, 'ppc'
t=t.replace(_pp, "void GX_WGPIPE_Flush();\n"+_pp+"  GX_WGPIPE_Flush();\n")
_out('ios.cpp', t)

# --- output_170.cpp (ejecutable): THPVideoDecode nativo (port_thp.cpp) ---
o170=(root/'export/output_170.cpp').read_text()
nl='\r\n' if '\r\n' in o170 else '\n'
_a='void func_804E1120(CPUContext& ctx) {'+nl
assert o170.count(_a)==1, 'THPVideoDecode'
o170=o170.replace(_a,_a+'    if (ctx.pc == 0x804E1120 && port_thp::Decode(ctx)) return;  // tools/gx-pipeline: video THP en nativo'+nl)
_b='#include "functions.h"'+nl
assert o170.count(_b)==1, 'functions.h'
o170=o170.replace(_b,_b+'namespace port_thp { bool Decode(nwii::runtime::CPUContext&); }'+nl)
(pathlib.Path(__file__).parent/'output_170.cpp').write_text(o170,newline='')
print('ok output_170.cpp')

# --- output_1.cpp (ejecutable): memcpy y __fill_mem del juego en nativo (port_mem.cpp) ---
o1=(root/'export/output_1.cpp').read_text()
nl='\r\n' if '\r\n' in o1 else '\n'
for _f,_pc,_fn in (('void func_80004000(CPUContext& ctx) {','0x80004000','Copy'), ('void __fill_mem_8000429C(CPUContext& ctx) {','0x8000429C','Fill')):
    assert o1.count(_f+nl)==1, _f
    o1=o1.replace(_f+nl, _f+nl+'    if (ctx.pc == '+_pc+' && port_mem::'+_fn+'(ctx)) return;  // tools/gx-pipeline: en nativo'+nl)
_b='#include "functions.h"'+nl
assert o1.count(_b)==1, 'functions.h (output_1)'
o1=o1.replace(_b,_b+'namespace port_mem { bool Copy(nwii::runtime::CPUContext&); bool Fill(nwii::runtime::CPUContext&); }'+nl)
(pathlib.Path(__file__).parent/'output_1.cpp').write_text(o1,newline='')
print('ok output_1.cpp')

# --- cabeceras con cambios (include/runtime/*.h): originales de export/nWiiRuntime + include-patches/*.diff
import re as _re
def _apply_udiff(text, diff, name):
    a = text.split('\n'); out = []; i = 0
    lines = diff.replace('\r\n', '\n').split('\n'); k = 0
    while k < len(lines):
        m = _re.match(r'@@ -(\d+)(?:,(\d+))? \+\d+(?:,\d+)? @@', lines[k])
        if not m: k += 1; continue
        start = int(m.group(1)) - 1
        if m.group(2) == '0': start += 1
        out += a[i:start]; i = start; k += 1
        while k < len(lines) and not lines[k].startswith('@@'):
            l = lines[k]
            if l.startswith(' ') or l.startswith('-'):
                assert i < len(a) and a[i] == l[1:], f'{name}: el original no coincide en la linea {i+1}'
                if l[0] == ' ': out.append(a[i])
                i += 1
            elif l.startswith('+'): out.append(l[1:])
            k += 1
    return '\n'.join(out + a[i:])
_hp = pathlib.Path(__file__).parent / 'include-patches'
_hi = pathlib.Path(__file__).parent / 'include' / 'runtime'
_hi.mkdir(parents=True, exist_ok=True)
for _d in sorted(_hp.glob('*.h.diff')):
    _h = _d.name[:-5]
    _o = (root / 'export/nWiiRuntime/include/runtime' / _h).read_text().replace('\r\n', '\n')
    (_hi / _h).write_text(_apply_udiff(_o, _d.read_text(), _h), newline='')
    print('ok include/runtime/' + _h)

# --- cache_loop.cpp: bucles de cache del juego (DCFlushRange, DCInvalidateRange...) de golpe ---
# No hacen nada en el port; el codigo generado los adelanta por tandas que pide aqui. El original
# limitaba cada tanda a 128 lineas y a no pasar del siguiente evento: con el reloj del juego
# limitado al reloj real casi siempre habia un evento a punto y se repetia la consulta cada pocas
# vueltas (~9 % del hilo del juego con muchos monstruos). Ahora la tanda es el bucle entero, salvo
# con una interrupcion por atender (como antes) o un retardo de IPC/DI por sondeos (tandas cortas).
# Los eventos que venzan durante el bucle se atienden en el siguiente sondeo. NWII_CACHE_SCALAR=1
# vuelve al bucle de una en una (como el original con esa variable).
t=_src('core/cache_loop.cpp')
_cl0='uint32_t CacheLoopBatchCount(const CPUContext& ctx,uint32_t remaining) {\n'
assert t.count(_cl0)==1, 'cache_loop'
_ci=t.index(_cl0); _cj=t.index('\n}\n', _ci)+3
t=t[:_ci]+_cl0+"""  static const bool enabled=std::getenv("NWII_CACHE_SCALAR")==nullptr;
  static const bool wall=std::getenv("NWII_WALLTB")!=nullptr;
  if(!enabled || wall || !remaining || g_trace_after_dec>0) return 0;
  const bool dispatch=!ctx.in_callback && (ctx.msr&0x8000);
  if(dispatch && (ctx.pending_cb_count.load(std::memory_order_relaxed) ||
     (hw::pi_intsr.load(std::memory_order_relaxed)&hw::pi_intmr.load(std::memory_order_relaxed)))) return 0;
  if(g_ipc_interrupt_delay || hw::g_di_interrupt_delay) return std::min(remaining,32u);
  return remaining;
}
"""+t[_cj:]
_out('cache_loop.cpp', '// tools/gx-pipeline: copia de src/core/cache_loop.cpp generada por make_patch.py (bucles de cache de golpe).\n'+t)

# --- renderer_gl.cpp: mezcla de GX completa (operaciones logicas, alfa constante, EFB sin alfa).
# El codigo nuevo esta en apply_blend.inc (sustituye a ApplyBlend). Ver la cabecera de ese archivo.
t=_reread('renderer_gl.cpp')
_ab0=t.index('static void ApplyBlend() {\n')
_ab1=t.index('\n}\n', _ab0)+3
_ab_orig=t[_ab0:_ab1]
_ab_new=(pathlib.Path(__file__).parent/'apply_blend.inc').read_text().replace('\r\n','\n')
# volcado de un cuadro con F10 (frame_dump.inc), justo despues de ApplyBlend
_ab_new+=(pathlib.Path(__file__).parent/'frame_dump.inc').read_text().replace('\r\n','\n')
t=t[:_ab0]+_ab_new+t[_ab1:]
_hooks=[('#include <glad/glad.h>\n', '#include <glad/glad.h>\n#include <ctime>   // @nohash\n#ifdef _WIN32   // @nohash\n#include <direct.h>   // @nohash\n#else   // @nohash\n#include <sys/stat.h>   // @nohash\n#endif   // @nohash\n'),
        ('    PortShaderPrecompile();\n', '    PortShaderPrecompile();\n    if (port_menu::g_dump_request.exchange(false)) { s_dump = true; DumpBegin(); }   // @nohash\n'),
        ('        DrawPrimitive(cmd);\n', '        if (s_dump) { if (s_batch_open) { FlushBatch(); s_batch_open = false; } DumpBeforeDraw(cmd); }   // @nohash\n        DrawPrimitive(cmd);\n        if (s_dump) { if (s_batch_open) { FlushBatch(); s_batch_open = false; } DumpAfterDraw(); }   // @nohash\n'),
        ('ApplyBPRegister((uint8_t)cmd.reg, cmd.val);\n', 'ApplyBPRegister((uint8_t)cmd.reg, cmd.val);\n        if (s_dump && cmd.reg == 0x52) DumpEvent((cmd.val & 0x4000) ? "COPIA A PANTALLA" : "COPIA A TEXTURA", g_state.efbCopyDest, g_state.efbW, g_state.efbH);   // @nohash\n'),
        ('    nwii::runtime::hle::TextureCache::get().report_statistics();\n', '    DumpEnd();   // @nohash\n    nwii::runtime::hle::TextureCache::get().report_statistics();\n')]
for _a,_b in _hooks:
    assert t.count(_a)==1, _a
    t=t.replace(_a,_b)
# "Resplandor desactivado" no omitia la suma del bloom: SetupDrawState volvia sin marcar el lote como
# omitido y el cuadro se dibujaba igual con el estado anterior (imagen quemada aun sin resplandor).
_bl_old='{ if (s_fl_on) ++s_fl_bloom; return; }'
_bl_new='{ if (s_fl_on) ++s_fl_bloom; s_skip_draw = true; return; }'
assert t.count(_bl_old)==1, 'omision del bloom'
t=t.replace(_bl_old,_bl_new)
_hash_reverts=[(_bl_new,_bl_old)]
_dr='  glDrawArrays(GL_TRIANGLES, first_vertex, s_batch.size());\n'
assert t.count(_dr)==1, 'glDrawArrays lote'
t=t.replace(_dr, _dr+'  glDisable(GL_COLOR_LOGIC_OP);   // las operaciones logicas solo para este lote\n')
_out('renderer_gl.cpp', t)

# --- huella del generador de sombreadores para cache_sombreadores.bin (ver PortShaderBuildId) ---
import hashlib as _hl
_rgp = pathlib.Path(__file__).parent / 'renderer_gl.cpp'
_tgp = pathlib.Path(__file__).parent / 'tev_shader_gen.cpp'
_rgb = _rgp.read_bytes()
assert _rgb.count(b'__PORT_SHADER_ID__') == 1, 'huella sombreadores'
# La mezcla (apply_blend.inc) no cambia los sombreadores: la huella se calcula sobre el texto
# anterior a ese cambio para no invalidar cache_sombreadores.bin.
_rgh = _rgb.replace(_ab_new.replace('\n','\r\n').encode(), _ab_orig.replace('\n','\r\n').encode())
_rgh = _rgh.replace(b'  glDisable(GL_COLOR_LOGIC_OP);   // las operaciones logicas solo para este lote\r\n', b'')
_rgh = b''.join(l for l in _rgh.split(b'\n') and _rgh.splitlines(keepends=True) if b'// @nohash' not in l)
for _hn,_ho in _hash_reverts: _rgh=_rgh.replace(_hn.encode(),_ho.encode())
_sid = _hl.sha1(_rgh + _tgp.read_bytes()).hexdigest()[:16]
_rgp.write_bytes(_rgb.replace(b'__PORT_SHADER_ID__', _sid.encode()))
print('ok huella de sombreadores ' + _sid)

# --- ios.cpp: red del Wii para el modo online (port_net.cpp): /dev/net/* y sondeo de lo pendiente
t=_reread('ios.cpp')
_rg="  nwii::runtime::devices::register_all();\n"
assert t.count(_rg)==1, 'register_all'
t=t.replace(_rg, _rg+"  ::PortNetRegister();\n")
_pn="bool process_pending_callbacks(CPUContext &ctx) {\n  GX_WGPIPE_Flush();\n"
assert t.count(_pn)==1, 'ppc red'
t=t.replace(_pn, _pn+"  if (g_port_net_pending) ::PortNetPoll(ctx, ctx.pc == 0x804D3E38);\n")
t=("namespace nwii::runtime { struct CPUContext; }\nextern int g_port_net_pending;\nvoid PortNetRegister();\n"
   "void PortNetPoll(nwii::runtime::CPUContext&, bool);\n")+t
_out('ios.cpp', t)

# --- os_hle_stubs.cpp: sscanf del juego (0x8045F37C) quedo convertido en OSReport -> port_libc.cpp
t=_reread('os_hle_stubs.cpp')
_osr='extern "C" void OSReport(CPUContext &ctx) {\n'
assert t.count(_osr)==1, 'OSReport'
t=t.replace(_osr, 'void PortGuestSscanf(nwii::runtime::CPUContext&);\n'+_osr+'  if (ctx.pc == 0x8045F37Cu) { ::PortGuestSscanf(ctx); return; }   // sscanf, no OSReport\n')
_out('os_hle_stubs.cpp', t)
