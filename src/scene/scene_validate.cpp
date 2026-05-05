/**
 * @file scene_loader.cpp
 * @brief Implementation of scene loading with JSON validation.
 */

#include "scene_loader.h"
#include <fstream>
#include <sstream>

namespace Haruka {

SceneLoader::SceneLoader() = default;
SceneLoader::~SceneLoader() = default;

bool SceneLoader::loadScene(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        lastError = "Failed to open scene file: " + filepath;
        return false;
    }

    try {
        file >> sceneData;
    } catch (const std::exception& e) {
        lastError = "JSON parse error: " + std::string(e.what());
        return false;
    }

    if (!validateScene(sceneData)) {
        lastError = "Scene validation failed";
        return false;
    }

    return true;
}

bool SceneLoader::registerEntities(Scene* scene, WorldSystem* worldSystem, PlanetarySystem* planetarySystem) {
    if (!scene || !worldSystem || !planetarySystem) {
        lastError = "Invalid pointer: scene, worldSystem, or planetarySystem is null";
        return false;
    }

    try {
        // Process planets from the scene data
        if (sceneData.contains("planets") && sceneData["planets"].is_array()) {
            for (const auto& planetJson : sceneData["planets"]) {
                if (!planetJson.is_object()) {
                    lastError = "Invalid planet object in planets array";
                    return false;
                }

                // Parse planet configuration
                std::string planetName = planetJson.value("name", "UnnamedPlanet");
                PlanetarySystem::PlanetConfig config = jsonToPlanetConfig(planetJson);
                
                // Generate planet in PlanetarySystem
                planetarySystem->generatePlanet(config, planetName);

                // Register celestial body in WorldSystem
                CelestialBody body;
                body.name = planetName;
                body.radius = config.baseRadiusKm;
                body.mass = config.baseRadiusKm * 1e6f; // Approximate mass
                body.color = glm::vec3(0.5f, 0.7f, 1.0f); // Default blue color
                body.emissionStrength = 0.0f;
                body.worldPos = Haruka::WorldPos(0.0, 0.0, 0.0);
                worldSystem->addBody(body);
            }
        }

        // Process terrain chunks settings
        if (sceneData.contains("terrain") && sceneData["terrain"].is_object()) {
            const auto& terrainJson = sceneData["terrain"];
            if (!validateTerrainSettings(terrainJson)) {
                lastError = "Terrain settings validation failed";
                return false;
            }
            // Additional terrain setup can go here
        }

        return true;
    } catch (const std::exception& e) {
        lastError = "Entity registration error: " + std::string(e.what());
        return false;
    }
}

bool SceneLoader::validateScene(const nlohmann::json& sceneData) {
    // Require basic scene structure
    if (!sceneData.is_object()) {
        return false;
    }

    // Scene must have either planets or terrain defined
    bool hasPlanets = sceneData.contains("planets") && sceneData["planets"].is_array();
    bool hasTerrain = sceneData.contains("terrain") && sceneData["terrain"].is_object();
    
    if (!hasPlanets && !hasTerrain) {
        return false;
    }

    // Validate all planets if present
    if (hasPlanets) {
        for (const auto& planet : sceneData["planets"]) {
            if (!planet.is_object() || !planet.contains("name")) {
                return false;
            }
            if (!validatePlanetConfig(planet)) {
                return false;
            }
        }
    }

    // Validate terrain if present
    if (hasTerrain && !validateTerrainSettings(sceneData["terrain"])) {
        return false;
    }

    return true;
}

