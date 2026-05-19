#pragma once
#include <imgui.h>

namespace Haruka::UI {

    // Representa los 4 lados (como en CSS: top, right, bottom, left)
    struct RectOffset {
        float top, right, bottom, left;

        RectOffset(float all = 0.0f) : top(all), right(all), bottom(all), left(all) {}
        RectOffset(float vertical, float horizontal) : top(vertical), right(horizontal), bottom(vertical), left(horizontal) {}
        RectOffset(float t, float r, float b, float l) : top(t), right(r), bottom(b), left(l) {}
    };

    struct BoxStyle {
        RectOffset margin  = 0.0f;
        RectOffset padding = 0.0f;
        float borderThickness = 0.0f;
        
        ImVec4 borderColor     = ImVec4(1,1,1,1);
        ImVec4 backgroundColor = ImVec4(0,0,0,0);
        float rounding         = 0.0f;
    };
}