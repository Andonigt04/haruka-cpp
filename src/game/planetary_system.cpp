/**
 * @file planetary_system.cpp
 * @brief `PlanetarySystem`: los planetas, sus órbitas y todo lo que se le puede
 *        preguntar a una superficie.
 *
 * Este sistema es la **autoridad sobre el terreno**. Cuando la física quiere
 * saber a qué altura está el suelo, cuando el jugador cava, cuando el agua
 * necesita su nivel o cuando el clima pide su muestra, la respuesta sale de aquí
 * y no de la malla que se está dibujando. Esa es la razón de que existan
 * `sampleTerrainHeight()`, `sampleSurface()`, `sampleWaterLevel()`,
 * `getSeaSurface()`, `weatherAt()` y `groundCoverAt()`: son el contrato de
 * @ref Haruka::Physics::IWorldProvider visto desde el lado del mundo.
 *
 * Los bloques del fichero:
 *
 * - **Órbitas** — `updateOrbits()`, `setPlanetOrbit()`, `validateOrbits()`.
 * - **Escena** — `buildFromScene()` / `syncFromScene()` / `updatePlanetFromScene()`:
 *   el planeta se construye a sí mismo desde su configuración.
 * - **Muestreo** — las funciones de arriba, y `groundHeightKmAtDir()` para la
 *   consulta por dirección.
 * - **Edición** — `editTerrain()` y `levelTerrain()`, que escriben en el mapa de
 *   alturas del autor (`editHeightsInRadius`, indexado por `dirToHeightKey`).
 * - **Planetas simples** — los cuerpos lejanos que solo necesitan una esfera.
 *
 * Las cifras de LOD no se escriben aquí: vienen de `core/planet/terrain_lod.h`,
 * que es el único sitio donde viven y el gemelo declarado de los shaders.
 * El oleaje también tiene gemelo (`core/planet/ocean_wave.h`), para que la física
 * y el render vean literalmente la misma ola.
 */
#include "planetary_system.h"
#include "core/terrain/cube_sphere.h"
#include "core/planet/terrain_grid.h"
#include "core/planet/terrain_lod.h"   // terrainTriM: el piso del campo cercano, no un literal
#include "core/planet/geology.h"       // GeologyConfig (addSimplePlanet/rebuildSimplePlanet)
#include "core/planet/ocean_wave.h"   // gemelo CPU del oleaje: la física ve la MISMA ola
#include "renderer/primitive_shapes.h"
#include "core/components/mesh_renderer_component.h"
#include "core/planet/prop_layer.h"           // PropLayerTable (propLayers del surfaceConfig)
#include "tools/profiler.h"   // HARUKA_PROFILE (sub-scopes de planetary.update: lod.recompute / lod.stream)

#include <chrono>
#include <algorithm>
#include <functional>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <random>
#include <filesystem>
#include <system_error>
#include "core/logger.h"
#include <utility>
#if defined(_WIN32)
  #include <windows.h>
#else
  #include <unistd.h>
#endif
#include <unordered_map>
#include <unordered_set>

