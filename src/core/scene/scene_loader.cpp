#include "scene_loader.h"
#include "io/blob_codec.h"
#include "core/components/material_component.h"

#include <fstream>
#include <iostream>
#include <filesystem>
#include <cstring>

namespace {

using Haruka::LODSettings;
using Haruka::StreamingSettings;
using Haruka::TerrainGeneratorSettings;

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
        settings.rawConfig = config; // keep the full, unfiltered config
        // (`config.layers` se parseaba aquí a un mapa tipado para el generador v1. Ese generador ya no
        //  existe; si una escena antigua trae el bloque, sigue viajando intacto en rawConfig y
        //  simplemente no lo lee nadie.)
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
    if (!config.empty()) value["config"] = std::move(config);
    return value;
}

}

namespace Haruka {

// Map obfuscation key. The .hmap codec uses this; ships in the binary (same
// honest scope as saves: stops casual reading/editing, not strong DRM).
static const char* kMapKey = "haruka.map.v1";

bool SceneLoader::loadFromFile(const std::string& filepath) {
    nlohmann::json data;

    // A packed .hmap is binary (zstd+XOR). A .scene is plain JSON text. Decide by
    // extension; for any other path try plain text first, then packed.
    auto endsWith = [](const std::string& s, const char* suf) {
        size_t n = std::strlen(suf);
        return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };

    if (endsWith(filepath, ".hmap")) {
        if (!loadPacked(filepath, data)) return false;
    } else {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "[Loader] Error: No se pudo abrir " << filepath << std::endl;
            return false;
        }
        try { file >> data; }
        catch (const std::exception& e) {
            std::cerr << "[Loader] Error JSON: " << e.what() << std::endl;
            return false;
        }
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

bool SceneLoader::loadPacked(const std::string& filepath, nlohmann::json& out) {
    std::ifstream f(filepath, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "[Loader] Error: No se pudo abrir " << filepath << std::endl;
        return false;
    }
    Haruka::codec::Bytes blob((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    std::string text;
    if (!Haruka::codec::decodeString(blob, kMapKey, text)) {
        std::cerr << "[Loader] Error: .hmap corrupto o clave incorrecta" << std::endl;
        return false;
    }
    try { out = nlohmann::json::parse(text); }
    catch (const std::exception& e) {
        std::cerr << "[Loader] Error JSON en .hmap: " << e.what() << std::endl;
        return false;
    }
    return true;
}

// Packs a plain .scene JSON file into an obfuscated .hmap. Used by the `mapc` tool.
bool SceneLoader::packToFile(const std::string& sceneJsonPath, const std::string& outHmapPath) {
    std::ifstream in(sceneJsonPath);
    if (!in.is_open()) return false;
    std::string text((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    // Validate it's parseable JSON before packing (accept() checks syntax without
    // building the DOM and returns a bool, so there's no nodiscard to ignore).
    if (!nlohmann::json::accept(text)) return false;

    Haruka::codec::Bytes blob = Haruka::codec::encodeString(text, kMapKey);
    if (blob.empty()) return false;
    std::ofstream out(outHmapPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(blob.data()), (std::streamsize)blob.size());
    return out.good();
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
    obj->objectType = Haruka::classifyObjectType(obj->type);
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
    if (mergedJson.contains("surface") && mergedJson["surface"].is_object()) {
        obj->surfaceConfig = mergedJson["surface"];
    }
    if (mergedJson.contains("components"))       obj->components = mergedJson["components"];
    if (mergedJson.contains("properties"))       obj->properties = mergedJson["properties"];
    if (mergedJson.contains("modelPath"))        obj->modelPath  = mergedJson["modelPath"].get<std::string>();
    // `material` (MaterialComponent) también es parte del objeto y se serializa en save(). Sin esto,
    // las texturas que hornea el editor de node graph (y cualquier material asignado por código) se
    // perdían al recargar la escena: el loader lo ignoraba y `save()` no lo escribía.
    if (mergedJson.contains("material") && mergedJson["material"].is_object()) {
        obj->material = std::make_shared<MaterialComponent>();
        obj->material->fromJSON(mergedJson["material"]);
    }

    // Sol/estrella: material PROPIO y dedicado (no reutiliza uno genérico). Un cuerpo luminoso
    // (`castLight`) sin terreno emite su propia luz: el disco del sol (sun_disc.png) + emisión
    // hacen que brille desde el shader. Vale en el IDE y en el juego, sin editar la escena.
    if (obj->flags.castLight &&
        (obj->objectType == ObjectType::PLANET || obj->objectType == ObjectType::STAR)) {
        glm::vec3 sunColor(1.0f, 0.9f, 0.7f);
        if (obj->components.is_object() && obj->components.contains("light") &&
            obj->components["light"].is_object()) {
            const auto& lc = obj->components["light"];
            if (lc.contains("color") && lc["color"].is_array() && lc["color"].size() >= 3)
                sunColor = glm::vec3(lc["color"][0].get<float>(),
                                     lc["color"][1].get<float>(),
                                     lc["color"][2].get<float>());
        }
        obj->material = std::make_shared<MaterialComponent>();
        obj->material->name = obj->name + "_SunMaterial";
        obj->material->albedo    = glm::vec3(1.0f);
        obj->material->emission  = sunColor;
        obj->material->roughness = 0.15f;
        obj->material->textures.clear();
        obj->material->textures["albedo"] = "assets/textures/sun_disc.png";
    }

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
        // `surface` y `modelPath` los LEE el loader pero no los escribía nadie: cada guardado
        // BORRABA la configuración de superficie de los planetas y la malla de los objetos con
        // modelo. Y un planeta sin `surface` es INVISIBLE por construcción — `classifySceneObject`
        // no le da comando de dibujo (delega en el terreno) y `buildFromScene` no lo adopta (exige
        // `surfaceConfig`), así que nadie lo pinta y no hay error que lo diga. Guardar una escena
        // desde el IDE bastaba para perder los planetas para siempre.
        if (!obj.surfaceConfig.is_null() && !obj.surfaceConfig.empty()) item["surface"] = obj.surfaceConfig;
        if (!obj.modelPath.empty()) item["modelPath"] = obj.modelPath;
        if (obj.material) item["material"] = obj.material->toJSON();
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