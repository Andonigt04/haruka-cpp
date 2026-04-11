#pragma once
#include "core/world_system.h"
#include "core/math_types.h"
#include "character.h"
#include "core/scene.h"
#include "renderer/terrain.h"
#include "renderer/shader.h"
#include <memory>
#include <unordered_map>

namespace Haruka {

/**
 * @brief Coordinates planetary bodies, orbital simulation, terrain, and player flight state.
 */
class PlanetarySystem {
public:
    
    // --- Tipos internos para generación procedural ---
    struct PlanetConfig {
        float radius = 1.0f;
        int subdivisions = 4;
        float baseRadiusKm = 6371.0f;
        int seedBase = 42;
        int seedContinents = 1337;
        int seedMacro = 2024;
        int seedDetail = 9001;
        bool enableContinents = true;
        bool enableMountains = true;
        bool useGPU = true;
        float seaLevel = 0.52f;
        float continentFrequency = 1.2f;
        float continentWarpStrength = 0.15f;
        float continentHeightStrength = 0.02f;
        int octavesContinents = 5;
        float macroFrequency = 2.0f;
        float macroHeightStrength = 0.01f;
        int octavesMacro = 5;
        float detailFrequency = 8.0f;
        float detailHeightStrength = 0.002f;
        int octavesDetail = 4;
        float persistence = 0.5f;
        float lacunarity = 2.0f;
    };
    struct ChunkConfig {
        int face = 0;
        int lod = 0;
        int tileX = 0;
        int tileY = 0;
        int tilesPerFace = 1;
        int neighborLodN = 0;
        int neighborLodS = 0;
        int neighborLodE = 0;
        int neighborLodW = 0;
    };
    struct PlanetData {
        std::vector<glm::vec3> vertices;
        std::vector<glm::vec3> normals;
        std::vector<unsigned int> indices;
        float radius = 0.0f;
        float minHeight = 0.0f;
        float maxHeight = 0.0f;
    };
    struct ChunkData {
        std::vector<glm::vec3> vertices;
        std::vector<glm::vec3> normals;
        std::vector<unsigned int> indices;
        float minHeight = 0.0f;
        float maxHeight = 0.0f;
    };
    /** @brief Constructs an uninitialized planetary system. */
    PlanetarySystem();
    /** @brief Releases owned runtime resources. */
    ~PlanetarySystem();
    
    /** @brief Binds the scene and world systems used by the simulation. */
    void init(Scene* scene, WorldSystem* worldSystem);
    /** @brief Advances orbital/player simulation by one timestep. */
    void update(double dt);

    /** @name Terrain setup */
    ///@{
    void initTerrain(int size = 1024, float heightScale = 200.0f, int seed = 42);
    void renderTerrain(Shader& shader, const Camera* camera);
    void setDetailedSurfaceData(const std::string& bodyName, const PlanetData& data);
    ///@}
    
    /** @name Celestial body creation */
    ///@{
    CelestialBody* addStar(const std::string& name, double mass = 1.989e30, double radius = Units::STAR_RADIUS_MEDIUM);
    CelestialBody* addPlanet(const std::string& name, double orbitalDistance, double mass, double radius, glm::vec3 color = glm::vec3(1.0f));
    CelestialBody* addBody(const CelestialBody& body);
    ///@}
    
    /** @name Accessors */
    ///@{
    Scene* getScene() { return scene; }
    WorldSystem* getWorldSystem() { return worldSystem; }
    CelestialBody* findBody(const std::string& name);
    CelestialBody* getStar() { return star; }
    CelestialBody* getClosestPlanet();
    
    Character* getPlayer() { return player.get(); }
    void setPlayer(std::unique_ptr<Character> p) { player = std::move(p); }
    Terrain* getTerrain() { return terrain.get(); }
    ///@}
    
    /** @name Simulation configuration */
    ///@{
    void setTimeScale(double scale) { timeScale = scale; }
    double getTimeScale() const { return timeScale; }
    ///@}
    
    /** @name Planetary physics */
    ///@{
    double calculateGravityAtPosition(const glm::dvec3& worldPos, glm::dvec3& gravityDirection);
    void applyPlanetaryPhysics(double dt);
    void setPlayerFlightMode(bool enabled);
    ///@}
    
    // Genera y almacena el planeta completo (malla base)
    void generatePlanet(const PlanetConfig& config, const std::string& bodyName);
    ChunkData generateChunk(const PlanetConfig& config, const ChunkConfig& chunk, const std::string& bodyName);
    const PlanetData* getPlanetData(const std::string& bodyName) const;
    const ChunkData* getChunkData(const std::string& bodyName, const ChunkConfig& chunk) const;
    
private:
    Scene* scene = nullptr;
    WorldSystem* worldSystem = nullptr;
    std::unique_ptr<Character> player;
    std::unique_ptr<Terrain> terrain;
    std::unordered_map<std::string, PlanetData> detailedSurfaceData;
    
    CelestialBody* star = nullptr;
    std::vector<std::string> bodyNames;
    
    double simulationTime = 0.0;
    double timeScale = 1000.0;
    
    const double G = 6.67430e-11;
    
    /** @brief Synchronizes scene object transforms with orbital simulation state. */
    void syncSceneWithOrbits();
    /** @brief Updates player attachment/orientation on a planet surface. */
    void updatePlayerOnPlanet();
    /** @brief Updates world origin based on current simulation focus. */
    void updateWorldOrigin();
    /** @brief Integrates orbital positions for all managed bodies. */
    void integrateOrbits(double dt);
    /** @brief Samples a generated body surface radius in a given direction. */
    double getSurfaceRadiusAtDirection(const CelestialBody* body, const glm::dvec3& planetToPoint) const;
    PlanetData generatePlanetInternal(const PlanetConfig& config);
    ChunkData generateChunkInternal(const PlanetConfig& config, const ChunkConfig& chunk);
};

inline bool operator<(const PlanetarySystem::ChunkConfig& a, const PlanetarySystem::ChunkConfig& b) {
    return std::tie(a.face, a.lod, a.tileX, a.tileY, a.tilesPerFace, a.neighborLodN, a.neighborLodS, a.neighborLodE, a.neighborLodW) < std::tie(b.face, b.lod, b.tileX, b.tileY, b.tilesPerFace, b.neighborLodN, b.neighborLodS, b.neighborLodE, b.neighborLodW);
}

}