namespace Haruka {

// ── PlanetarySystem: el orquestador fino ──────────────────────────────────
// Cada planeta (TerrestrialPlanet) se crea a sí mismo desde su config: interpreta su biomePalette,
// sus materiales, sus mapas de zona/elevación, calcula su nivel del mar, construye su malla y
// sube sus texturas. Lo que queda aquí es lo que es del SISTEMA y no de un planeta suelto: la
// lista de cuerpos con sus órbitas (cadena de padres), el clima del mundo, la superficie de
// referencia de la física, las alturas editadas por el jugador, y la selección/delegación por
// nombre hacia los planetas.

PlanetarySystem::PlanetarySystem() {}
PlanetarySystem::~PlanetarySystem() {
    Haruka::Planet::TerrestrialPlanet::cleanupStatics();
}

void PlanetarySystem::init() {}

// Paso de la retícula de deformaciones. Era `ReferenceSurface::latticeStep`; se queda aquí porque es
// lo ÚNICO de esa clase que hacía algo — el resto calculaba cero.
static double terrainLatticeStep(int refLod, int chunkSize) {
    return 2.0 / (double(1u << refLod) * (double)chunkSize);
}

// Retícula de las deformaciones: los mismos valores que usaban `editTerrain` y la difunta
// `ensureReferenceSurface`, ahora en un solo sitio.
static constexpr int kEditRefLod    = 4;
static constexpr int kEditChunkSize = 24;

uint64_t PlanetarySystem::dirToHeightKey(const glm::dvec3& dir, int refLod, int chunkSize) {
    PlanetFace f;
    double lx, ly;
    dirToCubeFace(dir, f, lx, ly);
    const double step = terrainLatticeStep(refLod, chunkSize);
    const int64_t k = (int64_t)std::floor((lx + 1.0) / step);
    const int64_t l = (int64_t)std::floor((ly + 1.0) / step);
    return ((uint64_t)((int)f & 0x7))
         | ((uint64_t)((uint64_t)k & 0x1FFFFFFF) << 3)
         | ((uint64_t)((uint64_t)l & 0x1FFFFFFF) << 32);
}

void PlanetarySystem::editHeightsInRadius(std::unordered_map<uint64_t, float>& edits,
                                           const glm::dvec3& center, double radius,
                                           const std::vector<Planet>& planets,
                                           int refLod, int chunkSize,
                                           std::function<float(float)> modifyFn)
{
    if (planets.empty()) return;
    // Find nearest planet
    const Planet* planet = nullptr;
    double bestDist = 1e300;
    for (const auto& p : planets) {
        double d = glm::length(p.position - center);
        if (d < bestDist) { bestDist = d; planet = &p; }
    }
    if (!planet) return;

    const glm::dvec3 local = center - planet->position;
    const double localLen = glm::length(local);
    if (localLen < 1e-9) return;
    const glm::dvec3 centerDir = local / localLen;

    // Compute face-local bounds for the brush
    PlanetFace cf; double clx, cly;
    dirToCubeFace(centerDir, cf, clx, cly);
    const double step = terrainLatticeStep(refLod, chunkSize);
    // Radius in face-local units
    const double radiusLocal = radius / planet->radius;
    const int brushK = std::max(1, (int)std::ceil(radiusLocal / step));
    const int64_t ck = (int64_t)std::floor((clx + 1.0) / step);
    const int64_t cl = (int64_t)std::floor((cly + 1.0) / step);

    for (int dk = -brushK; dk <= brushK; ++dk) {
        for (int dl = -brushK; dl <= brushK; ++dl) {
            const int64_t k = ck + dk, l = cl + dl;
            // Face-local position of this grid point
            const double lx2 = (double)k * step * 2.0 - 1.0 + step;
            const double ly2 = (double)l * step * 2.0 - 1.0 + step;
            if (lx2 < -1.0 || lx2 > 1.0 || ly2 < -1.0 || ly2 > 1.0) continue;
            const glm::dvec3 worldDir = cubeFaceToDir(cf, lx2, ly2);
            const glm::dvec3 worldPt = planet->position + worldDir * planet->radius;
            const double dist = glm::length(worldPt - center);
            if (dist > radius) continue;

            const uint64_t key = dirToHeightKey(worldDir, refLod, chunkSize);
            // Falloff from center (linear)
            float falloff = 1.0f - (float)(dist / radius);
            edits[key] = modifyFn((edits.count(key) ? edits[key] : 0.0f));
        }
    }
}

void PlanetarySystem::editTerrain(const glm::dvec3& worldPos, double radius, double step, bool dig) {
    const int refLod = 4;
    const int chunkSize = 24;
    const double sign = dig ? -1.0 : 1.0;
    editHeightsInRadius(m_heightEdits, worldPos, radius, m_planets, refLod, chunkSize,
                        [&](float existing) { return existing + (float)(sign * step); });
    rebuildPlanetMeshes();
}

void PlanetarySystem::levelTerrain(const glm::dvec3& worldPos, double radius, double targetHeight) {
    const int refLod = 4;
    const int chunkSize = 24;
    editHeightsInRadius(m_heightEdits, worldPos, radius, m_planets, refLod, chunkSize,
                        [&](float) { return (float)targetHeight; });
    rebuildPlanetMeshes();
}

// ⚠️ LA DEFORMACIÓN DEL TERRENO NO LLEGA A LA SUPERFICIE. Aviso una vez, porque fallar en silencio
// es lo peor: el jugador cava, no pasa nada, y no hay forma de saber por qué.
//
// La cadena está rota en el paso 3:
//   1. `editTerrain` guarda el desnivel en `m_heightEdits`.                        ✅
//   2. `rebuildWithEdits` reconstruye la malla con `base + edits`.                  ✅
//   3. Pero NO rehornea el bake (`bakeHeightMap` solo lo llaman `build`/`rebuild`). ❌
//   4. Y el bake es quien manda: los dos tess eval leen `baseH` de `uHeightTex`, y
//      `sampleHeight` (la física) lee `m_heightCPU`. Las alturas de los VÉRTICES solo
//      se usan de respaldo cuando NO hay bake.
//
// Es deuda de la fase 2b: el bake pasó a ser la fuente de verdad y este camino se quedó atrás.
//
// Y no se arregla rehorneando: el bake cuesta ~2 s y se cachea por hash en disco — imposible por
// palada. Lo que hace falta es una CAPA DE DEFORMACIÓN aparte (textura dispersa de deltas) que
// muestreen los DOS lados encima del bake, con su gemelo GLSL, igual que el resto del §5.
static void warnDeformationInert() {
    static bool warned = false;
    if (warned) return;
    warned = true;
    HARUKA_LOGW("Terrain",
        "la deformacion del terreno NO tiene efecto: las ediciones van a la malla, pero la altura la "
        "manda el bake (uHeightTex / m_heightCPU) y ese no se rehornea. Hace falta una capa de "
        "deformacion muestreada por render y fisica, no un rebuild de la malla.");
}

void PlanetarySystem::rebuildPlanetMeshes() {
    warnDeformationInert();
    const int refLod = 4;
    const int chunkSize = 24;
    // Cada planeta reconstruye SU malla sobre la base con la que la construyó (la guarda en
    // `m_baseHeightFn`), sumándole las ediciones del jugador. No hay que re-derivar geología ni
    // mapas aquí: eso es del planeta, y este método solo le pasa el desfase.
    for (auto& p : m_simplePlanets) {
        auto editFn = [&](const glm::dvec3& dir) -> float {
            const uint64_t key = dirToHeightKey(dir, refLod, chunkSize);
            auto it = m_heightEdits.find(key);
            return it != m_heightEdits.end() ? it->second : 0.0f;
        };
        p->rebuildWithEdits(editFn);
    }
}

void PlanetarySystem::update(double dt, const glm::dvec3& cameraPos) {
    m_simulationTime += dt;

    // ⚠️ `HARUKA_SIM_TIME=<segundos>` CONGELA EL RELOJ DE SIMULACION. No es para jugar: es la segunda
    // mitad de lo que hace comparables dos capturas. `HARUKA_DAY_ANGLE` fija el Sol, pero el clima,
    // las nubes y la marea van con ESTE reloj, que se acumula con el `dt` — asi que dos backends con
    // ritmos distintos llegan al mismo segundo de reloj de pared con distinto tiempo simulado.
    //
    // Sin esto, el mismo careo GL<->Vulkan daba **0,40 % a hora 1,6 y 49 % a hora 1,5**: dos angulos
    // casi iguales con resultados opuestos. Esa contradiccion no era de los backends, era de aqui.
    { static const double s_fixed = [] {
          const char* e = std::getenv("HARUKA_SIM_TIME");
          return e ? std::atof(e) : -1.0; }();
      if (s_fixed >= 0.0) m_simulationTime = s_fixed; }

    // ⚠️ UN SOLO dt PARA TODO EL FRAME. El terreno lo necesita para repartir su presupuesto de
    // streaming por SEGUNDO (ver `TerrainNodeRenderer::prepare`), y tiene que ser exactamente el
    // mismo número que mueve las órbitas, el clima y la física — no un reloj propio del renderer.
    if (auto* tpm = activeTerrestrialMut()) tpm->setFrameDelta(dt);

    // CLIMA: la seed del planeta activo siembra los frentes (idempotente) y el reloj del MUNDO los
    // mueve. Va con `m_simulationTime` y no con un steady_clock a propósito: es el mismo número que
    // usan las órbitas, y es el que el servidor puede replicar para ver la misma tormenta.
    {
        uint32_t pseed = 42;
        for (const auto& p : m_planets)
            if (p.isHome) { pseed = p.seed; break; }
        m_weather.configure(pseed);
        m_weather.setTime(m_simulationTime);
    }

    // 1. Mover los planetas en sus órbitas
    { HARUKA_PROFILE("lod.orbits"); updateOrbits(dt); }
    { HARUKA_PROFILE("simple.orbits"); updateSimpleOrbits(dt); }

    // 2. EL ESTADO DEL MAR del frame. Va DESPUÉS de las órbitas porque la marea depende de dónde
    //    están la luna y el sol AHORA. Se calcula una vez, se publica a la GPU y lo lee la física:
    //    un solo cálculo, dos consumidores. Ver `OceanState`.
    { HARUKA_PROFILE("ocean.state"); updateOceanState(cameraPos); }
}

void PlanetarySystem::updateOceanState(const glm::dvec3& cameraPos) {
    glm::dvec3 pc; double pr = 0.0;
    if (!getActivePlanet(pc, pr) || pr <= 0.0) { m_oceanState = Haruka::Planet::oceanDefaultState(); return; }
    const glm::dvec3 rel = cameraPos - pc;
    const double len = glm::length(rel);
    const glm::dvec3 dir = (len > 1e-9) ? rel / len : glm::dvec3(0, 1, 0);

    // ── LA MAREA ────────────────────────────────────────────────────────────────────────────────
    // ⚠️ SE EVALÚA EN UN SOLO PUNTO (bajo la cámara) y se usa como ESCALAR para todo el mar visible.
    // No es pereza: el término P₂ varía a escala del PLANETA. Sobre el alcance del clipmap (16 km)
    // el coseno cambia 16/6371 = 0,0025, y como `d(3c²−1)/dc ≤ 6`, la marea se mueve ~1,5 % de su
    // amplitud de un extremo al otro de lo que se ve — centímetros. Modelarla por vértice costaría
    // los cuerpos en un UBO y una evaluación por vértice para eso.
    double tideM = 0.0;
    if (const auto* tp = activeTerrestrial()) {
        const auto& bodies = tp->tidalBodies();
        if (!bodies.empty()) {
            std::vector<glm::dvec3> pos; std::vector<double> gm;
            pos.reserve(bodies.size()); gm.reserve(bodies.size());
            for (const auto& b : bodies) { pos.push_back(b.posCenter); gm.push_back(b.gm); }
            tideM = Haruka::Planet::oceanTideAt(dir, pr, pos.data(), gm.data(), (int)bodies.size());
        }
    }

    // ── EL VIENTO QUE LEVANTA EL MAR ────────────────────────────────────────────────────────────
    // El mismo campo de clima que mueve las nubes y la lluvia. Antes el oleaje era una tabla fija:
    // una galerna y un día en calma daban olas idénticas.
    const Haruka::WeatherSample ws = weatherAt(cameraPos);
    // El viento del clima viene en el marco local del planeta; sus dos primeras componentes sirven
    // de rumbo en el plano tangente, que es lo que `oceanStateFromWind` espera.
    const float speed = glm::length(ws.wind);
    m_oceanState = Haruka::Planet::oceanStateFromWind(speed, ws.wind.x, ws.wind.z, (float)tideM);

    // Publicarlo a quien lo DIBUJA. La física lo lee de `oceanState()`, no de otra derivación.
    if (auto* tpm = activeTerrestrialMut()) tpm->setOceanState(m_oceanState);
}

void PlanetarySystem::updateOrbits(double /*dt*/) {
    // ÓRBITA KEPLER ANALÍTICA: la posición es función del TIEMPO ABSOLUTO (m_simulationTime), no una
    // integración incremental → SIN deriva ni inestabilidad (la órbita cierra exacta cada periodo).
    // Resolvemos la CADENA DE PADRES RECURSIVAMENTE (memoizada) → el orden en m_planets da igual y
    // funciona multinivel: Luna orbita Tierra que orbita Sol, sin lag. `done` marca lo ya resuelto
    // este frame (y corta ciclos: un padre en su propio linaje usa la posición actual).
    const size_t n = m_planets.size();
    std::vector<char> done(n, 0);
    std::function<glm::dvec3(size_t)> resolve = [&](size_t i) -> glm::dvec3 {
        Planet& p = m_planets[i];
        if (done[i]) return glm::dvec3(p.position);
        done[i] = 1;
        if (p.orbitParent >= 0 && (size_t)p.orbitParent < n && p.orbitPeriod > 0.0 && (size_t)p.orbitParent != i) {
            const glm::dvec3 focus = resolve((size_t)p.orbitParent);
            // La posición sale de los ELEMENTOS evaluados en `t`, no de una base congelada: la elipse
            // precesa dentro de su plano y el plano gira, así que la órbita nunca se repite. Sigue
            // siendo función de `t` (O(1), sin deriva, idéntica en todos los clientes del DGS).
            p.position = focus + Haruka::Planet::orbitPositionAt(p.orbit, m_simulationTime);
        }
        return glm::dvec3(p.position);
    };
    for (size_t i = 0; i < n; ++i) resolve(i);
}

bool PlanetarySystem::setPlanetOrbit(const std::string& planetName, const std::string& parentName,
                                     double period, double ecc) {
    int pi = -1, par = -1;
    for (size_t k = 0; k < m_planets.size(); ++k) {
        if (m_planets[k].name == planetName) pi  = (int)k;
        if (m_planets[k].name == parentName) par = (int)k;
    }
    if (pi < 0) return false;
    Planet& pl = m_planets[(size_t)pi];
    if (parentName.empty() || period <= 0.0 || par < 0 || par == pi) {
        pl.orbitParent = -1; pl.orbitPeriod = 0.0; return true;
    }
    const glm::dvec3 focus = m_planets[(size_t)par].position;
    glm::dvec3 rel = glm::dvec3(pl.position) - focus;
    double dist = glm::length(rel);
    if (dist < 1e-6) return false;
    const double e = glm::clamp(ecc, 0.0, 0.9);
    // El cuerpo está AHORA en su periastro: la base se ancla a su posición actual y de ahí salen los
    // elementos. `orbitElementsFromBasis` es la inversa exacta de la base, así que esto no cambia la
    // órbita inicial — solo la deja expresada en elementos, que es lo que permite que precese.
    const glm::dvec3 u = rel / dist;
    const glm::dvec3 axis = (std::abs(u.y) < 0.95) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 v = glm::normalize(glm::cross(axis, u));
    pl.orbitParent = par;
    pl.orbit = Haruka::Planet::OrbitElements{};
    pl.orbit.a = dist / std::max(1.0 - e, 1e-3);
    pl.orbit.e = e;
    pl.orbit.period = period;
    pl.orbit.meanAnom0 = -2.0 * 3.14159265358979323846 * (m_simulationTime / period);
    Haruka::Planet::orbitElementsFromBasis(u, v, pl.orbit.incRad, pl.orbit.nodeRad, pl.orbit.argPRad);
    Haruka::Planet::defaultPrecession(pl.seed, pl.orbit);
    pl.orbitA = pl.orbit.a; pl.orbitEcc = pl.orbit.e;
    pl.orbitPeriod = pl.orbit.period; pl.orbitPhase = pl.orbit.meanAnom0;
    return true;
}

// Malla de Luna: esfera base (unidad) + desplazamiento de cráteres deterministas
// (cuenca + borde elevado) y ruido fino; normales recalculadas de la malla
// deformada para que los cráteres tengan sombreado real. El SceneObject la escala.
static void buildCrateredMoonMesh(std::vector<glm::vec3>& verts,
                                  std::vector<glm::vec3>& norms,
                                  std::vector<glm::vec3>& cols,
                                  std::vector<unsigned int>& idx) {
    std::vector<glm::vec3> base, bn;
    PrimitiveShapes::createSphereLOD(1.0f, 72, 54, base, bn, idx);

    struct Crater { glm::vec3 dir; float ang; float depth; };
    std::vector<Crater> craters;
    uint32_t s = 0xC0FFEEu;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return float(s >> 8) * (1.0f / 16777216.0f); };
    for (int i = 0; i < 48; ++i) {
        glm::vec3 d = glm::normalize(glm::vec3(rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1));
        float ang   = glm::mix(0.04f, 0.30f, rnd() * rnd());
        float depth = ang * glm::mix(0.25f, 0.55f, rnd());
        craters.push_back({ d, ang, depth });
    }

