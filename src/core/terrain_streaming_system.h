#pragma once

#include <future>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "math_types.h"
#include "scene.h"
#include "world_system.h"
#include "camera.h"
#include "game/planetary_system.h"

class RaycastSimple;
class Camera;

namespace Haruka {

class MaterialComponent;

struct TerrainStreamingStats {
    int visibleChunks = 0;
    int residentChunks = 0;
    int pendingChunkLoads = 0;
    int pendingChunkEvictions = 0;
    int residentMemoryMB = 0;
    int trackedChunks = 0;
    int maxMemoryMB = 0;
};

class TerrainStreamingSystem {
public:
    /**
     * @brief Updates terrain streaming based on camera state.
     * @param scene Active scene with terrain chunks
     * @param worldSystem World/chunk management system
     * @param raycastSystem Physics raycast system for collisions
     * @param camera Camera pointer for position, orientation, and FOV
     * @param outStats Optional stats output (visible chunks, memory, etc)
     * 
     * BENEFITS of passing Camera* directly:
     * - No memory copies of position/direction/matrix
     * - Direct access to all camera state
     * - Camera always current (real-time reference)
     * - Fewer function parameters
     */
    void update(Scene* scene, WorldSystem* worldSystem, PlanetarySystem* planetarySystem, RaycastSimple* raycastSystem, Camera* camera, TerrainStreamingStats* outStats = nullptr);
    
    // Invalidar chunks cuando cambia la semilla (sin cambiar escena)
    void invalidateChunksForSeedChange() {
        currentSeedGeneration++;  // Incrementar sin limpiar
    }

private:
    static constexpr size_t MAX_READY_CHUNKS_CACHED = 64;

    struct TerrainChunkTemplate {
        std::string sourceName = "Earth";
        int chunkTilesY = 16;
        int chunkTilesX = 16;
        std::string type = "Mesh";
        glm::dvec3 color = glm::dvec3(1.0);
        double intensity = 1.0;
        int parentIndex = -1;
        std::shared_ptr<Haruka::MaterialComponent> material;
    };

    std::map<PlanetChunkKey, std::future<Haruka::PlanetarySystem::ChunkData>> chunkGenJobs;
    std::map<PlanetChunkKey, Haruka::PlanetarySystem::ChunkData> chunkReadyData;
    uint64_t currentSceneVersion = 0;
    uint32_t lastCheckedSeed = 0;  // Detectar cambios de semilla
    uint32_t currentSeedGeneration = 0;  // Versión de semilla actual para invalidación

    static bool isTerrainChunkObject(const SceneObject& obj);
    static SceneObject* findTerrainChunkByKey(Scene* scene, const PlanetChunkKey& key);
    static bool buildTerrainChunkTemplate(Scene* scene, TerrainChunkTemplate& outTpl);
    static SceneObject* createTerrainChunkForKey(Scene* scene, const PlanetChunkKey& key);
    static int findPlanetRootIndex(Scene* scene);
    static void addChunkAsChild(Scene* scene, SceneObject& chunk, int parentIdx);
    static void ensureTerrainChunkKeysAndGrid(Scene* scene, WorldSystem* worldSystem);
    static Haruka::PlanetarySystem::PlanetConfig buildPlanetConfigFromChunkSource(const SceneObject& chunkObj, Scene* scene);
    static Haruka::PlanetarySystem::ChunkConfig buildChunkConfigFromObjectAndKey(const SceneObject& chunkObj, const PlanetChunkKey& key, WorldSystem* worldSystem);
    static uint32_t calculateChunkSizeBytes(const SceneObject& chunk);
    static std::string makeChunkCollisionId(const PlanetChunkKey& key);
    static void buildCollisionProxy(const std::vector<glm::vec3>& verts,
                                    const std::vector<unsigned int>& inds,
                                    std::vector<glm::vec3>& outVerts,
                                    std::vector<unsigned int>& outInds);

    void pollChunkGenerationJobs();
    void invalidateStaleChunkJobs(uint64_t newSceneVersion);
};

} // namespace Haruka
