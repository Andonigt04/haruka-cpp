#include "core/terrain_streaming_system.h"
#include "core/error_reporter.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
#endif

#include "core/camera.h"
#include "core/components/mesh_renderer_component.h"
#include "physics/raycast_simple.h"
#include "renderer/motor_instance.h"
#include "core/application.h"

namespace Haruka {

// Detectar RAM disponible del sistema
static uint64_t getSystemMemoryBytes() {
    #ifdef _WIN32
        MEMORYSTATUSEX memStatus;
        memStatus.dwLength = sizeof(memStatus);
        GlobalMemoryStatusEx(&memStatus);
        return memStatus.ullTotalPhys;
    #else
        long pages = sysconf(_SC_PHYS_PAGES);
        long pageSize = sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && pageSize > 0) {
            return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize);
        }
        return 8ull * 1024 * 1024 * 1024;  // Default 8GB si no se puede detectar
    #endif
}

bool TerrainStreamingSystem::isTerrainChunkObject(const SceneObject& obj) {
    if (!obj.properties.is_object()) return false;
    if (!obj.properties.contains("terrainEditor")) return false;
    const auto& te = obj.properties["terrainEditor"];
    return te.is_object() && te.value("isChunk", false);
}

SceneObject* TerrainStreamingSystem::findTerrainChunkByKey(Scene* scene, const PlanetChunkKey& key) {
    if (!scene) return nullptr;
    for (auto& obj : scene->getObjectsMutable()) {
        if (!isTerrainChunkObject(obj)) continue;
        const auto& te = obj.properties["terrainEditor"];
        if (!te.is_object()) continue;
        if (!te.contains("chunkFace") || !te.contains("chunkLod") || !te.contains("chunkX") || !te.contains("chunkY")) continue;
        if (te.value("chunkFace", -1) != key.face) continue;
        if (te.value("chunkLod", -1) != key.lod) continue;
        if (te.value("chunkX", -1) != key.x) continue;
        if (te.value("chunkY", -1) != key.y) continue;
        return &obj;
    }
    return nullptr;
}

bool TerrainStreamingSystem::buildTerrainChunkTemplate(Scene* scene, TerrainChunkTemplate& outTpl) {
    if (!scene) return false;
    for (const auto& obj : scene->getObjects()) {
        if (!isTerrainChunkObject(obj)) continue;
        const auto& te = obj.properties["terrainEditor"];
        if (!te.is_object()) continue;
        outTpl.sourceName = te.value("source", outTpl.sourceName);
        outTpl.chunkTilesY = std::max(1, te.value("chunkTilesY", outTpl.chunkTilesY));
        outTpl.chunkTilesX = std::max(1, te.value("chunkTilesX", outTpl.chunkTilesX));
        outTpl.type = obj.type;
        outTpl.color = obj.color;
        outTpl.intensity = obj.intensity;
        outTpl.parentIndex = obj.parentIndex;
        outTpl.material = obj.material;
        return true;
    }

    // Source name from the explicit planet root (isPlanetRoot=true only).
    if (outTpl.sourceName.empty()) {
        for (const auto& obj : scene->getObjects()) {
            if (!obj.properties.is_object() || !obj.properties.contains("terrainEditor")) continue;
            const auto& te = obj.properties["terrainEditor"];
            if (te.is_object() && te.value("isPlanetRoot", false)) {
                outTpl.sourceName = obj.name;
                break;
            }
        }
    }

    if (SceneObject* source = scene->getObject(outTpl.sourceName)) {
        outTpl.type = source->type;
        outTpl.color = source->color;
        outTpl.intensity = source->intensity;
        outTpl.material = source->material;
        outTpl.parentIndex = -1;
        return true;
    }
    return false;
}

