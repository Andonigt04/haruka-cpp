#pragma once

#include <vector>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "core/terrain/floating_islands.h"
#include "tools/math_types.h"

namespace Haruka {

    /**
     * @brief Dibuja las islas flotantes de un planeta en UN SOLO draw call.
     *
     * Todas las islas se FUSIONAN en una malla (vértices anclados a un origen del
     * casquete; el shader la coloca camera-relativa con u_chunkOffset). Reusa el
     * shader de planeta (aPos(0)+aNormal(1); u_morphFactor=0 lo deja el llamador).
     */
    class FloatingIslandRenderer {
    public:
        ~FloatingIslandRenderer();

        /** @brief Fija las islas (CPU). Se fusionan y suben a GPU en el siguiente render. */
        void setIslands(std::vector<FloatingIsland> islands);

        /** @brief Dibuja las islas (1 draw call). El shader de planeta debe estar activo. */
        void render(const Haruka::WorldPos& cameraPos);

        int count() const { return m_islandCount; }

    private:
        std::vector<FloatingIsland> m_pending; // CPU, pendiente de subir
        bool   m_uploaded = false;
        GLuint m_vao = 0, m_vbo = 0, m_nbo = 0, m_ebo = 0;
        uint32_t   m_indexCount = 0;
        glm::dvec3 m_origin{0.0};
        int    m_islandCount = 0;
        void uploadPending();
        void cleanup();
    };

} // namespace Haruka

