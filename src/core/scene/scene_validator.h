#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <iostream>

#include "core/planet/prop_cond.h"                 // parsePropCond: valida el `when` de las capas

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
            validatePlanetSurface(s, result, owner);
        }

        /** @brief Bloque `surface` de un PLANETA: zonas del autor, materiales del terreno y capas
         *  de props. La escena de planetas pone aquí `zoneMap`/`materials`/`propLayers` (el loader
         *  lo copia a `surfaceConfig` del objeto). El motor los interpreta en TerrestrialPlanet
         *  y PropLayerTable; validar aquí adelanta los errores de autoría al abrir la escena. */
        static void validatePlanetSurface(const nlohmann::json& s, ValidationResult& result, const std::string& owner) {
            const std::string pfx = owner + ".surface";

            // ZONAS del autor: cada entrada es {name + (center/radiusM | perimeter) | color}.
            if (s.contains("zones")) {
                if (!s["zones"].is_array()) {
                    result.addError(pfx + ".zones debe ser un array.");
                } else {
                    for (const auto& z : s["zones"]) {
                        if (!z.is_object()) { result.addError(pfx + ".zones: cada zona debe ser un objeto."); continue; }
                        const std::string zn = z.value("name", std::string());
                        const std::string zowner = pfx + ".zones['" + (zn.empty() ? "?" : zn) + "']";
                        if (zn.empty()) result.addError(zowner + " no tiene 'name'.");
                        const bool hasCircle = z.contains("center");
                        const bool hasPoly   = z.contains("perimeter");
                        const bool hasColor  = z.contains("color");
                        if (hasCircle) {
                            if (!z["center"].is_array() || z["center"].size() < 2)
                                result.addError(zowner + ".center debe ser [lat, lon].");
                            if (!z.contains("radiusM") || !z["radiusM"].is_number())
                                result.addError(zowner + " con 'center' necesita 'radiusM' (metros).");
                        }
                        if (hasPoly) {
                            if (!z["perimeter"].is_array())
                                result.addError(zowner + ".perimeter debe ser un array de [lat, lon].");
                            else for (const auto& v : z["perimeter"])
                                if (!v.is_array() || v.size() < 2)
                                    result.addError(zowner + ".perimeter: cada vértice debe ser [lat, lon].");
                        }
                        if (hasColor && (!z["color"].is_array() || z["color"].size() < 3))
                            result.addError(zowner + ".color debe ser [r, g, b].");
                        if (!hasCircle && !hasPoly && !hasColor)
                            result.addError(zowner + " no delimita nada: usa 'center'/'radiusM', 'perimeter' o 'color'.");
                    }
                }
            }

            // MATERIALES del terreno: nombre obligatorio, resto informativo.
            if (s.contains("materials")) {
                if (!s["materials"].is_array()) {
                    result.addError(pfx + ".materials debe ser un array.");
                } else {
                    for (const auto& m : s["materials"]) {
                        if (!m.is_object()) { result.addError(pfx + ".materials: cada material debe ser un objeto."); continue; }
                        if (!m.contains("name") || !m["name"].is_string() || m["name"].get<std::string>().empty())
                            result.addError(pfx + ".materials: material sin 'name'.");
                    }
                }
            }

            // CAPAS DE PROPS: `when` (condición booleana) se valida sintácticamente; el resto de
            // campos se comprueban por tipo. El orden de la lista es la prioridad de construcción.
            if (s.contains("propLayers")) {
                if (!s["propLayers"].is_array()) {
                    result.addError(pfx + ".propLayers debe ser un array.");
                } else {
                    int idx = 0;
                    for (const auto& p : s["propLayers"]) {
                        const std::string powner = pfx + ".propLayers[" + std::to_string(idx) + "]";
                        ++idx;
                        if (!p.is_object()) { result.addError(powner + " debe ser un objeto."); continue; }
                        if (p.contains("name") && !p["name"].is_string())
                            result.addError(powner + ".name debe ser string.");
                        if (p.contains("mesh") && !p["mesh"].is_string())
                            result.addError(powner + ".mesh debe ser string.");
                        if (p.contains("densityMap") && !p["densityMap"].is_string())
                            result.addError(powner + ".densityMap debe ser string.");
                        if (p.contains("zones")) {
                            if (!p["zones"].is_array())
                                result.addError(powner + ".zones debe ser un array de nombres.");
                            else for (const auto& z : p["zones"])
                                if (!z.is_string())
                                    result.addError(powner + ".zones: cada entrada debe ser un nombre de zona.");
                        }
                        // CONDICIÓN BOOLEANA: la sintaxis se valida con el mismo parser del motor.
                        if (p.contains("when")) {
                            if (!p["when"].is_string()) {
                                result.addError(powner + ".when debe ser string.");
                            } else {
                                std::string perr;
                                if (!Haruka::Planet::parsePropCond(p["when"].get<std::string>(), perr))
                                    result.addError(powner + ".when: " + perr);
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