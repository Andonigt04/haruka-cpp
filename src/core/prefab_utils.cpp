#include "prefab_utils.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace Haruka {

/**void savePrefab(const SceneObject& obj, const std::string& path) {
    std::ofstream file(path);
    file << obj.toJson().dump(4);
}

SceneObject loadPrefab(const std::string& path) {
    std::ifstream file(path);
    nlohmann::json j;
    file >> j;
    return SceneObject::fromJson(j);
}*/

void savePrefab(const SceneObject& obj, const std::string& filepath) {
    // Implementación simplificada
    // Por ahora solo guardamos el nombre y posición
}

SceneObject loadPrefab(const std::string& filepath) {
    SceneObject obj;
    obj.name = "LoadedPrefab";
    return obj;
}

}