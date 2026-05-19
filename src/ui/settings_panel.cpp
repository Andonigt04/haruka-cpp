#include "ui/settings_panel.h"
#include "settings/settings_manager.h"
#include "renderer/motor_instance.h"
#include "core/application.h"

#include <imgui.h>
#include <SDL3/SDL.h>
#include <cstring>

namespace Haruka::UI {

static const char* texQualityNames[]  = { "Low", "Medium", "High", "Ultra" };
static const char* shadowQualNames[]  = { "Off",  "Low",   "Medium", "High" };
static const char* aaNames[]          = { "None", "FXAA",  "TAA" };

bool SettingsPanel::render() {
    auto& sm = SettingsManager::get();
    bool close = false;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(580, 480), ImGuiCond_Always);

    if (!ImGui::Begin("Settings##panel", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus))
    {
        ImGui::End();
        return false;
    }

    if (ImGui::BeginTabBar("##settingsTabs")) {
        if (ImGui::BeginTabItem("Graphics"))  { tabGraphics(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Controls"))  { tabControls(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Save & Close", ImVec2(120, 0))) {
        sm.save();
        if (auto* app = MotorInstance::getInstance().getApplication())
            app->applyGraphicsSettings();
        close = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0)))
        close = true;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !m_waitingForKey)
        close = true;

    if (close) { m_rebindingAction.clear(); m_waitingForKey = false; }

    ImGui::End();
    return close;
}

// ── Graphics tab ─────────────────────────────────────────────────────────────

void SettingsPanel::tabGraphics() {
    auto& g = SettingsManager::get().graphics();
    auto& a = SettingsManager::get().audio();

    ImGui::SeparatorText("Rendering");

    int tq = (int)g.textureQuality;
    if (ImGui::Combo("Texture Quality", &tq, texQualityNames, 4))
        g.textureQuality = (Settings::TextureQuality)tq;

    int sq = (int)g.shadowQuality;
    if (ImGui::Combo("Shadow Quality", &sq, shadowQualNames, 4))
        g.shadowQuality = (Settings::ShadowQuality)sq;

    int aa = (int)g.antialiasing;
    if (ImGui::Combo("Anti-aliasing", &aa, aaNames, 3))
        g.antialiasing = (Settings::AntialiasingMode)aa;

    ImGui::SliderFloat("FOV", &g.fov, 60.0f, 120.0f, "%.0f°");
    ImGui::SliderFloat("Render Scale", &g.renderScale, 0.5f, 2.0f, "%.2f");

    ImGui::SeparatorText("Post-processing");
    ImGui::Checkbox("VSync",        &g.vsync);
    ImGui::Checkbox("SSAO",         &g.ssao);
    ImGui::Checkbox("Bloom",        &g.bloom);
    ImGui::Checkbox("Motion Blur",  &g.motionBlur);

    ImGui::SeparatorText("Audio");
    ImGui::SliderFloat("Master Volume", &a.masterVolume, 0.0f, 1.0f);
    ImGui::SliderFloat("Music Volume",  &a.musicVolume,  0.0f, 1.0f);
    ImGui::SliderFloat("SFX Volume",    &a.sfxVolume,    0.0f, 1.0f);
}

// ── Controls tab ─────────────────────────────────────────────────────────────

void SettingsPanel::tabControls() {
    auto& sm = SettingsManager::get();

    // While waiting for a key press, capture from SDL
    if (m_waitingForKey) {
        ImGui::TextColored(ImVec4(1,1,0,1), "Press a key to bind to \"%s\"  (Escape = cancel)",
                           m_rebindingAction.c_str());

        int numKeys = 0;
        const bool* kbd = SDL_GetKeyboardState(&numKeys);
        for (int i = 1; i < numKeys; ++i) {
            if (!kbd[i]) continue;
            SDL_Scancode sc = (SDL_Scancode)i;
            if (sc == SDL_SCANCODE_ESCAPE) {
                m_rebindingAction.clear();
                m_waitingForKey = false;
            } else {
                sm.addBinding(m_rebindingAction, sc);
                m_waitingForKey = false;
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

        ImGui::TableSetupColumn("Action",    ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("Bindings",  ImGuiTableColumnFlags_WidthStretch, 0.50f);
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
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                    sm.removeBinding(action->name, keys[i]);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Right-click to remove");

                ImGui::PopID();
                if (i + 1 < keys.size()) ImGui::SameLine(0, 4);
            }

            // Add / clear buttons
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(action->name.c_str());

            if (ImGui::SmallButton("+ Add")) {
                m_rebindingAction = action->name;
                m_waitingForKey   = true;
            }
            ImGui::SameLine(0, 6);
            if (ImGui::SmallButton("Clear"))
                sm.setBindings(action->name, {});

            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

} // namespace Haruka::UI