    verts.resize(base.size());
    cols.resize(base.size());
    for (size_t i = 0; i < base.size(); ++i) {
        glm::vec3 v = glm::normalize(base[i]);
        float disp  = 0.012f * std::sin(v.x * 40.0f) * std::sin(v.y * 37.0f) * std::sin(v.z * 41.0f);
        float shade = 1.0f;
        for (const auto& c : craters) {
            float a = std::acos(glm::clamp(glm::dot(v, c.dir), -1.0f, 1.0f));
            if (a < c.ang) {
                float t    = a / c.ang;
                float bowl = -c.depth * (1.0f - t * t);
                float rim  = c.depth * 0.5f * std::exp(-((t - 0.92f) * (t - 0.92f)) / 0.004f);
                disp  += bowl + rim;
                shade *= glm::mix(0.80f, 1.0f, t);
            }
        }
        verts[i] = v * (1.0f + disp);
        float g  = glm::clamp((0.56f + 0.10f * std::sin(v.y * 19.0f)) * shade, 0.15f, 0.95f);
        cols[i]  = glm::vec3(g, g, g * 1.03f);
    }

    norms.assign(verts.size(), glm::vec3(0.0f));
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        unsigned a = idx[t], b = idx[t + 1], c = idx[t + 2];
        glm::vec3 fn = glm::cross(verts[b] - verts[a], verts[c] - verts[a]);
        norms[a] += fn; norms[b] += fn; norms[c] += fn;
    }
    for (auto& n : norms) n = (glm::length(n) > 1e-6f) ? glm::normalize(n) : glm::vec3(0, 1, 0);
}

namespace {

// Interpreta un SceneObject como un TerrestrialPlanetConfig: identidad (nombre, posición, radio,
// seed) + surface (tiling, texRes, landFraction, mapas, rango de elevación) + el surfaceConfig
// crudo (`raw`), que el planeta reinterpreta en su `build()` (biomePalette, materials, …).
// Es la MISMA interpretación para `buildFromScene` (crear) y `updatePlanetFromScene` (regenerar
// tras editarlo en el editor): un planeta se ve igual la primera vez que después de cambiarlo.
Haruka::Planet::TerrestrialPlanetConfig planetConfigFromObject(const Haruka::SceneObject& obj) {
    Haruka::Planet::TerrestrialPlanetConfig cfg;
    cfg.name     = obj.name;
    cfg.position = obj.position;
    cfg.radius   = std::max({obj.scale.x, obj.scale.y, obj.scale.z});
    cfg.seed     = obj.surfaceConfig.value("seed", 0u);
    cfg.surface.tiling       = obj.surfaceConfig.value("tiling", 100.0f);
    cfg.surface.texRes       = obj.surfaceConfig.value("texRes", 512);
    cfg.surface.macroRes     = obj.surfaceConfig.value("macroRes", 2048);
    cfg.surface.landFraction = obj.surfaceConfig.value("landFraction", 0.29f);
    cfg.surface.zoneMap      = obj.surfaceConfig.value("zoneMap", std::string());
    cfg.surface.elevationMap = obj.surfaceConfig.value("elevationMap", std::string());
    if (obj.surfaceConfig.contains("elevationRange") &&
        obj.surfaceConfig["elevationRange"].is_array() &&
        obj.surfaceConfig["elevationRange"].size() >= 2) {
        cfg.surface.elevationRange = glm::vec2(obj.surfaceConfig["elevationRange"][0],
                                               obj.surfaceConfig["elevationRange"][1]);
    }
    cfg.raw     = obj.surfaceConfig;
    cfg.faceRes = obj.surfaceConfig.value("faceRes", 256);
    return cfg;
}

} // namespace

