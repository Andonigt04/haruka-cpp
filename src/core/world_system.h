#ifndef WORLD_SYSTEM_H
#define WORLD_SYSTEM_H

#include "math_types.h"
#include "camera.h"
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <map>
#include <cstdint>

struct CelestialBody
{
    /** @brief High-precision world-space position. */
    Haruka::WorldPos worldPos;
    /** @brief Camera-relative/local position cache. */
    Haruka::LocalPos localPos;
    glm::vec3 velocity;
    float radius;
    float mass;
    std::string name;
    glm::vec3 color;
    float emissionStrength;
    uint32_t visible;
    uint32_t lodLevel;
};

namespace Haruka {

/** @brief Planet chunk identifier in cube-sphere space. */
struct PlanetChunkKey {
    int face = 0;
    int lod = 0;
    int x = 0;
    int y = 0;

    bool operator<(const PlanetChunkKey& other) const {
        if (lod != other.lod) return lod < other.lod;
        if (face != other.face) return face < other.face;
        if (y != other.y) return y < other.y;
        return x < other.x;
    }
};

/** @brief Runtime state for one streamed chunk. */
struct PlanetChunkState {
    bool visible = false;
    bool resident = false;
    float distanceKm = 0.0f;
    uint64_t lastTouchedFrame = 0;
    uint32_t bytesUsed = 0;
};

/**
 * @brief Manages celestial body storage, origin shifting, and culling metadata.
 */
class WorldSystem {
public:
    WorldSystem();
    ~WorldSystem();

    /**
     * @brief Calcula los LODs de los vecinos inmediatos de un chunk.
     * @param key Clave del chunk actual
     * @param outN, outS, outE, outW Referencias de salida para los LODs vecinos
     * @note Esqueleto, implementar lógica real según el mapa de chunks visibles.
     */
    void getNeighborLods(const PlanetChunkKey& key, int& outN, int& outS, int& outE, int& outW) const;

    /** @brief Converts world-space position into local-space relative to reference. */
    static Haruka::LocalPos toLocal(Haruka::WorldPos objectPos, Haruka::WorldPos referencePos);

    /** @brief Updates world origin anchor. */
    void updateOrigin(Haruka::WorldPos newOrigin);
    /** @brief Recomputes local positions for all registered bodies. */
    void updateLocalPositions(Haruka::WorldPos cameraWorldPos);

    /** @brief Adds a celestial body copy to internal storage. */
    void addBody(const CelestialBody& body);
    /** @brief Removes celestial body by name if present. */
    void removeBody(const std::string& name);
    /** @brief Finds body by name and returns mutable non-owning pointer. */
    CelestialBody* findBody(const std::string& name);

    /** @name Accessors */
    ///@{
    const std::vector<CelestialBody>& getBodies() const;
    size_t getBodyCount() const;
    Haruka::WorldPos getOrigin() const;
    ///@}

    /** @brief Initializes compute resources for culling path. */
    void initComputeShaders();
    /** @brief Sets LOD transition distances. */
    void setLODDistances(float lod0, float lod1, float lod2, float lod3);
    /** @brief Executes frustum culling update for registered bodies. */
    void frustumCull(Haruka::WorldPos cameraPos, const glm::mat4& viewProj, float frustumDistance);

