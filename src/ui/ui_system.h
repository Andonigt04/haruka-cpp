#pragma once
#include "ui/ui_world_panel.h"
#include "ui/ui_interaction_system.h"
#include "renderer/shader.h"

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>

namespace Haruka::UI {

/**
 * Top-level manager for world-space UI panels.
 *
 * Usage (per frame):
 *   // 1. Render each panel's ImGui content
 *   for (auto& id : uiSystem.panelIds()) {
 *       auto* p = uiSystem.panel(id);
 *       p->beginImGui();
 *       ImGui::Text("Hello from %s", id.c_str());
 *       p->endImGui();
 *   }
 *
 *   // 2. Update interaction
 *   uiSystem.updateInteraction(camPos, camFwd, interactJustPressed);
 *
 *   // 3. Draw panels into the 3-D scene
 *   uiSystem.drawAll(panelShader, view, proj, camPos);
 */
class UISystem {
public:
    UISystem();
    ~UISystem();

    // -- Panel management ----------------------------------------------------

    // Creates a new panel and returns a raw pointer (owned by UISystem).
    UIWorldPanel* addPanel(UIWorldPanel::Desc desc);

    // Retrieves a panel by id. Returns nullptr if not found.
    UIWorldPanel* panel(const std::string& id);

    // Removes a panel by id.
    void removePanel(const std::string& id);

    std::vector<std::string> panelIds() const;

    // -- Per-frame API -------------------------------------------------------

    // Raycasts panels from camera, fires focus/unfocus/interact callbacks.
    void updateInteraction(const glm::vec3& camPos,
                           const glm::vec3& camFwd,
                           bool interactPressed);

    // Draws all visible panels into the scene.
    // Shader requirements: see UIWorldPanel::draw().
    void drawAll(Shader& shader, const glm::mat4& view,
                 const glm::mat4& proj, const glm::vec3& camPos) const;

    // -- Query ---------------------------------------------------------------

    const std::string& focusedPanelId() const { return m_interaction.focusedId(); }

private:
    std::unordered_map<std::string, std::unique_ptr<UIWorldPanel>> m_panels;
    UIInteractionSystem m_interaction;
};

} // namespace Haruka::UI