void PlanetarySystem::buildFromScene(SceneManager& scene) {
    struct OrbitIntent { size_t planetIdx; std::string parent; double period, ecc; };
    std::vector<OrbitIntent> orbitIntents;
    for (auto& objPtr : scene.getObjectsMutable()) {
        if (!objPtr) continue;
        auto& obj = *objPtr;

        // PLANETA: if the object has a surface config, create a Planet entry
        if (obj.surfaceConfig.is_object() && !obj.surfaceConfig.empty()) {
            Planet planet;
            planet.name     = obj.name;
            planet.position = obj.position;
            planet.radius   = std::max({obj.scale.x, obj.scale.y, obj.scale.z});
            planet.isHome   = obj.flags.originShiftingTarget;
            planet.seed     = obj.surfaceConfig.value("seed", 0u);
            // Copy surface config (tiling, texRes only)
            planet.surface.tiling = obj.surfaceConfig.value("tiling", 100.0f);
            planet.surface.texRes = obj.surfaceConfig.value("texRes", 512);
            planet.surface.landFraction = obj.surfaceConfig.value("landFraction", 0.29f);
            planet.surface.zoneMap = obj.surfaceConfig.value("zoneMap", std::string());
            planet.surface.elevationMap = obj.surfaceConfig.value("elevationMap", std::string());
            if (obj.surfaceConfig.contains("elevationRange") &&
                obj.surfaceConfig["elevationRange"].is_array() &&
                obj.surfaceConfig["elevationRange"].size() >= 2) {
                planet.surface.elevationRange = glm::vec2(obj.surfaceConfig["elevationRange"][0],
                                                          obj.surfaceConfig["elevationRange"][1]);
            }

            m_planets.push_back(planet);

            // EL PLANETA SE CREA A SÍ MISMO: recibe su identidad + el surfaceConfig crudo y él solo
            // interpreta biomePalette, materials, mapas de zona/elevación, nivel del mar, malla y
            // texturas. El sistema solo le pasa la config y lo guarda.
            auto cfg = planetConfigFromObject(obj);
            auto p = std::make_unique<Haruka::Planet::TerrestrialPlanet>();
            p->build(cfg);
            m_simplePlanets.push_back(std::move(p));

            // ¿Orbita?
            std::string op = obj.getProperty<std::string>("orbitParent", std::string{});
            double per = (double)obj.getProperty<float>("orbitPeriod", 0.0f);
            if (!op.empty() && per > 0.0)
                orbitIntents.push_back({ m_planets.size() - 1, op, per,
                                         (double)obj.getProperty<float>("orbitEcc", 0.0f) });
        }

        // Procedural mesh (e.g. cratered moon)
        if (!obj.meshRenderer &&
            obj.getProperty<std::string>("proceduralMesh", std::string{}) == "cratered_moon") {
            auto mrc = std::make_shared<MeshRendererComponent>();
            std::vector<glm::vec3> v, n, c; std::vector<unsigned int> idx;
            buildCrateredMoonMesh(v, n, c, idx);
            mrc->setMesh(v, n, c, idx);
            obj.meshRenderer = mrc;
            HARUKA_LOGI("PlanetarySystem", "Procedural mesh attached to celestial body '%s'", obj.name.c_str());
        }
    }

    // Resolve orbits — la cadena de padres es del SISTEMA: el planeta guarda su órbita y el sistema
    // se la entrega ya resuelta (índice de padre + elementos orbitales).
    for (const auto& oi : orbitIntents) {
        int parentIdx = -1;
        for (size_t k = 0; k < m_planets.size(); ++k)
            if (m_planets[k].name == oi.parent) { parentIdx = (int)k; break; }
        if (parentIdx < 0 || parentIdx == (int)oi.planetIdx) continue;
        Planet& pl = m_planets[oi.planetIdx];
        const glm::dvec3 focus = m_planets[(size_t)parentIdx].position;
        glm::dvec3 rel = glm::dvec3(pl.position) - focus;
        double dist = glm::length(rel);
        if (dist < 1e-6) continue;
        const double e = glm::clamp(oi.ecc, 0.0, 0.9);
        const glm::dvec3 u = rel / dist;
        const glm::dvec3 axis = (std::abs(u.y) < 0.95) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        const glm::dvec3 v = glm::normalize(glm::cross(axis, u));
        pl.orbitParent = parentIdx;
        pl.orbit = Haruka::Planet::OrbitElements{};
        pl.orbit.a = dist / std::max(1.0 - e, 1e-3);
        pl.orbit.e = e;
        pl.orbit.period = oi.period;
        Haruka::Planet::orbitElementsFromBasis(u, v, pl.orbit.incRad, pl.orbit.nodeRad, pl.orbit.argPRad);
        Haruka::Planet::defaultPrecession(pl.seed, pl.orbit);
        pl.orbitA = pl.orbit.a; pl.orbitEcc = pl.orbit.e;
        pl.orbitPeriod = pl.orbit.period; pl.orbitPhase = pl.orbit.meanAnom0;
        HARUKA_LOGI("Orbit", "'%s' orbita '%s': a=%.3e e=%.2f T=%.1fs · precesion: apsides 1 vuelta/%.0f "
                    "orbitas, nodos 1/%.0f",
                pl.name.c_str(), oi.parent.c_str(), pl.orbit.a, e, pl.orbit.period,
                pl.orbit.apsidalCycles, std::abs(pl.orbit.nodalCycles));
        // Sync orbit al SimplePlanet (el planeta renderizable se mueve igual que el cuerpo físico).
        // Se pasan los ELEMENTOS, no la base: si se reconvirtieran a (u,v) y de vuelta, el planeta
        // renderizado precesaría con otras tasas que el cuerpo físico y los dos se separarían.
        for (auto& sp : m_simplePlanets) {
            if (sp->config().name == pl.name) {
                sp->setOrbit(pl.orbitParent, pl.orbit);
                break;
            }
        }
    }
    // El sistema recién resuelto se AUDITA: que no pueda haber colisiones no se espera, se comprueba.
    validateOrbits();

    HARUKA_LOGI("Scene", "planetas añadidos a m_planets: %zu", m_planets.size());
    for (const auto& p : m_planets)
        HARUKA_LOGI("Scene", "  - '%s' r=%.0f", p.name.c_str(), p.radius);

    // PROPS: el scatter GLOBAL (Application::refreshPropScatter → scatterPropsNear) coloca las
    // instancias por CELDA MUNDIAL alrededor del jugador, determinista y vía GPUInstancing. El
    // parche ecuatorial provisional (placePropsForPlanet → SceneObjects) quedó OBSULETO: duplicaba
    // los props en el mundo con un ancla fija y se renderizaban por la ruta normal, fuera del pase
    // instanciado. Los props de la escena solo salen del scatter global.
}

void PlanetarySystem::updatePlanetFromScene(SceneManager& scene, const std::string& name) {
    const auto obj = scene.getObject(name);
    if (!obj || !obj->surfaceConfig.is_object() || obj->surfaceConfig.empty()) {
        HARUKA_LOGW("SimplePlanet", "updatePlanetFromScene: '%s' sin surfaceConfig en la escena", name.c_str());
        return;
    }
    const auto cfg = planetConfigFromObject(*obj);

    // Refresca la entrada de `m_planets` (el orquestador lee de ahí seed/radio/órbita).
    for (auto& pl : m_planets) {
        if (pl.name != name) continue;
        pl.seed     = cfg.seed;
        pl.position = cfg.position;
        pl.radius   = cfg.radius;
        pl.surface.tiling         = cfg.surface.tiling;
        pl.surface.texRes         = cfg.surface.texRes;
        pl.surface.macroRes       = cfg.surface.macroRes;
        pl.surface.landFraction   = cfg.surface.landFraction;
        pl.surface.zoneMap        = cfg.surface.zoneMap;
        pl.surface.elevationMap   = cfg.surface.elevationMap;
        pl.surface.elevationRange = cfg.surface.elevationRange;
        break;
    }

    // El planeta se reconstruye a sí mismo con la config nueva (interpreta todo de nuevo).
    for (auto& sp : m_simplePlanets) {
        if (sp->config().name != name) continue;
        sp->build(cfg);
        HARUKA_LOGI("SimplePlanet", "'%s': regenerado desde la escena", name.c_str());
        // Los PROPS los recoloca el scatter global (refreshPropScatter), determinista por celda
        // mundial; el parche ecuatorial provisional quedó obsoleto (ver buildFromScene).
        return;
    }
    HARUKA_LOGW("SimplePlanet", "updatePlanetFromScene: '%s' no es un SimplePlanet activo", name.c_str());
}

void PlanetarySystem::setDebugView(int view) {
    for (auto& p : m_simplePlanets) p->setDebugView(view);
}

int PlanetarySystem::debugView() const {
    if (m_simplePlanets.empty()) return 0;
    return m_simplePlanets.front()->debugView();
}

