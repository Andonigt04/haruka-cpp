/**
 * @file fluid_host.cpp
 * @brief Implementación del host del stack de fluido (Hito 2). Ver fluid_host.h.
 *
 * Todo el archivo cuelga de HARUKA_MOD_FLUIDS (default ON): cuando el módulo está OFF los
 * sources de physics/fluid + renderers de fluido se excluyen del build, así que este TU queda
 * vacío y `Application` no referencia a FluidHost (el miembro y el hook también van gateados).
 */
#include "fluid_host.h"
#include "tools/profiler.h"   // sub-scopes: este bloque se comia el 97% del frame
#include "core/modules.h" // HARUKA_MOD_FLUIDS — application.h/application_render lo incluyen

#ifdef HARUKA_MOD_FLUIDS

#include "physics/fluid/shallow_water.h"
#include "physics/fluid/pbf_solver.h"
#include "physics/fluid/hybrid_water.h"
#include "renderer/shallow_water_renderer.h"
#include "renderer/fluid_renderer.h"
#include <algorithm>
#include <cmath>

namespace Haruka {

FluidHost::FluidHost() {
    m_sim             = std::make_unique<fluid::ShallowWaterSim>();
    m_pbf             = std::make_unique<fluid::PBFSolver>();
    m_hybrid          = std::make_unique<fluid::HybridWater>();
    m_shallowRenderer = std::make_unique<ShallowWaterRenderer>();
    m_fluidRenderer   = std::make_unique<FluidRenderer>();

    m_hybrid->heightfield = m_sim.get();
    m_hybrid->particles   = m_pbf.get();
    m_shallowRenderer->setSim(m_sim.get());
    m_fluidRenderer->setSolver(m_pbf.get());
    m_fluidRenderer->setRadius(0.30f);

    // Splash de partículas: pequeñas y espaciadas ~0.3 m (h = 0.45 → spacing 0.5h). El acoplador
    // absorbe por volumen; un h menor da gotas discretas y estables sin inundar el lago.
    m_pbf->h = 0.45f;
    m_pbf->solverIters = 2;
}

FluidHost::~FluidHost() = default;

namespace {
// Base ortonormal estable del plano tangente: up = normal de superficie, tan/bit en el plano.
glm::dvec3 basisTangent(const glm::dvec3& up) {
    const glm::dvec3 axis = (std::abs(up.y) < 0.9) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 t = glm::cross(axis, up);
    const double l = glm::length(t);
    return (l > 1e-9) ? t / l : glm::dvec3(1, 0, 0);
}
} // namespace

void FluidHost::ensurePatch(const WorldPos& cameraPos) {
    // Re-anclamos solo cuando el jugador se desplaza una fracción del span (la re-muestreada de
    // terreno es n² celdas ≈ barata, pero tampoco hay que hacerla cada frame).
    const double kReanchorM = 160.0;
    const double kSpan = 420.0;
    const int    kN    = 96;
    // ⚠️ Distancia TANGENCIAL (sobre la superficie), NO la 3D a la cámara.
    //
    // El ancla está en el SUELO (ver el comentario de abajo), así que `length(cameraPos - m_anchor)`
    // incluye la ALTITUD. En cuanto la cámara sube más de kReanchorM sobre el terreno —volar, caer,
    // un salto largo— la guarda deja de cumplirse SIEMPRE y esto re-ancla cada frame: 9216 muestreos
    // del terreno procedural a ~2,15 µs cada uno = ~21 ms por frame, medidos. El parche solo tiene
    // que seguir al jugador en HORIZONTAL; su altura da igual porque el ancla ya se proyecta al suelo.
    if (m_anchored) {
        double moved;
        if (hasPlanet) {
            const glm::dvec3 camDir = glm::normalize(cameraPos - planetCenter);
            const glm::dvec3 ancDir = glm::normalize(m_anchor  - planetCenter);
            moved = glm::length((camDir - ancDir) * planetRadius);   // cuerda sobre la esfera
        } else {
            moved = glm::length(cameraPos - m_anchor);
        }
        if (moved < kReanchorM) return;
    }
    if (!terrainHeightFn) return;
    // 96×96 = 9216 llamadas al terreno procedural. Solo al re-anclar, pero cuando pasa se nota.
    HARUKA_PROFILE("fluid.ensurePatch(96x96 terreno)");

    const glm::dvec3 up  = hasPlanet ? glm::normalize(cameraPos - planetCenter) : glm::dvec3(0, 1, 0);
    const glm::dvec3 tan = basisTangent(up);
    const glm::dvec3 bit = glm::normalize(glm::cross(up, tan));
    // ANCLA EN EL SUELO, no en la cámara: el plano base del parche pasa por la SUPERFICIE bajo la
    // cámara, no por los ojos. Si ancláramos en cameraPos, el agua del centro quedaría a la altura
    // del ojo y "taparía" la vista (el agua-en-cámara). `terrainHeightFn` devuelve la COTA del
    // terreno en la dirección radial (sampleHeight es independiente de la altitud del punto), así
    // que R + cota sobre `up` es el punto de la superficie en esa dirección.
    const double hGround = hasPlanet ? terrainHeightFn(cameraPos) : 0.0;
    const glm::dvec3 anchor =
        hasPlanet ? planetCenter + up * (planetRadius + hGround) : cameraPos;
    m_sim->init(anchor, tan, bit, up, kSpan, kN, terrainHeightFn);
    m_anchor = anchor;
    m_anchored = true;
    m_pendingRain = 0.0f;
}

void FluidHost::update(float dt, const WorldPos& cameraPos) {
    // Sin planeta activo el parche no tiene terreno ni mar real que acoplar: no hay que dibujar
    // ninguna lámina. `anchored` queda false → render() no hace nada.
    if (!m_sim || !terrainHeightFn || !hasPlanet) return;
    ensurePatch(cameraPos);

    // Nivel del mar en el marco del parche: m_terrain es RELATIVO al suelo del ancla (h0), y la
    // superficie del océano de marea va ABSOLUTA (rel. al mar base 0). seaLevel = tidal - h0.
    const glm::dvec3 up = m_sim->up();
    const double h0 = terrainHeightFn(m_sim->anchor());
    const double tidal = tidalSeaAlongUpFn ? tidalSeaAlongUpFn(up) : 0.0;
    const double seaLevel = tidal - h0;

    // Lluvia acumulada: se vierte en saltos de ≥2 cm para no spamear addRain cada frame.
    m_pendingRain += rainPerSec * dt;
    if (m_pendingRain >= 0.02f) { m_sim->addRain(m_pendingRain); m_pendingRain = 0.0f; }

    // Ríos/lagos: avanza la altura (modelo de tubos, sub-step CFL) y pinza la costa al mar.
    { HARUKA_PROFILE("fluid.sim.step(shallow water)");
      m_sim->step(dt);
      m_sim->applySeaLevel((float)seaLevel); }

    // Splash PBF: cascada por bordes de terreno pronunciados (colecta desbordes del heightfield).
    if (m_pbf) {
        m_pbf->origin = cameraPos;   // posiciones relativas a la cámara (precisión float)
        if (hasPlanet) {
            const glm::vec3 gdir = glm::vec3(glm::normalize(planetCenter - cameraPos));
            m_pbf->gravityProvider =
                [gdir](const glm::dvec3&) { return gdir * 9.8f; };
            const glm::dvec3 pc = planetCenter;
            const double     R  = planetRadius;
            const auto terrainFn = terrainHeightFn;
            m_pbf->surfaceQuery =
                [pc, R, terrainFn](const glm::dvec3& wp, glm::dvec3& outS, glm::dvec3& outN) {
                    glm::dvec3 dir = wp - pc;
                    const double l = glm::length(dir);
                    if (l < 1e-9) return false;
                    dir /= l;
                    const double h = terrainFn ? terrainFn(wp) : 0.0;
                    outS = pc + dir * (R + h);
                    outN = dir;
                    return true;
                };
        } else {
            m_pbf->gravityProvider = nullptr;
            m_pbf->surfaceQuery    = nullptr;
        }

        // Cascadas: una gota nueva por intervalo, con un impulso inicial cuesta abajo.
        constexpr double kDropM = 2.5, kSpawnInterval = 0.06;
        constexpr int    kMaxParticles = 400;
        m_splashAccum += dt;
        std::vector<fluid::ShallowWaterSim::Overflow> overflow;
        m_sim->collectOverflowEdges((float)kDropM, overflow);
        if (!overflow.empty() && m_splashAccum >= kSpawnInterval && m_pbf->count() < kMaxParticles) {
            m_splashAccum = 0.0f;
            const auto& o = overflow.front();
            m_pbf->add(glm::vec3(o.worldPos - cameraPos) + glm::vec3(o.flowDir) * 0.25f);
            m_pbf->velocity.back() = glm::vec3(o.flowDir) * 1.1f;
        }

        // ⚠️ Cada `collide` de aquí llama a `surfaceQuery`, que es `sampleTerrainHeight` COMPLETO
        // (bilineal del bake + 5 octavas de ruido en double). Son n partículas × solverIters.
        { HARUKA_PROFILE("fluid.pbf.step(colision vs terreno)"); m_pbf->step(dt); }
    }

    // Acoplador: 1) río→mar (fijar costa al nivel del mar), 2) cascada→lago (absorber splash).
    if (m_hybrid) {
        HARUKA_PROFILE("fluid.hybrid.update");
        m_hybrid->seaLevelAlongUp = (float)seaLevel;
        m_hybrid->update(dt);
    }
}

void FluidHost::render(const WorldPos& cameraPos, int vpW, int vpH,
                       RHI::RenderPassHandle scenePass) {
    if (!m_anchored || !m_sim) return;
    // `scenePass` es el target de escena (`{0}` == backbuffer, válido en el modo sin post).

    // El binding 0 (PerFrameData) lo dejó puesto el llamador (application_render rebindea
    // m_uboPerFrameH antes). Dibuja el parche de ríos/lagos y luego el splash (modo superficie).
    if (m_shallowRenderer) { HARUKA_PROFILE("fluid.render.lamina(malla 96x96/frame)");
                             m_shallowRenderer->render(cameraPos); }
    if (m_fluidRenderer)   { HARUKA_PROFILE("fluid.render.particulas(screen-space)");
                             m_fluidRenderer->render(cameraPos, vpW, vpH, scenePass); }
}

} // namespace Haruka

#endif // HARUKA_MOD_FLUIDS