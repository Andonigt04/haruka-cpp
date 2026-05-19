/**
 * @file transform_component.h
 * @brief Double-precision spatial transform component (position, rotation, scale).
 */
#pragma once

#include "../component.h"
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace Haruka {

/**
 * @brief Componente de transformación espacial en doble precisión.
 */
class TransformComponent : public Component {
public:
    /** @brief Posición en espacio local/mundo según contexto del ECS. */
    glm::dvec3 position{0};
    /** @brief Rotación Euler (grados/radianes según convención del motor). */
    glm::dvec3 rotation{0};
    /** @brief Escala no uniforme. */
    glm::dvec3 scale{1,1,1};

    /** @brief Serializa estado completo a JSON. */
    nlohmann::json toJson() const override {
        nlohmann::json j;
        j["type"] = getType();
        j["position"] = { position.x, position.y, position.z };
        j["rotation"] = { rotation.x, rotation.y, rotation.z };
        j["scale"] = { scale.x, scale.y, scale.z };
        return j;
    }

    /**
     * @brief Crea un `TransformComponent` desde JSON.
     * @pre `j` contiene `position`, `rotation`, `scale` como arreglos de 3 valores.
     */
    static std::shared_ptr<TransformComponent> fromJson(const nlohmann::json& j) {
        auto comp = std::make_shared<TransformComponent>();
        auto pos = j["position"];
        comp->position = glm::dvec3(pos[0], pos[1], pos[2]);
        auto rot = j["rotation"];
        comp->rotation = glm::dvec3(rot[0], rot[1], rot[2]);
        auto scl = j["scale"];
        comp->scale = glm::dvec3(scl[0], scl[1], scl[2]);
        return comp;
    }

    /** @brief Identificador estático del componente. */
    static std::string staticType() { return "Transform"; }
    /** @brief Identificador dinámico del componente. */
    std::string getType() const override { return staticType(); }
    /** @brief Inspector ImGui de posición/rotación/escala. */
    void renderInspector() override;
};

}