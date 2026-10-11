#pragma once

#include <glm/glm.hpp>
#include <functional>
#include <unordered_map>
#include <vector>

#include "Haruka/World/Vox/VoxWorld.h"
#include "Haruka/RHI/RHIResources.h"
#include "Haruka/Renderer/ResourceStore.h"

namespace Haruka::Renderer {

/**
 * @file VoxRenderer.h
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
 *     un borde suave de 3 m), a diferencia de usarlo para la rejilla (ver `VoxWorld.h`).
 */
class VoxRenderer {
public:
    static constexpr int    kCutRes    = 160;   ///< ventana 160² (téxel ~4 m sobre ±320 m): con 256² el rebarrido por texel era ~40 ms
    static constexpr double kCutHalfM  = 320.0;   ///< la ventana cubre ±320 m
    static constexpr int    kMaxRemeshPerFrame = 16;    ///< tope duro; el que manda es el tiempo
    static constexpr double kRemeshBudgetMs    = 5.0;   ///< ms de remallado por frame (3 chunks fijos tardaban 7 s en levantar una isla)

    /** @brief Remalla los chunks sucios (acotado) y rehace la ventana de recorte si hace falta.
     *  @param camRelPlanet cámara relativa al CENTRO del planeta (m) */
    void update(World::Vox::VoxWorld& world, const glm::dvec3& camRelPlanet, double surfaceElevM);

    /** @brief Dibuja los chunks cargados. Dentro del render pass de la escena.
     *  @param rotVP proj·rot(view) · @param camRelPlanet ídem que en `update` */
    void draw(const World::Vox::VoxWorld& world, const glm::mat4& rotVP, const glm::dvec3& camRelPlanet,
              const glm::vec3& sunDir, const glm::vec3& viewDir = glm::vec3(0.0f));

    /** @brief La textura de roca: el array de albedo del terreno y su capa; tiling en metros por tile. */
    void setRock(RHI::TextureHandle albedoArray, int layer, float tilingM) { m_albedo = albedoArray; m_rockLayer = layer; m_tiling = tilingM; }

    RHI::TextureHandle cutTexture() const { return m_cutTex; }
    /** @brief Lleva una posición RELATIVA A LA CÁMARA a la ventana de recorte ([0,1]²). */
    const glm::mat4& cutSpace() const { return m_cutSpace; }
    /** @brief Versión del campo que la ventana ACTUAL refleja (más fiable que comparar el id de la
     *  textura: el id se recicla y un reencuadre por >40 m no cambia el contenido). Un consumidor
     *  (la hierba) regenera cuando esto cambia Y SU TEXURA SIGUE SIENDO LA MISMA ALGUN FRAME. */
    uint64_t cutVersion() const { return m_cutVersion; }
    bool cutValid() const { return RHI::valid(m_cutTex) && m_cutReady; }

    /** @brief La copia CPU de cada malla, con su revisión: es lo que la física convierte en cuerpos
     *  (`PhysicsEngine::setVoxMesh`). Quien la lea guarda la revisión que vio. La lleva el almacén
     *  central (`Renderer::resources()`), no este renderer. */
    void forEachCpuMesh(const std::function<void(const World::Vox::VoxKey&, const World::Vox::CaveMesh&, uint64_t revision)>& fn) const;
    static uint64_t physicsKey(const World::Vox::VoxKey& k) {
        return ((uint64_t)(uint32_t)k.face << 60) ^ ((uint64_t)(uint32_t)k.i << 40)
             ^ ((uint64_t)(uint32_t)k.j << 20) ^ (uint64_t)(uint32_t)(k.k + 512);
    }

    size_t drawnLastFrame() const { return m_drawn; }
    size_t trisLastFrame()  const { return m_tris; }

private:
    /// Lo que el dibujo necesita de la versión SUBIDA de un chunk (va en `GpuSet::meta`). Los
    /// buffers van en `GpuSet::buffers`: [0] vértices, [1] índices, [2] UBO —uno POR CHUNK:
    /// `updateBuffer` entre draws del mismo command buffer no versiona—.
    struct VoxDraw { glm::dvec3 origin{0.0}; uint32_t indexCount = 0; };
    static Renderer::GpuSet uploadMesh(const void* cpu);
    struct VoxUBO { glm::mat4 rotVP; glm::vec4 originRel; glm::vec4 lightDir; glm::vec4 color; glm::vec4 up; glm::vec4 texOrigin; glm::vec4 texInfo; };

    bool ensurePipeline();
    void rebuildCut(World::Vox::VoxWorld& world, const glm::dvec3& camRelPlanet, double surfaceElevM);

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
    bool                m_cutDirty = false;   ///< un chunk DENTRO de la ventana se remalló → rehacer
    bool                m_failed = false;
    size_t              m_drawn = 0, m_tris = 0, m_culled = 0;
    /// ⚠️ ANTES ERAN DOS MAPAS (`m_gpu` y `m_cpu`) que tenían que tener las mismas claves a mano. Ahora
    /// un chunk es UN recurso del almacén central: CPU, GPU y revisión juntos. Aquí solo queda qué
    /// recurso es cada chunk; que exista la clave = "mallado alguna vez".
    std::unordered_map<World::Vox::VoxKey, Renderer::ResourceId, World::Vox::VoxKeyHash> m_ids;
    /// ⚠️ AQUÍ HUBO UNA CACHÉ "POR CHUNK" DEL RECORTE, y era un valor POR CHUNK (128 m) que se
    /// copiaba a todos sus téxeles: el primero que se miraba decidía el chunk entero. En campo
    /// abierto ese téxel casi nunca cae en el hoyo, así que un cráter no recortaba el suelo NUNCA
    /// (medido: 0 téxeles abiertos tras picar r=7; desde fuera no se veía, desde dentro sí porque
    /// el suelo se ve por detrás). Ahora sólo se evalúan los téxeles sobre columnas con hueco (el
    /// resto es 0 exacto, ver `loadedCutColumns`), cada uno el suyo.
    std::vector<unsigned char> m_cutPixels;
};

} // namespace Haruka::Renderer
