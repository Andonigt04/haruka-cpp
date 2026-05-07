#pragma once
#include <imgui.h>
#include "ui_styles.h"

namespace Haruka::UI {

    struct RectOffset {
        float top, right, bottom, left;
        RectOffset(float all = 0.0f) : top(all), right(all), bottom(all), left(all) {}
        RectOffset(float v, float h) : top(v), right(h), bottom(v), left(h) {}
    };

    struct BoxStyle {
        RectOffset margin  = 0.0f;
        RectOffset padding = 0.0f;
        float borderSize   = 0.0f;
        float rounding     = Metrics::Rounding;
        ImVec4 bg          = Colors::Surface;
        ImVec4 border      = Colors::Border;
    };

    // --- Box Model Container ---
    class ScopedBox {
    public:
        ScopedBox(const char* id, const BoxStyle& style);
        ~ScopedBox();
    private:
        BoxStyle m_style;
        ImVec2 m_startPos;
    };

    // --- Layout Helpers (Flexbox) ---
    class VStack {
    public:
        VStack(float spacing = Metrics::SpacingMedium) : m_spacing(spacing) { ImGui::BeginGroup(); }
        ~VStack() { ImGui::EndGroup(); }
        void gap() { ImGui::Dummy({0, m_spacing}); }
    private: float m_spacing;
    };

    class HStack {
    public:
        HStack(float spacing = Metrics::SpacingMedium) : m_spacing(spacing) { ImGui::BeginGroup(); }
        ~HStack() { ImGui::EndGroup(); }
        void gap() { ImGui::SameLine(0, m_spacing); }
    private: float m_spacing;
    };

    // --- Inputs & Text ---
    inline bool Input(const char* id, char* buf, size_t sz) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, InputColors::Bg);
        bool res = ImGui::InputText(id, buf, sz);
        ImGui::PopStyleColor();
        return res;
    }
}