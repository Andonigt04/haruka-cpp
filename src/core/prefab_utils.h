/**
 * @file prefab_utils.h
 * @brief Prefab save/load utilities for `SceneObject` reuse.
 *
 * A prefab is a standalone JSON snapshot of a single `SceneObject` (including
 * its components and properties) that can be instantiated into any scene.
 * The implementation lives in `prefab_utils.cpp`.
 */
#pragma once

#include "scene.h"
#include <string>

namespace Haruka {

/** @brief Saves one scene object as a prefab file. */
void savePrefab(const SceneObject& obj, const std::string& path);
/** @brief Loads a prefab file into a scene object record. */
SceneObject loadPrefab(const std::string& path);

}