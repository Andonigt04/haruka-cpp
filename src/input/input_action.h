#pragma once
#include <string>
#include <vector>
#include <variant>
#include <glm/glm.hpp>
#include <SDL3/SDL.h>

namespace Haruka::Input {

// ── Binding sources ──────────────────────────────────────────────────────────
// Define HOW keyboard keys map to a typed value.

// Any of the listed keys held → bool true / float 1.0
struct DirectBinding {
    std::vector<SDL_Scancode> keys;
};

// Two keys → float in [-1, 1]
struct Axis1DBinding {
    SDL_Scancode positive;
    SDL_Scancode negative;
};

// Four keys → glm::vec2 in [-1,1]×[-1,1], optionally normalized
struct Axis2DBinding {
    SDL_Scancode up, down, left, right;
    bool normalize = true;
};

using BindingSource = std::variant<DirectBinding, Axis1DBinding, Axis2DBinding>;

// ── Action value ─────────────────────────────────────────────────────────────
// The typed output produced by an action each frame.

using ActionValue = std::variant<bool, float, glm::vec2, glm::vec3>;

// Returns true if the value is "active" (non-zero / true).
bool isActive(const ActionValue& v);

// ── InputAction ──────────────────────────────────────────────────────────────

struct InputAction {
    std::string   name;
    std::string   displayName;
    std::string   group;
    BindingSource source;

    ActionValue value     = false;  // current frame
    ActionValue prevValue = false;  // previous frame

    // Recompute value from raw keyboard state.
    void evaluate(const bool* kbd);

    // Phase queries (Unity-style)
    bool isPerformed()  const; // active this frame
    bool isStarted()    const; // became active this frame
    bool isCanceled()   const; // became inactive this frame

    // For the settings panel: keys that can be individually displayed / rebound.
    // Returns the internal key list regardless of binding type.
    std::vector<SDL_Scancode> keys() const;
    void setKeys(const std::vector<SDL_Scancode>& keys);
};

} // namespace Haruka::Input
