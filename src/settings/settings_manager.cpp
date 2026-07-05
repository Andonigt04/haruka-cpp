#include "settings/settings_manager.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <algorithm>

namespace Haruka {

const Input::ActionValue SettingsManager::s_nullValue = false;

SettingsManager& SettingsManager::get() {
    static SettingsManager instance;
    return instance;
}

// ── helpers ──────────────────────────────────────────────────────────────────

static std::vector<std::string> splitCSV(const char* s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
        while (!tok.empty() && tok.back()  == ' ') tok.pop_back();
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

// Serialises an action's key list with a type prefix so composites round-trip.
// Format:  "Direct:W,Space"  |  "Axis1D:Equals,Minus"  |  "Axis2D:W,S,A,D"
static std::string serializeBinding(const Input::InputAction& a) {
    auto keys = a.keys();
    std::string prefix = std::visit([](const auto& src) -> std::string {
        using T = std::decay_t<decltype(src)>;
        if constexpr (std::is_same_v<T, Input::Axis1DBinding>) return "Axis1D:";
        if constexpr (std::is_same_v<T, Input::Axis2DBinding>) return "Axis2D:";
        return "Direct:";
    }, a.source);

    std::string out = prefix;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) out += ',';
        out += SDL_GetScancodeName(keys[i]);
    }
    return out;
}

// Parses "TypePrefix:K1,K2,..." and updates the action's key list.
// Ignores the prefix when the action already has the correct binding type.
static void deserializeBinding(Input::InputAction& action, const char* val) {
    const char* colon = strchr(val, ':');
    const char* keyPart = colon ? colon + 1 : val; // fallback: no prefix = Direct

    auto names = splitCSV(keyPart);
    std::vector<SDL_Scancode> codes;
    codes.reserve(names.size());
    for (const auto& n : names) {
        SDL_Scancode sc = SDL_GetScancodeFromName(n.c_str());
        if (sc != SDL_SCANCODE_UNKNOWN) codes.push_back(sc);
    }
    action.setKeys(codes);
}

// ── ImGuiSettingsHandler callbacks ───────────────────────────────────────────

static void* gsReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char*) {
    return (void*)1;
}

static void gsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
    auto& sm = SettingsManager::get();
    auto& g  = sm.graphics();
    auto& a  = sm.audio();

    char key[64] = {}, val[64] = {};
    if (sscanf(line, "%63[^=]=%63s", key, val) != 2) return;

    // Valor COMPLETO (con espacios) para nombres de dispositivo: todo tras el primer '='.
    auto fullValue = [&]() -> std::string {
        const char* eq = strchr(line, '=');
        std::string s = eq ? std::string(eq + 1) : std::string();
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s;
    };
    if      (!strcmp(key, "InputDevice"))    { a.inputDevice  = fullValue(); return; }
    else if (!strcmp(key, "OutputDevice"))   { a.outputDevice = fullValue(); return; }
    else if (!strcmp(key, "Language"))       { sm.language()  = fullValue(); return; }

    if      (!strcmp(key, "TextureQuality")) g.textureQuality = (Settings::TextureQuality)atoi(val);
    else if (!strcmp(key, "ShadowQuality"))  g.shadowQuality  = (Settings::ShadowQuality)atoi(val);
    else if (!strcmp(key, "Antialiasing"))   g.antialiasing   = (Settings::AntialiasingMode)atoi(val);
    else if (!strcmp(key, "FOV"))            g.fov            = (float)atof(val);
    else if (!strcmp(key, "RenderScale"))    g.renderScale    = (float)atof(val);
    else if (!strcmp(key, "VSync"))          g.vsync          = atoi(val) != 0;
    else if (!strcmp(key, "SSAO"))           g.ssao           = atoi(val) != 0;
    else if (!strcmp(key, "Bloom"))          g.bloom          = atoi(val) != 0;
    else if (!strcmp(key, "BloomThreshold")) g.bloomThreshold = (float)atof(val);
    else if (!strcmp(key, "BloomStrength"))  g.bloomStrength  = (float)atof(val);
    else if (!strcmp(key, "WaterQuality"))   g.waterQuality   = (Settings::WaterQuality)atoi(val);
    else if (!strcmp(key, "TerrainQuality")) g.terrainQuality = (Settings::TerrainQuality)atoi(val);
    else if (!strcmp(key, "ChunkMemoryMB"))  g.chunkMemoryMB  = atoi(val);
    else if (!strcmp(key, "MaxFps"))         g.maxFps         = atoi(val);
    else if (!strcmp(key, "AdaptiveLOD"))    g.adaptiveLOD    = atoi(val) != 0;
    else if (!strcmp(key, "LODTargetPx"))    g.lodTargetPx    = atoi(val);
    else if (!strcmp(key, "MotionBlur"))     g.motionBlur     = atoi(val) != 0;
    else if (!strcmp(key, "WindowMode"))     g.windowMode     = (Settings::WindowMode)atoi(val);
    else if (!strcmp(key, "MasterVolume"))   a.masterVolume   = (float)atof(val);
    else if (!strcmp(key, "MusicVolume"))    a.musicVolume    = (float)atof(val);
    else if (!strcmp(key, "SFXVolume"))      a.sfxVolume      = (float)atof(val);
}

