#include "init.h"

#include "game/character.h"
#include "core/camera.h"
#include "core/application.h"
#include "renderer/motor_instance.h"
#include "settings/settings_manager.h"
#include "ui/settings_panel.h"
#include "ui/ui_system.h"
#include "renderer/shader.h"
#include <memory>
#include <algorithm>
#include <glm/glm.hpp>
#include <imgui.h>
#include <glad/glad.h>
#ifdef HARUKA_NETWORK
#include "tools/dgs_bridge.h"
#endif

// 3x3 km room — chunks (-1,-1) to (1,1) in X and Z.
// Player spawns at world center (0,0,0) = chunk (0,0,0), local (0,0,0).
// All scene coordinates are in METRES (Units::METER = 1.0, Units::KM = 1000.0).
// Objects that represent km-scale structures use Units::KM in their scale/position.
static constexpr double ROOM_HALF_EXTENT = 1.5 * Haruka::Units::KM; // 1500 m = 1.5 km each side
static constexpr uint32_t PLAYER_UUID    = 1;
static const char*        PLAYER_NAME    = "player1";

static Camera*                            g_gameCamera      = nullptr;
static Haruka::Character*                 g_playerCharacter = nullptr;
static Haruka::SceneManager*              g_scene           = nullptr;
static std::unique_ptr<Haruka::Character> g_playerOwned;
static std::shared_ptr<RigidBody>         g_playerBody;

// Chat state
static char                     g_chatInput[256] = {};
static std::vector<std::string> g_chatLog;
static bool                     g_chatFocused    = false;

// Settings
static Haruka::UI::SettingsPanel g_settingsPanel;
static bool                      g_showSettings = false;

// World-space UI
static Haruka::UI::UISystem      g_uiSystem;
static std::unique_ptr<Shader>   g_panelShader;

// Event-driven key state — updated by gameOnEvent so input works even when
// SDL_GetKeyboardState returns zeros (Wayland remote without kb focus).
static bool  g_evtKeys[SDL_SCANCODE_COUNT] = {};
static int   g_evtKeyCount   = 0;
static int   g_evtMouseCount = 0;
static float        g_mouseDx = 0.f, g_mouseDy = 0.f;
static SDL_Scancode g_lastScancode = SDL_SCANCODE_UNKNOWN;
static SDL_Keycode  g_lastKeycode  = SDLK_UNKNOWN;

static PhysicsEngine* getEngine() {
    Application* app = MotorInstance::getInstance().getApplication();
    return app ? app->getPhysicsEngine() : nullptr;
}

static void registerActions() {
    auto& sm = Haruka::SettingsManager::get();

    // Movement — WASD produces a vec2 (x=strafe, y=forward)
    sm.registerAction("Move",    "Move",         "Movement",
        Haruka::Input::Axis2DBinding{ SDL_SCANCODE_W, SDL_SCANCODE_S,
                                      SDL_SCANCODE_A, SDL_SCANCODE_D });
    sm.registerAction("Jump",    "Jump / Ascend","Movement", { SDL_SCANCODE_SPACE });
    sm.registerAction("Descend", "Descend",      "Movement", { SDL_SCANCODE_LCTRL });
    sm.registerAction("Sprint",  "Sprint",       "Movement", { SDL_SCANCODE_LSHIFT });

    // Combat
    sm.registerAction("Attack",  "Attack",  "Combat", { SDL_SCANCODE_F });
    sm.registerAction("Block",   "Block",   "Combat", { SDL_SCANCODE_G });
    sm.registerAction("Reload",  "Reload",  "Combat", { SDL_SCANCODE_R });

    // UI
    sm.registerAction("Chat",      "Open Chat", "UI", { SDL_SCANCODE_T });
    sm.registerAction("Settings",  "Settings",  "UI", { SDL_SCANCODE_ESCAPE });
    sm.registerAction("Inventory", "Inventory", "UI", { SDL_SCANCODE_I });
    sm.registerAction("Map",       "Map",       "UI", { SDL_SCANCODE_M });
    sm.registerAction("Interact",  "Interact",  "UI", { SDL_SCANCODE_E });
    sm.registerAction("Quit",      "Quit",      "UI", { SDL_SCANCODE_F4 });
}

