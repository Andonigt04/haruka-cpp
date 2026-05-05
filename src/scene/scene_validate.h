/**
 * @file scene_loader.h
 * @brief Scene loading with JSON validation and entity registration.
 */
#pragma once

#include "core/json.hpp"
#include "core/world_system.h"
#include "core/scene.h"
#include "game/planetary_system.h"
#include <string>
#include <vector>
#include <memory>

namespace Haruka {

/**
 * @brief Loads scenes from JSON files with validation.
 * 
 * Separates loading logic from registration logic for better modularity.
 * Validates terrain settings, planet configurations, and layer parameters.
 */
class SceneLoader {
public:
    SceneLoader();
    ~SceneLoader();

    /**
     * @brief Loads a scene from a JSON file.
     * @param filepath Path to the scene JSON file
     * @return true if loading succeeded, false otherwise
     */
    bool loadScene(const std::string& filepath);

    /**
     * @brief Registers loaded entities into a scene and world system.
     * @param scene Target scene to add entities to
     * @param worldSystem World system for entity registration
     * @param planetarySystem Planetary system for planet initialization
     * @return true if registration succeeded, false otherwise
     */
    bool registerEntities(Scene* scene, WorldSystem* worldSystem, PlanetarySystem* planetarySystem);

    /**
     * @brief Validates a complete scene JSON structure.
     * @param sceneData JSON object to validate
     * @return true if all required fields are present and valid
     */
    static bool validateScene(const nlohmann::json& sceneData);

    /**
     * @brief Validates terrain settings structure.
     * @param terrainSettings JSON object with generation and rendering settings
     * @return true if terrain settings are valid
     */
    static bool validateTerrainSettings(const nlohmann::json& terrainSettings);

    /**
     * @brief Validates planet configuration structure.
     * @param planetConfig JSON object with planet parameters
     * @return true if planet config is valid
     */
    static bool validatePlanetConfig(const nlohmann::json& planetConfig);

    /**
     * @brief Validates layer settings structure.
     * @param layerSettings JSON object with layer parameters
     * @return true if layer settings are valid
     */
    static bool validateLayerSettings(const nlohmann::json& layerSettings);

    /**
     * @brief Gets last error message from loading/validation.
     */
    const std::string& getLastError() const { return lastError; }

    /**
     * @brief Gets the loaded scene data (for inspection).
     */
    const nlohmann::json& getSceneData() const { return sceneData; }

private:
    nlohmann::json sceneData;
    std::string lastError;

    /**
     * @brief Parses a planet object from JSON.
     * @param obj JSON object representing a planet
     * @return Pointer to created entity, nullptr on error
     */
    SceneObject* parsePlanetObject(const nlohmann::json& obj);

    /**
     * @brief Converts JSON planet config to PlanetarySystem::PlanetConfig.
     */
    static PlanetarySystem::PlanetConfig jsonToPlanetConfig(const nlohmann::json& json);

    /**
     * @brief Converts JSON layer settings to LayerSettings.
     */
    static PlanetarySystem::LayerSettings jsonToLayerSettings(const nlohmann::json& json);
};

} // namespace Haruka
