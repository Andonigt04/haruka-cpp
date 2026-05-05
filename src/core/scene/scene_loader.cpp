#include "scene_loader.h"

#include <fstream>
#include <iostream>

namespace Haruka {

bool SceneLoader::loadFromFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "[Loader] Error: No se pudo abrir " << filepath << std::endl;
        return false;
    }

    nlohmann::json data;
    try {
        file >> data;
    } catch (const std::exception& e) {
        std::cerr << "[Loader] Error JSON: " << e.what() << std::endl;
        return false;
    }

    // 1. Validar antes de hacer nada
    auto report = SceneValidator::validate(data);
    if (!report.isValid) {
        std::cerr << "[Loader] La escena no es válida:" << std::endl;
        for (const auto& err : report.errors) std::cerr << " - " << err << std::endl;
        return false;
    }

    // 2. Cargar templates en memoria temporal
    parseTemplates(data);

    // 3. Cargar objetos
    parseObjects(data["objects"]);

    return true;
}

void SceneLoader::parseTemplates(const nlohmann::json& data) {
    if (data.contains("templates")) {
        m_templates = data["templates"];
    }
}

void SceneLoader::parseObjects(const nlohmann::json& objectsArray) {
    for (const auto& objJson : objectsArray) {
        auto obj = createObjectFromJSON(objJson);
        if (obj) {
            // Aquí el manager lo guarda en su registro O(1) y lista global
            m_manager.addLoadedObject(obj);
        }
    }
}

std::shared_ptr<SceneObject> SceneLoader::createObjectFromJSON(const nlohmann::json& objJson) {
    // Clonamos el JSON del objeto para poder modificarlo sin romper el original
    nlohmann::json mergedJson = objJson;

    // Lógica de Herencia: Si el objeto tiene un template, mezclamos los datos
    if (objJson.contains("template")) {
        applyTemplate(mergedJson, objJson["template"]);
    }

    auto obj = std::make_shared<SceneObject>();
    
    // Asignación de campos básicos (ya validados por el SceneValidator)
    obj->name = mergedJson.at("name").get<std::string>();
    obj->type = mergedJson.at("type").get<std::string>();
    
    // Transformaciones
    obj->position = parseDVec3(mergedJson, "position", {0,0,0});
    obj->scale    = parseDVec3(mergedJson, "scale",    {1,1,1});
    obj->rotation = parseDVec3(mergedJson, "rotation", {0,0,0});

    // Flags de sistema
    if (mergedJson.contains("flags")) {
        obj->hasChunks = mergedJson["flags"].value("hasChunks", false);
    }

    // Bloques de datos complejos
    if (mergedJson.contains("lod"))              obj->lodSettings = mergedJson["lod"];
    if (mergedJson.contains("streaming"))        obj->streamingSettings = mergedJson["streaming"];
    if (mergedJson.contains("terrainGenerator")) obj->terrainSettings = mergedJson["terrainGenerator"];
    if (mergedJson.contains("components"))       obj->components = mergedJson["components"];
    if (mergedJson.contains("properties"))       obj->properties = mergedJson["properties"];

    return obj;
}

void SceneLoader::applyTemplate(nlohmann::json& target, const std::string& templateName) {
    if (m_templates.contains(templateName)) {
        const auto& templ = m_templates[templateName];
        
        // Mezclamos recursivamente: el objeto (target) tiene prioridad sobre el template
        // Esto permite que el objeto "sobreescriba" solo lo que necesite
        for (auto it = templ.begin(); it != templ.end(); ++it) {
            if (!target.contains(it.key())) {
                target[it.key()] = it.value();
            } else if (target[it.key()].is_object() && it.value().is_object()) {
                // Si ambos son objetos (ej: lod_settings), mezclamos sus campos internos
                target[it.key()].merge_patch(it.value());
            }
        }
    }
}

glm::dvec3 SceneLoader::parseDVec3(const nlohmann::json& j, const std::string& key, glm::dvec3 defaultValue) {
    if (j.contains(key) && j[key].is_array() && j[key].size() == 3) {
        return glm::dvec3(j[key][0], j[key][1], j[key][2]);
    }
    return defaultValue;
}

}