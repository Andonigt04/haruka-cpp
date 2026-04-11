#pragma once

#include "scene.h"
#include "camera.h"
#include <GLFW/glfw3.h>

namespace Haruka {

/**
 * @brief ABI-friendly bridge for project-defined gameplay callbacks.
 *
 * The editor loads a project implementation and calls these function pointers
 * to integrate game-specific logic without hard-coding gameplay into the editor.
 */
struct GameInterface {
    /** @brief Called once when the game session starts. */
    typedef void (*OnInitFunc)(Scene* scene);
    /** @brief Called every frame while the game is running. */
    typedef void (*OnUpdateFunc)(GLFWwindow* window, float deltaTime);
    /** @brief Called once during shutdown. */
    typedef void (*OnShutdownFunc)();
    
    /** @brief Returns the active camera, if the game exposes one. */
    typedef Camera* (*GetCameraFunc)();
    /** @brief Returns the active scene, if the game exposes one. */
    typedef Scene* (*GetSceneFunc)();
    
    /** @name Gameplay callbacks */
    ///@{
    OnInitFunc onInit = nullptr;
    OnUpdateFunc onUpdate = nullptr;
    OnShutdownFunc onShutdown = nullptr;
    GetCameraFunc getCamera = nullptr;
    GetSceneFunc getScene = nullptr;
    ///@}
    
    /** @name Metadata */
    ///@{
    const char* name = "UnnamedGame";
    const char* version = "1.0.0";
    ///@}
};

/** @brief Export macro for game interface symbols. */
#define GAME_INTERFACE_EXPORT extern "C"

}
