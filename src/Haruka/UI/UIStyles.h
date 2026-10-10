#pragma once
#include <imgui.h>

namespace Haruka::UI {
    // --- Tokens de Tipografía ---
    struct FontSize {
        inline static const float H1 = 24.0f;
        inline static const float H2 = 20.0f;
        inline static const float Body = 16.0f;
        inline static const float Small = 12.0f;
    };

    // --- Paleta de Colores Semántica ---
    struct Colors {
        inline static const ImVec4 Primary     = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
        inline static const ImVec4 Background  = ImVec4(0.06f, 0.06f, 0.08f, 0.94f);
        inline static const ImVec4 Surface     = ImVec4(0.12f, 0.12f, 0.14f, 1.00f);
        inline static const ImVec4 Text        = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
        inline static const ImVec4 Border      = ImVec4(0.20f, 0.20f, 0.25f, 1.00f);
        inline static const ImVec4 Highlight   = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    };

    // --- Estilos de Input ---
    struct InputColors {
        inline static const ImVec4 Bg      = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
        inline static const ImVec4 Text    = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    };

    struct Metrics {
        inline static const float SpacingSmall  = 4.0f;
        inline static const float SpacingMedium = 8.0f;
        inline static const float Rounding      = 6.0f;
    };
}