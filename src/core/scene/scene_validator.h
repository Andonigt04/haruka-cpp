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

        static void validateRotationField(const nlohmann::json& obj, ValidationResult& result, const std::string& owner) {
            if (!obj.contains("rotation")) return;
            if (!obj["rotation"].is_array()) {
                result.addError(owner + ".'rotation' debe ser un array de 3 o 4 elementos.");
                return;
            }
            const auto size = obj["rotation"].size();
            if (size != 3 && size != 4) {
                result.addError(owner + ".'rotation' debe ser un array de 3 o 4 elementos.");
            }
        }

        static void validateFlags(const nlohmann::json& obj, ValidationResult& result, const std::string& owner) {
            // Flags block is mandatory
            if (!obj.contains("flags")) {
                result.addError(owner + " debe tener un bloque 'flags' obligatorio.");
                return;
            }
            if (obj["flags"].is_object()) {
                const auto& flags = obj["flags"];
                validateBoolField(flags, result, owner + ".flags", "hasChunks");
                validateBoolField(flags, result, owner + ".flags", "isPersistent");
                validateBoolField(flags, result, owner + ".flags", "originShiftingTarget");
                validateBoolField(flags, result, owner + ".flags", "castLight");
                return;
            }
            if (!obj["flags"].is_array()) {
                result.addError(owner + ".flags debe ser un objeto.");
                return;
            }
            if (obj["flags"].size() != 4) {
                result.addError(owner + ".flags debe tener 4 elementos booleanos.");
            }
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
            // (old terrain system removed; block is accepted but ignored)
        }

        static void validateSurfaceBlock(const nlohmann::json& obj, ValidationResult& result, const std::string& owner) {
            if (!obj.contains("surface")) return;
            if (!obj["surface"].is_object()) {
                result.addError(owner + ".surface debe ser un objeto.");
                return;
            }
            const auto& s = obj["surface"];
            if (s.contains("albedo") && !s["albedo"].is_string())
                result.addError(owner + ".surface.albedo debe ser string.");
            if (s.contains("normal") && !s["normal"].is_string())
                result.addError(owner + ".surface.normal debe ser string.");
            if (s.contains("height") && !s["height"].is_string())
                result.addError(owner + ".surface.height debe ser string.");
            if (s.contains("tiling") && !s["tiling"].is_number())
                result.addError(owner + ".surface.tiling debe ser número.");
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
                    validateSurfaceBlock(t, result, "Template '" + it.key() + "'");
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
                std::string tName = obj.value("template", "");
                if (!tName.empty() && (!fullData.contains("templates") || !fullData["templates"].contains(tName))) {
                    result.addError("Objeto '" + name + "' referencia a un template inexistente: " + tName);
                }
            }

            // Regla: Validación de posición (precisión astronómica)
            if (obj.contains("position")) {
                validateArray3Field(obj, result, "Objeto '" + name + "'", "position");
            }
            validateRotationField(obj, result, "Objeto '" + name + "'");
            if (obj.contains("scale")) {
                validateArray3Field(obj, result, "Objeto '" + name + "'", "scale");
            }

            validateFlags(obj, result, "Objeto '" + name + "'");
            validateLodBlock(obj, result, "Objeto '" + name + "'");
            validateStreamingBlock(obj, result, "Objeto '" + name + "'");
            validateTerrainBlock(obj, result, "Objeto '" + name + "'");
            validateSurfaceBlock(obj, result, "Objeto '" + name + "'");

            if (obj.contains("components") && !obj["components"].is_object()) {
                result.addError("Objeto '" + name + "'.components debe ser un objeto.");
            }
            if (obj.contains("properties") && !obj["properties"].is_object()) {
                result.addError("Objeto '" + name + "'.properties debe ser un objeto.");
            }

            validateVisualBlock(obj, result, "Objeto '" + name + "'");

            // Regla: Validación específica para Planetas
            if (obj.value("type", "") == "Planet") {
                if (!hasInlineOrTemplateBlock(obj, fullData, "surface")) {
                    result.addWarning("El planeta '" + name + "' no define surface (texture config) ni lo hereda de su template.");
                }
            }
        }
    };
}