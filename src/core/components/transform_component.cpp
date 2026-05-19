#include "transform_component.h"
#include <imgui.h>

namespace Haruka {

// Editor de transform con controles escalares por componente.
void TransformComponent::renderInspector() {
    ImGui::InputDouble("Position X", &position.x);
    ImGui::InputDouble("Position Y", &position.y);
    ImGui::InputDouble("Position Z", &position.z);

    ImGui::InputDouble("Rotation X", &rotation.x);
    ImGui::InputDouble("Rotation Y", &rotation.y);
    ImGui::InputDouble("Rotation Z", &rotation.z);

    ImGui::InputDouble("Scale X", &scale.x);
    ImGui::InputDouble("Scale Y", &scale.y);
    ImGui::InputDouble("Scale Z", &scale.z);
}

}