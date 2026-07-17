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
#include <cmath>
#include <cstring>

namespace
{
    int validateMove(const DGS::MoveSample* s, const DGS::WorldQuery* w)
    {
        if (!s || !s->now || !w) return 1;
        const float dt = s->dtSeconds;
        if (dt <= 0.0f || dt > 2.0f) return 1;   // sin muestra fiable → no penalizar

        // Posición GLOBAL reportada (m). El host manda chunk + pos coherentes en metros.
        const double gx = (double)s->now->chunkX * w->chunkSizeX + s->now->pos[0];
        const double gy = (double)s->now->chunkY * w->chunkSizeY + s->now->pos[1];
        const double gz = (double)s->now->chunkZ * w->chunkSizeZ + s->now->pos[2];

        // (1) VELOCIDAD / TELEPORT: el salto desde el último punto no puede superar maxSpeed·dt.
        const double dx = gx - s->lastGX, dy = gy - s->lastGY, dz = gz - s->lastGZ;
        const double distSq = dx * dx + dy * dy + dz * dz;
        const double budget = (double)s->maxSpeed * dt + 1.0;   // +1 m (latencia/tolerancia)
        if (distSq > budget * budget) return 0;                 // speed-hack / teleport

        // (2) TERRENO: no atravesar el suelo (noclip). Se muestrea el terreno ANALÍTICO — el MISMO
        // sampler CPU que usa el cliente, sin GL, determinista → cliente y host coinciden. Antes esto
        // era un TODO; ahora el host provisiona el planeta en el WorldQuery.
        if (w->planetRadius > 1.0) {
            const glm::dvec3 center(w->planetCenter[0], w->planetCenter[1], w->planetCenter[2]);
            const glm::dvec3 rel = glm::dvec3(gx, gy, gz) - center;
            const double playerR = glm::length(rel);
            if (playerR > 1.0) {
                Haruka::WorldGenParams W = Haruka::deriveWorldParams(w->seed, w->planetRadius);
                W.reliefStrength = w->reliefStrength;
                W.profile        = w->profile;
                const glm::vec3 dir = glm::vec3(rel / playerR);
                const double elevKm  = Haruka::sampleTerrainV2(dir, W, w->planetRadius).elevKm;
                const double surface = w->planetRadius + elevKm * 1000.0;
                // Por DEBAJO del suelo con margen = noclip/tunneling. Margen generoso (5 m) para no
                // penalizar el desfase malla↔analítico ni la interpolación entre chunks.
                if (playerR < surface - 5.0) return 0;
            }
        }
        return 1;
    }

    // Validación GENÉRICA de una acción. El blob es OPACO: solo se asume que ABRE con un ActionHeader
    // (formato común del motor); el payload que sigue es del juego y NO se toca. Se comprueban SOLO
    // invariantes que no requieren estado de mundo — malformación o valores absurdos, que ningún juego
    // legítimo emite. La semántica con estado (¿posee el ítem?, ¿está en alcance?, ¿es el dueño del
    // inventario?) la resuelve el módulo del PROYECTO, que sí conoce la estructura. NULO → host acepta.
    int validateAction(uint32_t /*actor*/, const uint8_t* blob, uint16_t n, const DGS::WorldQuery* /*w*/)
    {
        if (!blob || n < sizeof(DGS::ActionHeader)) return 0;   // sin encabezado válido → rechazo

        DGS::ActionHeader h{};
        std::memcpy(&h, blob, sizeof(h));                       // sin alias: el blob no está alineado

        if (h.verb == DGS::ACT_NONE || h.verb >= DGS::ACT__COUNT) return 0;  // verbo desconocido
        if (h.flags != 0) return 0;                                          // bits reservados puestos
        if (!std::isfinite(h.amount) || h.amount < 0.0f) return 0;           // cantidad absurda
        // Cotas de cordura por verbo (un cliente honesto nunca las cruza; un cheat sí).
        if (h.verb == DGS::ACT_DAMAGE   && h.amount > 1.0e6f)  return 0;     // daño imposible
        if (h.verb == DGS::ACT_TRANSFER && h.amount > 1.0e5f)  return 0;     // stack imposible
        for (int i = 0; i < 3; ++i) if (!std::isfinite(h.at[i])) return 0;   // punto no finito
        return 1;   // bien formada; el resto (estado) lo decide el módulo del proyecto
    }

    // Self-check: al cargar el módulo, prueba que el sampler del motor está VIVO dentro del .so
    // (GL-free) — deriva params de un seed demo y muestrea una dirección. Log una vez, no valida nada.
    void selfCheck()
    {
        Haruka::WorldGenParams W = Haruka::deriveWorldParams(42u, 1.5e6);
        Haruka::TerrainSample  t = Haruka::sampleTerrainV2(glm::normalize(glm::vec3(1, 1, 1)), W, 1.5e6);
        std::fprintf(stderr, "[haruka-rules] sampler del motor OK (GL-free): elevKm=%.4f en dir(1,1,1)\n", t.elevKm);
    }

    const DGS::GameModule g_module = { DGS::GAME_MODULE_ABI, "haruka-default", &validateMove, &validateAction };
}

extern "C" const DGS::GameModule* dgs_game_module_v1(void)
{
    static const bool once = [] { selfCheck(); return true; }();
    (void)once;
    return &g_module;
}
