/**
 * @file SoftbodyRenderer.h
 * @brief Renders XPBD softbodies by streaming particle positions into a dynamic
 *        VBO each frame (Fase B). Self-contained (own pipeline/VBO/shader), drawn from
 *        the game's onRenderWorld hook — decoupled like the water/terrain renderers.
 *
 * Positions are camera-relative (particle.origin - cameraPos + localPos), so the
 * vertex shader only needs view-rotation + projection, matching the planet/water
 * convention and staying float-safe at planetary distances.
 */
#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include "Haruka/RHI/RHITypes.h"
#include "Haruka/Physics/XPBD/Softbody.h"
#include "Haruka/Tools/MathTypes.h"

namespace Haruka { namespace Renderer { class Shader; } }

namespace Haruka::Renderer {

class SoftBodyRenderer {
public:
    SoftBodyRenderer() = default;
    ~SoftBodyRenderer();

    /** @brief Registra un cuerpo blando para dibujarlo. Devuelve un id para `remove`. */
    int  add(const Physics::XPBD::SoftBodyHandle& handle, Physics::XPBD::XPBDSolver* solver,
             const glm::vec3& color = glm::vec3(0.8f, 0.2f, 0.25f));
    /**
     * @brief Una MALLA DE AUTOR que se dibuja con las posiciones de la tela (la piel de una criatura
     * hecha en Blender). Los vértices de DIBUJO no son las partículas: Blender parte un vértice en cada
     * costura de UV y la física necesita uno solo por punto, así que cada vértice de dibujo dice de qué
     * partícula toma la posición (`vertexParticle`, índice GLOBAL del solver). La normal sale de la
     * tela de ese frame, sumada por partícula: una costura de UV no se ve como una arista.
     */
    struct MeshDraw {
        std::vector<int>       vertexParticle;   ///< vértice de dibujo → partícula
        std::vector<glm::vec2> uv;               ///< por vértice de dibujo (vacío = sin textura)
        std::vector<glm::vec3> color;            ///< por vértice de dibujo (vacío = blanco)
        std::vector<uint32_t>  indices;          ///< triángulos sobre los vértices de dibujo
        std::string            texture;          ///< PNG del color base (vacío = sólo color por vértice)
        glm::vec3              tint{1.0f};
    };
    int  addMesh(const MeshDraw& md, Physics::XPBD::XPBDSolver* solver);
    /** @brief Deja de dibujarlo y libera sus buffers (lo que dibuja una criatura muere con ella). */
    void remove(int id);

    void render(const Tools::WorldPos& cameraPos);

    void clear();

private:
    struct Entry {
        Physics::XPBD::SoftBodyHandle handle;
        Physics::XPBD::XPBDSolver*    solver = nullptr;
        glm::vec3            color{0.8f};
        Haruka::RHI::BufferHandle hVbo, hEbo;
        /// ⚠️ UN UBO DE COLOR POR ENTRADA. Era uno para todas, reescrito entre dibujos: en GL cada
        /// draw lee lo último escrito ANTES de él, pero en Vulkan el búfer se lee al EJECUTAR el
        /// frame, así que todas salían del color de la última entrada.
        Haruka::RHI::BufferHandle hParams;
        int id = 0;
        std::vector<float> cpuVerts;
        bool initialized = false;
        // ── modo MALLA (addMesh) ──
        bool                     mesh = false;
        MeshDraw                 md;
        Haruka::RHI::TextureHandle tex;         ///< la textura de `md.texture` (o el blanco de 1x1)
    };
    std::vector<Entry> m_entries;
    Haruka::RHI::PipelineHandle m_pso;
    Haruka::RHI::PipelineHandle m_psoMesh;   ///< softbody_mesh.vert/frag: posición, normal, uv, color
    Haruka::RHI::TextureHandle  m_white;     ///< 1x1 blanco: la malla sin textura (nunca un descriptor sin atar)
    Haruka::RHI::BufferHandle   m_paramsUBO; // (ya no se usa para dibujar: ver Entry::hParams)
    int m_nextId = 1;

    void ensurePSO();
    void ensureMeshPSO();
    void uploadEntry(Entry& e, const Tools::WorldPos& cameraPos);
    void uploadMesh(Entry& e, const Tools::WorldPos& cameraPos);
};

} // namespace Haruka::Renderer

