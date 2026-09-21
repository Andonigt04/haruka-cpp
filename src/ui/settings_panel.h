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
    bool          m_rebindPad     = false; // true = el hueco es del MANDO (boton o eje), no una tecla
    bool          m_rebindPadAxis = false; // ...y de sus ejes (stick/gatillo) en vez de sus botones
    bool          m_rebindPadGroup = false; // el hueco es un GRUPO: stick entero (2 ejes) o cruceta (4 botones)
    bool          m_rebindChain    = false; // teclado de una accion de direccion: tras este hueco, el siguiente
    int           m_chainWaitRelease = 0; // (SDL_Scancode) la tecla del hueco anterior, hasta que se suelte; 0 = ninguna
    void beginRebind(const std::string& action, int slotIndex, bool pad = false, bool padAxis = false,
                     bool padGroup = false, bool chain = false); // -1 = append
    void cancelRebind();
};

} // namespace Haruka::UI