std::vector<std::string> PlanetarySystem::activeTerrainLayerNames() const {
    std::vector<std::string> names;
    if (m_simplePlanets.empty()) return names;
    // TODOS los materiales, en orden: el índice es la POSICIÓN en `surface.materials`, que es
    // exactamente el valor de debug (10+i). Incluye agua/hielo, que no tienen textura pero sí zona
    // y color — aislarlos es cómo se ve dónde manda cada material.
    for (const auto& m : m_simplePlanets.front()->terrainMaterials().materials)
        names.push_back(m.name);
    return names;
}

std::vector<std::string> PlanetarySystem::activePropLayerNames() const {
    std::vector<std::string> names;
    if (m_simplePlanets.empty()) return names;
    // TODAS las capas, en orden de prioridad: el índice es la POSICIÓN en `propLayers`, que es
    // exactamente el valor de debug (40+i). Así el editor pinta el área de spawn de cada capa.
    for (const auto& L : m_simplePlanets.front()->propLayers().layers) {
        if (L.mesh.empty()) continue;   // capa informativa: no instala nada, no se lista
        names.push_back(L.name.empty() ? L.mesh : L.name);
    }
    return names;
}

Haruka::Planet::TerrestrialPlanet::RenderStats PlanetarySystem::getTerrainRenderStats() const {
    Haruka::Planet::TerrestrialPlanet::RenderStats sum;
    for (const auto& sp : m_simplePlanets) {
        if (!sp) continue;
        const auto& s = sp->lastRenderStats();
        sum.baseVertices  += s.baseVertices;  sum.baseTriangles  += s.baseTriangles;
        sum.clipVertices  += s.clipVertices;  sum.clipTriangles  += s.clipTriangles;
        sum.drawCalls     += s.drawCalls;
    }
    return sum;
}

void PlanetarySystem::syncFromScene(const SceneManager& scene) {
    HARUKA_PROFILE("lod.sync");
    for (auto& planet : m_planets) {
        auto obj = scene.getObject(planet.name);
        if (!obj) continue;
        planet.position = obj->position;
        planet.radius   = std::max({obj->scale.x, obj->scale.y, obj->scale.z});
    }
    // Sync positions to SimplePlanets too
    for (auto& sp : m_simplePlanets) {
        auto obj = scene.getObject(sp->config().name);
        if (!obj) continue;
        sp->setPosition(obj->position);
    }
}

bool PlanetarySystem::getActivePlanet(glm::dvec3& center, double& radius) const {
    if (m_planets.empty()) return false;
    for (const auto& planet : m_planets) {
        if (planet.isHome) {
            center = glm::dvec3(planet.position);
            radius = planet.radius;
            return true;
        }
    }
    const auto& planet = m_planets.front();
    center = glm::dvec3(planet.position);
    radius = planet.radius;
    return true;
}

bool PlanetarySystem::getSeaSurface(const glm::dvec3& worldPos, glm::dvec3& outCenter, double& outSeaRadius) const {
    if (m_planets.empty()) return false;
    const Planet* nearest = nullptr;
    double bestDist = 1e300;
    for (const auto& p : m_planets) {
        double d = glm::length(p.position - worldPos);
        if (d < bestDist) { bestDist = d; nearest = &p; }
    }
    if (!nearest) return false;
    outCenter    = nearest->position;
    outSeaRadius = nearest->radius;
    return true;
}

Haruka::WeatherSample PlanetarySystem::weatherAt(const glm::dvec3& worldPos) const {
    glm::dvec3 pc; double pr;
    if (!getActivePlanet(pc, pr)) return Haruka::WeatherSample{};

    glm::dvec3 dir = worldPos - pc;
    const double len = glm::length(dir);
    if (len < 1e-9) return Haruka::WeatherSample{};
    dir /= len;

    constexpr double kFieldRefreshM = 500.0;
    if (glm::length(worldPos - m_weatherFieldPos) > kFieldRefreshM) {
        const Haruka::TerrainSample ts = sampleSurface(worldPos);
        m_weatherFieldTempC = ts.tempC;
        m_weatherFieldHumid = ts.humidity;
        m_weatherFieldPos   = worldPos;
    }
    return m_weather.sampleAt(dir, m_weatherFieldTempC, m_weatherFieldHumid);
}

PlanetarySystem::GroundCover PlanetarySystem::groundCoverAt(const glm::dvec3& worldPos,
                                                           float snowAccum) const {
    GroundCover c;
    if (snowAccum > 0.05f) { c.material = Haruka::GroundMaterial::Snow; c.amount = snowAccum; return c; }

    constexpr double kFieldRefreshM = 500.0;
    if (glm::length(worldPos - m_weatherFieldPos) > kFieldRefreshM) {
        const Haruka::TerrainSample ts = sampleSurface(worldPos);
        m_weatherFieldTempC = ts.tempC;
        m_weatherFieldHumid = ts.humidity;
        m_weatherFieldPos   = worldPos;
    }
    const float arid = 1.0f - glm::smoothstep(0.08f, 0.26f, m_weatherFieldHumid);
    if (arid > 0.15f) { c.material = Haruka::GroundMaterial::Sand; c.amount = arid; }
    return c;
}

/**
 * ⚠️ ESTA FUNCIÓN ERA UN STUB, Y CON ELLA TODO EL AGUA DE LA FÍSICA.
 *
 * Buscaba el planeta más cercano y devolvía un `TerrainSample` RECIÉN CONSTRUIDO, sin tocar. Como el
 * valor por defecto de `waterType` es `None`, `sampleWaterLevel` salía por su primer `return` y
 * contestaba `kNoWater` en todo el universo. Cadena completa de lo que eso apagaba:
 *
 *     sampleSurface (stub) → waterType=None → sampleWaterLevel=kNoWater → Application::getWaterLevelAt
 *     → el juego nunca ve agua → no se nada, no se flota, no se ahoga
 *
 * Y de propina dejaba INALCANZABLE a `ocean_wave.h`, el gemelo CPU del oleaje: su única llamada está
 * tres líneas después de ese `return`. Estaba escrito, documentado y verificado, y no se ejecutaba.
 *
 * Las dos decisiones de esta función, y por qué son ésas:
 *
 *  · LA COTA sale de `sampleTerrainHeight`, no de un muestreo propio. Es el contrato del suelo de
 *    todo el motor (el mismo que pisa Jolt y que dibuja el clipmap). Un segundo muestreador aquí
 *    sería otra superficie más que mantener de acuerdo — el error que este repo ya ha pagado dos
 *    veces (el terreno, y las tres aguas del render).
 *
 *  · DÓNDE HAY MAR es `elevKm < 0`, que es LITERALMENTE la regla del shader: `ocean.frag` calcula
 *    `depth = level − baseH` y descarta el fragmento con `depth <= 0`. Si la física usara cualquier
 *    otro criterio, la orilla que se ve y la que se nada serían dos líneas distintas. El bake ya
 *    viene desplazado para que el nivel del mar sea la cota 0, así que no hay constante que elegir.
 */
Haruka::TerrainSample PlanetarySystem::sampleSurface(const glm::dvec3& worldPos) const {
    Haruka::TerrainSample s;
    glm::dvec3 pc; double pr = 0.0;
    if (!getActivePlanet(pc, pr) || pr <= 0.0) return s;
    const glm::dvec3 rel = worldPos - pc;
    const double     len = glm::length(rel);
    if (len < 1e-9) return s;
    const glm::dvec3 dir = rel / len;

    // EL SUELO, de la fuente única. (No puede recursar: el fallback de `sampleTerrainHeight` que
    // llamaba aquí se cortó — ver la nota de su `return` final.)
    s.elevKm = (float)(sampleTerrainHeight(worldPos) / 1000.0);
    s.normal = glm::vec3(dir);   // radial: la normal analítica del relieve es cosa del sampler v2

    // EL AGUA, con la regla del shader. El océano está en la cota 0; un lago NO se decide aquí — lo
    // publica la simulación de aguas someras y lo compone `HybridWater::waterLevelAlongUp`, que es
    // quien sabe de láminas locales. Meter lagos aquí sería una tercera respuesta a "¿hay agua?".
    // El nivel NO es 0 fijo: la marea sube y baja la lámina, y con ella la línea de costa. Es el
    // mismo `seaLevelM` que se subió a la GPU, así que la orilla que se nada y la que se ve son una.
    const float seaKm = m_oceanState.seaLevelM / 1000.0f;
    if (s.elevKm < seaKm) {
        s.waterType    = Haruka::WaterType::Ocean;
        s.waterLevelKm = seaKm;
    }

    // EL CLIMA, del campo horneado del planeta — el MISMO con el que se pintaron los biomas y se
    // repartieron los props. Sin esto `groundCoverAt` recibía los valores por defecto (15 °C, 0.5 de
    // humedad) y su `arid` salía 0 siempre: la capa de arena nunca se activaba en ningún punto.
    if (const auto* tp = activeTerrestrial()) {
        const Haruka::FieldSample f = tp->fieldSampleAt(glm::vec3(dir));
        s.tempC    = f.tempC;
        s.humidity = f.humidity;
    }
    return s;
}

