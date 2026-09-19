#pragma once

#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

#include "core/terrain/vox_world.h"
#include "rhi/rhi_resources.h"

namespace Haruka {

/**
 * @file vox_renderer.h
 * @brief Dibuja las paredes del campo volumétrico (`VoxWorld`) y mantiene la VENTANA DE RECORTE que
 *        el terreno usa para no dibujar suelo donde hay boca.
 *
 * Dos cosas, y las dos siguen al mismo campo:
 *
 *   · **Mallas por chunk**: cada chunk `dirty` se remalla (surface nets con delantal) y se sube; el
 *     draw es uno por chunk, con el origen del chunk relativo a la cámara. Acotado por frame
 *     (`kMaxRemeshPerFrame`): remallar cuesta ~ms y un tirón de 30 chunks a la vez es un parón.
 *
 *   · **La ventana de recorte** (`cutTexture`): una textura R8 de `kCutRes²` sobre ±`kCutHalfM`
 *     alrededor del pie de la cámara, en un marco tangente LOCAL. Cada téxel es
 *     `VoxWorld::surfaceCut(dir)` — la MISMA función que consulta la CPU—, así que lo que el terreno
 *     deja de dibujar es exactamente donde el campo dice aire. Es una ventana de LOOKUP para el
 *     shader, no almacenamiento: el marco tangente aquí es inofensivo (cm de error sobre 300 m con
 *     un borde suave de 3 m), a diferencia de usarlo para la rejilla (ver `vox_world.h`).
 */
class VoxRenderer {
public:
    static constexpr int    kCutRes    = 256;
    static constexpr double kCutHalfM  = 320.0;   ///< la ventana cubre ±320 m
    static constexpr int    kMaxRemeshPerFrame = 16;    ///< tope duro; el que manda es el tiempo
    static constexpr double kRemeshBudgetMs    = 5.0;   ///< ms de remallado por frame (3 chunks fijos tardaban 7 s en levantar una isla)

    /** @brief Remalla los chunks sucios (acotado) y rehace la ventana de recorte si hace falta.
     *  @param camRelPlanet cámara relativa al CENTRO del planeta (m) */
    void update(VoxWorld& world, const glm::dvec3& camRelPlanet, double surfaceElevM);

    /** @brief Dibuja los chunks cargados. Dentro del render pass de la escena.
     *  @param rotVP proj·rot(view) · @param camRelPlanet ídem que en `update` */
    void draw(const VoxWorld& world, const glm::mat4& rotVP, const glm::dvec3& camRelPlanet,
              const glm::vec3& sunDir, const glm::vec3& viewDir = glm::vec3(0.0f));

    /** @brief La textura de roca: el array de albedo del terreno y su capa; tiling en metros por tile. */
    void setRock(RHI::TextureHandle albedoArray, int layer, float tilingM) { m_albedo = albedoArray; m_rockLayer = layer; m_tiling = tilingM; }

    RHI::TextureHandle cutTexture() const { return m_cutTex; }
    /** @brief Lleva una posición RELATIVA A LA CÁMARA a la ventana de recorte ([0,1]²). */
    const glm::mat4& cutSpace() const { return m_cutSpace; }
    bool cutValid() const { return RHI::valid(m_cutTex) && m_cutReady; }

    /** @brief La copia CPU de cada malla subida, con su revisión: es lo que la física convierte en
     *  cuerpos (`PhysicsEngine::setVoxMesh`). Quien la lea guarda la revisión que vio. */
    struct CpuMesh { CaveMesh mesh; uint64_t revision = 0; };
    const std::unordered_map<VoxKey, CpuMesh, VoxKeyHash>& cpuMeshes() const { return m_cpu; }
    static uint64_t physicsKey(const VoxKey& k) {
        return ((uint64_t)(uint32_t)k.face << 60) ^ ((uint64_t)(uint32_t)k.i << 40)
             ^ ((uint64_t)(uint32_t)k.j << 20) ^ (uint64_t)(uint32_t)(k.k + 512);
    }

    size_t drawnLastFrame() const { return m_drawn; }
    size_t trisLastFrame()  const { return m_tris; }

private:
    struct Gpu {
        RHI::BufferHandle vb{}, ib{};
        RHI::BufferHandle ubo{};   ///< uno POR CHUNK: `updateBuffer` entre draws del mismo command buffer no versiona
        uint32_t indexCount = 0;
        glm::dvec3 origin{0.0};
    };
    struct VoxUBO { glm::mat4 rotVP; glm::vec4 originRel; glm::vec4 lightDir; glm::vec4 color; glm::vec4 up; glm::vec4 texOrigin; glm::vec4 texInfo; };

    bool ensurePipeline();
    void rebuildCut(VoxWorld& world, const glm::dvec3& camRelPlanet, double surfaceElevM);

    RHI::PipelineHandle m_pipe{};
    RHI::BufferHandle   m_ubo{};
    RHI::TextureHandle  m_cutTex{}, m_cutTexOld{};
    RHI::TextureHandle  m_albedo{};
    int                 m_rockLayer = -1;
    float               m_tiling = 100.0f;
    glm::mat4           m_cutSpace{1.0f};
    glm::dvec3          m_cutCenter{0.0};   ///< pie de la ventana, relativo al centro del planeta
    glm::dvec3          m_cutE1{1, 0, 0}, m_cutE2{0, 0, 1}, m_cutUp{0, 1, 0};   ///< el marco de la ventana
    void refreshCutSpace(const glm::dvec3& camRelPlanet);
    bool                m_cutReady = false;
    uint64_t            m_cutVersion = ~0ull;
    bool                m_failed = false;
    size_t              m_drawn = 0, m_tris = 0, m_culled = 0;
    std::unordered_map<VoxKey, Gpu, VoxKeyHash> m_gpu;
    std::unordered_map<VoxKey, CpuMesh, VoxKeyHash> m_cpu;
    std::vector<unsigned char> m_cutPixels;
};

} // namespace Haruka
