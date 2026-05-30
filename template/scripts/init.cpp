#include "init.h"

#include "game/character.h"
#include "core/camera.h"
#include "core/application.h"
#include "renderer/motor_instance.h"
#include "settings/settings_manager.h"
#include <SDL3/SDL.h>
#include <memory>
#include <cstdlib>
#include <glm/glm.hpp>

static Camera*                            g_camera      = nullptr;
static Haruka::Character*                 g_player      = nullptr;
static Haruka::SceneManager*              g_scene       = nullptr;
static std::unique_ptr<Haruka::Character> g_playerOwned;

static void registerActions() {
    auto& sm = Haruka::SettingsManager::get();
    sm.registerAction("Move",    "Move",    "Movement",
        Haruka::Input::Axis2DBinding{ SDL_SCANCODE_W, SDL_SCANCODE_S,
                                      SDL_SCANCODE_A, SDL_SCANCODE_D });
    sm.registerAction("Jump",    "Jump",    "Movement", { SDL_SCANCODE_SPACE });
    sm.registerAction("Sprint",  "Sprint",  "Movement", { SDL_SCANCODE_LSHIFT });
    sm.registerAction("Quit",    "Quit",    "UI",       { SDL_SCANCODE_ESCAPE });
}

void gameOnInit(Haruka::SceneManager* scene) {
    g_scene = scene;

    registerActions();
    Haruka::SettingsManager::get().init("imgui.ini");

    // Player name and spawn position from env vars, then scene object, then defaults.
    const char* envName = std::getenv("PLAYER_NAME");
    std::string playerName = envName ? envName : "player";

    glm::dvec3 spawnPos(0.0, 0.0, 0.0);
    if (scene) {
        for (const auto& obj : scene->getAllObjects()) {
            if (!obj || obj->name != "Player") continue;
            spawnPos = glm::dvec3(obj->position);
            break;
        }
    }
    const char* px = std::getenv("SPAWN_X");
    const char* py = std::getenv("SPAWN_Y");
    const char* pz = std::getenv("SPAWN_Z");
    if (px) spawnPos.x = std::atof(px);
    if (py) spawnPos.y = std::atof(py);
    if (pz) spawnPos.z = std::atof(pz);

    g_playerOwned = std::make_unique<Haruka::Character>(
        Haruka::WorldPos(spawnPos), playerName);
    g_camera = g_playerOwned->getCamera();
    g_player = g_playerOwned.get();

    // Wire input
    {
        auto& sm = Haruka::SettingsManager::get();
        Haruka::CharacterInputProvider provider;
        provider.readValue   = [&sm](const std::string& a) { return sm.readValue(a); };
        provider.isPerformed = [&sm](const std::string& a) { return sm.isPerformed(a); };
        provider.isStarted   = [&sm](const std::string& a) { return sm.isStarted(a); };
        provider.isCanceled  = [&sm](const std::string& a) { return sm.isCanceled(a); };
        g_playerOwned->setInputProvider(provider);
    }

    using AT = Haruka::ActionTrigger;
    g_playerOwned->bindAction<glm::vec2>("Move", AT::Performed,
        [](Haruka::Character& c, float dt, glm::vec2 v) { c.move(v, dt); });
    g_playerOwned->bindAction("Jump",   AT::Started,
        [](Haruka::Character& c, float) { c.jump(); });
    g_playerOwned->bindAction("Sprint", AT::Started,
        [](Haruka::Character& c, float) { c.sprint(true); });
    g_playerOwned->bindAction("Sprint", AT::Canceled,
        [](Haruka::Character& c, float) { c.sprint(false); });
}

// Event-driven input state for Wayland / remote desktop compatibility
static bool  g_evtKeys[SDL_SCANCODE_COUNT] = {};
static float g_mouseDx = 0.f, g_mouseDy = 0.f;

bool gameOnEvent(const SDL_Event* e) {
    if (!e) return false;
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode < SDL_SCANCODE_COUNT) {
        g_evtKeys[e->key.scancode] = true;
        return false;
    }
    if (e->type == SDL_EVENT_KEY_UP && e->key.scancode < SDL_SCANCODE_COUNT) {
        g_evtKeys[e->key.scancode] = false;
        return false;
    }
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        float dx = (float)e->motion.xrel;
        float dy = (float)e->motion.yrel;
        static float s_lastX = -1.f, s_lastY = -1.f;
        if (dx == 0.f && dy == 0.f && s_lastX >= 0.f) {
            dx = (float)e->motion.x - s_lastX;
            dy = (float)e->motion.y - s_lastY;
        }
        s_lastX = (float)e->motion.x;
        s_lastY = (float)e->motion.y;
        g_mouseDx += dx;
        g_mouseDy += dy;
        return false;
    }
    return false;
}

void gameOnUpdate(SDL_Window* window, float deltaTime) {
    static bool s_raised = false;
    if (!s_raised && window) { SDL_RaiseWindow(window); s_raised = true; }

    static bool s_mouseLocked = false;
    if (!s_mouseLocked && window)
        s_mouseLocked = SDL_SetWindowRelativeMouseMode(window, true);

    // Merge polled + event-driven keyboard state
    int numKeys = 0;
    const bool* polled = SDL_GetKeyboardState(&numKeys);
    bool merged[SDL_SCANCODE_COUNT] = {};
    int n = (numKeys < SDL_SCANCODE_COUNT) ? numKeys : SDL_SCANCODE_COUNT;
    for (int i = 0; i < n; ++i)
        merged[i] = polled[i] || g_evtKeys[i];
    Haruka::SettingsManager::get().update(merged);

    auto& sm = Haruka::SettingsManager::get();
    if (sm.justPressed("Quit")) {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }

    if (!g_playerOwned) return;

    if (g_mouseDx != 0.f || g_mouseDy != 0.f) {
        g_playerOwned->rotate(g_mouseDx, -g_mouseDy);
        g_playerOwned->updateCamera();
        { float tx = 0.f, ty = 0.f; SDL_GetRelativeMouseState(&tx, &ty); }
    }
    g_mouseDx = g_mouseDy = 0.f;

    g_playerOwned->update(deltaTime);
    g_playerOwned->processInput(window, deltaTime);
}

Camera* gameGetCamera() { return g_camera; }

void gameOnShutdown() {
    g_playerOwned.reset();
    g_player  = nullptr;
    g_camera  = nullptr;
    g_scene   = nullptr;
}

namespace GameLogic {
    Haruka::GameInterface gameInterface = {
        .onInit     = gameOnInit,
        .onUpdate   = gameOnUpdate,
        .onShutdown = gameOnShutdown,
        .getCamera  = gameGetCamera,
        .onEvent    = gameOnEvent,
    };
}

extern "C" {
    Haruka::GameInterface* getGameInterface() {
        return &GameLogic::gameInterface;
    }
}
