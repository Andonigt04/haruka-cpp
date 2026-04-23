#pragma once

#include "scene.h"
#include <string>

namespace Haruka {

/** @brief Saves one scene object as a prefab file. */
void savePrefab(const SceneObject& obj, const std::string& path);
/** @brief Loads a prefab file into a scene object record. */
SceneObject loadPrefab(const std::string& path);

}