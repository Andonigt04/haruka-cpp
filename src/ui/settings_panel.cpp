#include "ui/settings_panel.h"
#include "settings/settings_manager.h"
#include "renderer/motor_instance.h"
#include "core/application.h"
#include "core/locale.h"
#include "core/modules.h"
#ifdef HARUKA_MOD_AUDIO
#include "audio/audio_manager.h"
#endif

#include <imgui.h>
#include <SDL3/SDL.h>
#include <cstring>
#include <filesystem>
#include <string>

namespace Haruka::UI {

void SettingsPanel::beginRebind(const std::string& action, int slotIndex) {
    m_rebindingAction = action;
    m_rebindIndex     = slotIndex;
    m_waitingForKey   = true;
    SettingsManager::get().setCapturing(true); // freeze all actions until a key lands
}

void SettingsPanel::cancelRebind() {
    m_rebindingAction.clear();
    m_rebindIndex   = -1;
    m_waitingForKey = false;
    SettingsManager::get().setCapturing(false);
}

static const char* texQualityNames[]   = { "Low", "Medium", "High", "Ultra" };
static const char* shadowQualNames[]   = { "Off",  "Low",   "Medium", "High" };
static const char* aaNames[]           = { "None", "FXAA",  "TAA" };
static const char* waterQualityNames[] = { "Low", "Medium", "High", "Ultra" };

bool SettingsPanel::render() {
    auto& sm = SettingsManager::get();
    bool close = false;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(580, 480), ImGuiCond_Always);

    if (!ImGui::Begin((TR("settings.title") + "##panel").c_str(), nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus))
    {
        ImGui::End();
        return false;
    }

    if (ImGui::BeginTabBar("##settingsTabs")) {
        if (ImGui::BeginTabItem(TR("settings.graphics").c_str())) { tabGraphics(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(TR("settings.audio").c_str()))    { tabAudio();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(TR("settings.language").c_str())) { tabLanguage(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(TR("settings.controls").c_str())) { tabControls(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button(TR("ui.saveClose").c_str(), ImVec2(140, 0))) {
        sm.save();
        if (auto* app = MotorInstance::getInstance().getApplication())
            app->applyGraphicsSettings();
        close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("ui.cancel").c_str(), ImVec2(90, 0)))
        close = true;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !m_waitingForKey)
        close = true;

    if (close) cancelRebind(); // also clears the capture flag

    ImGui::End();
    return close;
}

// ── Graphics tab ─────────────────────────────────────────────────────────────

void SettingsPanel::tabGraphics() {
    auto& g = SettingsManager::get().graphics();

    ImGui::SeparatorText(TR("gfx.performance").c_str());
    if (ImGui::Button(TR("gfx.presetLow").c_str())) {
        Settings::applyLowPreset(g);
        if (auto* app = MotorInstance::getInstance().getApplication())
            app->applyGraphicsSettings();
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("gfx.presetHigh").c_str())) {
        Settings::applyHighPreset(g);
        if (auto* app = MotorInstance::getInstance().getApplication())
            app->applyGraphicsSettings();
    }
    {
        int fps = g.maxFps;
        if (ImGui::SliderInt(TR("gfx.fpsCap").c_str(), &fps, 0, 240,
                             fps == 0 ? TR("gfx.uncapped").c_str() : "%d"))
            g.maxFps = fps;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("gfx.fpsCap.tip").c_str());
    }

    ImGui::SeparatorText(TR("gfx.rendering").c_str());

    int tq = (int)g.textureQuality;
    if (ImGui::Combo(TR("gfx.textureQuality").c_str(), &tq, texQualityNames, 4))
        g.textureQuality = (Settings::TextureQuality)tq;

    int sq = (int)g.shadowQuality;
    if (ImGui::Combo(TR("gfx.shadowQuality").c_str(), &sq, shadowQualNames, 4))
        g.shadowQuality = (Settings::ShadowQuality)sq;

