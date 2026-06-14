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
#include "renderer/floating_island_renderer.h"
#include "renderer/texture.h"
#include "core/terrain/floating_islands.h"
#include "core/terrain/terrain_sampler_v2.h"
#include "settings/settings_manager.h"
#include <glad/glad.h>
#include <filesystem>
#include <chrono>

namespace Haruka {

PlanetarySystem::PlanetarySystem() {}
PlanetarySystem::~PlanetarySystem() {}

void PlanetarySystem::init() {
    // Inicializamos los subsistemas una sola vez
    m_cache = std::make_unique<ChunkCache>(384); // override por GraphicsSettings.chunkMemoryMB
    m_generator = std::make_unique<TerrainGenerator>();
    m_renderer = std::make_unique<TerrainRenderer>();
    m_waterRenderer = std::make_unique<WaterRenderer>();
    m_islandRenderer = std::make_unique<FloatingIslandRenderer>();
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

    // Islas flotantes (genVersion>=2): streaming-lite. Regenera el casquete de
    // islas alrededor de la cámara cuando esta se mueve lo bastante → islas
    // descubribles al explorar, sin generar millones en todo el planeta.
    if (m_islandRenderer) {
        for (const auto& planet : m_planets) {
            const auto& cfg = planet.terrainSettings.contains("config")
                            ? planet.terrainSettings["config"] : planet.terrainSettings;
            if (cfg.value("genVersion", 1) < 2) continue;
            glm::dvec3 up = cameraPos - glm::dvec3(planet.position);
            double ul = glm::length(up);
            if (ul < 1.0) break;
            glm::dvec3 camDir = up / ul;
            const float angRadius = 0.03f; // ~190 km de casquete
            if (m_islandsGenerated &&
                glm::dot(camDir, m_islandGenCamDir) > std::cos(angRadius * 0.4)) break; // apenas se movió
            uint32_t seed = (uint32_t)cfg.value("seed", 42);
            WorldGenParams W = deriveWorldParams(seed, planet.radius);
            W.reliefStrength = cfg.value("reliefStrength", 1.0f);
            auto islands = generateFloatingIslandsNear(seed, glm::dvec3(planet.position),
                                                       planet.radius, W, camDir, angRadius);
            m_islandRenderer->setIslands(std::move(islands));
            m_islandGenCamDir  = camDir;
            m_islandsGenerated = true;
            break; // un planeta con islas por ahora
        }
    }

    // (Los props/recursos los genera ahora el JUEGO — Survival ResourceSystem — usando
    //  getActivePlanet() + el sampler de terreno del motor.)

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

        // C. Cargar desde caché a GPU (la descarga es DIFERIDA, abajo)
        m_streaming->processLODUpdate(update);

        // El streaming solo sube el TERRENO desde caché. El AGUA debe seguir la
        // MISMA vida que el terreno (subirse desde caché aquí también), o al
        // recargar un chunk vuelve el terreno pero no el agua → mallas de agua
        // desincronizadas/huérfanas que se solapan (los "blobs"). addToScene se
        // auto-salta si ya está, así que reañadir es barato.
        if (m_waterRenderer && m_cache)
            for (const auto& k : update.chunksToLoad) {
                ChunkData wd;
                if (m_cache->getChunkCopy(k, wd))
                    m_waterRenderer->addToScene(planet.name, k, wd);
            }

        // Lo que sale de vista NO se borra: se marca STALE (sigue dibujándose). El
        // renderer lo retira solo cuando su área queda cubierta por el reemplazo
        // (padre al fusionar, o los 4 hijos al subdividir) vía purgeStaleCoveredBy.
        // Así nunca hay un agujero entre quitar el viejo y subir el nuevo — el mismo
        // mecanismo "sin hueco" que ya usaba el dig, ahora general para todo el LOD.
        for (const auto& k : update.chunksToUnload) {
            m_renderer->markStale(k);
            if (m_waterRenderer) m_waterRenderer->markStale(k);
        }
    }
    m_forceLOD = false;

    // Despachar generación pendiente (cercanos primero) CADA frame, aunque el LOD
    // esté en throttle por estar quieto → los chunks lejanos siguen entrando.
    m_streaming->pump();

