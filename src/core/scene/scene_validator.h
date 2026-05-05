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
        static void validateTemplates(const nlohmann::json& data, ValidationResult& result) {
            if (data.contains("templates")) {
                for (auto it = data["templates"].begin(); it != data["templates"].end(); ++it) {
                    const auto& t = it.value();
                    if (!t.contains("streaming")) result.addWarning("Template '" + it.key() + "' no define modo de streaming.");
                    if (!t.contains("lod")) result.addError("Template '" + it.key() + "' requiere definición de 'lod'.");
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
                if (!obj["position"].is_array() || obj["position"].size() != 3) {
                    result.addError("Posición de '" + name + "' debe ser un array [x, y, z].");
                }
            }

            // Regla: Validación específica para Planetas
            if (obj.value("type", "") == "Planet") {
                if (!obj.contains("terrainGenerator") && !obj.contains("template")) {
                    result.addWarning("El planeta '" + name + "' no tiene generador ni template. Será una esfera vacía.");
                }
            }
        }
    };
}