double PlanetarySystem::sampleWaterLevel(const glm::dvec3& worldPos) const {
    const Haruka::TerrainSample s = sampleSurface(worldPos);
    if (s.waterType == Haruka::WaterType::None) return kNoWater;
    const double levelM = double(s.waterLevelKm) * 1000.0;

    // ── Y LA OLA, QUE HASTA AHORA NO EXISTÍA PARA LA FÍSICA ─────────────────────────────────────
    //
    // El render levanta olas de metro y medio con `harukaGerstner`; esto devolvía una cota plana.
    // Resultado: una barca flota atravesando la cresta, un nadador sube y baja por una ola que para
    // él no está, y el ahogamiento se decide contra un nivel que no es el que se ve. Es la misma
    // discrepancia "lo que se pisa contra lo que se dibuja" del terreno, en el agua.
    //
    // `ocean_wave.h` es el gemelo exacto del shader: mismas cifras, mismo orden. El reloj es el
    // mismo que alimenta `uDebug.y`, así que la ola de la física va EN FASE con la que se ve.
    glm::dvec3 pc; double pr = 0.0;
    if (!getActivePlanet(pc, pr) || pr <= 0.0) return levelM;
    const glm::dvec3 rel = worldPos - pc;
    const double     r   = glm::length(rel);
    if (r < 1e-9) return levelM;
    const glm::dvec3 up  = rel / r;

    // Profundidad bajo ESTE punto: el mismo criterio que el shader (nivel del agua − cota del suelo).
    // La cota sale de la muestra que YA se hizo arriba: pedirla otra vez a `sampleTerrainHeight` era
    // un segundo muestreo del terreno (el camino caliente del fluido lo llama ~1600 veces por frame) y,
    // peor, abría la puerta a que la profundidad y la decisión "aquí hay mar" salieran de dos lecturas
    // distintas del suelo.
    const double depthRest = levelM - double(s.elevKm) * 1000.0;
    if (depthRest <= 0.0) return levelM;

    // EL MISMO reloj que llena `uDebug.y` en el shader — ahora de verdad. Aquí había un `static` local
    // propio: otro origen de tiempo, o sea otra fase. Ver la nota de `oceanClockSeconds()`.
    const float t = Haruka::Planet::oceanClockSeconds();
    // La ola se evalúa en la superficie EN REPOSO, igual que en el tese.
    const glm::vec3 wp = glm::vec3(up * (pr + levelM));
    // ⚠️ MISMO ORDEN QUE `ocean.tese`: primero la lámina trepa (swash), y la ola se evalúa sobre la
    // profundidad ya trepada. Invertirlo aquí daría una cota parecida pero no la misma, y "parecida"
    // es exactamente el fallo que se persigue — la orilla que se nada dejaría de ser la que se ve.
    const double swash = (double)Haruka::Planet::oceanSwash((float)depthRest, wp, glm::vec3(up), t,
                                                            m_oceanState);
    const double level2 = levelM + swash;
    const double depth2 = level2 - double(s.elevKm) * 1000.0;
    if (depth2 <= 0.0) return level2;
    const glm::vec3 wp2 = glm::vec3(up * (pr + level2));
    return level2 + (double)Haruka::Planet::oceanWaveHeight(wp2, glm::vec3(up), t, (float)depth2,
                                                            1.0f, m_oceanState);
}

double PlanetarySystem::sampleWaterDepth(const glm::dvec3& worldPos) const {
    const Haruka::TerrainSample s = sampleSurface(worldPos);
    if (s.waterType == Haruka::WaterType::None) return 0.0;
    return std::max(0.0, (double(s.waterLevelKm) - double(s.elevKm)) * 1000.0);
}

glm::dvec3 PlanetarySystem::sampleWaterVelocity(const glm::dvec3& worldPos) const {
    const Haruka::TerrainSample s = sampleSurface(worldPos);
    if (s.waterType == Haruka::WaterType::None) return glm::dvec3(0.0);
    glm::dvec3 pc; double pr = 0.0;
    if (!getActivePlanet(pc, pr) || pr <= 0.0) return glm::dvec3(0.0);
    const glm::dvec3 rel = worldPos - pc;
    const double     r   = glm::length(rel);
    if (r < 1e-9) return glm::dvec3(0.0);
    const glm::dvec3 up = rel / r;

    const double levelM = double(s.waterLevelKm) * 1000.0;
    const double depth  = levelM - double(s.elevKm) * 1000.0;
    if (depth <= 0.0) return glm::dvec3(0.0);

    const float     t  = Haruka::Planet::oceanClockSeconds();
    const glm::vec3 wp = glm::vec3(up * (pr + levelM));
    return glm::dvec3(Haruka::Planet::oceanWaveVelocity(wp, glm::vec3(up), t, (float)depth,
                                                    1.0f, m_oceanState));
}

double PlanetarySystem::sampleTerrainHeight(const glm::dvec3& worldPos) const {
    // Sin `triM` explícito = campo cercano: el piso de `terrainTriM`, que es el lado real del quad
    // del clipmap. Un literal aquí volvería a desalinear la física del render en silencio.
    return sampleTerrainHeight(worldPos, Haruka::Planet::terrainTriM(0.0));
}

PlanetarySystem::TerrainSampler PlanetarySystem::terrainSampler(const glm::dvec3& nearWorldPos) const {
    // SimplePlanet: el suelo sale de SU retícula base + el detalle compartido con la GPU. Va antes
    // que la superficie de referencia porque ésta cuelga de `sampleTerrainV2`, que en esta rama es
    // un stub que devuelve 0 — con ella la física caminaba sobre una esfera lisa.
    // (1) El planeta ACTIVO por nombre. ⚠️ El nombre cruza DOS listas distintas (`m_planets`, que
    // son los cuerpos, y `m_simplePlanets`, que son los renderizables): si no casan exactamente, esto
    // no encuentra nada y antes se caía a la esfera lisa SIN DECIR NADA.
    const std::string& activeName = getActivePlanetName();
    for (const auto& sp : m_simplePlanets)
        if (sp && sp->config().name == activeName) return { sp.get(), sp->config().position };

    // (2) Si el nombre no casó, el suelo es el del SimplePlanet MÁS CERCANO — que es el que se está
    // pisando y el que se está dibujando. Antes se pasaba directo a `ReferenceSurface`, o sea a
    // devolver 0: la física caminaba sobre una esfera lisa mientras el render pintaba relieve. Un
    // planeta renderizable a mano es infinitamente mejor suelo que el nivel del mar.
    const Haruka::Planet::TerrestrialPlanet* best = nullptr;
    double bestD = 1e300;
    for (const auto& sp : m_simplePlanets) {
        if (!sp) continue;
        const double d = glm::length(nearWorldPos - sp->config().position) - sp->config().radius;
        if (d < bestD) { bestD = d; best = sp.get(); }
    }
    if (best) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            HARUKA_LOGW("Terrain",
                "el planeta activo ('%s') no casa con ningun SimplePlanet: la fisica usa el mas "
                "cercano ('%s'). Revisa que los nombres de m_planets y m_simplePlanets coincidan.",
                activeName.c_str(), best->config().name.c_str());
        }
        return { best, best->config().position };
    }
    return {};
}

