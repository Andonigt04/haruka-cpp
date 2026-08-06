#include "planetary_system.h"
#include "core/terrain/cube_sphere.h"
#include "core/terrain/reference_surface.h"
#include "core/planet/terrain_grid.h"
#include "core/planet/geology.h"       // GeologyConfig (addSimplePlanet/rebuildSimplePlanet)
#include "renderer/primitive_shapes.h"
#include "core/components/mesh_renderer_component.h"
#include "core/planet/prop_layer.h"           // PropLayerTable (propLayers del surfaceConfig)
#include "tools/profiler.h"   // HARUKA_PROFILE (sub-scopes de planetary.update: lod.recompute / lod.stream)

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

uint64_t PlanetarySystem::dirToHeightKey(const glm::dvec3& dir, int refLod, int chunkSize) {
    PlanetFace f;
    double lx, ly;
    dirToCubeFace(dir, f, lx, ly);
    const double step = ReferenceSurface::latticeStep(refLod, chunkSize);
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
    const double step = ReferenceSurface::latticeStep(refLod, chunkSize);
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

void PlanetarySystem::rebuildPlanetMeshes() {
    m_refSurface.clear();
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

    ensureReferenceSurface();
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
            const double e = glm::clamp(p.orbitEcc, 0.0, 0.99);
            const double M = p.orbitPhase + 2.0 * 3.14159265358979323846 * (m_simulationTime / p.orbitPeriod);
            double E = M;
            for (int it = 0; it < 8; ++it) E -= (E - e * std::sin(E) - M) / (1.0 - e * std::cos(E));
            const double x = p.orbitA * (std::cos(E) - e);
            const double y = p.orbitA * std::sqrt(std::max(0.0, 1.0 - e * e)) * std::sin(E);
            p.position = focus + p.orbitU * x + p.orbitV * y;
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
    glm::dvec3 u = rel / dist;
    glm::dvec3 axis = (std::abs(u.y) < 0.95) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    glm::dvec3 v = glm::normalize(glm::cross(axis, u));
    pl.orbitParent = par; pl.orbitEcc = e; pl.orbitA = dist / std::max(1.0 - e, 1e-3);
    pl.orbitPeriod = period;
    pl.orbitPhase = -2.0 * 3.14159265358979323846 * (m_simulationTime / period);
    pl.orbitU = u; pl.orbitV = v;
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
        glm::dvec3 u = rel / dist;
        glm::dvec3 axis = (std::abs(u.y) < 0.95) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
        glm::dvec3 v = glm::normalize(glm::cross(axis, u));
        pl.orbitParent = parentIdx;
        pl.orbitEcc    = e;
        pl.orbitA      = dist / std::max(1.0 - e, 1e-3);
        pl.orbitPeriod = oi.period;
        pl.orbitPhase  = 0.0;
        pl.orbitU = u; pl.orbitV = v;
        HARUKA_LOGI("Orbit", "'%s' orbita '%s': a=%.3e e=%.2f T=%.1fs",
                pl.name.c_str(), oi.parent.c_str(), pl.orbitA, e, pl.orbitPeriod);
        // Sync orbit al SimplePlanet (el planeta renderizable se mueve igual que el cuerpo físico).
        for (auto& sp : m_simplePlanets) {
            if (sp->config().name == pl.name) {
                sp->setOrbit(pl.orbitParent, pl.orbitA, pl.orbitEcc, pl.orbitPeriod,
                             pl.orbitPhase, pl.orbitU, pl.orbitV);
                break;
            }
        }
    }

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
        sum.waterVertices += s.waterVertices; sum.waterTriangles += s.waterTriangles;
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

Haruka::TerrainSample PlanetarySystem::sampleSurface(const glm::dvec3& worldPos) const {
    Haruka::TerrainSample s;
    if (m_planets.empty()) return s;
    const Planet* nearest = nullptr;
    double bestDist = 1e300;
    for (const auto& p : m_planets) {
        double d = glm::length(p.position - worldPos);
        if (d < bestDist) { bestDist = d; nearest = &p; }
    }
    if (!nearest) return s;
    return s;
}

double PlanetarySystem::sampleWaterLevel(const glm::dvec3& worldPos) const {
    const Haruka::TerrainSample s = sampleSurface(worldPos);
    if (s.waterType == Haruka::WaterType::None) return kNoWater;
    return double(s.waterLevelKm) * 1000.0;
}

void PlanetarySystem::ensureReferenceSurface() const {
    Haruka::WorldGenParams W; double R = 0.0;
    if (!getActivePlanetParams(W, R)) return;
    const int chunkSize = 24;
    const int refLod = 4;
    m_refSurface.configure(W, R, refLod, chunkSize);
}

double PlanetarySystem::sampleTerrainHeight(const glm::dvec3& worldPos) const {
    const Planet* nearest = nullptr; double best = 1e300;
    for (const auto& p : m_planets) {
        const double d = glm::length(p.position - worldPos);
        if (d < best) { best = d; nearest = &p; }
    }
    // SimplePlanet: el suelo sale de SU retícula base + el detalle compartido con la GPU. Va antes
    // que la superficie de referencia porque ésta cuelga de `sampleTerrainV2`, que en esta rama es
    // un stub que devuelve 0 — con ella la física caminaba sobre una esfera lisa.
    for (const auto& sp : m_simplePlanets) {
        if (!sp || sp->config().name != getActivePlanetName()) continue;
        const glm::dvec3 rel = worldPos - sp->config().position;
        const double len = glm::length(rel);
        if (len > 1e-9) return sp->sampleHeight(rel / len);
    }

    if (nearest && nearest->name == getActivePlanetName()) {
        const glm::dvec3 rel = worldPos - nearest->position;
        const double len = glm::length(rel);
        if (len > 1e-9) {
            ensureReferenceSurface();
            double baseM = 0.0;
            if (m_refSurface.ready()) {
                baseM = m_refSurface.elevM(rel / len);
            }
            // Apply deformation edits
            const glm::dvec3 dir = rel / len;
            const uint64_t key = dirToHeightKey(dir, m_refSurface.refLod(), m_refSurface.chunkSize());
            auto it = m_heightEdits.find(key);
            if (it != m_heightEdits.end()) baseM += it->second;
            return baseM;
        }
    }
    return double(sampleSurface(worldPos).elevKm) * 1000.0;
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

    if (!m_refSurface.ready()) return false;
    double m = m_refSurface.elevM(d);
    outElevKm = (float)(m / 1000.0);
    return true;
}

std::string PlanetarySystem::getActivePlanetName() const {
    for (const auto& p : m_planets)
        if (p.isHome) return p.name;
    return m_planets.empty() ? std::string{} : m_planets.front().name;
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
                                  float ambientStrength) {
    for (auto& p : m_simplePlanets) p->setSunLight(dir, color, ambientStrength);
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
            const double e = glm::clamp(cfg.orbitEcc, 0.0, 0.99);
            const double M = cfg.orbitPhase + 2.0 * 3.14159265358979323846 * (m_simulationTime / cfg.orbitPeriod);
            double E = M;
            for (int it = 0; it < 8; ++it) E -= (E - e * std::sin(E) - M) / (1.0 - e * std::cos(E));
            const double x = cfg.orbitA * (std::cos(E) - e);
            const double y = cfg.orbitA * std::sqrt(std::max(0.0, 1.0 - e * e)) * std::sin(E);
            p.setPosition(focus + cfg.orbitU * x + cfg.orbitV * y);
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
