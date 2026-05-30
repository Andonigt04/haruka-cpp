#include "planetary_system.h"

#include "core/chunk_cache.h"
#include "core/lod_system.h"
#include "core/terrain/terrain_generator.h"
#include "core/terrain/terrain_streaming_system.h"
#include "renderer/terrain_renderer.h"

namespace Haruka {

PlanetarySystem::PlanetarySystem() {}
PlanetarySystem::~PlanetarySystem() {}

void PlanetarySystem::init() {
    // Inicializamos los subsistemas una sola vez
    m_cache = std::make_unique<ChunkCache>(512);
    m_generator = std::make_unique<TerrainGenerator>();
    m_renderer = std::make_unique<TerrainRenderer>();
    
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

    // 2. Para cada planeta, actualizar su terreno
    for (auto& planet : m_planets) {
        auto planetProxy = std::make_shared<SceneObject>();
        planetProxy->name = planet.name;
        planetProxy->position = planet.position;
        planetProxy->scale = glm::dvec3(planet.radius);

        // A. ¿Qué chunks deben verse?
        LODUpdate update = m_lod->updatePlanetLOD(planetProxy, cameraPos);

        // B. Generar chunks nuevos (async) pasando settings del planeta
        nlohmann::json streamSettings = planet.terrainSettings;
        streamSettings["radius"]      = planet.radius;
        streamSettings["planetName"]  = planet.name;
        streamSettings["planetOffsetX"] = planet.position.x;
        streamSettings["planetOffsetY"] = planet.position.y;
        streamSettings["planetOffsetZ"] = planet.position.z;
        m_streaming->update(update.chunksToLoad, streamSettings);

        // C. Cargar desde caché a GPU y descargar lo que ya no se ve
        m_streaming->processLODUpdate(update);
    }

    // 3. Recoger chunks terminados y subirlos a la GPU
    auto readyChunks = m_streaming->getReadyChunks();
    for (auto& chunk : readyChunks) {
        m_renderer->addToScene(chunk->planetName, chunk->key, *chunk);
    }
}

void PlanetarySystem::updateOrbits(double dt) {
    // Aquí mueves las posiciones de m_planets usando Kepler o integración Euler
    // Por ahora, podrías dejarlos estáticos o con una rotación simple
}

void PlanetarySystem::addPlanet(const Planet& planet) {
    m_planets.push_back(planet);
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

void PlanetarySystem::renderPlanetTerrain(const std::string& planetName, const glm::dvec3& cameraPos) {
    if (m_renderer) m_renderer->renderPlanet(planetName, cameraPos);
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
    return double(m_generator->sampleHeightAt(sphereDir, settings, nearest->radius));
}

}