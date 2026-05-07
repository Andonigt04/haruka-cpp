#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <iostream>

namespace Haruka {

    class SceneValidator {
    public:
        struct ValidationResult {
            bool isValid;
            std::vector<std::string> errors;
            std::vector<std::string> warnings;

            void addError(const std::string& msg) { isValid = false; errors.push_back(msg); }
            void addWarning(const std::string& msg) { warnings.push_back(msg); }
        };

        /**
         * @brief Valida el JSON completo de la escena según las reglas del motor.
         */
        static ValidationResult validate(const nlohmann::json& data) {
            ValidationResult result;
            result.isValid = true;

            // 1. Validar estructura raíz
            if (!data.contains("sceneName")) result.addWarning("Falta 'sceneName'. Se usará 'Untitled'.");
            if (!data.contains("objects") || !data["objects"].is_array()) {
                result.addError("Error crítico: No existe el array 'objects'.");
                return result; 
            }

            // 2. Validar Templates (si existen)
            validateTemplates(data, result);

            // 3. Validar Objetos
            for (const auto& obj : data["objects"]) {
                validateObject(obj, data, result);
            }

            return result;
        }

    private:
        static void validateBoolField(const nlohmann::json& obj, ValidationResult& result, const std::string& owner, const std::string& key) {
            if (obj.contains(key) && !obj[key].is_boolean()) {
                result.addError(owner + ".'" + key + "' debe ser booleano.");
            }
        }

        static void validateArray3Field(const nlohmann::json& obj, ValidationResult& result, const std::string& owner, const std::string& key) {
            if (!obj.contains(key)) return;
            if (!obj[key].is_array() || obj[key].size() != 3) {
                result.addError(owner + ".'" + key + "' debe ser un array de 3 elementos.");
            }
        }

        static void validateFlags(const nlohmann::json& obj, ValidationResult& result, const std::string& owner) {
            // Flags block is mandatory
            if (!obj.contains("flags")) {
                result.addError(owner + " debe tener un bloque 'flags' obligatorio.");
                return;
            }
            if (!obj["flags"].is_object()) {
                result.addError(owner + ".flags debe ser un objeto.");
                return;
            }
            const auto& flags = obj["flags"];
            validateBoolField(flags, result, owner + ".flags", "hasChunks");
            validateBoolField(flags, result, owner + ".flags", "isPersistent");
            validateBoolField(flags, result, owner + ".flags", "originShiftingTarget");
            validateBoolField(flags, result, owner + ".flags", "castLight");
        }

        static void validateLodBlock(const nlohmann::json& obj, ValidationResult& result, const std::string& owner) {
            if (!obj.contains("lod")) return;
            if (!obj["lod"].is_object()) {
                result.addError(owner + ".lod debe ser un objeto.");
                return;
            }
            const auto& lod = obj["lod"];
            if (lod.contains("type") && !lod["type"].is_string()) result.addError(owner + ".lod.type debe ser string.");
            if (lod.contains("maxDepth") && !lod["maxDepth"].is_number_integer()) result.addError(owner + ".lod.maxDepth debe ser entero.");
            if (lod.contains("splitThreshold") && !lod["splitThreshold"].is_number()) result.addError(owner + ".lod.splitThreshold debe ser numérico.");
            if (lod.contains("thresholds") && !lod["thresholds"].is_array()) result.addError(owner + ".lod.thresholds debe ser un array.");
            if (lod.contains("assets") && !lod["assets"].is_array()) result.addError(owner + ".lod.assets debe ser un array.");
        }

        static void validateStreamingBlock(const nlohmann::json& obj, ValidationResult& result, const std::string& owner) {
            if (!obj.contains("streaming")) return;
            if (!obj["streaming"].is_object()) {
                result.addError(owner + ".streaming debe ser un objeto.");
                return;
            }
            const auto& streaming = obj["streaming"];
            if (streaming.contains("mode") && !streaming["mode"].is_string()) result.addError(owner + ".streaming.mode debe ser string.");
            if (streaming.contains("priority") && !streaming["priority"].is_string()) result.addError(owner + ".streaming.priority debe ser string.");
            if (streaming.contains("enabled") && !streaming["enabled"].is_boolean()) result.addError(owner + ".streaming.enabled debe ser booleano.");
        }

        static void validateTerrainBlock(const nlohmann::json& obj, ValidationResult& result, const std::string& owner) {
            if (!obj.contains("terrainSettings")) return;
            if (!obj["terrainSettings"].is_object()) {
                result.addError(owner + ".terrainSettings debe ser un objeto.");
                return;
            }
            const auto& terrain = obj["terrainSettings"];
            if (terrain.contains("type") && !terrain["type"].is_string()) result.addError(owner + ".terrainSettings.type debe ser string.");
            if (terrain.contains("shader") && !terrain["shader"].is_string()) result.addError(owner + ".terrainSettings.shader debe ser string.");
            if (terrain.contains("config")) {
                if (!terrain["config"].is_object()) {
                    result.addError(owner + ".terrainSettings.config debe ser un objeto.");
                } else {
                    const auto& config = terrain["config"];
                    if (config.contains("seed") && !config["seed"].is_number_integer()) result.addError(owner + ".terrainSettings.config.seed debe ser entero.");
                    if (config.contains("chunkSize") && !config["chunkSize"].is_number_integer()) result.addError(owner + ".terrainSettings.config.chunkSize debe ser entero.");
                    if (config.contains("layers")) {
                        if (!config["layers"].is_object()) {
                            result.addError(owner + ".terrainSettings.config.layers debe ser un objeto.");
                        } else {
                            for (auto it = config["layers"].begin(); it != config["layers"].end(); ++it) {
                                if (!it.value().is_object()) {
                                    result.addError(owner + ".terrainSettings.config.layers.'" + it.key() + "' debe ser un objeto.");
                                    continue;
                                }
                                const auto& layer = it.value();
                                if (layer.contains("freq") && !layer["freq"].is_number()) result.addError(owner + ".terrainSettings.config.layers.'" + it.key() + "'.freq debe ser numérico.");
                                if (layer.contains("octaves") && !layer["octaves"].is_number_integer()) result.addError(owner + ".terrainSettings.config.layers.'" + it.key() + "'.octaves debe ser entero.");
                                if (layer.contains("strength") && !layer["strength"].is_number()) result.addError(owner + ".terrainSettings.config.layers.'" + it.key() + "'.strength debe ser numérico.");
                            }
                        }
                    }
                }
            }
        }

