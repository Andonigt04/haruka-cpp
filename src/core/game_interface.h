/**
 * @file game_interface.h
 * @brief ABI bridge between the editor/runtime and project-defined gameplay code.
 *
 * The engine loads the game DLL and reads a `GameInterface` struct exported
 * by the game code. Function pointers are assigned at load time; null pointers
 * mean the game doesn't implement that callback. The `GAME_INTERFACE_EXPORT`
 * macro ensures C linkage to avoid name-mangling across DLL boundaries.
 */
#pragma once

#include "camera.h"
#include <SDL3/SDL.h>

namespace Haruka {

class SceneManager;

/**
 * @brief ABI-friendly bridge for project-defined gameplay callbacks.
 *
 * The editor loads a project implementation and calls these function pointers
 * to integrate game-specific logic without hard-coding gameplay into the editor.
 */
struct GameInterface {
    /** @brief Called once when the game session starts. */
    typedef void (*OnInitFunc)(SceneManager* scene);
    /** @brief Called every frame while the game is running. */
    typedef void (*OnUpdateFunc)(SDL_Window* window, float deltaTime);
    /** @brief Called once during shutdown. */
    typedef void (*OnShutdownFunc)();
    
    /** @brief Returns the active camera, if the game exposes one. */
    typedef Camera* (*GetCameraFunc)();
    /** @brief Returns the active scene, if the game exposes one. */
    typedef SceneManager* (*GetSceneFunc)();
    
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
