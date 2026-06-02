#pragma once

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "tools/math_types.h"
#include "core/scene/scene_manager.h"

namespace Haruka {

class ChunkCache;
class TerrainGenerator;
class TerrainRenderer;
class WaterRenderer;
class TerrainStreamingSystem;
class LODSystem;

class PlanetarySystem {
public:
    PlanetarySystem();
    ~PlanetarySystem();

    // Estructura limpia para un planeta
    struct Planet {
        std::string name;
        Haruka::WorldPos position;
        double radius;
        nlohmann::json terrainSettings; // Semilla, capas de ruido, etc.
        // Aquí podrías añadir parámetros orbitales (semi-eje mayor, etc.)
    };

    void init();

    /** @brief Actualiza órbitas, LOD y streaming. */
    void update(double dt, const glm::dvec3& cameraPos);

    void addPlanet(const Planet& planet);

    /** @brief Renderiza el terrain de un planeta (shader ya activo, UBO model ya subido). */
    void renderPlanetTerrain(const std::string& planetName, const glm::dvec3& cameraPos);

    /** @brief Renderiza el océano de un planeta (shader de agua ya activo). */
    void renderPlanetWater(const std::string& planetName, const glm::dvec3& cameraPos);

    /** @brief Sets camera-relative VP for frustum culling terrain+water chunks. */
    void setTerrainCullMatrix(const glm::mat4& camRelViewProj);

    int getGPUWaterChunkCount() const;

    const std::vector<Planet>& getPlanets() const { return m_planets; }
          std::vector<Planet>& getPlanets()       { return m_planets; }

    void syncFromScene(const SceneManager& scene);

    int getGPUChunkCount()    const;
    int getPendingChunks()    const;
    int getCachedChunks()     const;
    int getCacheMemoryMB()    const;
    int getCacheMaxMemoryMB() const;

    struct TerrainDrawStats { int draws = 0; int vertices = 0; int triangles = 0; };
    TerrainDrawStats getTerrainDrawStats() const;

    /**
     * @brief Returns the terrain height (in metres) above the reference sphere
     *        surface at the given world position, for the nearest planet.
     *        Returns 0 if no planet is found or terrain settings are missing.
     */
    double sampleTerrainHeight(const glm::dvec3& worldPos) const;

    /**
     * @brief Mean sea surface for the nearest planet (sea level = planet radius).
     * @return true if a planet was found.
     */
    bool getSeaSurface(const glm::dvec3& worldPos, glm::dvec3& outCenter, double& outSeaRadius) const;

private:
    // Componentes del motor de terreno (Los "músculos")
    std::unique_ptr<ChunkCache> m_cache;
    std::unique_ptr<TerrainGenerator> m_generator;
    std::unique_ptr<TerrainRenderer> m_renderer;
    std::unique_ptr<WaterRenderer> m_waterRenderer;
    std::unique_ptr<TerrainStreamingSystem> m_streaming;
    std::unique_ptr<LODSystem> m_lod;

    // Datos del universo
    std::vector<Planet> m_planets;
    double m_simulationTime = 0.0;
    const double G = 6.67430e-11;

    // LOD throttle: skip the (allocating) quadtree rebuild when the camera has
    // barely moved since the last recompute. Per-planet last cam pos; force on
    // first frame / new planet.
    std::vector<glm::dvec3> m_lastLODCamPos;
    bool m_forceLOD = true;

    void updateOrbits(double dt);
};

}