    /** @brief Sets chunk streaming budgets (loads/evicts/max resident count and memory in MB).
     * 
     * CHUNK EVICTION POLICY (automático en scheduleChunkStreaming):
     * 
     * DECISIÓN: Un chunk se descarga si:
     * 1. Es residiente (está en memoria)
     * 2. No es visible (out-of-view, lejano, fuera de frustum)
     * 3. PRESIÓN DE MEMORIA:
     *    - residentMemoryBytes > maxMemoryBytes
     *    - Liberar chunks hasta bajar de límite
     * 4. LÍMITE DE CHUNKS:
     *    - getResidentChunkCount() > maxResidentChunks
     *    - Liberar chunks hasta bajar de límite
     * 5. PRIORIDAD LRU + DISTANCE:
     *    - Eliminar primero: lastTouchedFrame más viejo (LRU)
     *    - Tie-break: distanceKm más grande (distante)
     *    - Suave: max maxEvictsPerFrame por frame
     * 
     * LÍMITES TÍPICOS:
     * - maxLoadsPerFrame: 16 (cargar 16 chunks/frame máximo)
     * - maxEvictsPerFrame: 16 (descargar 16 chunks/frame máximo)
     * - maxResidentChunks: 256 (256 chunks máximo en memoria)
     * - maxMemoryMB: 512 (512 MB máximo para chunks)
     */
    void setChunkStreamingBudgets(size_t maxLoadsPerFrame, size_t maxEvictsPerFrame, size_t maxResidentChunks, size_t maxMemoryMB = 512);
    /** @brief Sets active chunk grid dimensions/key-space for visibility generation. */
    void setChunkGrid(int face, int lod, int tilesX, int tilesY, int maxLod = 0);
    /** @brief Sets the planet center in world space (default is origin). */
    void setPlanetCenter(Haruka::WorldPos center) { planetCenter = center; }
    /** @brief Returns the current planet center in world space. */
    Haruka::WorldPos getPlanetCenter() const { return planetCenter; }
    /** @brief Updates current visible chunk set from camera state. */
    void updateVisibleChunks(float viewDistanceKm, int lod, Camera* camera = nullptr);

    /**
     * @brief Indica si se debe renderizar solo el mesh base (sin chunks detallados).
     */
    bool shouldRenderBaseMesh() const { return renderBaseMeshOnly; }
    /** @brief Builds load/evict queues from current visibility and budgets. */
    void scheduleChunkStreaming();
    /** @brief Marks one chunk as resident/non-resident after upload/eviction. */
    void markChunkResident(const PlanetChunkKey& key, bool resident = true);
    /** @brief Stores CPU-side size for a chunk. */
    void setChunkSize(const PlanetChunkKey& key, uint32_t bytes);

    /** @name Chunk streaming queues */
    ///@{
    const std::vector<PlanetChunkKey>& getVisibleChunks() const { return visibleChunks; }
    const std::vector<PlanetChunkKey>& getPendingLoads() const { return pendingChunkLoads; }
    const std::vector<PlanetChunkKey>& getPendingEvictions() const { return pendingChunkEvictions; }
    size_t getResidentChunkCount() const;
    size_t getTrackedChunkCount() const { return chunkStates.size(); }
    size_t getResidentMemoryMB() const { return residentMemoryBytes / (1024 * 1024); }
    size_t getMaxMemoryMB() const { return maxMemoryBytes / (1024 * 1024); }
    ///@}

private:
    Haruka::WorldPos worldOrigin;
    Haruka::WorldPos planetCenter{0.0, 0.0, 0.0};
    std::vector<CelestialBody> celestialBodies;
    float lodDistances[4];

    // Radios de cúpulas LOD (en unidades)
    float domeRadius0 = 1000.0f;   // Máxima calidad
    float domeRadius1 = 5000.0f;   // Media
    float domeRadius2 = 20000.0f;  // Mínima

    // Flag para indicar si solo se debe renderizar el mesh base
    bool renderBaseMeshOnly = false;

    // Chunk streaming state
    std::map<PlanetChunkKey, PlanetChunkState> chunkStates;
    std::vector<PlanetChunkKey> visibleChunks;
    std::vector<PlanetChunkKey> pendingChunkLoads;
    std::vector<PlanetChunkKey> pendingChunkEvictions;
    size_t maxLoadsPerFrame = 16;
    size_t maxEvictsPerFrame = 16;
    size_t maxResidentChunks = 2048;
    size_t maxMemoryBytes = 512 * 1024 * 1024;
    size_t residentMemoryBytes = 0;
    uint64_t chunkFrameCounter = 0;
    int chunkGridFace = 0;
    int chunkGridLod = 0;
    int chunkGridTilesX = 0;
    int chunkGridTilesY = 0;
    int chunkGridMaxLod = 0;
};
}

#endif