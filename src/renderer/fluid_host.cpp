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
#include <cstdlib>
#include "core/logger.h"
#include <algorithm>
#include <cmath>

namespace Haruka {

FluidHost::FluidHost() {
    m_sim             = std::make_unique<fluid::ShallowWaterSim>();
    m_pbf             = std::make_unique<fluid::PBFSolver>();
    m_hybrid          = std::make_unique<fluid::HybridWater>();

    m_hybrid->heightfield = m_sim.get();
    m_hybrid->particles   = m_pbf.get();

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
    // ── ACARREO AL RE-ANCLAR ────────────────────────────────────────────────────────────────────
    // Sin esto, cada re-anclaje (cada 160 m de marcha) NACE SECO: el arroyo y el charco que llevabas
    // detrás desaparecen al cruzar una frontera invisible. Se recoge el agua viva del parche que
    // muere y se vierte por posición de mundo en el nuevo, así que lo que se conserva es el AGUA, no
    // la celda. Venía de `Survival/scripts/world.cpp`, que era donde vivía la otra sim.
    std::vector<std::pair<glm::dvec3, float>> carry;
    if (m_anchored && m_sim && m_sim->valid()) {
        const int   n    = m_sim->size();
        const float area = m_sim->cellSize() * m_sim->cellSize();
        carry.reserve(64);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const float d = m_sim->waterAt(i, j);
                if (d > 0.02f) carry.emplace_back(m_sim->worldPosAt(i, j), d * area);
            }
    }

    const double hGround = hasPlanet ? terrainHeightFn(cameraPos) : 0.0;
    const glm::dvec3 anchor =
        hasPlanet ? planetCenter + up * (planetRadius + hGround) : cameraPos;
    m_sim->init(anchor, tan, bit, up, kSpan, kN, terrainHeightFn, bakedWaterFn);
    m_anchor = anchor;
    m_anchored = true;
    m_pendingRain = 0.0f;
    for (const auto& [wp, vol] : carry) m_sim->addVolumeAtWorld(wp, vol);
}

void FluidHost::update(float dt, const WorldPos& cameraPos) {
    // Sin planeta activo el parche no tiene terreno ni mar real que acoplar: no hay que dibujar
    // ninguna lámina. `anchored` queda false → render() no hace nada.
    if (!m_sim || !terrainHeightFn || !hasPlanet) return;
    ensurePatch(cameraPos);

    // ── EL ACOPLE AL MAR SE HA RETIRADO (provisional, con el mar) ───────────────────────────────
    //
    // Aquí se calculaba `seaLevel = tidal - h0` y abajo se llamaba a `applySeaLevel`, que RELLENA de
    // agua hasta el nivel del mar toda celda cuyo terreno esté por debajo. Con el jugador en la
    // costa eso convierte el parche en una LÁMINA DE MAR de 420×420 m con celdas de 4,4 m, pegada a
    // la cámara: el "agua que solo sale en ese cuadrado".
    //
    // MEDIDO en `test3.rdc` / `test3.2.rdc`: draw con stride 32 (pos+normal+alpha+espuma), UBO 6,
    // blend alfa y culling desactivado — el `ShallowWaterRenderer`, 441 y 258 quads. No era el
    // clipmap ni el anillo cercano.
    //
    // Sin el acople, `m_seaLevelAlongUp` se queda en su centinela (−1e9 = "sin océano") y la lámina
    // dibuja SOLO el agua que de verdad hay: la que llega por lluvia y caudal. Los ríos y los lagos
    // siguen funcionando; lo que desaparece es el mar que el fluido inventaba.
    //
    // Se restaura junto con el mar nuevo, y entonces con una condición que hoy no existe: que el
    // nivel al que se pinza sea EL MISMO dato que decida dónde hay agua en el render, no un cálculo
    // paralelo en el host del fluido.

    // Lluvia acumulada: se vierte en saltos de ≥2 cm para no spamear addRain cada frame.
    m_pendingRain += rainPerSec * dt;
    if (m_pendingRain >= 0.02f) { m_sim->addRain(m_pendingRain); m_pendingRain = 0.0f; }

    // Ríos/lagos: avanza la altura (modelo de tubos, sub-step CFL) y pinza la costa al mar.
    { HARUKA_PROFILE("fluid.sim.step(shallow water)");
      m_sim->step(dt); }

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

    // ── PUBLICAR LA SUPERFICIE DEL AGUA INTERIOR ────────────────────────────────────────────────
    //
    // Ríos y lagos ya no tienen malla ni shader propios: los dibuja el MAR, con su misma superficie
    // y su mismo `ocean.frag`. Lo único que sale de aquí es el CAMPO — la cota del agua por celda —
    // y el marco del parche para localizarla. Una sola pregunta ("¿nivel del agua menos cota del
    // suelo?") con tres orígenes, en vez de tres aguas con tres respuestas distintas.
    //
    // Las cotas van ABSOLUTAS (m sobre el nivel del mar): dentro de la sim todo es relativo al suelo
    // del ancla (`h0`), pero el shader razona en la cota del planeta, así que la conversión se hace
    // aquí una vez y no en cada píxel.
    if (publishInlandWater && m_sim && m_sim->valid()) {
        const int n = m_sim->size();
        m_inlandSurface.assign((size_t)n * n, -1e9f);          // centinela: sin agua
        const double h0abs = terrainHeightFn(m_sim->anchor()); // cota del suelo bajo el ancla
        int wet = 0;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                if (!m_sim->hasWaterAt(i, j)) continue;        // seca: se queda el centinela
                m_inlandSurface[(size_t)j * n + i] = (float)(h0abs + m_sim->surfaceAt(i, j));
                ++wet;
            }
        const float span = (float)(m_sim->cellSize() * (n - 1));
        publishInlandWater(m_inlandSurface, n,
                           glm::vec3(m_sim->anchor() - cameraPos),
                           glm::vec3(m_sim->tangent()), glm::vec3(m_sim->bitangent()),
                           glm::vec3(m_sim->up()), span);
        static bool s_logged = false;
        if (!s_logged && wet > 0) {
            s_logged = true;
            HARUKA_LOGI("Fluid", "agua interior publicada al mar: %d celdas con agua de %d "
                                 "(parche %.0f m, celda %.1f m) — la dibuja `ocean.frag`, sin malla propia",
                        wet, n * n, (double)span, (double)m_sim->cellSize());
        }
    }

    // Acoplador: 2) cascada→lago (absorber splash). El paso 1 (río→mar: fijar la costa al nivel del
    // mar) queda DESACTIVADO con el resto del mar — `seaLevelAlongUp` se deja en su centinela, que es
    // lo que `HybridWater::update` interpreta como "sin frontera marina" (ver el comentario de arriba).
    if (m_hybrid) {
        HARUKA_PROFILE("fluid.hybrid.update");
        m_hybrid->seaLevelAlongUp = -1e9f;
        m_hybrid->update(dt);
    }
}

