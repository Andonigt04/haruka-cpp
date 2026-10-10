#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <functional>
#include <string>

namespace Haruka::UI {

// Where in the panel's local space the anchor sits (0..1 normalized).
// (0,0) = top-left, (0.5,0.5) = center, (1,1) = bottom-right
struct UIAnchor {
    glm::vec3 worldPos   = glm::vec3(0.0f);
    glm::vec3 normal     = glm::vec3(0.0f, 0.0f, 1.0f); // panel faces +Z by default
    glm::vec2 pivotNorm  = glm::vec2(0.5f, 0.5f);        // pivot within the panel
};

// Cylindrical bend: panel is projected onto a cylinder of given radius.
// arcAngle = 0 → flat panel.
struct UICurve {
    float radius   = 2.0f;
    float arcAngle = 0.0f;  // radians; >0 = bent toward viewer
};

// How the panel tracks the camera.
enum class UIBillboardMode {
    None,        // fixed in world space
    FaceCamera,  // always rotates to face the camera (full billboard)
    AxisLocked,  // rotates around Y axis only (vertical billboard)
};

// Interaction shape used for raycasting.
enum class UIColliderShape { Rect, Cylinder };

struct UIInteractConfig {
    float              interactRange = 3.0f;   // max distance to interact
    bool               requireLook   = true;   // must be looking at it (raycast)
    std::function<void(glm::vec2 hitUV)> onInteract;  // UV within panel [0..1]
    std::function<void()>                onFocus;
    std::function<void()>                onUnfocus;
};

} // namespace Haruka::UI
