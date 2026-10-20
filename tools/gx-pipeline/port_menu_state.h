#pragma once
// tools/gx-pipeline: estado compartido del menu del port (tecla P / Select+Start).
// Lo leen el bucle del invitado (pausa), el gestor de entrada (bloqueo),
// el renderizador (bloom) y la salida de audio (volumen).
#include <atomic>
#include <cstdint>
namespace port_menu {
inline std::atomic<bool> g_paused{false};         // el invitado espera mientras el menu esta abierto
inline std::atomic<bool> g_input_blocked{false};  // el juego no recibe botones
inline std::atomic<bool> g_no_bloom{false};       // omite la suma del resplandor
inline std::atomic<int>  g_volume{100};           // 0..100
inline std::atomic<bool> g_rerender{false};       // volver a dibujar el ultimo cuadro con los ajustes nuevos
inline std::atomic<int>  g_tex_filter{0};         // 0 original, 1 trilineal, 2..5 anisotropico 2x/4x/8x/16x
inline std::atomic<bool> g_hd_textures{false};     // texturas HD (paquete Dolphin en texturas_hd)
inline std::atomic<bool> g_tex_reload{false};      // vaciar la cache de texturas (cambio Original/HD)
inline std::atomic<int>  g_save_request{0};        // guardado rapido: ranura pedida (0 = nada)
inline std::atomic<int>  g_load_request{0};        // cargado rapido: ranura pedida
inline std::atomic<int>  g_last_result{0};         // +n guardado, -n cargado, 100+ errores
inline std::atomic<uint64_t> g_result_serial{0};   // cambia con cada resultado (aviso en pantalla)
inline std::atomic<bool> g_gx_after_load{false};   // el hilo principal descarta el cuadro anterior
inline std::atomic<int>  g_lang{-1};                // idioma del juego (port_lang.h); -1 = el de la configuracion guardada
inline std::atomic<float> g_hfov_scale{1.0f};
inline std::atomic<bool> g_force_clear{false};   // borrar el EFB al empezar el siguiente dibujo (redibujo del menu)
// Perfil del cuadro (registro de tirones): lo que hizo el dibujo de este cuadro.
inline std::atomic<uint32_t> g_prof_compiles{0};    // sombreadores compilados
inline std::atomic<uint32_t> g_prof_uploads{0};     // texturas subidas (nuevas o cambiadas)
inline std::atomic<uint64_t> g_prof_upload_us{0};   // tiempo decodificando + subiendo texturas
inline std::atomic<uint32_t> g_prof_hd{0};          // texturas HD puestas en este cuadro
inline std::atomic<uint64_t> g_prof_compile_us{0};     // 21:9 / 32:9: ensancha las proyecciones 16:9 del juego
}
