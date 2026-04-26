#ifndef PREFAB_UTILS_H
#define PREFAB_UTILS_H

#include <string>

#include "scene.h"

namespace Haruka {

/** @brief Saves one scene object as a prefab file. */
void savePrefab(const SceneObject& obj, const std::string& path);
/** @brief Loads a prefab file into a scene object record. */
SceneObject loadPrefab(const std::string& path);

}

#endif