SceneObject* TerrainStreamingSystem::createTerrainChunkForKey(Scene* scene, const PlanetChunkKey& key) {
    if (!scene) return nullptr;

    TerrainChunkTemplate tpl;
    if (!buildTerrainChunkTemplate(scene, tpl)) return nullptr;

    // Find planet root to establish parent-child relationship
    int parentIdx = -1;
    const auto& objects = scene->getObjects();
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto& obj = objects[i];
        if (!obj.properties.is_object()) continue;
        if (!obj.properties.contains("terrainEditor")) continue;
        const auto& te = obj.properties["terrainEditor"];
        if (te.is_object() && te.value("isPlanetRoot", false)) {
            parentIdx = static_cast<int>(i);
            break;
        }
    }

    // Only objects with explicit isPlanetRoot=true are valid terrain parents.
    if (parentIdx < 0) return nullptr;

    SceneObject obj;
    obj.name = tpl.sourceName + "_chunk_f" + std::to_string(key.face)
             + "_l" + std::to_string(key.lod)
             + "_x" + std::to_string(key.x)
             + "_y" + std::to_string(key.y);
    obj.type = tpl.type;
    obj.position = glm::dvec3(0.0);
    obj.rotation = glm::dvec3(0.0);
    obj.scale = glm::dvec3(1.0);
    obj.color = tpl.color;
    obj.intensity = tpl.intensity;
    obj.parentIndex = parentIdx;
    obj.material = tpl.material;
    obj.meshRenderer = std::make_shared<MeshRendererComponent>();

    obj.properties["terrainEditor"]["isChunk"] = true;
    obj.properties["terrainEditor"]["source"] = tpl.sourceName;
    obj.properties["terrainEditor"]["chunkFace"] = key.face;
    obj.properties["terrainEditor"]["chunkLod"] = key.lod;
    obj.properties["terrainEditor"]["chunkX"] = key.x;
    obj.properties["terrainEditor"]["chunkY"] = key.y;
    obj.properties["terrainEditor"]["chunkTilesX"] = tpl.chunkTilesX;
    obj.properties["terrainEditor"]["chunkTilesY"] = tpl.chunkTilesY;
    obj.properties["terrainEditor"]["isInstancedTerrainChunk"] = true;
    obj.properties["terrainEditor"]["elevationTexturing"] = true;

    scene->addObject(obj);
    addChunkAsChild(scene, obj, parentIdx);
    return findTerrainChunkByKey(scene, key);
}

int TerrainStreamingSystem::findPlanetRootIndex(Scene* scene) {
    if (!scene) return -1;
    const auto& objects = scene->getObjects();
    for (size_t i = 0; i < objects.size(); ++i) {
        const auto& obj = objects[i];
        if (!obj.properties.is_object()) continue;
        if (!obj.properties.contains("terrainEditor")) continue;
        const auto& te = obj.properties["terrainEditor"];
        if (te.is_object() && te.value("isPlanetRoot", false)) {
            return static_cast<int>(i);
        }
    }
    return -1; // Only explicit isPlanetRoot property marks a terrain root
}

void TerrainStreamingSystem::addChunkAsChild(Scene* scene, SceneObject& chunk, int parentIdx) {
    if (!scene || parentIdx < 0) return;
    auto& mutableObjects = scene->getObjectsMutable();
    if (parentIdx >= static_cast<int>(mutableObjects.size())) return;

    // Find chunk index (it's the one we just added, normally last or matches our name)
    int chunkIdx = -1;
    for (int i = static_cast<int>(mutableObjects.size()) - 1; i >= 0; --i) {
        if (mutableObjects[i].name == chunk.name) {
            chunkIdx = i;
            break;
        }
    }
    if (chunkIdx < 0) return;

    // Register chunk in parent's children list
    SceneObject* parent = &mutableObjects[parentIdx];
    auto it = std::find(parent->childrenIndices.begin(), parent->childrenIndices.end(), chunkIdx);
    if (it == parent->childrenIndices.end()) {
        parent->childrenIndices.push_back(chunkIdx);
    }
}

void TerrainStreamingSystem::ensureTerrainChunkKeysAndGrid(Scene* scene, WorldSystem* worldSystem) {
    if (!scene || !worldSystem) return;

    SceneObject* planetObj = nullptr;
    for (auto& obj : scene->getObjectsMutable()) {
        if (!obj.properties.is_object()) continue;
        if (!obj.properties.contains("terrainEditor")) continue;
        const auto& te = obj.properties["terrainEditor"];
        if (!te.is_object()) continue;
        if (te.value("isPlanetRoot", false)) {
            planetObj = &obj;
            break;
        }
    }

    if (!planetObj) return; // Only explicit isPlanetRoot=true is a valid terrain root

    // Tell WorldSystem the actual planet surface radius so updateVisibleChunks
    // can project chunk positions onto the real surface (not onto the camera-distance sphere).
    const float surfaceRadius = static_cast<float>(planetObj->scale.x);
    if (surfaceRadius > 1.0f) {
        worldSystem->setPlanetRadius(surfaceRadius);
    }

    // Hide the base sphere as soon as any chunks exist (tracked or resident).
    // Keeping the base mesh while chunks render causes Z-fighting between the
    // 48-segment sphere and the cube-sphere patches, which shows as ring artifacts.
    if (worldSystem && worldSystem->getTrackedChunkCount() > 0 && planetObj->meshRenderer) {
        planetObj->meshRenderer->releaseMesh();
    }

    int tilesPerFace = 16;
    int maxLod = 4;
    if (planetObj->properties.is_object() && planetObj->properties.contains("terrainEditor")) {
        const auto& te = planetObj->properties["terrainEditor"];
        if (te.is_object()) {
            tilesPerFace = te.value("tilesPerFace", 16);
            maxLod = te.value("maxLod", 4);
        }
    }

    tilesPerFace = std::clamp(tilesPerFace, 1, 128);
    maxLod = std::clamp(maxLod, 0, 8);
    worldSystem->setChunkGrid(0, 0, tilesPerFace, tilesPerFace, maxLod);
}

