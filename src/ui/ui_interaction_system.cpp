#include "ui/ui_interaction_system.h"

#include "ui/ui_world_panel.h"
#include <limits>

namespace Haruka::UI {

void UIInteractionSystem::update(const glm::vec3& camPos,
                                  const glm::vec3& camFwd,
                                  const std::vector<UIWorldPanel*>& panels,
                                  bool interactPressed)
{
    UIWorldPanel* nearest     = nullptr;
    float         nearestDist = std::numeric_limits<float>::max();
    glm::vec2     nearestUV;

    for (UIWorldPanel* panel : panels) {
        if (!panel || !panel->isVisible()) continue;

        const UIInteractConfig& cfg = panel->interactConfig();

        // Distance gate
        float dist = glm::length(panel->anchor().worldPos - camPos);
        if (dist > cfg.interactRange) continue;

        // Raycast gate (when requireLook is true)
        glm::vec2 hitUV;
        if (cfg.requireLook) {
            if (!panel->raycast(camPos, camFwd, hitUV)) continue;
        } else {
            hitUV = glm::vec2(0.5f); // center UV when proximity-only
        }

        if (dist < nearestDist) {
            nearestDist = dist;
            nearest     = panel;
            nearestUV   = hitUV;
        }
    }

    // Update focus state
    std::string newId = nearest ? nearest->id() : "";

    if (newId != m_focusedId) {
        // Unfocus previous
        for (UIWorldPanel* panel : panels) {
            if (panel && panel->id() == m_focusedId)
                panel->setFocused(false);
        }
        // Focus new
        if (nearest)
            nearest->setFocused(true);

        m_focusedId = newId;
    }

    // Interact
    if (interactPressed && nearest)
        nearest->triggerInteract(nearestUV);
}

} // namespace Haruka::UI
