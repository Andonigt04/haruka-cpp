#pragma once
/**
 * @file input/gamepad.h
 * @brief EL MANDO: abre el primero que haya, sigue a los que entran y salen, y da su estado por
 *        frame a las acciones (`InputState`).
 *
 * SDL no arranca el subsistema de mandos con `SDL_INIT_VIDEO`, y nadie lo pedia: por eso el juego
 * "recibia" (SDL enumera el joystick) pero ninguna accion podia atarse a el. Aqui se inicia
 * `SDL_INIT_GAMEPAD` la primera vez que alguien pregunta, se abre el primer mando reconocido y se
 * lee con `SDL_GetGamepadAxis/Button` — los valores los actualiza `SDL_PumpEvents`, que el bucle de
 * la app ya llama. Si el mando se desconecta, el siguiente `poll` lo suelta y `gamepad = false`:
 * las acciones vuelven a ser solo de teclado sin que nadie tenga que enterarse.
 *
 * Un mando a la vez (el primero): el juego es de un jugador local. `HARUKA_GAMEPAD=0` lo apaga.
 */
#include "input/input_action.h"
#include <string>

namespace Haruka::Input {

class Gamepad {
public:
    static Gamepad& get();
    /// Rellena `axes`, `buttons` y `gamepad` de `in` (deja `kbd` como este). Abre/cierra mandos.
    void poll(InputState& in);
    bool connected() const { return m_pad != nullptr; }
    const std::string& name() const { return m_name; }
    /// Nombres para el panel/ini: los de SDL ("south", "leftx"...), y su inverso.
    static const char* buttonName(SDL_GamepadButton b);
    static const char* axisName(SDL_GamepadAxis a);
    static SDL_GamepadButton buttonFromName(const char* n);
    static SDL_GamepadAxis   axisFromName(const char* n);
private:
    Gamepad() = default;
    void ensureInit();
    void openIfNeeded();
    bool         m_init = false, m_disabled = false;
    SDL_Gamepad* m_pad  = nullptr;
    std::string  m_name;
};

} // namespace Haruka::Input
