#ifndef MODEL_COMPONENT_H
#define MODEL_COMPONENT_H

#include <string>
#include <nlohmann/json.hpp>

#include "core/component.h"

namespace Haruka {

/**
 * @brief Model asset reference component.
 */
class ModelComponent : public Component {
public:
    /** @brief Model asset path. */
    std::string modelPath;

    /** @brief Serializes to JSON (`type`, `modelPath`). */
    nlohmann::json toJson() const override {
        nlohmann::json j;
        j["type"] = getType();
        j["modelPath"] = modelPath;
        return j;
    }

    /** @brief Builds a component from JSON with missing-field tolerance. */
    static std::shared_ptr<Component> fromJson(const nlohmann::json& j) {
        auto comp = std::make_shared<ModelComponent>();
        comp->modelPath = j.value("modelPath", "");
        return comp;
    }

    /** @brief Static component identifier. */
    static std::string staticType() { return "Model"; }
    /** @brief Dynamic component identifier. */
    std::string getType() const override { return staticType(); }
    /** @brief Empty inspector (no editable parameters yet). */
    void renderInspector() override {}
};

}

#endif