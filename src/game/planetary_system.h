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
    
    /**
     * @brief El corazón del sistema: actualiza órbitas y dispara el LOD/Streaming.
     */
    void update(double dt, const glm::dvec3& cameraPos);

    void addPlanet(const Planet& planet);

private:
    // Componentes del motor de terreno (Los "músculos")
    std::unique_ptr<ChunkCache> m_cache;
    std::unique_ptr<TerrainGenerator> m_generator;
    std::unique_ptr<TerrainRenderer> m_renderer;
    std::unique_ptr<TerrainStreamingSystem> m_streaming;
    std::unique_ptr<LODSystem> m_lod;

    // Datos del universo
    std::vector<Planet> m_planets;
    double m_simulationTime = 0.0;
    const double G = 6.67430e-11;

    void updateOrbits(double dt);
};

}