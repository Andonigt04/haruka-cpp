#include "scene_loader.h"

#include <fstream>
#include <iostream>
#include <filesystem>

namespace {

using Haruka::LODSettings;
using Haruka::StreamingSettings;
using Haruka::TerrainGeneratorSettings;
using Haruka::TerrainLayerSettings;

bool parseBoolField(const nlohmann::json& value, const char* key, bool defaultValue) {
    if (value.contains(key) && value[key].is_boolean()) {
        return value[key].get<bool>();
    }
    return defaultValue;
}

Haruka::Flags parseFlags(const nlohmann::json& value) {
    Haruka::Flags flags;
    if (value.is_object()) {
        flags.hasChunks = parseBoolField(value, "hasChunks", flags.hasChunks);
        flags.isPersistent = parseBoolField(value, "isPersistent", flags.isPersistent);
        flags.originShiftingTarget = parseBoolField(value, "originShiftingTarget", flags.originShiftingTarget);
        flags.castLight = parseBoolField(value, "castLight", flags.castLight);
        return flags;
    }

    if (value.is_array() && value.size() == 4) {
        flags.hasChunks = value[0].is_boolean() ? value[0].get<bool>() : flags.hasChunks;
        flags.isPersistent = value[1].is_boolean() ? value[1].get<bool>() : flags.isPersistent;
        flags.originShiftingTarget = value[2].is_boolean() ? value[2].get<bool>() : flags.originShiftingTarget;
        flags.castLight = value[3].is_boolean() ? value[3].get<bool>() : flags.castLight;
    }

    return flags;
}

Haruka::Rotation parseRotation(const nlohmann::json& value) {
    if (value.is_array()) {
        if (value.size() == 4) {
            const double x = value[0].get<double>();
            const double y = value[1].get<double>();
            const double z = value[2].get<double>();
            const double w = value[3].get<double>();
            return Haruka::Rotation(w, x, y, z);
        }
        if (value.size() == 3) {
            return Haruka::Rotation(glm::radians(glm::dvec3(
                value[0].get<double>(),
                value[1].get<double>(),
                value[2].get<double>()
            )));
        }
    }

    return Haruka::Rotation(1.0, 0.0, 0.0, 0.0);
}

TerrainLayerSettings parseTerrainLayer(const nlohmann::json& value) {
    TerrainLayerSettings layer;
    layer.freq = value.value("freq", 0.0);
    layer.octaves = value.value("octaves", 0);
    layer.strength = value.value("strength", 0.0);
    return layer;
}

LODSettings parseLodSettings(const nlohmann::json& value) {
    LODSettings settings;
    settings.type = value.value("type", "");
    settings.maxDepth = value.value("maxDepth", 0);
    settings.splitThreshold = value.value("splitThreshold", 0.0);
    if (value.contains("thresholds") && value["thresholds"].is_array()) {
        for (const auto& item : value["thresholds"]) {
            if (item.is_number()) settings.thresholds.push_back(item.get<double>());
        }
    }
    if (value.contains("assets") && value["assets"].is_array()) {
        for (const auto& item : value["assets"]) {
            if (item.is_string()) settings.assets.push_back(item.get<std::string>());
        }
    }
    return settings;
}

StreamingSettings parseStreamingSettings(const nlohmann::json& value) {
    StreamingSettings settings;
    settings.mode = value.value("mode", "");
    settings.priority = value.value("priority", "");
    settings.enabled = value.value("enabled", true);
    return settings;
}

TerrainGeneratorSettings parseTerrainSettings(const nlohmann::json& value) {
    TerrainGeneratorSettings settings;
    settings.type = value.value("type", "");
    settings.shader = value.value("shader", "");
    if (value.contains("config") && value["config"].is_object()) {
        const auto& config = value["config"];
        settings.seed = config.value("seed", 0);
        settings.chunkSize = config.value("chunkSize", 0);
        if (config.contains("layers") && config["layers"].is_object()) {
            for (auto it = config["layers"].begin(); it != config["layers"].end(); ++it) {
                if (it.value().is_object()) {
                    settings.layers[it.key()] = parseTerrainLayer(it.value());
                }
            }
        }
    }
    return settings;
}

nlohmann::json toJson(const LODSettings& settings) {
    nlohmann::json value;
    if (!settings.type.empty()) value["type"] = settings.type;
    if (settings.maxDepth != 0) value["maxDepth"] = settings.maxDepth;
    if (settings.splitThreshold != 0.0) value["splitThreshold"] = settings.splitThreshold;
    if (!settings.thresholds.empty()) value["thresholds"] = settings.thresholds;
    if (!settings.assets.empty()) value["assets"] = settings.assets;
    return value;
}

nlohmann::json toJson(const StreamingSettings& settings) {
    nlohmann::json value;
    if (!settings.mode.empty()) value["mode"] = settings.mode;
    if (!settings.priority.empty()) value["priority"] = settings.priority;
    value["enabled"] = settings.enabled;
    return value;
}

nlohmann::json toJson(const TerrainGeneratorSettings& settings) {
    nlohmann::json value;
    if (!settings.type.empty()) value["type"] = settings.type;
    if (!settings.shader.empty()) value["shader"] = settings.shader;
    nlohmann::json config;
    if (settings.seed != 0) config["seed"] = settings.seed;
    if (settings.chunkSize != 0) config["chunkSize"] = settings.chunkSize;
    if (!settings.layers.empty()) {
        nlohmann::json layers = nlohmann::json::object();
        for (const auto& [name, layer] : settings.layers) {
            nlohmann::json entry;
            if (layer.freq != 0.0) entry["freq"] = layer.freq;
            if (layer.octaves != 0) entry["octaves"] = layer.octaves;
            if (layer.strength != 0.0) entry["strength"] = layer.strength;
            layers[name] = entry;
        }
        config["layers"] = std::move(layers);
    }
    if (!config.empty()) value["config"] = std::move(config);
    return value;
}

}

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
        const std::string templateName = objJson.value("template", "");
        if (!templateName.empty()) {
            applyTemplate(mergedJson, templateName);
        }
    }

    auto obj = std::make_shared<SceneObject>();
    
    // Asignación de campos básicos (ya validados por el SceneValidator)
    obj->name = mergedJson.at("name").get<std::string>();
    obj->type = mergedJson.at("type").get<std::string>();
    obj->templateName = mergedJson.value("template", "");
    
    // Transformaciones
    obj->position = parseDVec3(mergedJson, "position", {0,0,0});
    obj->scale    = parseDVec3(mergedJson, "scale",    {1,1,1});
    obj->rotation = parseRotation(mergedJson.contains("rotation") ? mergedJson["rotation"] : nlohmann::json());

    // Flags block is mandatory - read from flags object and assign to struct
    if (mergedJson.contains("flags")) {
        obj->flags = parseFlags(mergedJson["flags"]);
    }

    // Bloques de datos complejos
    if (mergedJson.contains("lod") && mergedJson["lod"].is_object()) {
        obj->lodSettings = parseLodSettings(mergedJson["lod"]);
    }
    if (mergedJson.contains("streaming") && mergedJson["streaming"].is_object()) {
        obj->streamingSettings = parseStreamingSettings(mergedJson["streaming"]);
    }
    if (mergedJson.contains("terrainSettings") && mergedJson["terrainSettings"].is_object()) {
        obj->terrainSettings = parseTerrainSettings(mergedJson["terrainSettings"]);
    }
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

