#pragma once
/**
 * @file audio/sound_synth.h
 * @brief Los SONIDOS del mundo, sintetizados (no hay ni un asset de audio en el juego). Bucles
 *        (viento en el oido, hojas, rompiente, chapoteo, bajo el agua) y disparos (pisadas por
 *        material, chapuzon, crujido de rama). Puro: sin OpenAL, corre en el banco headless.
 *
 * Cada bucle es ruido filtrado con una envolvente que tiene un numero ENTERO de periodos en la
 * longitud del bucle (asi la envolvente no salta en la costura) y un fundido cola→cabeza para el
 * ruido. Determinista por semilla: dos arranques suenan igual.
 *
 * Quien decide DONDE suena cada cosa es `SoundPoints` (sound_points.h); esto solo fabrica el PCM.
 */
#include <cstdint>
#include <vector>

namespace Haruka::Audio {

enum class Sound : int {
    // Bucles (objetos que suenan mientras algo les pasa)
    WindEar = 0,    ///< el viento en el propio oido: sin direccion, gana con la velocidad
    Leaves,         ///< hojas de un arbol al viento
    Surf,           ///< rompiente (espuma en la orilla)
    Lapping,        ///< agua quieta chapoteando
    Underwater,     ///< el oido sumergido
    // Disparos (hechos)
    StepGrass, StepSand, StepRock, StepSnow, StepWater,
    Splash,         ///< entrar/salir del agua
    Crack,          ///< rama o tronco que se rompe
    COUNT
};
constexpr int kRate = 22050;   ///< Hz. Ruido hasta ~8 kHz: sobra para viento, agua y pisadas.

inline bool isLoop(Sound s) { return (int)s <= (int)Sound::Underwater; }
const char* soundName(Sound s);

/// Segundos del bucle (o del disparo).
float soundSeconds(Sound s);

/// PCM mono 16 bits. Los bucles salen sin costura (la cola fundida sobre la cabeza).
std::vector<int16_t> synthSound(Sound s, uint32_t seed = 1234u);
/// Lo mismo en float [-1,1], antes de cuantizar: es lo que mide el banco.
std::vector<float>   synthSoundF(Sound s, uint32_t seed = 1234u);

} // namespace Haruka::Audio