PlanetarySystem::PlanetConfig TerrainStreamingSystem::buildPlanetConfigFromChunkSource(const SceneObject& chunkObj, Scene* scene) {
    PlanetarySystem::PlanetConfig cfg;
    cfg.useGPU = false;

    auto applyGeneratorJson = [&cfg](const nlohmann::json& gen) {
        if (!gen.is_object()) return;
        cfg.seedBase = gen.value("seed", cfg.seedBase);
        cfg.seedContinents = gen.value("seedContinents", cfg.seedBase + 100);
        cfg.seedMacro = gen.value("seedMacro", cfg.seedBase + 200);
        cfg.seedDetail = gen.value("seedDetail", cfg.seedBase + 300);
        cfg.baseRadiusKm = gen.value("baseRadiusKm", cfg.baseRadiusKm);
        cfg.enableContinents = gen.value("enableContinents", cfg.enableContinents);
        cfg.enableMountains = gen.value("enableMountains", cfg.enableMountains);
        cfg.seaLevel = gen.value("seaLevel", cfg.seaLevel);
        cfg.continentFrequency = gen.value("continentFrequency", 1.0f);  // Más detalle en continentes
        cfg.continentWarpStrength = gen.value("continentWarpStrength", 0.2f);
        cfg.continentHeightStrength = gen.value("continentHeightStrength", 0.1f);
        cfg.macroFrequency = gen.value("macroFrequency", 4.5f);  // Aumentado para más definición
        cfg.macroHeightStrength = gen.value("macroHeightStrength", 0.25f);  // Aumentado
        cfg.detailFrequency = gen.value("detailFrequency", 16.0f);  // Más detalle fino
        cfg.detailHeightStrength = gen.value("detailHeightStrength", 0.05f);  // Aumentado
        cfg.octavesContinents = gen.value("octavesContinents", 5);  // +1 octava
        cfg.octavesMacro = gen.value("octavesMacro", 6);  // +1 octava
        cfg.octavesDetail = gen.value("octavesDetail", 5);  // +1 octava
        cfg.persistence = gen.value("persistence", 0.55f);  // Slightly higher para más complejidad
        cfg.lacunarity = gen.value("lacunarity", 2.1f);  // Slightly higher
    };

    if (!chunkObj.properties.is_object() || !chunkObj.properties.contains("terrainEditor")) return cfg;
    const auto& te = chunkObj.properties["terrainEditor"];
    if (!te.is_object()) return cfg;

    if (te.contains("generator") && te["generator"].is_object()) {
        applyGeneratorJson(te["generator"]);
        return cfg;
    }

    const std::string sourceName = te.value("source", "");
    if (sourceName.empty() || !scene) return cfg;

    SceneObject* sourceObj = scene->getObject(sourceName);
    if (!sourceObj || !sourceObj->properties.is_object()) return cfg;
    if (!sourceObj->properties.contains("terrainEditor")) return cfg;
    const auto& srcTe = sourceObj->properties["terrainEditor"];
    if (!srcTe.is_object() || !srcTe.contains("generator") || !srcTe["generator"].is_object()) return cfg;

    applyGeneratorJson(srcTe["generator"]);
    return cfg;
}

