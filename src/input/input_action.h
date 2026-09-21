#pragma once
/**
 * @file input/input_action.h
 * @brief Acciones de entrada tipadas: teclado Y mando en la MISMA accion.
 *
 * Hasta el 21-09 esto era solo teclado: las tres fuentes (`DirectBinding`, `Axis1DBinding`,
 * `Axis2DBinding`) eran listas de `SDL_Scancode` y `evaluate` recibia `SDL_GetKeyboardState`. No
 * habia forma de asignar un mando: ni el subsistema (`SDL_INIT_GAMEPAD`) se iniciaba. Andoni:
 * "solo acepta de teclado y no de joystick... ¿no esta planteado que se asigne otro device?" — no
 * lo estaba.
 *
 * Ahora cada fuente lleva SUS dos mitades: las teclas y el mando (botones y ejes). La accion vale
 * lo que diga cualquiera de las dos (OR en los botones; en los ejes gana el de mayor magnitud), asi
 * que el juego no distingue con que se juega: lee "Move" y le da igual si son WASD o el stick.
 * El estado crudo llega en `InputState` (teclas + ejes normalizados + botones); quien lo rellena es
 * `Input::Gamepad` (input/gamepad.h) a traves de `SettingsManager::update`.
 */
#include <string>
#include <vector>
#include <variant>
#include <glm/glm.hpp>
#include <SDL3/SDL.h>

namespace Haruka::Input {

// ── Estado crudo de un frame ──────────────────────────────────────────────────────────────────
struct InputState {
    const bool* kbd = nullptr;                          ///< SDL_GetKeyboardState (o la mezcla del juego)
    bool  gamepad = false;                              ///< hay mando abierto
    float axes[SDL_GAMEPAD_AXIS_COUNT] = {};            ///< −1..1 (gatillos 0..1), SIN zona muerta aplicada
    bool  buttons[SDL_GAMEPAD_BUTTON_COUNT] = {};
    float deadzone = 0.18f;                             ///< radial para los sticks, lineal para los gatillos
};

// ── Fuentes de la accion ──────────────────────────────────────────────────────────────────────

/// Cualquiera de las teclas o botones pulsado → bool. Un eje (gatillo) por encima del umbral cuenta
/// como pulsado: asi "Conjurar" puede ser R o el gatillo derecho.
struct DirectBinding {
    std::vector<SDL_Scancode>      keys;
    std::vector<SDL_GamepadButton> buttons;
    SDL_GamepadAxis                axis = SDL_GAMEPAD_AXIS_INVALID;
    float                          axisThreshold = 0.5f;
};

/// Dos teclas (o dos botones) → float en [−1, 1]; o un eje del mando tal cual.
struct Axis1DBinding {
    SDL_Scancode      positive = SDL_SCANCODE_UNKNOWN;
    SDL_Scancode      negative = SDL_SCANCODE_UNKNOWN;
    SDL_GamepadAxis   axis = SDL_GAMEPAD_AXIS_INVALID;
    SDL_GamepadButton positiveBtn = SDL_GAMEPAD_BUTTON_INVALID;
    SDL_GamepadButton negativeBtn = SDL_GAMEPAD_BUTTON_INVALID;
    bool              invert = false;     ///< cambia el signo del valor final (teclas y mando)
};

/// Cuatro teclas (o la cruceta) → vec2 en [−1,1]², o un stick. `normalize` recorta a la unidad.
/// El eje Y del stick de SDL crece hacia ABAJO: aqui se invierte para que arriba sea +Y como las teclas.
struct Axis2DBinding {
    SDL_Scancode      up = SDL_SCANCODE_UNKNOWN, down = SDL_SCANCODE_UNKNOWN;
    SDL_Scancode      left = SDL_SCANCODE_UNKNOWN, right = SDL_SCANCODE_UNKNOWN;
    bool              normalize = true;
    SDL_GamepadAxis   axisX = SDL_GAMEPAD_AXIS_INVALID, axisY = SDL_GAMEPAD_AXIS_INVALID;
    SDL_GamepadButton upBtn = SDL_GAMEPAD_BUTTON_INVALID, downBtn = SDL_GAMEPAD_BUTTON_INVALID;
    SDL_GamepadButton leftBtn = SDL_GAMEPAD_BUTTON_INVALID, rightBtn = SDL_GAMEPAD_BUTTON_INVALID;
    /// Invertir cada eje del valor FINAL (vale para teclas y mando): quien quiera el stick de mirar
    /// "de avion" (arriba = mirar abajo) o el X al reves lo marca en el panel; el ini lo guarda.
    bool              invertX = false, invertY = false;
};

using BindingSource = std::variant<DirectBinding, Axis1DBinding, Axis2DBinding>;

// ── Valor de la accion ────────────────────────────────────────────────────────────────────────
using ActionValue = std::variant<bool, float, glm::vec2, glm::vec3>;

/// true si el valor esta "activo" (distinto de cero / true).
bool isActive(const ActionValue& v);

// ── InputAction ───────────────────────────────────────────────────────────────────────────────
struct InputAction {
    std::string   name;
    std::string   displayName;
    std::string   group;
    BindingSource source;

    ActionValue value     = false;  // este frame
    ActionValue prevValue = false;  // el anterior

    /// Recalcula el valor con el estado crudo (teclado + mando).
    void evaluate(const InputState& in);
    /// Solo teclado (compatibilidad): sin mando.
    void evaluate(const bool* kbd) { InputState s; s.kbd = kbd; evaluate(s); }

    bool isPerformed()  const; // activa este frame
    bool isStarted()    const; // se activo este frame
    bool isCanceled()   const; // se desactivo este frame

    /// Para el panel de ajustes: las teclas, en el orden de la fuente (up/down/left/right, +/−).
    std::vector<SDL_Scancode> keys() const;
    void setKeys(const std::vector<SDL_Scancode>& keys);
    /// Idem para el mando: `pad` = botones (en el orden de la fuente) y `axes` = ejes (0..2).
    std::vector<SDL_GamepadButton> padButtons() const;
    std::vector<SDL_GamepadAxis>   padAxes() const;
    void setPad(const std::vector<SDL_GamepadButton>& buttons, const std::vector<SDL_GamepadAxis>& axes);
    /// Inversion de ejes (Axis1D: solo X). Devuelve/fija (invX, invY); en Direct no hace nada.
    void invert(bool& x, bool& y) const;
    void setInvert(bool x, bool y);
};

} // namespace Haruka::Input