    int tq2 = (int)g.terrainQuality;
    if (ImGui::Combo(TR("gfx.terrainQuality").c_str(), &tq2, waterQualityNames, 4))
        g.terrainQuality = (Settings::TerrainQuality)tq2;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("gfx.terrainQuality.tip").c_str());

    int wq = (int)g.waterQuality;
    if (ImGui::Combo(TR("gfx.waterQuality").c_str(), &wq, waterQualityNames, 4))
        g.waterQuality = (Settings::WaterQuality)wq;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("gfx.waterQuality.tip").c_str());

    int aa = (int)g.antialiasing;
    if (ImGui::Combo(TR("gfx.antialiasing").c_str(), &aa, aaNames, 3))
        g.antialiasing = (Settings::AntialiasingMode)aa;
    if (aa == (int)Settings::AntialiasingMode::TAA)
        ImGui::TextDisabled("  %s", TR("gfx.taaNote").c_str());

    ImGui::SliderFloat(TR("gfx.fov").c_str(), &g.fov, 60.0f, 120.0f, "%.0f°");
    ImGui::SliderFloat(TR("gfx.renderScale").c_str(), &g.renderScale, 0.5f, 2.0f, "%.2f");

    ImGui::SliderInt(TR("gfx.chunkMemory").c_str(), &g.chunkMemoryMB, 64, 4096);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("gfx.chunkMemory.tip").c_str());

    ImGui::SeparatorText(TR("gfx.postProcessing").c_str());
    ImGui::Checkbox(TR("gfx.vsync").c_str(), &g.vsync);
    ImGui::Checkbox(TR("gfx.ssao").c_str(),  &g.ssao);
    if (g.ssao) ImGui::TextDisabled("  %s", TR("gfx.ssaoNote").c_str());
    ImGui::Checkbox(TR("gfx.bloom").c_str(), &g.bloom);
    if (g.bloom) {
        ImGui::Indent();
        ImGui::SliderFloat(TR("gfx.bloomThreshold").c_str(), &g.bloomThreshold, 0.0f, 1.5f, "%.2f");
        ImGui::SliderFloat(TR("gfx.bloomStrength").c_str(),  &g.bloomStrength,  0.0f, 2.0f, "%.2f");
        ImGui::Unindent();
    }
    ImGui::Checkbox(TR("gfx.motionBlur").c_str(), &g.motionBlur);
    if (g.motionBlur) ImGui::TextDisabled("  %s", TR("gfx.motionBlurNote").c_str());
}

// ── Audio tab ────────────────────────────────────────────────────────────────
// Dispositivos = combo con los que enumera SDL3. Guarda el NOMBRE (vacío = predeterminado);
// el juego (voz/OpenAL) abre el dispositivo por nombre. Cambia en caliente al guardar.
static void deviceCombo(const char* label, bool recording, std::string& sel) {
    int count = 0;
    SDL_AudioDeviceID* ids = recording ? SDL_GetAudioRecordingDevices(&count)
                                       : SDL_GetAudioPlaybackDevices(&count);
    std::string def = TR("audio.defaultDevice");
    const char* preview = sel.empty() ? def.c_str() : sel.c_str();
    if (ImGui::BeginCombo(label, preview)) {
        if (ImGui::Selectable(def.c_str(), sel.empty())) sel.clear();
        for (int i = 0; i < count; ++i) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            if (!name) continue;
            bool chosen = (sel == name);
            if (ImGui::Selectable(name, chosen)) sel = name;
        }
        ImGui::EndCombo();
    }
    if (ids) SDL_free(ids);
}

void SettingsPanel::tabAudio() {
    auto& a = SettingsManager::get().audio();

    ImGui::SeparatorText(TR("audio.devices").c_str());
    // Entrada (micro): por SDL (la voz/Vosk lee de SDL).
    deviceCombo(TR("audio.input").c_str(),  /*recording*/true,  a.inputDevice);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", TR("audio.input.tip").c_str());

    // Salida: la reproducción va por OpenAL → enumera con OpenAL (no SDL). Así sí aparecen
    // todos los dispositivos y elegir uno cambia a dónde suena (al reiniciar).
#ifdef HARUKA_MOD_AUDIO
    {
        std::string def = TR("audio.defaultDevice");
        const char* preview = a.outputDevice.empty() ? def.c_str() : a.outputDevice.c_str();
        if (ImGui::BeginCombo(TR("audio.output").c_str(), preview)) {
            if (ImGui::Selectable(def.c_str(), a.outputDevice.empty())) a.outputDevice.clear();
            for (const auto& d : AudioManager::playbackDevices()) {
                bool sel = (a.outputDevice == d);
                if (ImGui::Selectable(d.c_str(), sel)) a.outputDevice = d;
            }
            ImGui::EndCombo();
        }
    }
#else
    deviceCombo(TR("audio.output").c_str(), /*recording*/false, a.outputDevice);
#endif
    ImGui::TextDisabled("%s", TR("audio.applyOnSave").c_str());

    ImGui::SeparatorText(TR("audio.volume").c_str());
    ImGui::SliderFloat(TR("audio.master").c_str(), &a.masterVolume, 0.0f, 1.0f);
    ImGui::SliderFloat(TR("audio.music").c_str(),  &a.musicVolume,  0.0f, 1.0f);
    ImGui::SliderFloat(TR("audio.sfx").c_str(),    &a.sfxVolume,    0.0f, 1.0f);
}

