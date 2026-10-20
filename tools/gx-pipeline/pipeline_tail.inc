
// ---- Canalizacion GX (tools/gx-pipeline) ----------------------------------
// Un hilo de trabajo lee y decodifica el FIFO en cuanto llega (ahi se emiten
// las senales PE finish/token que espera GXDrawDone) y entrega cuadros
// completos; el hilo principal solo dibuja con OpenGL. Asi el juego no espera
// al dibujado del cuadro anterior. NWII_GX_SERIAL=1 recupera el
// comportamiento original (todo en el hilo principal, como antes).
// Registro de tirones (NWII_HITCH_LOG=1): cuando entre dos cuadros dibujados pasan
// mas de 45 ms, anota cuanto tardo cada fase para separar dibujado, preparacion
// (lectura/decodificacion del FIFO) y cuadros que el juego entrego tarde.
static std::atomic<uint32_t> s_last_prep_us{0};
static std::atomic<uint32_t> s_last_prep_draws{0};
static void RenderGXFrame(std::vector<nwii::runtime::gx::GXCommand>& frame) {
    static const bool hitch_log = std::getenv("NWII_HITCH_LOG") != nullptr;
    static const auto t_origin = std::chrono::steady_clock::now();
    static auto last_end = t_origin;
    const auto h0 = std::chrono::steady_clock::now();
    ++g_stat_frames;
    if (g_window) {
        if (!g_renderer) {
            g_renderer = nwii::runtime::gx::IRenderer::Create();
            g_renderer->Initialize(nullptr);
        }
        if (g_renderer) {
            auto r0 = std::chrono::steady_clock::now();
            g_renderer->Render(frame);
            auto r1 = std::chrono::steady_clock::now();
            g_stat_render_us +=
                std::chrono::duration_cast<std::chrono::microseconds>(r1 - r0)
                    .count();
        }
    }
    if (hitch_log) {
        const auto h1 = std::chrono::steady_clock::now();
        const double gap = std::chrono::duration<double, std::milli>(h1 - last_end).count();
        const double rend = std::chrono::duration<double, std::milli>(h1 - h0).count();
        if (gap > 45.0 && g_stat_frames > 1) {
            size_t draws = 0;
            for (const auto& c : frame)
                if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive) ++draws;
            std::printf("[Hitch] t=%.2fs gap=%.1fms render=%.1fms prep=%.1fms draws=%zu wait=%.1fms somb=%u(%.0fms) tex=%u(%.0fms) hd=%u\n",
                std::chrono::duration<double>(h1 - t_origin).count(), gap, rend,
                s_last_prep_us.load() / 1000.0, draws, gap - rend,
                port_menu::g_prof_compiles.load(), port_menu::g_prof_compile_us.load() / 1000.0,
                port_menu::g_prof_uploads.load(), port_menu::g_prof_upload_us.load() / 1000.0, port_menu::g_prof_hd.load());
        }
        last_end = h1;
        port_menu::g_prof_compiles = 0; port_menu::g_prof_compile_us = 0; port_menu::g_prof_uploads = 0; port_menu::g_prof_upload_us = 0; port_menu::g_prof_hd = 0;
    }
}

static std::mutex s_pipe_mtx;
static std::vector<nwii::runtime::gx::GXCommand> s_pipe_frame;
static bool s_pipe_ready = false;
uint64_t g_stat_pipe_merged = 0;

static void GXPipelineWorker() {
    std::vector<nwii::runtime::gx::GXCommand> f;
    for (;;) {
        const auto p0 = std::chrono::steady_clock::now();
        if (!PrepareGXFrame(f)) {
            std::this_thread::sleep_for(std::chrono::microseconds(250));
            continue;
        }
        s_last_prep_us.store((uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - p0).count());
        std::lock_guard<std::mutex> lk(s_pipe_mtx);
        if (!s_pipe_ready) {
            s_pipe_frame.swap(f);
            s_pipe_ready = true;
        } else {
            // El dibujado va con retraso: como el codigo original con varios
            // cuadros pendientes, se conservan los comandos de estado/copias
            // del cuadro anterior y solo se descartan sus dibujos.
            std::vector<nwii::runtime::gx::GXCommand> merged;
            merged.reserve(s_pipe_frame.size() + f.size());
            for (auto& c : s_pipe_frame)
                if (c.type != nwii::runtime::gx::GXCommandType::DrawPrimitive)
                    merged.push_back(std::move(c));
            for (auto& c : f) merged.push_back(std::move(c));
            s_pipe_frame.swap(merged);
            ++g_stat_pipe_merged;
        }
        f.clear();
    }
}

