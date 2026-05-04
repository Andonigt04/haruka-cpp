/**
 * @file planetary_system.h
 * @brief Planetary body simulation — orbital integration, terrain generation, and player flight state.
 */
#pragma once
#include "core/world_system.h"
#include "core/math_types.h"
#include "character.h"
#include "core/scene.h"
#include "renderer/terrain.h"
#include "renderer/shader.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <glm/glm.hpp>

namespace Haruka {

/**
 * @brief Coordinates planetary bodies, orbital simulation, terrain, and player flight state.
 */
class PlanetarySystem {
public:
    
    // --- Nueva Estructura de Configuración (Matches JSON) ---
    
    struct LayerSettings {
        float freq = 1.0f;
        float strength = 1.0f;
        int octaves = 5;
        float warp = 0.0f; // Solo usado en continentes mayormente
    };

    struct PlanetConfig {
        // [config]
        int seedBase = 42;
        float baseRadiusKm = 6371.0f;
        float seaLevel = 0.52f;
        bool enableContinents = true;
        bool enableMountains = true;
        
        // [fractal]
        float persistence = 0.5f;
        float lacunarity = 2.0f;

        // [layers]
        LayerSettings continents;
        LayerSettings macro;
        LayerSettings detail;

        // legacy/internal (puedes mantenerlos o eliminarlos si ya no usas subdivisions directas)
        int subdivisions = 4;
        bool useGPU = true;
    };

    struct ChunkConfig {
        int face = 0;
        int lod = 0;
        int tileX = 0;
        int tileY = 0;
        int tilesPerFace = 1;
        // Datos de costura (LOD adyacentes)
        int neighborLodN = 0;
        int neighborLodS = 0;
        int neighborLodE = 0;
        int neighborLodW = 0;
    };

    struct PlanetData {
        std::vector<glm::vec3> vertices;
        std::vector<glm::vec3> normals;
        std::vector<glm::vec3> colors; // Añadido para Biomas
        std::vector<unsigned int> indices;
        float radius = 0.0f;
        float minHeight = 0.0f;
        float maxHeight = 0.0f;
    };

    struct ChunkData {
        std::vector<glm::vec3> vertices;
        std::vector<glm::vec3> normals;
        std::vector<glm::vec3> colors; // Añadido para Biomas
        std::vector<unsigned int> indices;
        float minHeight = 0.0f;
        float maxHeight = 0.0f;
    };

    // --- Resto de la Clase ---

    PlanetarySystem();
    ~PlanetarySystem();
    
    void init(Scene* scene, WorldSystem* worldSystem);
    void update(double dt);

    /** @name Terrain setup */
    ///@{
    void initTerrain(int size = 1024, float heightScale = 200.0f, int seed = 42);
    void renderTerrain(Shader& shader, const Camera* camera);
    void setDetailedSurfaceData(const std::string& bodyName, const PlanetData& data);
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
    
    void syncSceneWithOrbits();
    void updatePlayerOnPlanet();
    void updateWorldOrigin();
    void integrateOrbits(double dt);
    double getSurfaceRadiusAtDirection(const CelestialBody* body, const glm::dvec3& planetToPoint) const;
    
    PlanetData generatePlanetInternal(const PlanetConfig& config);
    ChunkData generateChunkInternal(const PlanetConfig& config, const ChunkConfig& chunk);
};

// Operador de comparación para usar ChunkConfig como clave en mapas (std::map<ChunkConfig, ...>)
inline bool operator<(const PlanetarySystem::ChunkConfig& a, const PlanetarySystem::ChunkConfig& b) {
    return std::tie(a.face, a.lod, a.tileX, a.tileY, a.tilesPerFace, a.neighborLodN, a.neighborLodS, a.neighborLodE, a.neighborLodW) < 
           std::tie(b.face, b.lod, b.tileX, b.tileY, b.tilesPerFace, b.neighborLodN, b.neighborLodS, b.neighborLodE, b.neighborLodW);
}

} // namespace Haruka