void gameOnInit(Haruka::SceneManager* scene) {
    g_scene = scene;
    g_playerOwned.reset();

    registerActions();
    Haruka::SettingsManager::get().init("game_settings.ini");

    // Demo world-space panel — floating sign 3 m in front of spawn
    {
        Haruka::UI::UIWorldPanel::Desc d;
        d.id             = "InfoPanel";
        d.anchor.worldPos = glm::vec3(0.f, 1.5f, -3.f);
        d.anchor.normal   = glm::vec3(0.f, 0.f,  1.f);   // faces +Z (toward player)
        d.anchor.pivotNorm = glm::vec2(0.5f, 0.5f);
        d.widthMeters    = 1.2f;
        d.heightMeters   = 0.8f;
        d.billboard      = Haruka::UI::UIBillboardMode::None;
        d.interact.interactRange = 3.0f;
        d.interact.requireLook   = true;
        d.interact.onFocus   = []() { SDL_Log("InfoPanel focused"); };
        d.interact.onUnfocus = []() { SDL_Log("InfoPanel unfocused"); };
        d.interact.onInteract = [](glm::vec2 uv) {
            SDL_Log("InfoPanel interacted at UV (%.2f, %.2f)", uv.x, uv.y);
        };
        g_uiSystem.addPanel(std::move(d));
    }

    g_panelShader = std::make_unique<Shader>(
        "shaders/ui_world_panel.vert", "shaders/ui_world_panel.frag");

    g_playerOwned     = std::make_unique<Haruka::Character>(Haruka::WorldPos(0.0, 0.0, 0.0), PLAYER_NAME);
    g_gameCamera      = g_playerOwned->getCamera();
    g_playerCharacter = g_playerOwned.get();
    g_playerOwned->setFlightMode(false);

    g_playerBody           = std::make_shared<RigidBody>();
    g_playerBody->name     = "player";
    g_playerBody->radius   = 0.4;
    // Engine convention: position = sphere center, so start at (0, radius, 0)
    g_playerBody->position = glm::dvec3(0.0, g_playerBody->radius, 0.0);
    g_playerBody->velocity = glm::dvec3(0.0);
    g_playerBody->mass     = 70.0;
    g_playerOwned->setPhysicsBody(g_playerBody);

    // Register player body + static scene geometry with the engine
    if (PhysicsEngine* eng = getEngine()) {
        eng->setGravity(glm::dvec3(0.0, -9.81, 0.0));
        eng->addBody(g_playerBody);

        eng->clearStaticBoxes();
        if (g_scene) {
            for (const auto& objPtr : g_scene->getAllObjects()) {
                if (!objPtr) continue;
                const std::string& t = objPtr->type;
                if (t == "CelestialBody" || t == "Light" ||
                    t == "Camera"        || t == "Empty") continue;
                eng->addStaticBox(glm::dvec3(objPtr->position),
                                  glm::dvec3(objPtr->scale) * 0.5);
            }
        }
    }

    // Wire SettingsManager as the input provider
    {
        auto& sm = Haruka::SettingsManager::get();
        Haruka::CharacterInputProvider provider;
        provider.readValue   = [&sm](const std::string& a) { return sm.readValue(a); };
        provider.isPerformed = [&sm](const std::string& a) { return sm.isPerformed(a); };
        provider.isStarted   = [&sm](const std::string& a) { return sm.isStarted(a); };
        provider.isCanceled  = [&sm](const std::string& a) { return sm.isCanceled(a); };
        g_playerOwned->setInputProvider(provider);
    }

    // Bind behaviors — developer decides which action does what, with full freedom
    using AT = Haruka::ActionTrigger;

    // Move: vec2 action (WASD composite) → character.move()
    g_playerOwned->bindAction<glm::vec2>("Move", AT::Performed,
        [](Haruka::Character& c, float dt, glm::vec2 v) { c.move(v, dt); });

    // Jump: physics impulse — works because physicsBody is now set
    g_playerOwned->bindAction("Jump", AT::Started,
        [](Haruka::Character& c, float) { c.jump(); });

    // Sprint: held → sprint on, released → sprint off
    g_playerOwned->bindAction("Sprint", AT::Started,
        [](Haruka::Character& c, float) { c.sprint(true); });
    g_playerOwned->bindAction("Sprint", AT::Canceled,
        [](Haruka::Character& c, float) { c.sprint(false); });

#ifdef HARUKA_NETWORK
    g_playerCharacter->onTransformChanged = [](const Haruka::WorldPos& pos, const Haruka::Rotation& rot) {
        Haruka::Network::sendTransform(PLAYER_UUID, pos, rot);
    };
#endif
}

