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

// ── evaluate ─────────────────────────────────────────────────────────────────

void InputAction::evaluate(const bool* kbd) {
    prevValue = value;

    std::visit([&](const auto& src) {
        using T = std::decay_t<decltype(src)>;

        if constexpr (std::is_same_v<T, DirectBinding>) {
            bool held = false;
            for (SDL_Scancode sc : src.keys)
                if (sc < SDL_SCANCODE_COUNT && kbd[sc]) { held = true; break; }
            value = held;

        } else if constexpr (std::is_same_v<T, Axis1DBinding>) {
            float v = 0.f;
            if (src.positive < SDL_SCANCODE_COUNT && kbd[src.positive]) v += 1.f;
            if (src.negative < SDL_SCANCODE_COUNT && kbd[src.negative]) v -= 1.f;
            value = v;

        } else if constexpr (std::is_same_v<T, Axis2DBinding>) {
            glm::vec2 v{0.f};
            if (src.up    < SDL_SCANCODE_COUNT && kbd[src.up])    v.y += 1.f;
            if (src.down  < SDL_SCANCODE_COUNT && kbd[src.down])  v.y -= 1.f;
            if (src.right < SDL_SCANCODE_COUNT && kbd[src.right]) v.x += 1.f;
            if (src.left  < SDL_SCANCODE_COUNT && kbd[src.left])  v.x -= 1.f;
            if (src.normalize && glm::length(v) > 1.f) v = glm::normalize(v);
            value = v;
        }
    }, source);
}

// ── phase queries ─────────────────────────────────────────────────────────────

bool InputAction::isPerformed() const { return isActive(value); }
bool InputAction::isStarted()   const { return isActive(value) && !isActive(prevValue); }
bool InputAction::isCanceled()  const { return !isActive(value) && isActive(prevValue); }

// ── key list accessors (for settings panel) ──────────────────────────────────

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

} // namespace Haruka::Input
