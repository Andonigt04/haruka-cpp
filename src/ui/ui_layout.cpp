#include "ui_layout.h"
#include <imgui_internal.h>

namespace Haruka::UI {

    ScopedBox::ScopedBox(const char* id, const BoxStyle& style) : m_style(style) {
        // Margen
        ImGui::SetCursorPos({ ImGui::GetCursorPosX() + style.margin.left, ImGui::GetCursorPosY() + style.margin.top });
        
        m_startPos = ImGui::GetCursorScreenPos();
        
        // Padding
        ImGui::SetCursorPos({ ImGui::GetCursorPosX() + style.padding.left, ImGui::GetCursorPosY() + style.padding.top });
        
        ImGui::BeginGroup();
    }

    ScopedBox::~ScopedBox() {
        ImGui::EndGroup();
        
        ImVec2 min = m_startPos;
        ImVec2 max = ImGui::GetItemRectMax();
        max.x += m_style.padding.right;
        max.y += m_style.padding.bottom;

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        
        // Render Fondo y Borde
        drawList->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(m_style.bg), m_style.rounding);
        if (m_style.borderSize > 0)
            drawList->AddRect(min, max, ImGui::ColorConvertFloat4ToU32(m_style.border), m_style.rounding, 0, m_style.borderSize);

        // Compensar Margen inferior
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + m_style.margin.bottom);
    }
}