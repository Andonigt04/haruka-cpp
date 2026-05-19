#pragma once
#include <string>

namespace Haruka::UI {

// Immediate-mode settings panel.  Call render() each frame while open.
// Returns true when the user clicks "Close" or presses Escape.
class SettingsPanel {
public:
    // Returns true → caller should hide the panel.
    bool render();

private:
    void tabGraphics();
    void tabControls();

    // Rebind state
    std::string   m_rebindingAction; // empty = not rebinding
    bool          m_waitingForKey = false;
};

} // namespace Haruka::UI