bool gameOnEvent(const SDL_Event* e) {
    if (!e) return false;

    if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode < SDL_SCANCODE_COUNT) {
        g_evtKeys[e->key.scancode] = true;
        g_lastScancode = e->key.scancode;
        g_lastKeycode  = e->key.key;
        ++g_evtKeyCount;
        return false;
    }
    if (e->type == SDL_EVENT_KEY_UP && e->key.scancode < SDL_SCANCODE_COUNT) {
        g_evtKeys[e->key.scancode] = false;
        return false;
    }
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        ++g_evtMouseCount;
        float dx = (float)e->motion.xrel;
        float dy = (float)e->motion.yrel;
        // Wayland remote desktops often send absolute positions with xrel=yrel=0.
        // Fall back to computing the delta from the previous absolute position.
        static float s_lastAbsX = -1.f, s_lastAbsY = -1.f;
        if (dx == 0.f && dy == 0.f && s_lastAbsX >= 0.f) {
            dx = (float)e->motion.x - s_lastAbsX;
            dy = (float)e->motion.y - s_lastAbsY;
        }
        s_lastAbsX = (float)e->motion.x;
        s_lastAbsY = (float)e->motion.y;
        g_mouseDx += dx;
        g_mouseDy += dy;
        return false;
    }
    return false;
}

void gameOnUpdate(SDL_Window* window, float deltaTime) {
    // Retry each frame until Wayland compositor grants pointer lock
    static bool s_mouseCaptured = false;
    if (!s_mouseCaptured && window)
        s_mouseCaptured = SDL_SetWindowRelativeMouseMode(window, true);

    // Merge polled kbd state with event-driven state so either path works
    int numKeys = 0;
    const bool* polledKbd = SDL_GetKeyboardState(&numKeys);
    bool mergedKbd[SDL_SCANCODE_COUNT] = {};
    int mergeCount = (numKeys < SDL_SCANCODE_COUNT) ? numKeys : SDL_SCANCODE_COUNT;
    for (int i = 0; i < mergeCount; ++i)
        mergedKbd[i] = polledKbd[i] || g_evtKeys[i];
    Haruka::SettingsManager::get().update(mergedKbd);
    auto& sm = Haruka::SettingsManager::get();

    // Quit (F4)
    if (sm.justPressed("Quit")) {
        SDL_Event quitEvent{};
        quitEvent.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quitEvent);
    }

    // Settings panel toggle (Escape)
    if (sm.justPressed("Settings") && !g_chatFocused) {
        g_showSettings = !g_showSettings;
        SDL_SetWindowRelativeMouseMode(window, !g_showSettings);
    }
    if (g_showSettings) {
        if (g_settingsPanel.render()) {
            g_showSettings = false;
            SDL_SetWindowRelativeMouseMode(window, true);
        }
        return; // block game input while settings open
    }

    // World-space UI: disabled temporarily to isolate GPU hang
    // if (auto* p = g_uiSystem.panel("InfoPanel")) {
    //     p->beginImGui(); ... p->endImGui();
    // }

    // Update panel interaction
    if (g_gameCamera) {
        glm::dvec3 pos3d = g_playerCharacter->getPosition();
        glm::vec3 camPos = glm::vec3((float)pos3d.x, (float)pos3d.y, (float)pos3d.z);
        glm::vec3 camFwd = g_gameCamera->getFront();
        bool interact = sm.justPressed("Interact") && !g_chatFocused;
        g_uiSystem.updateInteraction(camPos, camFwd, interact);
    }

    // Physics step: gravity integration + sphere-sphere + sphere-AABB (static boxes)
    if (PhysicsEngine* eng = getEngine())
        eng->update((double)deltaTime);

    // Update character state (physics, camera sync)
    if (g_playerOwned) {
        g_playerOwned->update(deltaTime);
    }

    // Input: use the registered action system (SettingsManager already has
    // the merged kbd state from above).
    if (g_playerOwned && !g_chatFocused) {
        // Always apply event-based mouse deltas first. On Wayland remote,
        // SDL_GetWindowRelativeMouseMode can return true while the compositor
        // never actually captured the pointer, leaving SDL_GetRelativeMouseState
        // returning zeros. Using events avoids that false-positive entirely.
        if (g_mouseDx != 0.f || g_mouseDy != 0.f) {
            g_playerOwned->rotate(g_mouseDx, -g_mouseDy);
            g_playerOwned->updateCamera();
            // Drain SDL's hardware delta buffer so processInput doesn't double-rotate.
            { float tx = 0.f, ty = 0.f; SDL_GetRelativeMouseState(&tx, &ty); }
        }
        g_mouseDx = g_mouseDy = 0.f;

        // Fires all bindAction callbacks (Move→move(), Jump, Sprint)
        // and calls updateCamera() once at the end.
        g_playerOwned->processInput(window, deltaTime);

        // Sync X/Z: move() changes character foot position; the engine body
        // position is the sphere center, so center.x/z == foot.x/z. Y stays
        // owned by the engine (gravity + static collision).
        if (g_playerBody) {
            auto pos = g_playerOwned->getPosition(); // foot position
            g_playerBody->position.x = pos.x;
            g_playerBody->position.z = pos.z;
        }
    }

