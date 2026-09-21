#include "input/input_action.h"
#include <glm/gtc/type_ptr.hpp>
#include <cmath>

namespace Haruka::Input {

// ── isActive ─────────────────────────────────────────────────────────────────

bool isActive(const ActionValue& v) {
    return std::visit([](const auto& val) -> bool {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, bool>)      return val;
        if constexpr (std::is_same_v<T, float>)     return val != 0.f;
        if constexpr (std::is_same_v<T, glm::vec2>) return glm::length(val) > 1e-6f;
        if constexpr (std::is_same_v<T, glm::vec3>) return glm::length(val) > 1e-6f;
        return false;
    }, v);
}

// ── helpers del mando ─────────────────────────────────────────────────────────

static bool keyDown(const InputState& in, SDL_Scancode sc) {
    return in.kbd && sc > SDL_SCANCODE_UNKNOWN && sc < SDL_SCANCODE_COUNT && in.kbd[sc];
}
static bool btnDown(const InputState& in, SDL_GamepadButton b) {
    return in.gamepad && b > SDL_GAMEPAD_BUTTON_INVALID && b < SDL_GAMEPAD_BUTTON_COUNT && in.buttons[b];
}
static float axisRaw(const InputState& in, SDL_GamepadAxis a) {
    return (in.gamepad && a > SDL_GAMEPAD_AXIS_INVALID && a < SDL_GAMEPAD_AXIS_COUNT) ? in.axes[a] : 0.f;
}
// Zona muerta LINEAL con reescalado: por debajo 0, y de ahi a 1 sin escalon (un eje que pasara de
// 0 a 0,18 de golpe se notaria como un tiron).
static float deadzone1(float v, float dz) {
    const float a = std::fabs(v);
    if (a <= dz) return 0.f;
    const float s = (a - dz) / (1.f - dz);
    return (v < 0.f ? -s : s);
}
// Zona muerta RADIAL para un stick: la misma en todas las direcciones (por eje, el diagonal
// entraba antes que el recto, y el circulo de reposo era un cuadrado).
static glm::vec2 deadzone2(glm::vec2 v, float dz) {
    const float l = glm::length(v);
    if (l <= dz) return glm::vec2(0.f);
    const float s = std::min((l - dz) / (1.f - dz), 1.f);
    return v * (s / l);
}

// ── evaluate ─────────────────────────────────────────────────────────────────

void InputAction::evaluate(const InputState& in) {
    prevValue = value;

    std::visit([&](const auto& src) {
        using T = std::decay_t<decltype(src)>;

        if constexpr (std::is_same_v<T, DirectBinding>) {
            bool held = false;
            for (SDL_Scancode sc : src.keys) if (keyDown(in, sc)) { held = true; break; }
            for (SDL_GamepadButton b : src.buttons) if (btnDown(in, b)) { held = true; break; }
            if (src.axis != SDL_GAMEPAD_AXIS_INVALID && axisRaw(in, src.axis) >= src.axisThreshold) held = true;
            value = held;

        } else if constexpr (std::is_same_v<T, Axis1DBinding>) {
            float v = 0.f;
            if (keyDown(in, src.positive) || btnDown(in, src.positiveBtn)) v += 1.f;
            if (keyDown(in, src.negative) || btnDown(in, src.negativeBtn)) v -= 1.f;
            // El eje analogico solo manda si dice mas que las teclas: asi el stick no anula una
            // tecla pulsada ni la tecla anula el stick.
            const float a = deadzone1(axisRaw(in, src.axis), in.deadzone);
            if (std::fabs(a) > std::fabs(v)) v = a;
            value = src.invert ? -v : v;

        } else if constexpr (std::is_same_v<T, Axis2DBinding>) {
            glm::vec2 v{0.f};
            if (keyDown(in, src.up)    || btnDown(in, src.upBtn))    v.y += 1.f;
            if (keyDown(in, src.down)  || btnDown(in, src.downBtn))  v.y -= 1.f;
            if (keyDown(in, src.right) || btnDown(in, src.rightBtn)) v.x += 1.f;
            if (keyDown(in, src.left)  || btnDown(in, src.leftBtn))  v.x -= 1.f;
            if (src.normalize && glm::length(v) > 1.f) v = glm::normalize(v);
            if (src.axisX != SDL_GAMEPAD_AXIS_INVALID || src.axisY != SDL_GAMEPAD_AXIS_INVALID) {
                // Y de SDL crece hacia abajo; aqui arriba es +Y, como la tecla `up`.
                glm::vec2 s(axisRaw(in, src.axisX), -axisRaw(in, src.axisY));
                s = deadzone2(s, in.deadzone);
                if (glm::length(s) > glm::length(v)) v = s;
            }
            if (src.invertX) v.x = -v.x;
            if (src.invertY) v.y = -v.y;
            value = v;
        }
    }, source);
}

// ── fases ─────────────────────────────────────────────────────────────────────