// ---- Cuadros intermedios (NWII_INTERP=1) ----------------------------------
// El juego calcula su logica a 30 Hz. Para mostrar 60 sin tocar la velocidad,
// cuando llega el cuadro real B se dibuja primero un cuadro intermedio entre el
// anterior (A) y B, y en la siguiente vuelta del bucle (siguiente refresco) se
// dibuja B. Cada dibujo de B se empareja con el de A que tiene el mismo tipo de
// primitiva, numero de vertices, coordenadas de textura y color del primer y
// ultimo vertice (en el orden en que aparecen); las posiciones (ya en espacio
// de vista) y normales se promedian. Lo que no empareja se dibuja como en B.
// Si empareja menos de la mitad (cambio de camara/escena) no se interpola.
// Coste: un cuadro de latencia de 16,7 ms y dibujar el doble.
namespace {
using GXCmdVec = std::vector<nwii::runtime::gx::GXCommand>;
inline uint64_t InterpMix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h; }
inline uint64_t InterpBits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
uint64_t InterpKey(const nwii::runtime::gx::GXCommand& c) {
    const auto& v = c.vertices;
    uint64_t h = InterpMix(uint64_t(c.prim_type) << 32 | v.size(), 0x51ull);
    for (const auto* x : {&v.front(), &v.back()}) {
        h = InterpMix(h, InterpBits(x->tex[0][0]) << 32 | InterpBits(x->tex[0][1]));
        h = InterpMix(h, InterpBits(x->color[0]) << 32 | InterpBits(x->color[3]));
        h = InterpMix(h, uint64_t(x->has_tex[0]) | uint64_t(x->has_color) << 1 | uint64_t(x->pre_xf) << 2);
    }
    return h;
}
struct InterpStats { size_t draws = 0, matched = 0; };
double g_render_ema_ms = 0.0;
uint64_t g_interp_frames = 0, g_interp_skipped = 0, g_interp_slow = 0;
// Promedia B con A en el sitio; guarda las posiciones originales de B en 'backup'.
bool InterpolateInPlace(const GXCmdVec& a, GXCmdVec& b, std::vector<float>& backup, std::vector<uint32_t>& touched) {
    using nwii::runtime::gx::GXCommandType;
    static std::unordered_map<uint64_t, std::vector<uint32_t>> index;
    static std::unordered_map<uint64_t, uint32_t> cursor;
    for (auto& e : index) e.second.clear();
    cursor.clear();
    static std::vector<char> used; used.assign(a.size(), 0);
    for (uint32_t i = 0; i < a.size(); ++i) {
        const auto& c = a[i];
        if (c.type != GXCommandType::DrawPrimitive || c.vertices.empty()) continue;
        index[InterpKey(c)].push_back(i);
    }
    backup.clear(); touched.clear();
    InterpStats st;
    for (uint32_t j = 0; j < b.size(); ++j) {
        auto& c = b[j];
        if (c.type != GXCommandType::DrawPrimitive || c.vertices.empty()) continue;
        ++st.draws;
        const uint64_t k = InterpKey(c);
        auto it = index.find(k);
        if (it == index.end()) continue;
        // Entre los candidatos con la misma clave (p. ej. matas de hierba iguales,
        // que el juego puede reordenar), se elige el mas cercano de una ventana.
        uint32_t& cur = cursor[k];
        auto& lst = it->second;
        while (cur < lst.size() && used[lst[cur]]) ++cur;
        if (cur >= lst.size()) continue;
        const float* pb = c.vertices[0].pos;
        uint32_t best = UINT32_MAX; float best_d2 = 0;
        for (uint32_t w = cur, seen = 0; w < lst.size() && seen < 12; ++w) {
            const uint32_t ai = lst[w];
            if (used[ai]) continue;
            ++seen;
            const auto& cand = a[ai];
            if (cand.vertices.size() != c.vertices.size()) continue;
            const float* pa = cand.vertices[0].pos;
            const float dx = pa[0]-pb[0], dy = pa[1]-pb[1], dz = pa[2]-pb[2];
            const float d2 = dx*dx + dy*dy + dz*dz;
            if (best == UINT32_MAX || d2 < best_d2) { best = ai; best_d2 = d2; }
        }
        if (best == UINT32_MAX) continue;
        const float r2 = pb[0]*pb[0] + pb[1]*pb[1] + pb[2]*pb[2];
        // Un salto grande respecto a la distancia a la camara = no es el mismo objeto.
        if (best_d2 > 0.04f * r2 + 1.0f) continue;
        used[best] = 1;
        const auto& ca = a[best];
        ++st.matched;
        touched.push_back(j);
        for (size_t n = 0; n < c.vertices.size(); ++n) {
            auto& vb = c.vertices[n]; const auto& va = ca.vertices[n];
            backup.insert(backup.end(), {vb.pos[0], vb.pos[1], vb.pos[2], vb.norm[0], vb.norm[1], vb.norm[2]});
            for (int q = 0; q < 3; ++q) { vb.pos[q] = 0.5f * (vb.pos[q] + va.pos[q]); vb.norm[q] = 0.5f * (vb.norm[q] + va.norm[q]); }
        }
    }
    if (st.draws == 0 || st.matched * 2 < st.draws) {
        // Cambio de escena o de camara: deshacer y no interpolar.
        size_t o = 0;
        for (uint32_t j : touched) for (auto& vb : b[j].vertices) {
            for (int q = 0; q < 3; ++q) { vb.pos[q] = backup[o + q]; vb.norm[q] = backup[o + 3 + q]; }
            o += 6;
        }
        touched.clear(); backup.clear();
        return false;
    }
    return true;
}
void RestoreInterpolated(GXCmdVec& b, const std::vector<float>& backup, const std::vector<uint32_t>& touched) {
    size_t o = 0;
    for (uint32_t j : touched) for (auto& vb : b[j].vertices) {
        for (int q = 0; q < 3; ++q) { vb.pos[q] = backup[o + q]; vb.norm[q] = backup[o + 3 + q]; }
        o += 6;
    }
}
} // namespace

