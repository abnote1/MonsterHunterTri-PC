#pragma once
// tools/gx-pipeline: rendimiento por hilo.
// - Tiempo de CPU por hilo y tiempo libre del juego para el registro ([STAT] cpu_..., juego_libre).
// - Reparto de nucleos: el hilo del juego (el que limita los FPS) se queda un nucleo fisico
//   entero y los hilos auxiliares (dibujo, graficos, decodificacion) van a los demas, para que
//   no le quiten ciclos por SMT. NWII_AFFINITY=0 lo desactiva.
// - SleepUs: espera precisa (temporizador de alta resolucion) para el hilo del juego cuando
//   esta ocioso, en vez de girar en vacio (en la Steam Deck libera energia y el nucleo hermano).
#include <cstdint>
#include <string>
namespace port_perf {
enum Thread { kGuest = 0, kMain = 1, kGx = 2, kCount = 3 };
void Mark(Thread t);        // llamar desde el propio hilo (registra y reparte nucleos)
void PinHelper();           // hilo auxiliar sin registro propio: fuera del nucleo del juego
double ThreadMs(Thread t);  // tiempo de CPU acumulado del hilo (ms); 0 si no se conoce
double ProcessMs();         // tiempo de CPU de todo el proceso (ms)
unsigned Cores();           // hilos de CPU del equipo
const char* AffinityInfo(); // descripcion del reparto, para el registro
void SleepUs(uint32_t us);  // espera precisa
void AddIdleUs(uint64_t us);
uint64_t IdleUs();          // tiempo que el juego ha dormido por estar ocioso (us, acumulado)
// Perfil por tiempo (NWII_CPU_SAMPLE=1): un hilo mira cada ms en que direccion del juego esta
// el hilo del juego (ctx.pc, que se actualiza en cada llamada y bucle) y cada 10 s escribe
// [Muestreo] con los tramos de 256 bytes mas frecuentes. A diferencia de NWII_CPU_PROFILE,
// no favorece a los bucles cortos.
void StartSampler(const volatile uint32_t* pc);
// Con el muestreo activo: cuenta lecturas de registros del hardware por direccion (para ver
// esperas activas del juego, p. ej. sondeando la GPU) y las escribe cada 10 s en [Registros].
extern bool g_count_regs;
void CountReg(uint32_t addr, bool write);
// Steam Deck (Proton): frecuencias de la CPU (todas y la del nucleo del juego), carga y reloj de
// la GPU, consumo y temperatura del APU, leidos de /proc y /sys por la unidad Z: de Wine.
// Para la linea [Deck] cada 5 s. Cadena vacia si no se puede leer.
std::string DeckInfo();
}  // namespace port_perf
