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
    void tabAudio();
    void tabLanguage();

    // Rebind state
    std::string   m_rebindingAction; // empty = not rebinding
    bool          m_waitingForKey = false;
    int           m_rebindIndex   = -1; // slot to replace; -1 = append a new key
    void beginRebind(const std::string& action, int slotIndex); // -1 = append
    void cancelRebind();
};

} // namespace Haruka::UI