        static void validateVisualBlock(const nlohmann::json& obj, ValidationResult& result, const std::string& owner) {
            if (!obj.contains("components") || !obj["components"].is_object()) {
                return;
            }

            const auto& components = obj["components"];
            if (!components.contains("visual")) {
                return;
            }
            if (!components["visual"].is_object()) {
                result.addError(owner + ".components.visual debe ser un objeto.");
                return;
            }

            const auto& visual = components["visual"];
            if (visual.contains("mesh") && !visual["mesh"].is_string()) {
                result.addError(owner + ".components.visual.mesh debe ser string.");
            }
            if (visual.contains("material") && !visual["material"].is_string()) {
                result.addError(owner + ".components.visual.material debe ser string.");
            }
            if (visual.contains("shader") && !visual["shader"].is_string()) {
                result.addError(owner + ".components.visual.shader debe ser string.");
            }
        }

        static bool hasInlineOrTemplateBlock(const nlohmann::json& obj, const nlohmann::json& fullData, const std::string& key) {
            if (obj.contains(key)) return true;
            if (!obj.contains("template")) return false;
            if (!fullData.contains("templates")) return false;
            const std::string templateName = obj.value("template", "");
            if (templateName.empty() || !fullData["templates"].contains(templateName)) return false;
            return fullData["templates"][templateName].contains(key);
        }

        static void validateTemplates(const nlohmann::json& data, ValidationResult& result) {
            if (data.contains("templates")) {
                for (auto it = data["templates"].begin(); it != data["templates"].end(); ++it) {
                    const auto& t = it.value();
                    // Templates only define optional blocks (lod, streaming, terrain)
                    // Flags are not needed in templates, only in objects
                    validateLodBlock(t, result, "Template '" + it.key() + "'");
                    validateStreamingBlock(t, result, "Template '" + it.key() + "'");
                    validateTerrainBlock(t, result, "Template '" + it.key() + "'");
                }
            }
        }

        static void validateObject(const nlohmann::json& obj, const nlohmann::json& fullData, ValidationResult& result) {
            std::string name = obj.value("name", "Unknown");

            // Regla: Todo objeto necesita un tipo
            if (!obj.contains("type")) {
                result.addError("Objeto '" + name + "' no tiene definido un 'type'.");
            }

            // Regla: Si usa template, el template debe existir
            if (obj.contains("template")) {
                std::string tName = obj["template"];
                if (!fullData.contains("templates") || !fullData["templates"].contains(tName)) {
                    result.addError("Objeto '" + name + "' referencia a un template inexistente: " + tName);
                }
            }

            // Regla: Validación de posición (precisión astronómica)
            if (obj.contains("position")) {
                validateArray3Field(obj, result, "Objeto '" + name + "'", "position");
            }
            if (obj.contains("rotation")) {
                validateArray3Field(obj, result, "Objeto '" + name + "'", "rotation");
            }
            if (obj.contains("scale")) {
                validateArray3Field(obj, result, "Objeto '" + name + "'", "scale");
            }

            validateFlags(obj, result, "Objeto '" + name + "'");
            validateLodBlock(obj, result, "Objeto '" + name + "'");
            validateStreamingBlock(obj, result, "Objeto '" + name + "'");
            validateTerrainBlock(obj, result, "Objeto '" + name + "'");

            if (obj.contains("components") && !obj["components"].is_object()) {
                result.addError("Objeto '" + name + "'.components debe ser un objeto.");
            }
            if (obj.contains("properties") && !obj["properties"].is_object()) {
                result.addError("Objeto '" + name + "'.properties debe ser un objeto.");
            }

            validateVisualBlock(obj, result, "Objeto '" + name + "'");

            // Regla: Validación específica para Planetas
            if (obj.value("type", "") == "Planet") {
                if (!hasInlineOrTemplateBlock(obj, fullData, "lod")) {
                    result.addError("El planeta '" + name + "' no define LOD ni lo hereda de su template.");
                }
                if (!hasInlineOrTemplateBlock(obj, fullData, "streaming")) {
                    result.addError("El planeta '" + name + "' no define streaming ni lo hereda de su template.");
                }
                if (!hasInlineOrTemplateBlock(obj, fullData, "terrainSettings")) {
                    result.addWarning("El planeta '" + name + "' no define terrainSettings ni lo hereda de su template.");
                }
            }
        }
    };
}