#ifdef HARUKA_NETWORK
    // Drain incoming chat messages
    for (const auto& msg : Haruka::Network::pollChats()) {
        std::string line = std::string(msg.username) + ": " + std::string(msg.text);
        g_chatLog.push_back(std::move(line));
        if (g_chatLog.size() > 50) g_chatLog.erase(g_chatLog.begin());
    }
#endif

    // Debug HUD — top-left overlay
    {
        Uint32 winFlags = window ? SDL_GetWindowFlags(window) : 0;
        bool kbFocus    = !!(winFlags & SDL_WINDOW_INPUT_FOCUS);
        bool mouseLock  = window && SDL_GetWindowRelativeMouseMode(window);

        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.70f);
        ImGui::Begin("##debug", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);

        // Position + per-frame delta (non-zero delta proves move() is firing)
        if (g_playerCharacter) {
            static glm::dvec3 s_prevPos{};
            auto p    = g_playerCharacter->getPosition();
            auto dp   = p - s_prevPos;
            s_prevPos = p;
            auto st   = g_playerCharacter->getState();
            const char* stateStr[] = {"IDLE","WALK","RUN","JUMP","FALL","CROUCH"};
            ImGui::Text("pos   %.3f  %.3f  %.3f", (float)p.x,  (float)p.y,  (float)p.z);
            ImGui::Text("dpos  %.4f  %.4f  %.4f", (float)dp.x, (float)dp.y, (float)dp.z);
            ImGui::Text("state %s  grnd=%d  spr=%d  crouch=%d",
                stateStr[(int)st],
                g_playerCharacter->isGrounded(),
                g_playerCharacter->isSprinting(),
                g_playerCharacter->isCrouching());
        }
        ImGui::Separator();

        // Input system state (SettingsManager — what the bindings actually see)
        {
            glm::vec2 move = sm.readValue<glm::vec2>("Move");
            ImGui::Text("IS  Move=(%.1f,%.1f)  perf=%d  start=%d  cancel=%d",
                move.x, move.y,
                sm.isPerformed("Move"), sm.isStarted("Move"), sm.isCanceled("Move"));
            ImGui::Text("IS  Sprint perf=%d  Jump perf=%d  Descend perf=%d",
                sm.isPerformed("Sprint"), sm.isPerformed("Jump"), sm.isPerformed("Descend"));
        }
        ImGui::Separator();

        // Raw sources for diagnosing why the input system may not see keys
        {
            int numKeys = 0;
            const bool* kbd = SDL_GetKeyboardState(&numKeys);
            ImGui::Text("poll W=%d S=%d A=%d D=%d  Shift=%d  Ctrl=%d",
                kbd[SDL_SCANCODE_W], kbd[SDL_SCANCODE_S],
                kbd[SDL_SCANCODE_A], kbd[SDL_SCANCODE_D],
                kbd[SDL_SCANCODE_LSHIFT], kbd[SDL_SCANCODE_LCTRL]);
            ImGui::Text("evnt W=%d S=%d A=%d D=%d  Shift=%d  Ctrl=%d",
                (int)g_evtKeys[SDL_SCANCODE_W], (int)g_evtKeys[SDL_SCANCODE_S],
                (int)g_evtKeys[SDL_SCANCODE_A], (int)g_evtKeys[SDL_SCANCODE_D],
                (int)g_evtKeys[SDL_SCANCODE_LSHIFT], (int)g_evtKeys[SDL_SCANCODE_LCTRL]);
        }
        ImGui::Text("key_ev=%d  mouse_ev=%d  last=%s",
            g_evtKeyCount, g_evtMouseCount, SDL_GetScancodeName(g_lastScancode));
        ImGui::Text("mouse_dx=%.2f  mouse_dy=%.2f  chat=%d",
            g_mouseDx, g_mouseDy, (int)g_chatFocused);
        ImGui::Separator();
        ImGui::Text("kb_focus=%d  mouse_focus=%d  ptr_lock=%d",
            kbFocus,
            (int)!!(winFlags & SDL_WINDOW_MOUSE_FOCUS),
            (int)mouseLock);
        if (!kbFocus)
            ImGui::TextColored(ImVec4(1,0.4f,0.2f,1), ">> CLICK WINDOW FOR KEYBOARD <<");
        if (g_chatFocused)
            ImGui::TextColored(ImVec4(1,0.6f,0.1f,1), ">> CHAT FOCUSED — press Esc <<");
        ImGui::End();
    }

    // Chat UI
    ImGuiIO& io = ImGui::GetIO();
    float chatW = 360.0f, chatH = 200.0f;
    ImGui::SetNextWindowPos(ImVec2(10.0f, io.DisplaySize.y - chatH - 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(chatW, chatH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::Begin("##chat", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar);

    // Message history
    float inputH = ImGui::GetFrameHeightWithSpacing() + 4.0f;
    ImGui::BeginChild("##chatlog", ImVec2(0, chatH - inputH - 16.0f), false, ImGuiWindowFlags_NoScrollbar);
    for (const auto& line : g_chatLog)
        ImGui::TextWrapped("%s", line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::Separator();

    // Input field — Enter to send, T to focus
    bool sendPressed = false;
    ImGui::SetNextItemWidth(chatW - 16.0f);
    if (sm.justPressed("Chat") && !g_chatFocused) {
        ImGui::SetKeyboardFocusHere();
        g_chatFocused = true;
    }
    if (ImGui::InputText("##chatinput", g_chatInput, sizeof(g_chatInput),
            ImGuiInputTextFlags_EnterReturnsTrue)) {
        sendPressed = true;
    }
    g_chatFocused = ImGui::IsItemActive();

    if (sendPressed && g_chatInput[0] != '\0') {
#ifdef HARUKA_NETWORK
        Haruka::Network::sendChat(PLAYER_UUID, PLAYER_NAME, g_chatInput);
#endif
        // Show own message immediately
        g_chatLog.push_back(std::string(PLAYER_NAME) + ": " + g_chatInput);
        if (g_chatLog.size() > 50) g_chatLog.erase(g_chatLog.begin());
        g_chatInput[0] = '\0';
        g_chatFocused  = false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && g_chatFocused) {
        g_chatFocused = false;
        ImGui::SetWindowFocus(nullptr);
    }

    ImGui::End();
}

Camera* gameGetCamera() { return g_gameCamera; }

void gameOnRenderWorld(const glm::mat4& view, const glm::mat4& proj, const glm::vec3& camPos) {
    if (!g_panelShader) return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    g_uiSystem.drawAll(*g_panelShader, view, proj, camPos);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void gameOnShutdown() {
    // Destroy GL-owned resources while the context is still alive.
    // g_uiSystem is a static — if we don't clear it here, UIWorldPanel's
    // destructor (glDeleteBuffers, RenderTarget, etc.) runs after the GL
    // context is gone, corrupting the heap.
    g_uiSystem.removePanel("InfoPanel");
    g_panelShader.reset();
    g_playerOwned.reset();
    g_playerBody.reset();
    g_playerCharacter = nullptr;
    g_gameCamera      = nullptr;
    g_scene           = nullptr;
}

Haruka::SceneManager* gameGetScene() { return g_scene; }

namespace GameLogic {
    Haruka::GameInterface gameInterface = {
        .onInit          = gameOnInit,
        .onUpdate        = gameOnUpdate,
        .onShutdown      = gameOnShutdown,
        .getCamera       = gameGetCamera,
        .getScene        = gameGetScene,
        .onRenderWorld   = gameOnRenderWorld,
        .onEvent         = gameOnEvent,
        .name            = "Server Test",
        .version         = "0.1.0"
    };
}

extern "C" {
    Haruka::GameInterface* getGameInterface() {
        return &GameLogic::gameInterface;
    }
}
