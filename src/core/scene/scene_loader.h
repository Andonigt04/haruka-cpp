#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "scene_manager.h" // Donde reside SceneObject
#include "scene_validator.h"

namespace Haruka {

    class SceneLoader {
    public:
        SceneLoader(SceneManager& manager) : m_manager(manager) {}

        /**
         * @brief Punto de entrada principal para cargar el archivo .scene
         */
        bool loadFromFile(const std::string& filepath);

    private:
        SceneManager& m_manager;
        nlohmann::json m_templates;

        // Procesamiento por etapas
        void parseTemplates(const nlohmann::json& data);
        void parseObjects(const nlohmann::json& objectsArray);
        
        // Resolución de herencia: Objeto <--- Template
        std::shared_ptr<SceneObject> createObjectFromJSON(const nlohmann::json& objJson);
        
        // Helper para mezclar JSON (Merge de propiedades de template y objeto)
        void applyTemplate(nlohmann::json& target, const std::string& templateName);
        
        // Parsers de tipos de datos
        glm::dvec3 parseDVec3(const nlohmann::json& j, const std::string& key, glm::dvec3 defaultValue);
    };

}