static void gsWriteAll(ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
    auto& g = SettingsManager::get().graphics();
    auto& a = SettingsManager::get().audio();
    buf->appendf("[%s][Settings]\n", h->TypeName);
    buf->appendf("TextureQuality=%d\n", (int)g.textureQuality);
    buf->appendf("ShadowQuality=%d\n",  (int)g.shadowQuality);
    buf->appendf("Antialiasing=%d\n",   (int)g.antialiasing);
    buf->appendf("FOV=%.2f\n",          g.fov);
    buf->appendf("RenderScale=%.2f\n",  g.renderScale);
    buf->appendf("VSync=%d\n",          g.vsync     ? 1 : 0);
    buf->appendf("SSAO=%d\n",           g.ssao      ? 1 : 0);
    buf->appendf("Bloom=%d\n",          g.bloom     ? 1 : 0);
    buf->appendf("BloomThreshold=%.2f\n", g.bloomThreshold);
    buf->appendf("BloomStrength=%.2f\n",  g.bloomStrength);
    buf->appendf("WaterQuality=%d\n",   (int)g.waterQuality);
    buf->appendf("TerrainQuality=%d\n", (int)g.terrainQuality);
    buf->appendf("ChunkMemoryMB=%d\n",  g.chunkMemoryMB);
    buf->appendf("MaxFps=%d\n",         g.maxFps);
    buf->appendf("AdaptiveLOD=%d\n",    g.adaptiveLOD ? 1 : 0);
    buf->appendf("LODTargetPx=%d\n",    g.lodTargetPx);
    buf->appendf("MotionBlur=%d\n",     g.motionBlur ? 1 : 0);
    buf->appendf("WindowMode=%d\n",     (int)g.windowMode);
    buf->appendf("MasterVolume=%.2f\n", a.masterVolume);
    buf->appendf("MusicVolume=%.2f\n",  a.musicVolume);
    buf->appendf("SFXVolume=%.2f\n",    a.sfxVolume);
    if (!a.inputDevice.empty())  buf->appendf("InputDevice=%s\n",  a.inputDevice.c_str());
    if (!a.outputDevice.empty()) buf->appendf("OutputDevice=%s\n", a.outputDevice.c_str());
    buf->appendf("Language=%s\n", SettingsManager::get().language().c_str());
    buf->appendf("\n");
}

static std::string s_currentGroup;

static void* kgReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    s_currentGroup = name;
    return (void*)1;
}

static void kgReadLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
    char key[64] = {}, val[256] = {};
    if (sscanf(line, "%63[^=]=%255[^\n]", key, val) != 2) return;

    auto* action = SettingsManager::get().findAction(key);
    if (!action) return;
    deserializeBinding(*action, val);
}

static void kgWriteAll(ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
    auto& sm = SettingsManager::get();
    for (const auto& group : sm.groups()) {
        buf->appendf("[%s][%s]\n", h->TypeName, group.c_str());
        for (const auto* a : sm.actionsInGroup(group))
            buf->appendf("%s=%s\n", a->name.c_str(), serializeBinding(*a).c_str());
        buf->appendf("\n");
    }
}

// ── init ─────────────────────────────────────────────────────────────────────

void SettingsManager::init(const std::string& iniPath) {
    m_iniPath = iniPath;
    ImGui::GetIO().IniFilename = nullptr;
    registerImGuiHandlers();
    ImGui::LoadIniSettingsFromDisk(m_iniPath.c_str());
}

void SettingsManager::registerImGuiHandlers() {
    {
        ImGuiSettingsHandler h{};
        h.TypeName   = "GameSettings";
        h.TypeHash   = ImHashStr("GameSettings");
        h.ReadOpenFn = gsReadOpen;
        h.ReadLineFn = gsReadLine;
        h.WriteAllFn = gsWriteAll;
        ImGui::AddSettingsHandler(&h);
    }
    {
        ImGuiSettingsHandler h{};
        h.TypeName   = "KeyGroup";
        h.TypeHash   = ImHashStr("KeyGroup");
        h.ReadOpenFn = kgReadOpen;
        h.ReadLineFn = kgReadLine;
        h.WriteAllFn = kgWriteAll;
        ImGui::AddSettingsHandler(&h);
    }
}

