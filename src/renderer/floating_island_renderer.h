#pragma once

#include <vector>
#include <glm/glm.hpp>
#include "rhi/rhi_types.h"
#include "core/terrain/floating_islands.h"
#include "tools/math_types.h"

namespace Haruka {

    /**
     * @brief Dibuja las islas flotantes de un planeta en UN SOLO draw call.
     *
     * Todas las islas se FUSIONAN en una malla (vértices anclados a un origen del casquete; el
     * shader la coloca camera-relativa con el offset por-draw). Reusa los SHADERS del planeta
     * (aPos(0) + aNormal(1)) pero con su PROPIO pipeline: layout distinto (dos streams de vec3
     * float, sin normales empaquetadas ni morph) y sin culling (el winding del blob varía).
     *
     * Los datos por-draw viajan por el MISMO SSBO (binding 7) que el terreno, con UN elemento
     * (gl_DrawID = 0 en un draw suelto): morph = 0 (las islas no morphan), mode = procedural.
     */
    class FloatingIslandRenderer {
    public:
        ~FloatingIslandRenderer();

        /** @brief Fija las islas (CPU). Se fusionan y suben a GPU en el siguiente render. */
        void setIslands(std::vector<FloatingIsland> islands);

        /** @brief Dibuja las islas (1 draw call). Ata su propio pipeline. */
        void render(const Haruka::WorldPos& cameraPos);

        int count() const { return m_islandCount; }

    private:
        std::vector<FloatingIsland> m_pending; // CPU, pendiente de subir
        bool   m_uploaded = false;
        Haruka::RHI::BufferHandle   m_vboH, m_nboH, m_eboH;
        Haruka::RHI::PipelineHandle m_pso;
        Haruka::RHI::BufferHandle   m_ssbo;    // DrawItem[1] (binding 7)
        uint32_t   m_indexCount = 0;
        glm::dvec3 m_origin{0.0};
        int    m_islandCount = 0;
        void uploadPending();
        void cleanup();
    };

} // namespace Haruka
