#ifndef COMPONENT_H
#define COMPONENT_H

#include <string>
#include <memory>
#include <nlohmann/json.hpp>

namespace Haruka {

class SceneObject;

/**
 * @brief Base interface for scene components.
 *
 * Components are owned by scene objects and may serialize themselves to JSON.
 */
class Component {
public:
    /** @brief Virtual destructor for derived components. */
    virtual ~Component() = default;
    /** @brief Serializes the component state. */
    virtual nlohmann::json toJson() const { return {}; }
    /** @brief Returns the runtime type name. */
    virtual std::string getType() const = 0;
    /** @brief Renders the component inspector UI, if any. */
    virtual void renderInspector() {}
    /** @brief Non-owning pointer to the owning scene object. */
    SceneObject* owner = nullptr;
};

/** @brief Shared ownership alias for components. */
using ComponentPtr = std::shared_ptr<Component>;

}

#endif