    // 3. Recoger chunks terminados y subirlos a la GPU — SOLO si siguen visibles.
    //    Un chunk cuya generación async terminó DESPUÉS de salir de la vista ya fue
    //    "descargado" por el diff del LOD mientras se generaba; si lo subiéramos
    //    igual quedaría en GPU para siempre (terreno apilado + coste creciente).
    //    Se queda en la caché y reaparece vía processLODUpdate si vuelves.
    auto readyChunks = m_streaming->getReadyChunks();
    for (auto& chunk : readyChunks) {
        if (m_lod && !m_lod->isVisible(chunk->key)) continue;
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

    // (Las islas flotantes se generan en update(), cerca de la cámara: streaming-lite.)
}

void PlanetarySystem::renderPlanetIslands(const std::string& planetName, const glm::dvec3& cameraPos) {
    (void)planetName;
    if (m_islandRenderer) m_islandRenderer->render(cameraPos);
}

bool PlanetarySystem::getActivePlanet(glm::dvec3& center, double& radius,
                                     uint32_t& seed, float& reliefStrength) const {
    for (const auto& planet : m_planets) {
        const auto& cfg = planet.terrainSettings.contains("config")
                        ? planet.terrainSettings["config"] : planet.terrainSettings;
        if (cfg.value("genVersion", 1) < 2) continue;   // solo planetas v2 tienen recursos
        center         = glm::dvec3(planet.position);
        radius         = planet.radius;
        seed           = (uint32_t)cfg.value("seed", 42);
        reliefStrength = cfg.value("reliefStrength", 1.0f);
        return true;
    }
    return false;
}

void PlanetarySystem::setTerrainCullMatrix(const glm::mat4& camRelViewProj) {
    if (m_renderer)      m_renderer->setCullMatrix(camRelViewProj);
    if (m_waterRenderer) m_waterRenderer->setCullMatrix(camRelViewProj);
}

// Texturas de bioma OPCIONALES, declaradas en la escena:
//   terrainSettings.config.textures = { "dir": "assets/textures/terrain" }
// Convención de tiers (la genera tools/gen_texture_tiers.sh):
//   <dir>/512, <dir>/1024 (base) y <dir>_hd/2048 (pack HD). Tier por TextureQuality,
//   con fallback a 1024. Sin config → procedural (u_hasTex=0). El engine core no
//   sabe de texturas; esto vive en el sistema de terreno.
void PlanetarySystem::bindTerrainTextures(const Planet& planet) {
    namespace fs = std::filesystem;
    const auto& cfg = planet.terrainSettings.contains("config")
                    ? planet.terrainSettings["config"] : planet.terrainSettings;

    std::string dir;
    if (cfg.contains("textures") && cfg["textures"].is_object())
        dir = cfg["textures"].value("dir", std::string{});

    if (dir.empty()) { glUniform1i(23, 0); return; } // sin texturas → procedural

    const int q    = (int)Haruka::SettingsManager::get().graphics().textureQuality;
    const int tier = (q <= 0) ? 512 : (q >= 3) ? 2048 : 1024;
    if (tier != m_texTier || dir != m_texDir) {
        std::string sub = (tier == 2048) ? (dir + "_hd/2048/")
                                         : (dir + "/" + std::to_string(tier) + "/");
        if (!fs::exists(sub + "sand_albedo.png")) sub = dir + "/1024/"; // fallback
        auto load = [&](const char* n) -> std::unique_ptr<Texture> {
            std::string p = sub + n;
            return fs::exists(p) ? std::make_unique<Texture>(p.c_str()) : nullptr;
        };
        m_texSandAlbedo  = load("sand_albedo.png");
        m_texSandNormal  = load("sand_normal.png");
        m_texGrassAlbedo = load("grass_albedo.png");
        m_texLandAlbedo  = load("land_albedo.png");
        m_texLandNormal  = load("land_normal.png");
        m_texTier = tier; m_texDir = dir;
    }
    bool has = m_texSandAlbedo && m_texGrassAlbedo && m_texLandAlbedo;
    if (has) {
        m_texSandAlbedo->use(4);  glUniform1i(21, 4);
        if (m_texSandNormal) { m_texSandNormal->use(5); glUniform1i(22, 5); }
        m_texGrassAlbedo->use(6); glUniform1i(24, 6);
        m_texLandAlbedo->use(7);  glUniform1i(25, 7);
        if (m_texLandNormal) { m_texLandNormal->use(8); glUniform1i(26, 8); }
    }
    glUniform1i(23, has ? 1 : 0);
}

void PlanetarySystem::renderPlanetTerrain(const std::string& planetName, const glm::dvec3& cameraPos) {
    if (!m_renderer) return;
    const Planet* planet = nullptr;
    for (const auto& p : m_planets)
        if (p.name == planetName) { m_renderer->setPlanetCenter(p.position); planet = &p; break; }
    if (planet) bindTerrainTextures(*planet);
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
int PlanetarySystem::getQueuedChunks()     const { return m_streaming ? m_streaming->getQueuedCount()               : 0; }
int PlanetarySystem::getCachedChunks()     const { return m_cache     ? (int)m_cache->getChunkCount()               : 0; }
int PlanetarySystem::getCacheMemoryMB()    const { return m_cache     ? (int)m_cache->getMemoryUsageMB()            : 0; }
int PlanetarySystem::getCacheMaxMemoryMB() const { return m_cache     ? (int)m_cache->getMaxMemoryMB()              : 0; }
void PlanetarySystem::setCacheMaxMemoryMB(int mb) { if (m_cache) m_cache->setMaxMemory((size_t)std::max(16, mb)); }
void PlanetarySystem::setLODParams(double splitFactor, int maxLOD) {
    if (!m_lod) return;
    if (m_lod->getSplitFactor() == splitFactor && m_lod->getMaxLOD() == maxLOD) return;
    m_lod->setParams(splitFactor, maxLOD);
    m_forceLOD = true; // re-evaluate the quadtree next update
}

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
    invalidateEditedChunks(worldPos, radius);
#else
    (void)worldPos; (void)radius; (void)strength; (void)dig;
#endif
}

void PlanetarySystem::levelTerrain(const glm::dvec3& worldPos, double radius, double targetHeightM) {
#ifdef HARUKA_MOD_DEFORM
    if (!m_deform) return;

    DeformationField::Brush b;
    b.center       = worldPos;
    b.radius       = radius;
    b.type         = DeformationField::Type::Flatten;
    b.targetHeight = targetHeightM;   // el área se aplana hacia esta elevación
    m_deform->addBrush(b);
    invalidateEditedChunks(worldPos, radius);
#else
    (void)worldPos; (void)radius; (void)targetHeightM;
#endif
}

void PlanetarySystem::levelTerrainBox(const glm::dvec3& center, const glm::dvec3& halfExtents,
                                      const glm::dmat3& rot, double targetHeightM, double band) {
#ifdef HARUKA_MOD_DEFORM
    if (!m_deform) return;

    DeformationField::Brush b;
    b.center       = center;
    b.type         = DeformationField::Type::Flatten;
    b.targetHeight = targetHeightM;
    b.box          = true;            // huella del objeto (no un círculo)
    b.halfExtents  = halfExtents;
    b.rot          = rot;
    b.radius       = band;            // ancho de transición del borde
    m_deform->addBrush(b);
    invalidateEditedChunks(center, glm::length(halfExtents) + band);
#else
    (void)center; (void)halfExtents; (void)rot; (void)targetHeightM; (void)band;
#endif
}

void PlanetarySystem::invalidateEditedChunks(const glm::dvec3& center, double radius) {
    // Marca los chunks GPU que toca la edición (+margen) como STALE (NO los borra → la malla
    // vieja se sigue viendo, sin agujero) y los quita de la caché para regenerarlos con la
    // edición. Al llegar el chunk regenerado, addToScene REEMPLAZA la malla vieja sin parón.
    const double margin = radius * 1.5 + 8.0;
    if (m_renderer) {
        auto keys = m_renderer->markStaleSphere(center, margin);
        for (const auto& k : keys) {
            if (m_cache) m_cache->removeChunk(k);
            if (m_waterRenderer) m_waterRenderer->removeFromScene("", k);
            // Olvídalo en el LOD: el próximo update lo re-pide (LOAD) → regenera y reemplaza.
            if (m_lod) m_lod->forgetChunk(k);
        }
    }
    m_forceLOD = true; // recomputa el LOD el próximo update → re-pide los chunks liberados
}

}