void FluidHost::render(const WorldPos& cameraPos, int vpW, int vpH,
                       RHI::RenderPassHandle scenePass) {
    // ── LA LÁMINA DE AGUA DEL FLUIDO SE HA BORRADO ──────────────────────────────────────────────
    //
    // Aquí se dibujaban DOS superficies de agua y las dos estaban mal hechas:
    //
    //   · `ShallowWaterRenderer` — una malla de 96×96 con celdas de 4,4 m pegada a la cámara. Su
    //     borde estaba cuantizado a esa rejilla, así que la orilla salía en escalones de 4 metros y
    //     el parche entero se leía como un cuadrado que te seguía. Además rellenaba hasta el nivel
    //     del mar toda celda por debajo de él (`applySeaLevel`), o sea que en la costa INVENTABA un
    //     mar propio encima del que ya había.
    //   · `FluidRenderer` en modo superficie — las partículas PBF compuestas en espacio de pantalla,
    //     que a ojo eran indistinguibles de la anterior y por eso apagar una no quitaba el agua.
    //
    // Ninguna de las dos describe la misma superficie que el mar (`planet/ocean.*`), ni comparte su
    // sombreado, ni su criterio de dónde hay agua. Eran una tercera y una cuarta agua compitiendo
    // con las otras dos. Se van enteras en vez de esconderse tras una variable de entorno: mientras
    // el código exista, vuelve.
    //
    // LA SIMULACIÓN NO SE TOCA. `ShallowWaterSim` y `PBFSolver` siguen corriendo en `update()`:
    // ríos, lagos, lluvia y splash existen como estado. Lo que ya no existe es su dibujado.
    //
    // Cuando ríos y lagos vuelvan a verse, la condición es que lo hagan con la MISMA superficie y el
    // MISMO `ocean.frag` que el mar, alimentados por un único campo de "dónde hay agua". Tres aguas
    // con tres shaders es exactamente lo que ha costado esta sesión.
    (void)cameraPos; (void)vpW; (void)vpH; (void)scenePass;
}

} // namespace Haruka

#endif // HARUKA_MOD_FLUIDS