// ── Language tab (i18n) ──────────────────────────────────────────────────────
// SOLO selección entre los idiomas INSTALADOS (los que tienen assets/lang/<c>.json). La
// gestión modular (instalar/quitar idiomas, voz y audio) se hace en el LAUNCHER, no aquí.
void SettingsPanel::tabLanguage() {
    auto& sm  = SettingsManager::get();
    auto& loc = Locale::get();

    static bool s_scanned = false;
    if (!s_scanned) { loc.scan(); s_scanned = true; }

    std::string curName = sm.language();
    for (const auto& l : loc.available()) if (l.code == sm.language()) curName = l.name;
    if (ImGui::BeginCombo(TR("language.active").c_str(), curName.c_str())) {
        for (const auto& l : loc.available()) {           // solo idiomas instalados
            bool sel = (l.code == sm.language());
            if (ImGui::Selectable((l.name + "  (" + l.code + ")").c_str(), sel)) {
                sm.language() = l.code;
                loc.load(l.code);   // aplica al instante (la UI usa TR())
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("%s", TR("language.managedInLauncher").c_str());
}

// ── Controls tab ─────────────────────────────────────────────────────────────

void SettingsPanel::tabControls() {
    auto& sm = SettingsManager::get();

    // While waiting for a key press, capture from SDL. SettingsManager is in
    // capture mode (set by beginRebind) so no action fires from this keypress.
    if (m_waitingForKey) {
        ImGui::TextColored(ImVec4(1,1,0,1), TR("ctrl.pressKey").c_str(), m_rebindingAction.c_str());

        int numKeys = 0;
        const bool* kbd = SDL_GetKeyboardState(&numKeys);
        for (int i = 1; i < numKeys; ++i) {
            if (!kbd[i]) continue;
            SDL_Scancode sc = (SDL_Scancode)i;
            if (sc == SDL_SCANCODE_ESCAPE) {
                cancelRebind(); // cancel, keep current binding
            } else if (m_rebindIndex >= 0) {
                // Replace a specific slot (works for axis bindings too, where the
                // slot count is fixed and appending would be discarded).
                if (auto* act = sm.findAction(m_rebindingAction)) {
                    auto keys = act->keys();
                    if (m_rebindIndex < (int)keys.size()) {
                        keys[m_rebindIndex] = sc;
                        sm.setBindings(m_rebindingAction, keys);
                    }
                }
                cancelRebind();
            } else {
                sm.addBinding(m_rebindingAction, sc); // append (Direct actions)
                cancelRebind();
            }
            break;
        }
        ImGui::Separator();
    }

    // Table layout: Group header → Action rows
    for (const auto& group : sm.groups()) {
        ImGui::SeparatorText(group.c_str());

        if (!ImGui::BeginTable(("##kg_" + group).c_str(), 3,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
            continue;

        ImGui::TableSetupColumn(TR("ctrl.action").c_str(),   ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn(TR("ctrl.bindings").c_str(), ImGuiTableColumnFlags_WidthStretch, 0.50f);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed,   90.0f);
        ImGui::TableHeadersRow();

        for (const auto* action : sm.actionsInGroup(group)) {
            ImGui::TableNextRow();

            // Action display name
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(action->displayName.c_str());

            // Current bindings as chips
            ImGui::TableSetColumnIndex(1);
            auto keys = action->keys();
            for (size_t i = 0; i < keys.size(); ++i) {
                const char* kname = SDL_GetScancodeName(keys[i]);
                ImGui::PushID((int)i);

                ImGui::SmallButton(kname);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    beginRebind(action->name, (int)i);  // rebind THIS slot
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                    sm.removeBinding(action->name, keys[i]);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", TR("ctrl.chipTip").c_str());

                ImGui::PopID();
                if (i + 1 < keys.size()) ImGui::SameLine(0, 4);
            }

            // Add / clear buttons
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(action->name.c_str());

            if (ImGui::SmallButton(TR("ctrl.add").c_str()))
                beginRebind(action->name, -1); // append a new key (Direct actions)
            ImGui::SameLine(0, 6);
            if (ImGui::SmallButton(TR("ctrl.clear").c_str()))
                sm.setBindings(action->name, {});

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

} // namespace Haruka::UI
