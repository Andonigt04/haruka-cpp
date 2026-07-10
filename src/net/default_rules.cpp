// ================================================================================================
// Módulo de reglas POR DEFECTO del MOTOR para el DGS (implementa el ABI dgs_game_module_v1).
//
// Arquitectura: el MOTOR trae la física/validación por defecto que el DGS carga (dlopen). Es GL-FREE
// (solo el ABI del DGS + haruka_simbase = el sampler de terreno CPU + glm) → arranca en un servidor
// headless. Un PROYECTO puede sustituirlo con reglas propias (un script del juego) vía GAME_MODULE_SO;
// si no, se usa ESTE. La MISMA física servirá al cliente para predicción.
// ================================================================================================
#include "include/dgs/game_module.h"
#include "core/terrain/terrain_sampler_v2.h"   // haruka_simbase: física por defecto (sampler de terreno)

#include <glm/glm.hpp>
#include <cstdio>

namespace
{
    int validateMove(const DGS::MoveSample* s, const DGS::WorldQuery* w)
    {
        if (!s || !s->now || !w) return 1;
        const float dt = s->dtSeconds;
        if (dt <= 0.0f || dt > 2.0f) return 1;   // sin muestra fiable → no penalizar

        const float gx = s->now->chunkX * w->chunkSizeX + s->now->pos[0];
        const float gy = s->now->chunkY * w->chunkSizeY + s->now->pos[1];
        const float gz = s->now->chunkZ * w->chunkSizeZ + s->now->pos[2];
        const float dx = gx - s->lastGX, dy = gy - s->lastGY, dz = gz - s->lastGZ;
        const float distSq = dx * dx + dy * dy + dz * dz;
        const float budget = s->maxSpeed * dt + 1.0f;   // +1 m de margen (latencia/tolerancia)

        // TODO(F1b-wiring): colisión REAL con Haruka::sampleTerrainV2(dir, W, radius).elevKm →
        // rechazar volar (y >> superficie sin flag) / atravesar el suelo. Falta provisionar al módulo
        // el seed/radio/genparams del mundo (world-state, C7) y mapear la posición global del DGS
        // (chunk+pos) a una dirección sobre el planeta. La infra ya está: haruka_simbase linkado.
        return (distSq <= budget * budget) ? 1 : 0;
    }

    // Self-check: al cargar el módulo, prueba que el sampler del motor está VIVO dentro del .so
    // (GL-free) — deriva params de un seed demo y muestrea una dirección. Log una vez, no valida nada.
    void selfCheck()
    {
        Haruka::WorldGenParams W = Haruka::deriveWorldParams(42u, 1.5e6);
        Haruka::TerrainSample  t = Haruka::sampleTerrainV2(glm::normalize(glm::vec3(1, 1, 1)), W, 1.5e6);
        std::fprintf(stderr, "[haruka-rules] sampler del motor OK (GL-free): elevKm=%.4f en dir(1,1,1)\n", t.elevKm);
    }

    const DGS::GameModule g_module = { DGS::GAME_MODULE_ABI, "haruka-default", &validateMove };
}

extern "C" const DGS::GameModule* dgs_game_module_v1(void)
{
    static const bool once = [] { selfCheck(); return true; }();
    (void)once;
    return &g_module;
}