bool SceneLoader::validateTerrainSettings(const nlohmann::json& terrainSettings) {
    if (!terrainSettings.is_object()) {
        return false;
    }

    // Require generation and rendering sections
    if (!terrainSettings.contains("generation") || !terrainSettings["generation"].is_object()) {
        return false;
    }
    if (!terrainSettings.contains("rendering") || !terrainSettings["rendering"].is_object()) {
        return false;
    }

    // Validate generation settings
    const auto& generation = terrainSettings["generation"];
    if (!generation.contains("seed") || !generation["seed"].is_number()) {
        return false;
    }

    // Validate rendering settings
    const auto& rendering = terrainSettings["rendering"];
    if (!rendering.contains("material") || !rendering["material"].is_string()) {
        return false;
    }

    return true;
}

bool SceneLoader::validatePlanetConfig(const nlohmann::json& planetConfig) {
    if (!planetConfig.is_object()) {
        return false;
    }

    // Required fields
    if (!planetConfig.contains("name") || !planetConfig["name"].is_string()) {
        return false;
    }

    // Config section validation
    if (planetConfig.contains("config") && planetConfig["config"].is_object()) {
        const auto& config = planetConfig["config"];
        if (config.contains("seedBase") && !config["seedBase"].is_number()) {
            return false;
        }
        if (config.contains("baseRadiusKm") && !config["baseRadiusKm"].is_number()) {
            return false;
        }
    }

    // Validate layers if present
    if (planetConfig.contains("layers") && planetConfig["layers"].is_object()) {
        const auto& layers = planetConfig["layers"];
        if (layers.contains("continents") && !validateLayerSettings(layers["continents"])) {
            return false;
        }
        if (layers.contains("macro") && !validateLayerSettings(layers["macro"])) {
            return false;
        }
        if (layers.contains("detail") && !validateLayerSettings(layers["detail"])) {
            return false;
        }
    }

    return true;
}

bool SceneLoader::validateLayerSettings(const nlohmann::json& layerSettings) {
    if (!layerSettings.is_object()) {
        return false;
    }

    // Optional layer fields validation
    if (layerSettings.contains("freq") && !layerSettings["freq"].is_number()) {
        return false;
    }
    if (layerSettings.contains("strength") && !layerSettings["strength"].is_number()) {
        return false;
    }
    if (layerSettings.contains("octaves") && !layerSettings["octaves"].is_number()) {
        return false;
    }
    if (layerSettings.contains("warp") && !layerSettings["warp"].is_number()) {
        return false;
    }

    return true;
}

PlanetarySystem::PlanetConfig SceneLoader::jsonToPlanetConfig(const nlohmann::json& json) {
    PlanetarySystem::PlanetConfig config;

    if (json.contains("config") && json["config"].is_object()) {
        const auto& cfg = json["config"];
        config.seedBase = cfg.value("seedBase", 42);
        config.baseRadiusKm = cfg.value("baseRadiusKm", 6371.0f);
        config.seaLevel = cfg.value("seaLevel", 0.52f);
        config.enableContinents = cfg.value("enableContinents", true);
        config.enableMountains = cfg.value("enableMountains", true);
    }

    if (json.contains("fractal") && json["fractal"].is_object()) {
        const auto& frac = json["fractal"];
        config.persistence = frac.value("persistence", 0.5f);
        config.lacunarity = frac.value("lacunarity", 2.0f);
    }

    if (json.contains("layers") && json["layers"].is_object()) {
        const auto& layers = json["layers"];
        if (layers.contains("continents")) {
            config.continents = jsonToLayerSettings(layers["continents"]);
        }
        if (layers.contains("macro")) {
            config.macro = jsonToLayerSettings(layers["macro"]);
        }
        if (layers.contains("detail")) {
            config.detail = jsonToLayerSettings(layers["detail"]);
        }
    }

    return config;
}

PlanetarySystem::LayerSettings SceneLoader::jsonToLayerSettings(const nlohmann::json& json) {
    PlanetarySystem::LayerSettings settings;
    settings.freq = json.value("freq", 1.0f);
    settings.strength = json.value("strength", 1.0f);
    settings.octaves = json.value("octaves", 5);
    settings.warp = json.value("warp", 0.0f);
    return settings;
}

} // namespace Haruka
