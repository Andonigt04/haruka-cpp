#include "script_component.h"
#include <imgui.h>

namespace Haruka {

// Inspector mínimo: edición directa del path del script.
void ScriptComponent::renderInspector() {
    ImGui::InputText("Script", &scriptPath[0], 128);
}

}