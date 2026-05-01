/**
 * @file mesh_component.h
 * @brief Lightweight mesh path reference component for serialization.
 */
#pragma once

#include "core/component.h"
#include <string>
#include <nlohmann/json.hpp>

namespace Haruka {

/**
 * @brief Mesh resource reference component.
 *
 * Stores a logical mesh path for serialization and reconstruction.
 */
class MeshComponent : public Component {
public:
    /** @brief Mesh resource path. */
    std::string meshPath;

    /**
     * @brief Serializes the component to JSON.
     * @return JSON with type and mesh path.
     */
    nlohmann::json toJson() const override {
        nlohmann::json j;
        j["type"] = getType();
        j["meshPath"] = meshPath;
        return j;
    }

    /**
        * @brief Builds an instance from JSON.
     * @param j Documento fuente.
        * @return Component created with defaults when fields are missing.
     */
    static std::shared_ptr<MeshComponent> fromJson(const nlohmann::json& j) {
        auto comp = std::make_shared<MeshComponent>();
        comp->meshPath = j.value("meshPath", "");
        return comp;
    }

    /** @brief Static component type identifier. */
    static std::string staticType() { return "Mesh"; }
    /** @brief Dynamic component type identifier. */
    std::string getType() const override { return staticType(); }
};

}