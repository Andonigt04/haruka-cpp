#pragma once
#include <imgui.h>
#include "ui_styles.h"

namespace Haruka::UI::Utils {

    /** @brief Dibuja una sombra "Drop Shadow" detrás de un rectángulo */
    void DrawShadow(ImDrawList* drawList, ImVec2 min, ImVec2 max, float thickness, float rounding);

    /** @brief Centra el siguiente elemento (ej. un título) */
    inline void CenterNext(float width) {
        float available = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (available - width) * 0.5f);
    }

    /** @brief Dibuja un separador con degradado */
    void GradientSeparator(ImVec4 color, float thickness = 1.0f);
}