bool InputAction::isPerformed() const { return isActive(value); }
bool InputAction::isStarted()   const { return isActive(value) && !isActive(prevValue); }
bool InputAction::isCanceled()  const { return !isActive(value) && isActive(prevValue); }

// ── teclas (panel de ajustes / ini) ───────────────────────────────────────────

std::vector<SDL_Scancode> InputAction::keys() const {
    return std::visit([](const auto& src) -> std::vector<SDL_Scancode> {
        using T = std::decay_t<decltype(src)>;
        if constexpr (std::is_same_v<T, DirectBinding>)
            return src.keys;
        if constexpr (std::is_same_v<T, Axis1DBinding>)
            return { src.positive, src.negative };
        if constexpr (std::is_same_v<T, Axis2DBinding>)
            return { src.up, src.down, src.left, src.right };
        return {};
    }, source);
}

void InputAction::setKeys(const std::vector<SDL_Scancode>& newKeys) {
    std::visit([&](auto& src) {
        using T = std::decay_t<decltype(src)>;
        if constexpr (std::is_same_v<T, DirectBinding>) {
            src.keys = newKeys;
        } else if constexpr (std::is_same_v<T, Axis1DBinding>) {
            if (newKeys.size() >= 1) src.positive = newKeys[0];
            if (newKeys.size() >= 2) src.negative = newKeys[1];
        } else if constexpr (std::is_same_v<T, Axis2DBinding>) {
            if (newKeys.size() >= 1) src.up    = newKeys[0];
            if (newKeys.size() >= 2) src.down  = newKeys[1];
            if (newKeys.size() >= 3) src.left  = newKeys[2];
            if (newKeys.size() >= 4) src.right = newKeys[3];
        }
    }, source);
}

// ── mando (panel de ajustes / ini) ────────────────────────────────────────────

std::vector<SDL_GamepadButton> InputAction::padButtons() const {
    return std::visit([](const auto& src) -> std::vector<SDL_GamepadButton> {
        using T = std::decay_t<decltype(src)>;
        if constexpr (std::is_same_v<T, DirectBinding>) return src.buttons;
        if constexpr (std::is_same_v<T, Axis1DBinding>) return { src.positiveBtn, src.negativeBtn };
        if constexpr (std::is_same_v<T, Axis2DBinding>) return { src.upBtn, src.downBtn, src.leftBtn, src.rightBtn };
        return {};
    }, source);
}

std::vector<SDL_GamepadAxis> InputAction::padAxes() const {
    return std::visit([](const auto& src) -> std::vector<SDL_GamepadAxis> {
        using T = std::decay_t<decltype(src)>;
        if constexpr (std::is_same_v<T, DirectBinding>) return { src.axis };
        if constexpr (std::is_same_v<T, Axis1DBinding>) return { src.axis };
        if constexpr (std::is_same_v<T, Axis2DBinding>) return { src.axisX, src.axisY };
        return {};
    }, source);
}

void InputAction::setPad(const std::vector<SDL_GamepadButton>& b, const std::vector<SDL_GamepadAxis>& ax) {
    std::visit([&](auto& src) {
        using T = std::decay_t<decltype(src)>;
        if constexpr (std::is_same_v<T, DirectBinding>) {
            src.buttons = b;
            src.axis = ax.empty() ? SDL_GAMEPAD_AXIS_INVALID : ax[0];
        } else if constexpr (std::is_same_v<T, Axis1DBinding>) {
            if (b.size() >= 1) src.positiveBtn = b[0];
            if (b.size() >= 2) src.negativeBtn = b[1];
            src.axis = ax.empty() ? SDL_GAMEPAD_AXIS_INVALID : ax[0];
        } else if constexpr (std::is_same_v<T, Axis2DBinding>) {
            if (b.size() >= 1) src.upBtn    = b[0];
            if (b.size() >= 2) src.downBtn  = b[1];
            if (b.size() >= 3) src.leftBtn  = b[2];
            if (b.size() >= 4) src.rightBtn = b[3];
            src.axisX = ax.size() >= 1 ? ax[0] : SDL_GAMEPAD_AXIS_INVALID;
            src.axisY = ax.size() >= 2 ? ax[1] : SDL_GAMEPAD_AXIS_INVALID;
        }
    }, source);
}

// ── inversion ─────────────────────────────────────────────────────────────────

void InputAction::invert(bool& x, bool& y) const {
    x = y = false;
    std::visit([&](const auto& src) {
        using T = std::decay_t<decltype(src)>;
        if constexpr (std::is_same_v<T, Axis1DBinding>) x = src.invert;
        if constexpr (std::is_same_v<T, Axis2DBinding>) { x = src.invertX; y = src.invertY; }
    }, source);
}

void InputAction::setInvert(bool x, bool y) {
    std::visit([&](auto& src) {
        using T = std::decay_t<decltype(src)>;
        if constexpr (std::is_same_v<T, Axis1DBinding>) src.invert = x;
        if constexpr (std::is_same_v<T, Axis2DBinding>) { src.invertX = x; src.invertY = y; }
    }, source);
}

} // namespace Haruka::Input
