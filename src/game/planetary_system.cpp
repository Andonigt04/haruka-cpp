#include "planetary_system.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include "core/modules.h"
#include "core/chunk_cache.h"
#include "core/lod_system.h"
#include "core/terrain/terrain_generator.h"
#include "core/terrain/terrain_streaming_system.h"
#ifdef HARUKA_MOD_DEFORM
#include "core/terrain/deformation_field.h"
#endif
#include "renderer/terrain_renderer.h"
#include "renderer/water_renderer.h"

namespace Haruka {

PlanetarySystem::PlanetarySystem() {}
PlanetarySystem::~PlanetarySystem() {}

void PlanetarySystem::init() {
    // Inicializamos los subsistemas una sola vez
    m_cache = std::make_unique<ChunkCache>(512);
    m_generator = std::make_unique<TerrainGenerator>();
    m_renderer = std::make_unique<TerrainRenderer>();
    m_waterRenderer = std::make_unique<WaterRenderer>();
#ifdef HARUKA_MOD_DEFORM
    m_deform = std::make_unique<DeformationField>();
    m_generator->setDeformationField(m_deform.get());
#endif
    
    // El streaming necesita a los otros tres
    m_streaming = std::make_unique<TerrainStreamingSystem>(*m_cache, *m_generator, *m_renderer);
    
    // El LOD es el que toma decisiones.
    // splitFactor 1.0: subdivide cuando la distancia a la superficie es menor que
    //   el tamaño del nodo (más detalle cerca de la cámara).
    // maxLOD: cada +1 nivel = chunks la mitad de grandes = 2x más definición
    //   cerca del jugador. 20 → chunk ~12 m (vs ~190 m a LOD16) = 16x más fino.
    //   Es barato porque solo subdivide donde está la cámara (rings concéntricos).
    //   Para más detalle súbelo (22 ≈ 64x, 23 ≈ 128x), pero el ruido debe tener
    //   frecuencia suficiente o los chunks pequeños salen lisos.
    m_lod = std::make_unique<LODSystem>(1.0, 20);
}

void PlanetarySystem::update(double dt, const glm::dvec3& cameraPos) {
    m_simulationTime += dt;

    // 1. Mover los planetas en sus órbitas
    updateOrbits(dt);

    // Keep the per-planet LOD-throttle state sized to the planet list.
    if (m_lastLODCamPos.size() != m_planets.size())
        m_lastLODCamPos.assign(m_planets.size(), glm::dvec3(1e300));

    // 2. Para cada planeta, actualizar su terreno
    for (size_t pi = 0; pi < m_planets.size(); ++pi) {
        auto& planet = m_planets[pi];

        // Throttle: el quadtree solo cambia al cruzar una frontera de split, así
        // que saltamos el rebuild (con allocs) cuando la cámara apenas se movió
        // desde el último recompute. Los chunks ya en GPU siguen renderizándose.
        const double finest = (planet.radius * 2.0) / double(1u << 16);
        const double moveThresh = std::max(0.5, finest * 0.125);
        if (!m_forceLOD && glm::length(cameraPos - m_lastLODCamPos[pi]) < moveThresh)
            continue;
        m_lastLODCamPos[pi] = cameraPos;

        SceneObject planetProxy;                 // pila, sin make_shared por frame
        planetProxy.name = planet.name;
        planetProxy.position = planet.position;
        planetProxy.scale = glm::dvec3(planet.radius);

        // A. ¿Qué chunks deben verse? (firma toma shared_ptr → deleter no-op)
        LODUpdate update = m_lod->updatePlanetLOD(
            std::shared_ptr<SceneObject>(&planetProxy, [](SceneObject*){}), cameraPos);

        // B. Generar chunks nuevos (async) pasando settings del planeta
        nlohmann::json streamSettings = planet.terrainSettings;
        streamSettings["radius"]      = planet.radius;
        streamSettings["planetName"]  = planet.name;
        streamSettings["planetOffsetX"] = planet.position.x;
        streamSettings["planetOffsetY"] = planet.position.y;
        streamSettings["planetOffsetZ"] = planet.position.z;

        // Ordenar los chunks deseados de MÁS CERCANO a más lejano al jugador, para
        // que el streaming los genere/cargue en ese orden (cercanos primero).
        {
            const glm::dvec3 pPos = planet.position;
            const double     pRad = planet.radius;
            std::vector<std::pair<double, PlanetChunkKey>> byDist;
            byDist.reserve(update.chunksToLoad.size());
            for (const auto& k : update.chunksToLoad) {
                const double cpa = std::pow(2.0, (double)k.lod); // chunks por eje en este LOD
                const double u = (double(k.x) + 0.5) / cpa;
                const double v = (double(k.y) + 0.5) / cpa;
                const glm::dvec3 c = m_lod->getCubeToSpherePos(k.face, u, v, pRad) + pPos;
                const glm::dvec3 d = c - cameraPos;
                byDist.emplace_back(glm::dot(d, d), k);
            }
            std::sort(byDist.begin(), byDist.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            std::vector<PlanetChunkKey> sorted;
            sorted.reserve(byDist.size());
            for (auto& p : byDist) sorted.push_back(p.second);
            m_streaming->setDesiredChunks(std::move(sorted), streamSettings);
        }

        // C. Cargar desde caché a GPU y descargar lo que ya no se ve
        m_streaming->processLODUpdate(update);
    }
    m_forceLOD = false;

    // Despachar generación pendiente (cercanos primero) CADA frame, aunque el LOD
    // esté en throttle por estar quieto → los chunks lejanos siguen entrando.
    m_streaming->pump();

    // 3. Recoger chunks terminados y subirlos a la GPU
    auto readyChunks = m_streaming->getReadyChunks();
    for (auto& chunk : readyChunks) {
        m_renderer->addToScene(chunk->planetName, chunk->key, *chunk);
        if (m_waterRenderer) m_waterRenderer->addToScene(chunk->planetName, chunk->key, *chunk);
    }
}

void PlanetarySystem::updateOrbits(double dt) {
    // Aquí mueves las posiciones de m_planets usando Kepler o integración Euler
    // Por ahora, podrías dejarlos estáticos o con una rotación simple
}

void PlanetarySystem::addPlanet(const Planet& planet) {
    m_planets.push_back(planet);
    m_forceLOD = true; // recompute LOD next update so the new planet appears at once
}

void PlanetarySystem::syncFromScene(const SceneManager& scene) {
    for (auto& planet : m_planets) {
        for (const auto& objPtr : scene.getAllObjects()) {
            if (!objPtr || objPtr->name != planet.name) continue;
            planet.position = objPtr->position;
            planet.radius   = std::max({objPtr->scale.x, objPtr->scale.y, objPtr->scale.z});
            break;
        }
    }
}

void PlanetarySystem::setTerrainCullMatrix(const glm::mat4& camRelViewProj) {
    if (m_renderer)      m_renderer->setCullMatrix(camRelViewProj);
    if (m_waterRenderer) m_waterRenderer->setCullMatrix(camRelViewProj);
}

void PlanetarySystem::renderPlanetTerrain(const std::string& planetName, const glm::dvec3& cameraPos) {
    if (!m_renderer) return;
    for (const auto& p : m_planets)
        if (p.name == planetName) { m_renderer->setPlanetCenter(p.position); break; }
    m_renderer->renderPlanet(planetName, cameraPos);
}

void PlanetarySystem::renderPlanetWater(const std::string& planetName, const glm::dvec3& cameraPos) {
    if (!m_waterRenderer) return;
    for (const auto& p : m_planets)
        if (p.name == planetName) {
            m_waterRenderer->setPlanetCenter(p.position);
            m_waterRenderer->setPlanetRadius(p.radius);
            break;
        }
    m_waterRenderer->renderPlanet(planetName, cameraPos);
}

int PlanetarySystem::getGPUWaterChunkCount() const {
    return m_waterRenderer ? m_waterRenderer->getGPUMeshCount() : 0;
}

int PlanetarySystem::getGPUChunkCount()    const { return m_renderer  ? m_renderer->getGPUMeshCount()              : 0; }
int PlanetarySystem::getPendingChunks()    const { return m_streaming ? m_streaming->getPendingCount()              : 0; }
int PlanetarySystem::getCachedChunks()     const { return m_cache     ? (int)m_cache->getChunkCount()               : 0; }
int PlanetarySystem::getCacheMemoryMB()    const { return m_cache     ? (int)m_cache->getMemoryUsageMB()            : 0; }
int PlanetarySystem::getCacheMaxMemoryMB() const { return m_cache     ? (int)m_cache->getMaxMemoryMB()              : 0; }

PlanetarySystem::TerrainDrawStats PlanetarySystem::getTerrainDrawStats() const {
    if (!m_renderer) return {};
    const auto s = m_renderer->getDrawStats();
    return { s.draws, s.vertices, s.triangles };
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
    outSeaRadius = nearest->radius; // sea level = elevation 0 = planet radius
    return true;
}

double PlanetarySystem::sampleTerrainHeight(const glm::dvec3& worldPos) const {
    if (!m_generator || m_planets.empty()) return 0.0;

    // Find the nearest planet to the query position.
    const Planet* nearest = nullptr;
    double bestDist = 1e300;
    for (const auto& p : m_planets) {
        double d = glm::length(p.position - worldPos);
        if (d < bestDist) { bestDist = d; nearest = &p; }
    }
    if (!nearest) return 0.0;

    // Direction from planet center to the query point (unit sphere direction).
    glm::dvec3 dir = worldPos - nearest->position;
    double len = glm::length(dir);
    if (len < 1e-9) return 0.0;
    glm::vec3 sphereDir = glm::vec3(dir / len);

    // Build settings JSON the same way streaming does (config lives under terrainSettings).
    nlohmann::json settings = nearest->terrainSettings;
    settings["planetOffsetX"] = nearest->position.x;
    settings["planetOffsetY"] = nearest->position.y;
    settings["planetOffsetZ"] = nearest->position.z;
    return double(m_generator->sampleHeightAt(sphereDir, settings, nearest->radius));
}

void PlanetarySystem::invalidateAllChunks() {
    // Drop every GPU chunk + cache entry + LOD memory so the next update fully
    // regenerates the terrain (applying whatever brushes are now in the field).
    if (!m_renderer) return;
    // A sphere centred on each planet large enough to cover the whole planet.
    for (const auto& p : m_planets) {
        auto keys = m_renderer->invalidateSphere(p.position, p.radius * 4.0);
        for (const auto& k : keys) {
            if (m_cache) m_cache->removeChunk(k);
            if (m_waterRenderer) m_waterRenderer->removeFromScene("", k);
            if (m_lod) m_lod->forgetChunk(k);
        }
    }
    m_forceLOD = true;
}

void PlanetarySystem::editTerrain(const glm::dvec3& worldPos, double radius, double strength, bool dig) {
#ifdef HARUKA_MOD_DEFORM
    if (!m_deform) return;

    DeformationField::Brush b;
    b.center   = worldPos;
    b.radius   = radius;
    b.strength = strength;
    b.type     = dig ? DeformationField::Type::Subtract : DeformationField::Type::Add;
    m_deform->addBrush(b);

    // Invalidate GPU chunks overlapping the edit (plus margin) and drop them from
    // the cache so streaming regenerates them with the new edit applied. A small
    // margin covers chunks whose centre is just outside but whose mesh reaches in.
    const double margin = radius * 1.5 + 8.0;
    if (m_renderer) {
        auto keys = m_renderer->invalidateSphere(worldPos, margin);
        for (const auto& k : keys) {
            if (m_cache) m_cache->removeChunk(k);
            if (m_waterRenderer) m_waterRenderer->removeFromScene("", k);
            // Forget it in the LOD so next update treats it as LOAD (re-upload),
            // not KEEP (which only refreshes the now-empty cache → chunk vanishes).
            if (m_lod) m_lod->forgetChunk(k);
        }
    }
    // Force the LOD to recompute next update so the freed chunks are re-requested.
    m_forceLOD = true;
#else
    (void)worldPos; (void)radius; (void)strength; (void)dig;
#endif
}

}