#ifndef DGS_GAME_MODULE_H
#define DGS_GAME_MODULE_H

// ================================================================================================
// ABI ESTABLE entre el HOST del DGS (que hace dlopen) y el MÓDULO DE REGLAS por proyecto (.so).
//
// Objetivo (ver docs PLAN_DGS_ANTICHEAT del juego): el DGS es GENÉRICO y NO se toca por juego. Cada
// proyecto entrega su lib<proyecto>_rules.so que exporta `dgs_game_module_v1()`; el DGS delega en él
// TODA la semántica de juego (física, casting, qué se mueve, edición de mundo). El mismo código se
// compila estático en el CLIENTE (predicción) y como .so para el DGS (validación) → mismas reglas.
//
// Versionado: si se rompe el ABI, se añade `dgs_game_module_v2()` (símbolo nuevo); el core NO se edita.
// F0: solo validateMove (replica el validate() histórico). F1+: validateAction, step, serialize...
// ================================================================================================
#include "include/dgs/types.h"
#include <cstdint>

namespace DGS
{
    static constexpr uint32_t GAME_MODULE_ABI = 1;

    // Estado de mundo de SOLO-LECTURA que el host presta al módulo (vive toda la sesión).
    struct WorldQuery
    {
        float chunkSizeX, chunkSizeY, chunkSizeZ;   // km — para des-cuantizar la posición global
        // F1+: getEntity(uuid), terrainHeight(x,z), reloj de mundo (mareas/viento)...
    };

    // Una muestra de movimiento a validar: estado NUEVO reportado vs último punto conocido.
    struct MoveSample
    {
        const EntityTransfer* now;      // lo que el cliente afirma AHORA
        float lastGX, lastGY, lastGZ;   // último punto GLOBAL conocido (m)
        float maxSpeed;                 // m/s permitidos (clase/estado)
        float dtSeconds;                // s desde el último punto (lo mide el host)
    };

    // vtable del módulo. Un puntero de función NULO = "sin regla" → el host aplica su fallback genérico.
    struct GameModule
    {
        uint32_t    abiVersion;   // DEBE == GAME_MODULE_ABI o el host lo rechaza
        const char* name;         // p.ej. "survival"

        // 1 = movimiento plausible/legal; 0 = cheat (el host descarta + escalará sospecha en F4).
        int (*validateMove)(const MoveSample* s, const WorldQuery* w);

        // --- Reservado F1+ (nulo en F0) ---
        // int  (*validateAction)(uint32_t actor, const uint8_t* blob, uint16_t n, const WorldQuery* w);
        // void (*step)(EntityTransfer* e, float dt, const WorldQuery* w);
    };
}

// CADA módulo exporta ESTE símbolo (C linkage → dlsym estable entre compiladores/versiones).
extern "C" const DGS::GameModule* dgs_game_module_v1(void);

#endif // DGS_GAME_MODULE_H