// Menu del port: vuelve a dibujar el ultimo cuadro (el juego esta en pausa) para
// que los cambios graficos se vean al momento.
static std::vector<nwii::runtime::gx::GXCommand> s_last_frame;
void RerenderLastGXFrame() {
    if (s_last_frame.empty()) return;
    port_menu::g_force_clear.store(true);
    RenderGXFrame(s_last_frame);
}

void ProcessGXFifo() {
    static const bool serial = std::getenv("NWII_GX_SERIAL") != nullptr;
    static const bool interp = std::getenv("NWII_INTERP") != nullptr && std::getenv("NWII_INTERP")[0] != '0';
    static thread_local std::vector<nwii::runtime::gx::GXCommand> f;
    if (serial || !g_window) {
        if (PrepareGXFrame(f)) RenderGXFrame(f);
        f.clear();
        return;
    }
    static bool started = false;
    if (!started) {
        started = true;
        std::cout << "[GXPipeline] hilo de lectura/decodificacion activo" << (interp ? " (cuadros intermedios activos)" : "") << "\n";
        std::thread(GXPipelineWorker).detach();
    }
    if (!interp) {
        {
            std::lock_guard<std::mutex> lk(s_pipe_mtx);
            if (!s_pipe_ready) return;
            f.swap(s_pipe_frame);
            s_pipe_ready = false;
        }
        RenderGXFrame(f);
        s_last_frame.swap(f);   // se conserva para el menu del port (RerenderLastGXFrame)
        f.clear();
        return;
    }
    // prev = ultimo cuadro real dibujado (A); pending = cuadro real ya interpolado
    // que falta mostrar (B); fresh = cuadro que llego mientras habia uno pendiente.
    static GXCmdVec prev, pending, fresh;
    static bool have_prev = false, have_pending = false, have_fresh = false, pending_clear = false;
    static std::vector<float> backup; static std::vector<uint32_t> touched;
    if (have_pending) {
        // Mostrar el cuadro real que sigue al intermedio de la vuelta anterior.
        {
            std::lock_guard<std::mutex> lk(s_pipe_mtx);
            if (s_pipe_ready && !have_fresh) { fresh.swap(s_pipe_frame); s_pipe_ready = false; have_fresh = true; }
        }
        if (pending_clear) nwii::runtime::gx::g_state.pe_clear_pending = true;
        const auto rp0 = std::chrono::steady_clock::now();
        RenderGXFrame(pending);
        g_render_ema_ms = 0.8 * g_render_ema_ms + 0.2 * std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rp0).count();
        prev.swap(pending); pending.clear();
        have_prev = true; have_pending = false;
        return;
    }
    GXCmdVec* next = nullptr;
    if (have_fresh) { next = &fresh; }
    else {
        std::lock_guard<std::mutex> lk(s_pipe_mtx);
        if (!s_pipe_ready) return;
        f.swap(s_pipe_frame);
        s_pipe_ready = false;
        next = &f;
    }
    // Solo se intercala un cuadro si hay tiempo: con el dibujado lento (media
    // reciente > 11 ms) dos dibujos no caben en un refresco y se veria peor.
    const bool time_ok = g_render_ema_ms < 11.0;
    if (have_prev && time_ok && InterpolateInPlace(prev, *next, backup, touched)) {
        pending_clear = nwii::runtime::gx::g_state.pe_clear_pending;
        const auto ri0 = std::chrono::steady_clock::now();
        RenderGXFrame(*next);                 // cuadro intermedio
        g_render_ema_ms = 0.8 * g_render_ema_ms + 0.2 * std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ri0).count();
        RestoreInterpolated(*next, backup, touched);
        pending.swap(*next); next->clear();
        have_pending = true; ++g_interp_frames;
    } else {
        const auto rr0 = std::chrono::steady_clock::now();
        RenderGXFrame(*next);                 // cuadro real sin intermedio
        g_render_ema_ms = 0.8 * g_render_ema_ms + 0.2 * std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rr0).count();
        if (!time_ok) ++g_interp_slow;
        prev.swap(*next); next->clear();
        have_prev = true; ++g_interp_skipped;
    }
    if (next == &fresh) have_fresh = false;
    static auto last_report = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report >= std::chrono::seconds(5)) {
        last_report = now;
        std::printf("[Intermedios] reales_con_intermedio=%llu sin_intermedio=%llu de_ellos_por_tiempo=%llu dibujo_medio=%.1fms\n",
                    (unsigned long long)g_interp_frames, (unsigned long long)g_interp_skipped, (unsigned long long)g_interp_slow, g_render_ema_ms);
        g_interp_frames = g_interp_skipped = g_interp_slow = 0;
    }
}
