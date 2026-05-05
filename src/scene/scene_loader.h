/**
 * @file scene.h
 * @brief `Scene` container and `SceneObject` record.
 *
 * A `Scene` holds a flat list of `SceneObject` entries that form an implicit
 * hierarchy via `parentIndex` / `childrenIndices`. Objects carry transform,
 * rendering references (material, mesh renderer), optional free-form JSON
 * properties, and world-transform helpers that walk the hierarchy.
 *
 * Persistence is handled by `Scene::save()` / `Scene::load()`. An optional
 * initializer script path can be embedded in the scene file to run startup
 * logic at load time.
 */
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <glm/glm.hpp>
#include "core/component.h"
#include <nlohmann/json.hpp>
#include "core/components/transform_component.h"
#include "core/components/mesh_component.h"
#include "core/components/model_component.h"
#include "core/components/material_component.h"
#include "components/mesh_renderer_component.h"
#include "core/event_manager.h"
#include "core/events.h"

namespace Haruka
{
    class Scene;

    /**
     * @brief Serializable scene object record.
     *
     * Contains transform, rendering references, optional hierarchy links,
     * and extensible JSON properties.
     */
    struct SceneObject {
        std::string name;
        std::string type;       ///< String form of `ObjectType` (e.g. "Mesh", "Light")
        std::string modelPath;  ///< Path to the 3D asset file, empty for primitive types

        glm::dvec3 position = glm::dvec3(0.0);
        glm::dvec3 rotation = glm::dvec3(0.0); ///< Euler angles in degrees
        glm::dvec3 scale    = glm::dvec3(1.0);
        glm::dvec3 color    = glm::dvec3(1.0); ///< Tint or light color
        double intensity = 1.0;                ///< Light intensity multiplier
        int renderLayer = 1;                   ///< Visibility layer [1..5]

        int parentIndex = -1;                  ///< Index into Scene::objects; -1 = root
        std::vector<int> childrenIndices;
        
        std::shared_ptr<MaterialComponent> material;
        std::shared_ptr<MeshRendererComponent> meshRenderer;
        
        nlohmann::json properties;
        std::vector<SceneObject> children;
        
        /** @brief Computes object world transform using scene hierarchy links. */
        glm::mat4 getWorldTransform(const Scene* scene) const;
        /** @brief Returns world-space position derived from hierarchy. */
        glm::dvec3 getWorldPosition(const Scene* scene) const;
        /** @brief Returns world-space rotation derived from hierarchy. */
        glm::dvec3 getWorldRotation(const Scene* scene) const;
        /** @brief Returns world-space scale derived from hierarchy. */
        glm::dvec3 getWorldScale(const Scene* scene) const;
    };
    
    /**
     * @brief Scene container with object lifecycle and persistence APIs.
     */
    class Scene {
    public:
        Scene();
        Scene(const std::string& name);
        ~Scene();
        
        std::string sceneName;
        std::string getInitializerPath() const { return initializerPath; }

        /** @brief Adds an object copy to the scene. */
        void addObject(const SceneObject& obj);
        /** @brief Removes object by name if present. */
        void removeObject(const std::string& name);
        /** @brief Finds object by name (mutable, non-owning pointer). */
        SceneObject* getObject(const std::string& name);
        
        std::vector<SceneObject>& getObjects() { return objects; }
        const std::vector<SceneObject>& getObjects() const { return objects; }
        std::vector<SceneObject>& getObjectsMutable() { return objects; }
        
        std::string getName() const { return sceneName; }
        void setName(const std::string& name) { sceneName = name; }
        
        /** @brief Saves scene to file. */
        bool save(const std::string& filepath);
        /** @brief Loads scene from file, then posts ObjectEvent::Created for each loaded object. */
        bool load(const std::string& filepath);

        /** @brief Injects the event manager used for post-load notifications. */
        void setEventManager(EventManager* mgr) { eventManager_ = mgr; }
        EventManager* getEventManager() const    { return eventManager_; }

    private:
        std::vector<SceneObject> objects;
        std::string initializerPath;
        EventManager* eventManager_ = nullptr;

        /** @brief Parses one scene object entry from JSON. */
        SceneObject parseSceneObject(const nlohmann::json& o);
        /** @brief Injects prefab components into scene object. */
        void loadPrefabComponents(const std::string& prefabPath, SceneObject& obj);
        /** @brief Executes scene initializer script if configured. */
        void executeInitializer(const std::string& scenePath);
    };
}