PlanetarySystem::ChunkConfig TerrainStreamingSystem::buildChunkConfigFromObjectAndKey(const SceneObject& chunkObj, const PlanetChunkKey& key, WorldSystem* worldSystem) {
    PlanetarySystem::ChunkConfig cc;
    cc.face = key.face;
    cc.lod = key.lod;
    cc.tileX = key.x;
    cc.tileY = key.y;

    if (chunkObj.properties.is_object() && chunkObj.properties.contains("terrainEditor")) {
        const auto& te = chunkObj.properties["terrainEditor"];
        if (te.is_object()) {
            cc.face = te.value("chunkFace", cc.face);
            cc.lod = te.value("chunkLod", cc.lod);
            cc.tileX = te.value("chunkX", cc.tileX);
            cc.tileY = te.value("chunkY", cc.tileY);
            int tilesX = std::max(1, te.value("chunkTilesX", 1));
            int tilesY = std::max(1, te.value("chunkTilesY", 1));
            cc.tilesPerFace = std::max(tilesX, tilesY);
        }
    }

    cc.tilesPerFace = std::max(1, cc.tilesPerFace);

    // Tiles per face es consistente para todos los LOD
    // LOD ya controla la subdivisión: LOD 0 = mayor resolución, LOD 4+ = menor
    // No multiplicamos tiles por LOD, eso causa sobreposición
    // Solo usamos el valor base configurado
    if (cc.tilesPerFace <= 0) cc.tilesPerFace = 16;

    if (worldSystem) {
        worldSystem->getNeighborLods(key, cc.neighborLodN, cc.neighborLodS, cc.neighborLodE, cc.neighborLodW);
    } else {
        cc.neighborLodN = cc.lod;
        cc.neighborLodS = cc.lod;
        cc.neighborLodE = cc.lod;
        cc.neighborLodW = cc.lod;
    }
    return cc;
}

uint32_t TerrainStreamingSystem::calculateChunkSizeBytes(const SceneObject& chunk) {
    if (!chunk.meshRenderer) return 0;
    const auto& verts = chunk.meshRenderer->getSourceVertices();
    const auto& inds = chunk.meshRenderer->getSourceIndices();
    uint32_t vertBytes = verts.size() * sizeof(glm::vec3);
    uint32_t normBytes = verts.size() * sizeof(glm::vec3);
    uint32_t indBytes = inds.size() * sizeof(unsigned int);
    return vertBytes + normBytes + indBytes;
}

std::string TerrainStreamingSystem::makeChunkCollisionId(const PlanetChunkKey& key) {
    return "chunk_col_f" + std::to_string(key.face)
        + "_l" + std::to_string(key.lod)
        + "_x" + std::to_string(key.x)
        + "_y" + std::to_string(key.y);
}

void TerrainStreamingSystem::buildCollisionProxy(const std::vector<glm::vec3>& verts,
                                                 const std::vector<unsigned int>& inds,
                                                 std::vector<glm::vec3>& outVerts,
                                                 std::vector<unsigned int>& outInds) {
    outVerts = verts;
    outInds.clear();
    if (inds.empty()) return;

    const size_t triCount = inds.size() / 3;
    const size_t targetTris = 4000;
    const size_t stride = std::max<size_t>(1, triCount / targetTris);
    outInds.reserve((triCount / stride) * 3);
    for (size_t t = 0; t < triCount; t += stride) {
        const size_t i = t * 3;
        if (i + 2 >= inds.size()) break;
        outInds.push_back(inds[i]);
        outInds.push_back(inds[i + 1]);
        outInds.push_back(inds[i + 2]);
    }
}

void TerrainStreamingSystem::pollChunkGenerationJobs() {
    // Limpiar jobs completados
    for (auto it = chunkGenJobs.begin(); it != chunkGenJobs.end();) {
        if (it->second.valid() && it->second.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            try {
                if (chunkReadyData.size() < MAX_READY_CHUNKS_CACHED) {
                    chunkReadyData[it->first] = it->second.get();
                } else {
                    it->second.get();  // Descartar
                }
            } catch (...) {
                // Ignorar errores
            }
            it = chunkGenJobs.erase(it);
        } else {
            ++it;
        }
    }

    // Limpiar datos listos excedentes
    while (chunkReadyData.size() > MAX_READY_CHUNKS_CACHED) {
        chunkReadyData.erase(chunkReadyData.begin());
    }
}

void TerrainStreamingSystem::invalidateStaleChunkJobs(uint64_t newSceneVersion) {
    if (currentSceneVersion != newSceneVersion) {
        chunkGenJobs.clear();
        chunkReadyData.clear();
        currentSceneVersion = newSceneVersion;
        currentSeedGeneration++;  // Incrementar también cuando cambia escena
    }
}