double PlanetarySystem::sampleTerrainHeight(const glm::dvec3& worldPos, float minFeatureM) const {
    // ⚠️ LA RESOLUCIÓN DEL PLANETA ESTÁ EN `terrainSampler`, NO AQUÍ. Antes vivía en el cuerpo de esta
    // función, y esta función es EL CAMINO CALIENTE: la malla de colisión la llama 235 564 veces por
    // reconstrucción y cada llamada repetía lo mismo — dos bucles sobre los vectores de planetas y una
    // comparación de `std::string` — para obtener un puntero que no cambia entre muestras. Sacarlo a un
    // objeto reutilizable permite al llamador masivo resolverlo UNA vez (ver
    // `WorldSystemProvider::terrainHeightFieldRings`) sin que existan dos definiciones de "qué planeta
    // es el suelo": ésta delega en la misma.
    if (const TerrainSampler s = terrainSampler(worldPos)) return s.heightAt(worldPos, minFeatureM);

    const Planet* nearest = nullptr; double best = 1e300;
    for (const auto& p : m_planets) {
        const double d = glm::length(p.position - worldPos);
        if (d < best) { best = d; nearest = &p; }
    }

    if (nearest && nearest->name == getActivePlanetName()) {
        const glm::dvec3 rel = worldPos - nearest->position;
        const double len = glm::length(rel);
        if (len > 1e-9) {
                    // Sin SimplePlanet no hay superficie muestreable: la base es el nivel del mar. Antes
            // esto pasaba por `ReferenceSurface`, que devolvía exactamente lo mismo (cero) tras un
            // snapshot atómico y una caché de 1 M entradas con mutex.
            double baseM = 0.0;
            // Apply deformation edits
            const glm::dvec3 dir = rel / len;
            const uint64_t key = dirToHeightKey(dir, kEditRefLod, kEditChunkSize);
            auto it = m_heightEdits.find(key);
            if (it != m_heightEdits.end()) baseM += it->second;
            return baseM;
        }
    }
    // ⚠️ AQUÍ SE LLAMABA A `sampleSurface`, Y AHORA SERÍA RECURSIÓN INFINITA: desde que `sampleSurface`
    // saca su cota de esta misma función (que es lo correcto — una sola fuente de suelo), el par se
    // llamaría hasta desbordar la pila. No es un apaño: llegar hasta aquí significa que NO hay ningún
    // muestreador de superficie (ni SimplePlanet, ni planeta activo que case), y en ese caso
    // `sampleSurface` tampoco podía devolver otra cosa que su `elevKm` por defecto, que es 0. El
    // resultado es idéntico; lo que desaparece es el ciclo.
    return 0.0;
}

bool PlanetarySystem::groundHeightKmAtDir(const glm::dvec3& dir, float& outElevKm) const {
    const double len = glm::length(dir);
    if (!(len > 1e-9)) return false;
    const glm::dvec3 d = dir / len;

    // EL SUELO REAL: el SimplePlanet (TerrestrialPlanet) es el que pinta la malla y pisa la física
    // (`sampleTerrainHeight` lo consulta antes que la referencia). La ReferenceSurface en esta rama
    // es un stub que devuelve 0 (esfera lisa) — anclar los props ahí era la causa de que flotaran.
    // `sampleHeight` lee solo `m_heightCPU` (inmutable tras el bake) → seguro desde el hilo async.
    for (const auto& sp : m_simplePlanets) {
        if (!sp || sp->config().name != getActivePlanetName()) continue;
        outElevKm = (float)(sp->sampleHeight(d) / 1000.0);
        return true;
    }

    // Sin SimplePlanet no hay altura que dar, y **devolver false es lo correcto**. Antes se
    // preguntaba a `ReferenceSurface`, que contestaba 0 siempre: el llamante recibía `true` con
    // "estás al nivel del mar" en cualquier punto del planeta — y ancló los props ahí, que es por lo
    // que flotaban. Un "no lo sé" honesto es mejor que un cero que parece un dato.
    return false;
}

const std::string& PlanetarySystem::getActivePlanetName() const {
    static const std::string kNone;   // para el caso sin planetas: referencia válida y estable
    for (const auto& p : m_planets)
        if (p.isHome) return p.name;
    return m_planets.empty() ? kNone : m_planets.front().name;
}

const Haruka::Planet::TerrestrialPlanet* PlanetarySystem::activeTerrestrial() const {
    const std::string name = getActivePlanetName();
    for (const auto& sp : m_simplePlanets)
        if (sp && sp->config().name == name) return sp.get();
    // Sin planeta declarado en la escena: si hay un solo SimplePlanet, es el que pinta el mundo.
    return m_simplePlanets.size() == 1 ? m_simplePlanets.front().get() : nullptr;
}

bool PlanetarySystem::getActivePlanetParams(Haruka::WorldGenParams& out, double& outRadius) const {
    if (m_planets.empty()) return false;
    const Planet* home = nullptr;
    for (const auto& p : m_planets) {
        if (p.isHome) { home = &p; break; }
        if (!home) home = &p;
    }
    if (!home) return false;
    uint32_t seed = home->seed ? home->seed : static_cast<uint32_t>(std::hash<std::string>{}(home->name));
    out = Haruka::deriveWorldParams(seed, home->radius);
    outRadius = home->radius;
    return true;
}

// ── SimplePlanet API: delega en los TerrestrialPlanet ──────────────────────

void PlanetarySystem::addSimplePlanet(const SimplePlanet& orbit,
                                      const Haruka::Planet::GeologyConfig& geo,
                                      const Haruka::Planet::TerrainGridConfig& grid) {
    try {
        auto p = std::make_unique<Haruka::Planet::TerrestrialPlanet>();
        Haruka::Planet::TerrestrialPlanetConfig cfg = orbit;   // SimplePlanet ES TerrestrialPlanetConfig
        // El llamador legado aporta semilla/resolución por `geo`/`grid`; los respeta si los trae.
        if (geo.seed != 0) cfg.seed = geo.seed;
        if (grid.faceRes > 0) cfg.faceRes = grid.faceRes;
        p->build(cfg);
        m_simplePlanets.push_back(std::move(p));

        // ⚠️ ESTA VÍA CREA MEDIO PLANETA. `buildFromScene` empuja a las DOS listas a la vez
        // (`m_planets` = el cuerpo con masa y órbita, `m_simplePlanets` = lo renderizable); esta
        // solo empuja a la segunda. El resultado se DIBUJA entero —terreno, mar, props— pero no
        // existe para nada más: sin gravedad, sin física, y `getActivePlanetName()` no lo ve, así
        // que `sampleTerrainHeight` nunca lo muestrea. Es decir, un planeta al que no se puede ir
        // y sobre el que no se puede caminar. Si eso no es lo que se quería, el planeta va en la
        // escena (con `surfaceConfig`), no por aquí.
        bool hasBody = false;
        for (const auto& b : m_planets) if (b.name == orbit.name) { hasBody = true; break; }
        if (!hasBody) {
            HARUKA_LOGW("SimplePlanet",
                "'%s' se DIBUJA pero no tiene cuerpo en m_planets: sin gravedad ni fisica, y la "
                "fisica del terreno no lo muestrea. Declaralo en la escena si debe ser pisable.",
                orbit.name.c_str());
        }
    } catch (const std::exception& e) {
        HARUKA_LOGE("SimplePlanet", "addSimplePlanet('%s') EXCEPTION: %s", orbit.name.c_str(), e.what());
    } catch (...) {
        HARUKA_LOGE("SimplePlanet", "addSimplePlanet('%s') UNKNOWN EXCEPTION", orbit.name.c_str());
    }
}

void PlanetarySystem::rebuildSimplePlanet(const std::string& name,
                                           const Haruka::Planet::GeologyConfig& geo,
                                           const Haruka::Planet::TerrainGridConfig& grid) {
    for (auto& p : m_simplePlanets) {
        if (p->config().name != name) continue;
        // El planeta regenera su geología y reconstruye malla + texturas; el sistema solo le pasa
        // la nueva semilla y la resolución.
        p->rebuild(geo, grid.faceRes);
        return;
    }
}

void PlanetarySystem::generateSimpleLOD(const std::string&, int) {
    // Single-mesh planets have no LOD — mesh is built once at full resolution.
}

// Trabajo de COMPUTE de todos los planetas, para hacerlo FUERA del render pass de la escena.
// Ver `TerrestrialPlanet::prepare`: un dispatch dentro de un render pass es ilegal en Vulkan.
void PlanetarySystem::prepareSimplePlanets(const glm::dvec3& cameraPos, const glm::dvec3& viewDir,
                                           double fovYRad, double aspect, double viewportH) {
    for (auto& p : m_simplePlanets) p->prepare(cameraPos, viewDir, fovYRad, aspect, viewportH);
}

void PlanetarySystem::renderSimplePlanet(const std::string& name,
                                          const glm::dvec3& cameraPos,
                                          const glm::mat4& proj,
                                          const glm::mat4& view) {
    for (auto& p : m_simplePlanets) {
        if (p->config().name != name) continue;
        p->render(cameraPos, proj, view);
        return;
    }
}

