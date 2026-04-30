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
        std::string type;
        std::string modelPath;
        
        glm::dvec3 position = glm::dvec3(0.0);
        glm::dvec3 rotation = glm::dvec3(0.0);
        glm::dvec3 scale = glm::dvec3(1.0);
        glm::dvec3 color = glm::dvec3(1.0);
        double intensity = 1.0;
        int renderLayer = 1; // 1..5
        
        int parentIndex = -1;
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
        /** @brief Loads scene from file. */
        bool load(const std::string& filepath);
        
        std::string sceneName;

        std::string getInitializerPath() const { return initializerPath; }
        
    private:
        std::vector<SceneObject> objects;
        std::string initializerPath;

        /** @brief Parses one scene object entry from JSON. */
        SceneObject parseSceneObject(const nlohmann::json& o);
        /** @brief Injects prefab components into scene object. */
        void loadPrefabComponents(const std::string& prefabPath, SceneObject& obj);
        /** @brief Executes scene initializer script if configured. */
        void executeInitializer(const std::string& scenePath);
    };
}