void SettingsManager::save() {
    ImGui::SaveIniSettingsToDisk(m_iniPath.c_str());
}

// ── action registration ───────────────────────────────────────────────────────

void SettingsManager::registerAction(const std::string& name,
                                     const std::string& displayName,
                                     const std::string& group,
                                     std::initializer_list<SDL_Scancode> defaults)
{
    registerAction(name, displayName, group,
                   Input::DirectBinding{ std::vector<SDL_Scancode>(defaults) });
}

void SettingsManager::registerAction(const std::string& name,
                                     const std::string& displayName,
                                     const std::string& group,
                                     Input::BindingSource source)
{
    if (findAction(name)) return;

    Input::InputAction a;
    a.name        = name;
    a.displayName = displayName;
    a.group       = group;
    a.source      = std::move(source);
    m_actions.push_back(std::move(a));
}

// ── update ────────────────────────────────────────────────────────────────────

void SettingsManager::update(const bool* kbd) {
    for (auto& a : m_actions) {
        // While the rebind UI is capturing a key, no action may fire — otherwise the
        // key being bound would also run its current action (and ESC would close the
        // panel instead of cancelling the bind). Treat exactly like a disabled group.
        if (m_capturing || !isGroupEnabled(a.group)) {
            // Force inactive — keep prevValue as-is so canceled fires correctly
            a.prevValue = a.value;
            a.value     = std::visit([](const auto& src) -> Input::ActionValue {
                using T = std::decay_t<decltype(src)>;
                if constexpr (std::is_same_v<T, Input::DirectBinding>)  return false;
                if constexpr (std::is_same_v<T, Input::Axis1DBinding>)  return 0.f;
                if constexpr (std::is_same_v<T, Input::Axis2DBinding>)  return glm::vec2{0.f};
                return false;
            }, a.source);
        } else {
            a.evaluate(kbd);
        }
    }
}

// ── value / phase queries ─────────────────────────────────────────────────────

const Input::ActionValue& SettingsManager::readValue(const std::string& action) const {
    const auto* a = findAction(action);
    return a ? a->value : s_nullValue;
}

bool SettingsManager::isPerformed(const std::string& action) const {
    const auto* a = findAction(action);
    return a && a->isPerformed();
}

bool SettingsManager::isStarted(const std::string& action) const {
    const auto* a = findAction(action);
    return a && a->isStarted();
}

bool SettingsManager::isCanceled(const std::string& action) const {
    const auto* a = findAction(action);
    return a && a->isCanceled();
}

// ── groups ────────────────────────────────────────────────────────────────────

void SettingsManager::enableGroup(const std::string& group) {
    m_disabledGroups.erase(group);
}

void SettingsManager::disableGroup(const std::string& group) {
    m_disabledGroups.insert(group);
}

bool SettingsManager::isGroupEnabled(const std::string& group) const {
    return m_disabledGroups.find(group) == m_disabledGroups.end();
}

// ── rebinding ─────────────────────────────────────────────────────────────────

void SettingsManager::setBindings(const std::string& action, std::vector<SDL_Scancode> keys) {
    if (auto* a = findAction(action)) a->setKeys(keys);
}

void SettingsManager::addBinding(const std::string& action, SDL_Scancode key) {
    if (auto* a = findAction(action)) {
        auto keys = a->keys();
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(key);
            a->setKeys(keys);
        }
    }
}

void SettingsManager::removeBinding(const std::string& action, SDL_Scancode key) {
    if (auto* a = findAction(action)) {
        auto keys = a->keys();
        keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
        a->setKeys(keys);
    }
}

// ── data access ───────────────────────────────────────────────────────────────

Input::InputAction* SettingsManager::findAction(const std::string& name) {
    for (auto& a : m_actions)
        if (a.name == name) return &a;
    return nullptr;
}

const Input::InputAction* SettingsManager::findAction(const std::string& name) const {
    for (const auto& a : m_actions)
        if (a.name == name) return &a;
    return nullptr;
}

std::vector<const Input::InputAction*> SettingsManager::actionsInGroup(const std::string& group) const {
    std::vector<const Input::InputAction*> out;
    for (const auto& a : m_actions)
        if (a.group == group) out.push_back(&a);
    return out;
}

std::vector<std::string> SettingsManager::groups() const {
    std::vector<std::string> out;
    for (const auto& a : m_actions)
        if (std::find(out.begin(), out.end(), a.group) == out.end())
            out.push_back(a.group);
    return out;
}

} // namespace Haruka
