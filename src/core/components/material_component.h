/**
 * @file material_component.h
 * @brief Serializable PBR material data component (shader path, textures, PBR params).
 */
#pragma once
#include <string>
#include <map>
#include <glm/glm.hpp>
#include <memory>
#include "../component.h"

namespace Haruka {

/**
 * @brief Serializable PBR material data container.
 *
 * Stores shader/texture paths and physically based shading parameters.
 * This component does not load resources; it only stores state.
 */
class MaterialComponent : public Component {
public:
    /** @brief Builds a default material with known texture slots. */
    MaterialComponent();
    ~MaterialComponent() override = default;
    
    std::string getType() const override { return "MaterialComponent"; }
    
    /** @name Identity */
    ///@{
    /** @brief Human-readable material name. */
    std::string name = "DefaultMaterial";
    /** @brief Fragment shader/base material path. */
    std::string shaderPath = "shaders/pbr.frag";
    ///@}
    
    /**
     * @brief Texture dictionary keyed by logical channel.
     *
     * Expected keys: `albedo`, `normal`, `metallic`, `roughness`, `ao`.
     */
    std::map<std::string, std::string> textures;
    
    /** @name PBR parameters */
    ///@{
    /** @brief Base color (linear space). */
    glm::vec3 albedo = glm::vec3(0.8f);
    /** @brief Metalness in range [0,1]. */
    float metallic = 0.0f;
    /** @brief Roughness in range [0,1]. */
    float roughness = 0.5f;
    /** @brief Ambient occlusion in range [0,1]. */
    float ao = 1.0f;
    
    /** @brief Linear-space RGB emission. */
    glm::vec3 emission = glm::vec3(0.0f);
    ///@}
    
    /**
     * @brief Serializes the component to JSON.
     * @return JSON object with all material fields.
     */
    nlohmann::json toJSON() const;

    /**
     * @brief Restores component state from JSON.
     * @param j Source JSON object.
     */
    void fromJSON(const nlohmann::json& j);
};

}