#pragma once
#include "ui/ui_types.h"
#include <glm/glm.hpp>
#include <string>

namespace Haruka::UI {

class UIWorldPanel;

/**
 * Each frame:
 *   1. Call update(camPos, camFwd, panels, interactPressed)
 *   2. System raycasts all visible panels, fires focus/unfocus/interact callbacks.
 *
 * No ownership over panels — the caller (UISystem) owns them.
 */
class UIInteractionSystem {
public:
    // interactPressed — true on the frame the player presses the interact key.
    void update(const glm::vec3& camPos,
                const glm::vec3& camFwd,
                const std::vector<UIWorldPanel*>& panels,
                bool interactPressed);

    // Returns the currently focused panel id, empty if none.
    const std::string& focusedId() const { return m_focusedId; }

private:
    std::string m_focusedId;
};

} // namespace Haruka::UI
