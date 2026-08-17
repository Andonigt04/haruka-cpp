/**
 * @file fluid_host.h
 * @brief Host del Hito 2: cablea el stack de fluido latente al mundo jugable.
 *
 * Une los tres dominios de agua en UN fluido coherente alrededor del jugador:
 *   - ShallowWaterSim  (heightfield de ríos/lagos anclado a la cámara)
 *   - PBFSolver        (splash/cascada de partículas)
 *   - HybridWater      (acoplador: río→mar por nivel del mar, cascada→lago por absorción)
 *
 * El host es dueño de los SIMS. Sus dos renderers de agua se borraron (ver `render()`), y le
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
#include <vector>
#include <glm/glm.hpp>
#include <functional>
#include "tools/math_types.h"
#include "rhi/rhi_types.h"

namespace Haruka::fluid { class ShallowWaterSim; class PBFSolver; class HybridWater; }

namespace Haruka {

class FluidHost {
public:
    FluidHost();
    ~FluidHost();

    // --- Fuente de verdad del mundo (las fija Application cada frame) ---
    /** @brief Elevación del terreno (m, relativa al nivel del mar 0) en un punto del mundo. */
    std::function<double(const glm::dvec3&)> terrainHeightFn;
    /** @brief Publica la superficie del agua interior donde la dibuje el MAR. Lo pone el llamador
     *  con `TerrestrialPlanet::setInlandWater`; sin él, los ríos y lagos simulan pero no se ven. */
    std::function<void(const std::vector<float>&, int, const glm::vec3&, const glm::vec3&,
                       const glm::vec3&, const glm::vec3&, float)> publishInlandWater;
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

    /** @brief La simulación de ríos/lagos del MOTOR. Se expone para que el juego le añada
     *  manantiales o vierta volumen SIN crear una segunda sim.
     *
     *  ⚠️ Había dos corriendo a la vez —una aquí y otra en `Survival/scripts/world.cpp`— con rejillas
     *  distintas (48² y 96²), y solo una publicaba su superficie al mar. Dos simulaciones del mismo
     *  fenómeno es la misma clase de error que las tres aguas del render: dos estados, dos
     *  respuestas, y ninguna forma de saber cuál es la buena. La del motor es la que manda. */
    fluid::ShallowWaterSim* sim() { return m_sim.get(); }

private:
    void ensurePatch(const WorldPos& cameraPos);

    std::unique_ptr<fluid::ShallowWaterSim> m_sim;
    /// Superficie del agua que se publica al planeta (m sobre el nivel del mar; centinela = seca).
    /// Miembro y no local: se rellena cada frame y reasignarla sería basura en el heap por frame.
    std::vector<float>                     m_inlandSurface;
    std::unique_ptr<fluid::PBFSolver>       m_pbf;
    std::unique_ptr<fluid::HybridWater>     m_hybrid;

    WorldPos m_anchor{0.0};
    bool     m_anchored = false;
    float    m_pendingRain = 0.0f;
    float    m_splashAccum = 0.0f;
};

} // namespace Haruka
