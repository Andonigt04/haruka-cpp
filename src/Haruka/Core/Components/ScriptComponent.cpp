#include "ScriptComponent.h"
#include <imgui.h>

namespace Haruka::Core::Components {

// Inspector mínimo: edición directa del path del script.
void ScriptComponent::renderInspector() {
    ImGui::InputText("Script", &scriptPath[0], 128);
}

} // namespace Haruka::Core::Components