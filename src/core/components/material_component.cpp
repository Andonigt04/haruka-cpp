#include "material_component.h"

#include <nlohmann/json.hpp>

namespace Haruka {

// Inicializa slots estándar para evitar comprobaciones de existencia repetidas.
MaterialComponent::MaterialComponent() {
    textures["albedo"] = "";
    textures["normal"] = "";
    textures["metallic"] = "";
    textures["roughness"] = "";
    textures["ao"] = "";
}

// Representación estable para persistencia de escena/proyecto.
nlohmann::json MaterialComponent::toJSON() const {
    nlohmann::json j;
    j["name"] = name;
    j["shaderPath"] = shaderPath;
    j["albedo"] = {albedo.x, albedo.y, albedo.z};
    j["metallic"] = metallic;
    j["roughness"] = roughness;
    j["ao"] = ao;
    j["emission"] = {emission.x, emission.y, emission.z};
    j["textures"] = textures;
    return j;
}

// Carga tolerante: sólo sobreescribe campos presentes en el JSON.
void MaterialComponent::fromJSON(const nlohmann::json& j) {
    if (j.contains("name")) name = j["name"];
    if (j.contains("shaderPath")) shaderPath = j["shaderPath"];
    
    if (j.contains("albedo")) {
        auto& a = j["albedo"];
        albedo = {a[0], a[1], a[2]};
    }
    if (j.contains("metallic")) metallic = j["metallic"];
    if (j.contains("roughness")) roughness = j["roughness"];
    if (j.contains("ao")) ao = j["ao"];
    
    if (j.contains("emission")) {
        auto& e = j["emission"];
        emission = {e[0], e[1], e[2]};
    }
    if (j.contains("textures")) textures = j["textures"];
}

}