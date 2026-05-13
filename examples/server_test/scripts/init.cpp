#include "init.h"
#include "player/controller/player_controller.h"
#include "game_globals.h"
#include "game/planetary_system.h"
#include "game/character.h"
#include "core/world_system.h"
#include "core/camera.h"
#include "renderer/primitive_shapes.h"
#include "core/components/material_component.h"
#include "core/components/mesh_renderer_component.h"
#include <iostream>
#include <memory>
#include <glm/glm.hpp>
#ifdef HARUKA_NETWORK
#include "network/dgs_bridge.h"
#endif

// Global state
Camera* g_gameCamera = nullptr;
Haruka::Character* g_playerCharacter = nullptr;
GameLogic::PlayerController* g_playerController = nullptr;
Haruka::PlanetarySystem* g_planetarySystem = nullptr;
Haruka::WorldSystem* g_worldSystem = nullptr;
Haruka::Scene* g_runtimeScene = nullptr;

namespace {
constexpr bool kEnableDetailedPlanetSurface = true;
constexpr int kPlanetPreset = 0; // Sustituir por preset propio si es necesario
constexpr int kPlanetSeed = 1337;
}

void gameOnInit(Haruka::Scene* scene) {
    g_runtimeScene = scene;

    if (g_playerController) {
        delete g_playerController;
        g_playerController = nullptr;
    }
    if (g_planetarySystem) {
        delete g_planetarySystem;
        g_planetarySystem = nullptr;
    }
    if (g_worldSystem) {
        delete g_worldSystem;
        g_worldSystem = nullptr;
    }
    
    g_worldSystem = new Haruka::WorldSystem();
    g_planetarySystem = new Haruka::PlanetarySystem();
    g_planetarySystem->init(scene, g_worldSystem);
    g_planetarySystem->setTimeScale(1.0);

    const glm::dvec3 earthWorldPos(0.0, 0.0, 0.0);
    glm::dvec3 playerSpawnDirection(0.0, 1.0, 0.0);
    double playerSpawnHeightKm = 6373.0;

    if (scene) {
        if (auto* sunObj = scene->getObject("Sun")) {
            glm::dvec3 toSun = glm::dvec3(sunObj->position) - earthWorldPos;
            if (glm::length(toSun) > 1e-9) {
                playerSpawnDirection = glm::normalize(toSun);
            }
        }
    }

    Haruka::WorldPos playerPos = Haruka::WorldPos(earthWorldPos + playerSpawnDirection * playerSpawnHeightKm);
    auto playerOwned = std::make_unique<Haruka::Character>(playerPos, "player1");
    g_gameCamera = playerOwned->getCamera();
    g_playerCharacter = playerOwned.get();

    if (scene) {
        if (auto* playerObj = scene->getObject("Player")) {
            playerObj->position = playerPos;
        }
    }

    g_planetarySystem->setPlayer(std::move(playerOwned));
    g_playerController = new GameLogic::PlayerController(g_playerCharacter);

#ifdef HARUKA_NETWORK
    g_playerCharacter->onTransformChanged = [](const Haruka::WorldPos& pos, const Haruka::Rotation& rot) {
        Haruka::Network::sendTransform(1, pos, rot);
    };
#endif
}

void gameOnUpdate(GLFWwindow* window, float deltaTime) {
    if (g_playerCharacter && window) {
        g_playerCharacter->processInput(window, deltaTime);
    }

    if (g_planetarySystem) {
        g_planetarySystem->update(deltaTime);
    }

    if (g_runtimeScene && g_playerCharacter) {
        if (auto* playerObj = g_runtimeScene->getObject("Player")) {
            playerObj->position = g_playerCharacter->getPosition();
        }
    }
    
    if (g_playerController) {
        g_playerController->update(deltaTime);
    }
}

Camera* gameGetCamera() {
    return g_gameCamera;
}

void gameOnShutdown() {
    if (g_playerController) {
        delete g_playerController;
        g_playerController = nullptr;
    }
    if (g_planetarySystem) {
        delete g_planetarySystem;
        g_planetarySystem = nullptr;
    }
    if (g_worldSystem) {
        delete g_worldSystem;
        g_worldSystem = nullptr;
    }

    g_playerCharacter = nullptr;
    g_gameCamera = nullptr;
    g_runtimeScene = nullptr;
}

Haruka::Scene* gameGetScene() {
    return g_runtimeScene;
}

namespace GameLogic {
    Haruka::GameInterface gameInterface = {
        .onInit = gameOnInit,
        .onUpdate = gameOnUpdate,
        .onShutdown = gameOnShutdown,
        .getCamera = gameGetCamera,
        .getScene = gameGetScene,
        .name = "Game",
        .version = "0.1.0"
    };
}

extern "C" {
    Haruka::GameInterface* getGameInterface() {
        return &GameLogic::gameInterface;
    }
}