void TerrainStreamingSystem::update(Scene* scene, WorldSystem* worldSystem, PlanetarySystem* planetarySystem, RaycastSimple* raycastSystem, Camera* camera, TerrainStreamingStats* outStats) {
    pollChunkGenerationJobs();
    if (!scene || !worldSystem || !camera) return;

    const uint64_t sceneVersion = reinterpret_cast<uint64_t>(scene);
    const bool sceneChanged = (currentSceneVersion != sceneVersion);
    invalidateStaleChunkJobs(sceneVersion);
    
    // Detectar cambios de semilla sin cambio de escena
    auto checkSeedChange = [this](Scene* scene) {
        int planetIdx = findPlanetRootIndex(scene);
        if (planetIdx >= 0 && planetIdx < static_cast<int>(scene->getObjects().size())) {
            SceneObject& root = scene->getObjects()[planetIdx];
            if (root.properties.contains("terrainEditor") &&
                root.properties["terrainEditor"].contains("generator")) {
                uint32_t currentSeed = root.properties["terrainEditor"]["generator"].value("seed", 0u);
                if (currentSeed != lastCheckedSeed && lastCheckedSeed != 0) {
                    // Semilla cambió: invalidar chunks
                    invalidateChunksForSeedChange();
                    HARUKA_SCENE_ERROR(ErrorCode::INVALID_OBJECT,
                        "TerrainStreaming: seed changed " + std::to_string(lastCheckedSeed)
                        + " -> " + std::to_string(currentSeed) + ", chunks invalidated");
                }
                lastCheckedSeed = currentSeed;
            }
        }
    };
    checkSeedChange(scene);
    
    if (raycastSystem && sceneChanged) {
        raycastSystem->clearMeshes();
    }

    ensureTerrainChunkKeysAndGrid(scene, worldSystem);

    // Sync planet center so WorldSystem computes chunk positions relative to the planet,
    // not the world origin. This is critical when the planet is not at (0,0,0).
    {
        int planetIdx = findPlanetRootIndex(scene);
        if (planetIdx >= 0 && planetIdx < static_cast<int>(scene->getObjects().size())) {
            const SceneObject& planetObj = scene->getObjects()[planetIdx];
            worldSystem->setPlanetCenter(planetObj.getWorldPosition(scene));
        }
    }

    // Dynamic streaming budgets based on camera distance to planet
    // Goal: load chunks continuously, evict only distant ones
    // Calculate optimal budgets: more loads than evicts to expand coverage
    int loadsPerFrame = 256;     // Increased: 256 chunks/frame
    int evictsPerFrame = 8;      // Very conservative eviction
    
    // Detectar RAM del sistema y usar casi toda para chunks
    uint64_t totalSystemMemory = getSystemMemoryBytes();
    uint64_t osReservedMemory = 2ull * 1024 * 1024 * 1024;  // Reserve 2GB para SO
    uint64_t availableMemory = (totalSystemMemory > osReservedMemory) 
        ? (totalSystemMemory - osReservedMemory) 
        : (totalSystemMemory / 2);  // Use 50% si no hay suficiente
    
    // 80% para resident chunks, 20% para pending
    size_t maxResidentMB = static_cast<size_t>((availableMemory * 80) / (100 * 1024 * 1024));
    size_t maxPendingMB = static_cast<size_t>((availableMemory * 20) / (100 * 1024 * 1024));
    
    worldSystem->setChunkStreamingBudgets(loadsPerFrame, evictsPerFrame, maxResidentMB, maxPendingMB);
    worldSystem->updateLocalPositions(camera->position);
        
    // Extended view distance for better LOD transitions and less pop-in
    float maxViewDistance = 60000000.0f;  // 60 000 km view distance
    worldSystem->frustumCull(camera->position, camera->getViewMatrix(), maxViewDistance);
    
    // Aggressive chunk loading: load chunks en dirección de la cámara
    // Solo cargar chunks que estén en el hemisferio hacia el que mira la cámara
    float loadDistance = 30000000.0f;    // 30 000 km load radius
    float unloadDistance = 45000000.0f;  // 45 000 km unload radius
    
    // Pasar pointer a camera directamente sin extraer variables
    worldSystem->updateVisibleChunks(loadDistance, 0, camera);
    worldSystem->scheduleChunkStreaming();

    if (outStats) {
        outStats->visibleChunks = static_cast<int>(worldSystem->getVisibleChunks().size());
        outStats->residentChunks = static_cast<int>(worldSystem->getResidentChunkCount());
        outStats->pendingChunkLoads = static_cast<int>(worldSystem->getPendingLoads().size());
        outStats->pendingChunkEvictions = static_cast<int>(worldSystem->getPendingEvictions().size());
        outStats->residentMemoryMB = static_cast<int>(worldSystem->getResidentMemoryMB());
        outStats->trackedChunks = static_cast<int>(worldSystem->getTrackedChunkCount());
        outStats->maxMemoryMB = static_cast<int>(worldSystem->getMaxMemoryMB());
    }

    const auto& loads = worldSystem->getPendingLoads();
    const auto& evicts = worldSystem->getPendingEvictions();

    for (const auto& key : loads) {
        SceneObject* chunk = findTerrainChunkByKey(scene, key);
        if (!chunk) chunk = createTerrainChunkForKey(scene, key);
        if (!chunk || !chunk->meshRenderer) continue;

        const auto& verts = chunk->meshRenderer->getSourceVertices();
        const auto& norms = chunk->meshRenderer->getSourceNormals();
        const auto& inds = chunk->meshRenderer->getSourceIndices();
        if (!verts.empty() && !inds.empty()) {
            chunk->meshRenderer->setMesh(verts, norms, inds);
        } else {
            auto readyIt = chunkReadyData.find(key);
            if (readyIt != chunkReadyData.end()) {
                const auto& cd = readyIt->second;
                if (!cd.vertices.empty() && !cd.indices.empty()) {
                    chunk->meshRenderer->setMesh(cd.vertices, cd.normals, cd.indices);
                }
                chunkReadyData.erase(readyIt);
            } else if (chunkGenJobs.find(key) == chunkGenJobs.end()) {
                PlanetarySystem::PlanetConfig cfg = buildPlanetConfigFromChunkSource(*chunk, scene);
                cfg.useGPU = false;
                PlanetarySystem::ChunkConfig cc = buildChunkConfigFromObjectAndKey(*chunk, key, worldSystem);

                // Se asume que planetarySystem* está accesible (ajustar si es necesario)
                PlanetChunkKey capturedKey = key;
                chunkGenJobs[key] = std::async(std::launch::async, [cfg, cc, capturedKey, planetarySystem]() mutable {
                    uint32_t posHash = (capturedKey.face * 1000000u) + (capturedKey.lod * 10000u) + (capturedKey.x * 100u) + capturedKey.y;
                    PlanetarySystem::PlanetConfig localCfg = cfg;
                    localCfg.seedBase = localCfg.seedBase + posHash;
                    localCfg.seedContinents = localCfg.seedContinents + posHash;
                    localCfg.seedMacro = localCfg.seedMacro + posHash;
                    localCfg.seedDetail = localCfg.seedDetail + posHash;
                    return planetarySystem->generateChunk(localCfg, cc, "Earth");
                });
                continue;
            } else {
                continue;
            }
        }

        const uint32_t chunkBytes = calculateChunkSizeBytes(*chunk);
        worldSystem->setChunkSize(key, chunkBytes);
        worldSystem->markChunkResident(key, true);

        if (raycastSystem) {
            std::vector<glm::vec3> proxyVerts;
            std::vector<unsigned int> proxyInds;
            buildCollisionProxy(chunk->meshRenderer->getSourceVertices(), chunk->meshRenderer->getSourceIndices(), proxyVerts, proxyInds);
            if (!proxyVerts.empty() && !proxyInds.empty()) {
                raycastSystem->addMesh(makeChunkCollisionId(key), proxyVerts, proxyInds);
            }
        }
    }

    for (const auto& key : evicts) {
        SceneObject* chunk = findTerrainChunkByKey(scene, key);
        if (!chunk || !chunk->meshRenderer) continue;

        chunk->meshRenderer->releaseMesh();
        worldSystem->markChunkResident(key, false);
        if (raycastSystem) raycastSystem->removeMesh(makeChunkCollisionId(key));

        chunkReadyData.erase(key);
        chunkGenJobs.erase(key);
    }

    if (chunkReadyData.size() > 64) {
        const auto& visibleKeys = worldSystem->getVisibleChunks();
        std::vector<PlanetChunkKey> keysToErase;
        for (const auto& [key, _] : chunkReadyData) {
            bool found = false;
            for (const auto& vk : visibleKeys) {
                if (vk.face == key.face && vk.lod == key.lod && vk.x == key.x && vk.y == key.y) {
                    found = true;
                    break;
                }
            }
            if (!found) keysToErase.push_back(key);
        }
        for (const auto& key : keysToErase) {
            chunkReadyData.erase(key);
        }
    }
}

} // namespace Haruka