bool SceneManager::load(const std::string& filepath) {
    clear();
    m_name = std::filesystem::path(filepath).stem().string();
    SceneLoader loader(*this);
    return loader.loadFromFile(filepath);
}

bool SceneManager::save(const std::string& filepath) const {
    nlohmann::json root;
    root["sceneName"] = m_name;
    root["version"] = "2.0";
    root["objects"] = nlohmann::json::array();

    for (const auto& objPtr : m_objects) {
        if (!objPtr) continue;
        const auto& obj = *objPtr;
        nlohmann::json item;
        item["name"] = obj.name;
        item["type"] = obj.type;
        item["template"] = obj.templateName;
        item["position"] = {obj.position.x, obj.position.y, obj.position.z};
        item["rotation"] = {obj.rotation.x, obj.rotation.y, obj.rotation.z, obj.rotation.w};
        item["scale"] = {obj.scale.x, obj.scale.y, obj.scale.z};
        item["flags"] = nlohmann::json::object({
            {"hasChunks", obj.flags.hasChunks},
            {"isPersistent", obj.flags.isPersistent},
            {"originShiftingTarget", obj.flags.originShiftingTarget},
            {"castLight", obj.flags.castLight}
        });
        if (obj.lodSettings) item["lod"] = toJson(*obj.lodSettings);
        if (obj.streamingSettings) item["streaming"] = toJson(*obj.streamingSettings);
        if (obj.terrainSettings) item["terrainSettings"] = toJson(*obj.terrainSettings);
        if (!obj.components.is_null() && !obj.components.empty()) item["components"] = obj.components;
        if (!obj.properties.is_null() && !obj.properties.empty()) item["properties"] = obj.properties;
        if (obj.parentIndex >= 0) item["parentIndex"] = obj.parentIndex;
        if (!obj.childrenIndices.empty()) item["childrenIndices"] = obj.childrenIndices;
        root["objects"].push_back(std::move(item));
    }

    std::ofstream out(filepath);
    if (!out.is_open()) {
        return false;
    }
    out << root.dump(4);
    return true;
}

}