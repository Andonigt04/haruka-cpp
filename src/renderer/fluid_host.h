/**
 * @file fluid_host.h
 * @brief Host del Hito 2: cablea el stack de fluido latente al mundo jugable.
 *
 * Une los tres dominios de agua en UN fluido coherente alrededor del jugador:
 *   - ShallowWaterSim  (heightfield de ríos/lagos anclado a la cámara)
 *   - PBFSolver        (splash/cascada de partículas)
 *   - HybridWater      (acoplador: río→mar por nivel del mar, cascada→lago por absorción)
 *
 * El host es dueño de los sims y sus renderers (ShallowWaterRenderer + FluidRenderer), y le
 * pregunta al mundo SOLO lo que no sabe: la altura del terreno, la elevación del mar de marea y
 * el planeta activo. `Application` se los suministra cada frame vía callbacks (ver el wiring en
 * application_render.cpp).
 *
 * El binding 0 (PerFrameData: view/projection/cámara/sol) lo deja puesto el llamador ANTES de
 * `render()`: es el UBO per-frame que ya sube el engine (`m_uboPerFrameH`), el mismo que esperan
 * softbody.vert y los cuatro shaders del fluido. Este host NO crea UBOs propios de cámara.
 */
#pragma once

#include <memory>
#include <glm/glm.hpp>
#include <functional>
#include "tools/math_types.h"
#include "rhi/rhi_types.h"

namespace Haruka::fluid { class ShallowWaterSim; class PBFSolver; class HybridWater; }
namespace Haruka { class ShallowWaterRenderer; class FluidRenderer; }

namespace Haruka {

class FluidHost {
public:
    FluidHost();
    ~FluidHost();

    // --- Fuente de verdad del mundo (las fija Application cada frame) ---
    /** @brief Elevación del terreno (m, relativa al nivel del mar 0) en un punto del mundo. */
    std::function<double(const glm::dvec3&)> terrainHeightFn;
    /** @brief Elevación del mar de marea (m sobre el nivel del mar base) en una dirección radial. */
    std::function<double(const glm::dvec3&)> tidalSeaAlongUpFn;
    glm::dvec3 planetCenter{0.0};
    double     planetRadius = 6371000.0;
    bool       hasPlanet = false;

    /** @brief Lluvia (m de profundidad por segundo) a verter en el parche. */
    float rainPerSec = 0.0f;

    /** @brief Avanza sims + acoplador. Re-ancla el parche cuando el jugador se aleja. */
    void update(float dt, const WorldPos& cameraPos);

    /** @brief Dibuja el parche de ríos/lagos y, si hay splash, el fluido (modo superficie
     *  compuesto sobre `scenePass`). El binding 0 debe estar en el UBO per-frame del engine. */
    void render(const WorldPos& cameraPos, int vpW, int vpH, RHI::RenderPassHandle scenePass);

private:
    void ensurePatch(const WorldPos& cameraPos);

    std::unique_ptr<fluid::ShallowWaterSim> m_sim;
    std::unique_ptr<fluid::PBFSolver>       m_pbf;
    std::unique_ptr<fluid::HybridWater>     m_hybrid;
    std::unique_ptr<ShallowWaterRenderer>   m_shallowRenderer;
    std::unique_ptr<FluidRenderer>          m_fluidRenderer;

    WorldPos m_anchor{0.0};
    bool     m_anchored = false;
    float    m_pendingRain = 0.0f;
    float    m_splashAccum = 0.0f;
};

} // namespace Haruka