const PlanetarySystem::SimplePlanet& PlanetarySystem::getSimplePlanet(size_t i) const {
    return m_simplePlanets[i]->config();
}

void PlanetarySystem::setSunLight(const glm::vec3& dir, const glm::vec3& color,
                                  const glm::vec3& ambientColor) {
    for (auto& p : m_simplePlanets) p->setSunLight(dir, color, ambientColor);
}

void PlanetarySystem::setSkyAmbientSH(const glm::vec3 (&coef)[9]) {
    for (auto& p : m_simplePlanets) p->setSkyAmbientSH(coef);
}

void PlanetarySystem::setGroundWet(float wet, float snow, Haruka::RHI::TextureHandle skyMask,
                                   const glm::mat4& skySpace) {
    for (auto& p : m_simplePlanets) p->setGroundWet(wet, snow, skyMask, skySpace);
}

int PlanetarySystem::validateOrbits() const {
    // ── LA AUDITORÍA DEL SISTEMA ────────────────────────────────────────────────────────────────
    //
    // "Que no choquen" no es una esperanza que se comprueba jugando: con elementos precesantes el
    // radio de cada cuerpo vive SIEMPRE en [a(1−eMax), a(1+eMax)], un intervalo fijo y conocido antes
    // de arrancar. Así que se puede DEMOSTRAR aquí, una vez, para todo t. Con integración N-cuerpos
    // ese intervalo no existe y esta función no se podría escribir.
    //
    // Se auditan dos cosas distintas y hacen falta las dos: que las órbitas no se cruzen (geometría)
    // y que estén lo bastante separadas para no perturbarse hasta cruzarse (dinámica).
    int problems = 0;
    // Se agrupa por PADRE: comparar la órbita de una luna con la de un planeta de otro sol no dice
    // nada (los semiejes están medidos respecto a focos distintos).
    for (size_t parent = 0; parent < m_planets.size(); ++parent) {
        std::vector<size_t> kids;
        for (size_t i = 0; i < m_planets.size(); ++i)
            if (m_planets[i].orbitParent == (int)parent && m_planets[i].orbit.period > 0.0)
                kids.push_back(i);
        if (kids.size() < 2) continue;
        // Ordenar por semieje: solo hay que comparar VECINOS. Si el vecino inmediato está separado,
        // los de más allá también (los intervalos son disjuntos y crecientes).
        std::sort(kids.begin(), kids.end(), [&](size_t a, size_t b) {
            return m_planets[a].orbit.a < m_planets[b].orbit.a;
        });
        const double centralMass = Haruka::Planet::bodyMassFromRadius(m_planets[parent].radius);
        for (size_t k = 0; k + 1 < kids.size(); ++k) {
            const Planet& in  = m_planets[kids[k]];
            const Planet& out = m_planets[kids[k + 1]];
            if (!Haruka::Planet::orbitsSeparated(in.orbit, out.orbit)) {
                HARUKA_LOGW("Orbit", "'%s' y '%s' PUEDEN CRUZARSE: apoapsis max %.4e >= periapsis min "
                            "%.4e (con e maxima %.3f y %.3f). Sube el semieje de '%s' o baja su "
                            "excentricidad.",
                            in.name.c_str(), out.name.c_str(), in.orbit.apoapsisMax(),
                            out.orbit.periapsisMin(), in.orbit.eMax(), out.orbit.eMax(),
                            out.name.c_str());
                ++problems;
                continue;   // sin separación geométrica, el criterio de Hill no aporta nada
            }
            const double delta = Haruka::Planet::mutualHillSeparation(
                in.orbit.a,  Haruka::Planet::bodyMassFromRadius(in.radius),
                out.orbit.a, Haruka::Planet::bodyMassFromRadius(out.radius), centralMass);
            // Δ > 10 sobrevive escalas de gigaaños; por debajo de 3.5 los pares se cruzan. Entre
            // ambos hay una zona gris: se avisa, no se rechaza, porque la precesión de este sistema
            // es una función acotada y no puede llevarlos a cruzarse (eso ya lo garantiza el test de
            // arriba). El aviso es para el AUTOR del sistema: un Δ bajo se ve raro, muy apretado.
            if (delta < 10.0) {
                HARUKA_LOGW("Orbit", "'%s' y '%s' estan a %.1f radios de Hill mutuos (se recomienda "
                            ">10). No pueden chocar, pero el par queda muy apretado.",
                            in.name.c_str(), out.name.c_str(), delta);
                ++problems;
            }
        }
    }
    if (problems == 0 && m_planets.size() > 1)
        HARUKA_LOGI("Orbit", "sistema auditado: ninguna pareja puede cruzarse para ningun t");
    return problems;
}

void PlanetarySystem::updateSimpleOrbits(double /*dt*/) {
    const size_t n = m_simplePlanets.size();
    if (n == 0) return;
    // Actualiza el reloj del clima de cada planeta (misma fuente que el del mundo).
    for (auto& p : m_simplePlanets)
        p->setWeatherTime(m_simulationTime);
    // Misma resolución de cadena de padres que `updateOrbits`, sobre la config de cada planeta.
    std::vector<char> done(n, 0);
    std::function<glm::dvec3(size_t)> resolve = [&](size_t i) -> glm::dvec3 {
        auto& p = *m_simplePlanets[i];
        const auto& cfg = p.config();
        if (done[i]) return p.position();
        done[i] = 1;
        if (cfg.orbitParent >= 0 && (size_t)cfg.orbitParent < n && cfg.orbitPeriod > 0.0) {
            const glm::dvec3 focus = resolve((size_t)cfg.orbitParent);
            // MISMA función que `updateOrbits`. Antes eran dos copias del resolvedor de Kepler, y con
            // la precesión serían dos copias que además tienen que precesar igual: el planeta que se
            // dibuja y el cuerpo que orbita se habrían separado en cuanto una de las dos cambiara.
            p.setPosition(focus + Haruka::Planet::orbitPositionAt(cfg.orbit, m_simulationTime));
        }
        return p.position();
    };
    for (size_t i = 0; i < n; ++i) resolve(i);

    // MAR DINÁMICO POR MASA (Hito 1): instantánea de los cuerpos masivos del sistema orbital
    // (m_planets: soles/planetas/lunas) que atraen y mueven el océano de cada planeta de agua.
    // Planet no expone masa: proxy por densidad uniforme a partir del radio ⇒ GM ≈ G·ρ·(4/3)π·r³.
    // Sin m_planets no hay cuerpos → setTidalBodies({}) deja el mar neutro (mismo que antes).
    if (!m_planets.empty() || m_simplePlanets.size() > 1) {
        using MB = Haruka::Planet::TerrestrialPlanet::MassiveBody;
        const double den = 3000.0;              // kg/m³ proxy planetario
        // Candidatos masivos = cuerpos orbitales (m_planets: soles) + el resto de planetas/lunas
        // de agua (m_simplePlanets). Cada uno se convierte a GM por proxy de radio y densidad.
        std::vector<MB> cand;
        std::vector<glm::dvec3> candPos;
        cand.reserve(m_planets.size() + m_simplePlanets.size());
        for (const auto& b : m_planets) {
            const double gm = G * den * (4.0 / 3.0) * 3.14159265358979323846
                              * b.radius * b.radius * b.radius;
            cand.push_back({ b.position, gm });
        }
        for (auto& b : m_simplePlanets) {
            cand.push_back({ b->position(), G * den * (4.0 / 3.0) * 3.14159265358979323846
                                              * b->radius() * b->radius() * b->radius() });
        }
        for (auto& p : m_simplePlanets) {
            const glm::dvec3 self = p->position();
            std::vector<MB> rel;
            rel.reserve(cand.size());
            for (const auto& b : cand) rel.push_back({ b.posCenter - self, b.gm });
            // El propio planeta se EXCLUYE (su campo es uniforme en su superficie → sin marea) y
            // nos quedamos con las 8 atracciones más influyentes (las más cercanas).
            std::sort(rel.begin(), rel.end(), [](const MB& a, const MB& b) {
                return glm::length(a.posCenter) < glm::length(b.posCenter);
            });
            std::vector<MB> nearest;
            nearest.reserve(8);
            for (const auto& b : rel) {
                if (glm::length(b.posCenter) > p->radius()) nearest.push_back(b);
                if ((int)nearest.size() >= 8) break;
            }
            p->setTidalBodies(std::move(nearest));
        }
    }
}

} // namespace Haruka
