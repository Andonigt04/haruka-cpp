#pragma once

#include <glm/glm.hpp>
#include <functional>
#include <future>
#include <unordered_map>
#include <vector>

#include "Haruka/World/Vox/VoxWorld.h"
#include "Haruka/World/Vox/VoxFineBricks.h"
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
    ~VoxRenderer();
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

    // ── FINE BRICKS: decimetre walls near the eye ───────────────────────────────────────────────
    // Within `fineRadius()` of the eye the walls are fine bricks (`VoxWorld::buildFineMesh`) and the
    // terrain is cut with the fine window; beyond it, the 4 m chunk meshes and the coarse window.
    // `vox.frag` and `lib/vox_cut.glsl` hand over at that same sphere, so every wall and every hole
    // is drawn by exactly one of the two. The radius is only as large as the bricks that are already
    // UPLOADED: a hole is never cut where its wall is not drawn yet.
    static constexpr double kFineRadiusM    = World::Vox::VoxFineBricks::kRadiusM;   ///< the fine sphere, at most
    static constexpr int    kFineCutRes     = 320;    ///< fine cut window: 320² texels...
    static constexpr double kFineCutHalfM   = 16.0;   ///< ...over ±16 m (10 cm texels)
    const glm::mat4& fineCutSpace() const { return m_fineCutSpace; }
    /** @brief What the fine window says at a position RELATIVE TO THE EYE (nearest texel, [0,1]; -1
     *  outside it or with no window): the CPU side of the shader's lookup, for tests. */
    float fineCutAt(const glm::vec3& posRelEye) const;
    /// Radius (m) around the eye where the fine walls and the fine cut apply now (0 = none).
    float fineRadius() const { return m_fineR; }
    /// Where each window sits in the cut texture (lib/vox_cut.glsl); x = 0 when it is not an atlas.
    glm::vec4 cutAtlas() const;
    size_t fineBricksLoaded() const { return m_fine.size(); }
    size_t fineBricksDrawn() const { return m_fineDrawn; }
    /** @brief The fine bricks (built in the world module, without RHI): the physics reads them too. */
    const World::Vox::VoxFineBricks& fineBricks() const { return m_fine; }

private:
    /// Lo que el dibujo necesita de la versión SUBIDA de un chunk (va en `GpuSet::meta`). Los
    /// buffers van en `GpuSet::buffers`: [0] vértices, [1] índices, [2] UBO —uno POR CHUNK:
    /// `updateBuffer` entre draws del mismo command buffer no versiona—.
    struct VoxDraw { glm::dvec3 origin{0.0}; uint32_t indexCount = 0; };
    static Renderer::GpuSet uploadMesh(const void* cpu);
    struct VoxUBO { glm::mat4 rotVP; glm::vec4 originRel; glm::vec4 lightDir; glm::vec4 color; glm::vec4 up; glm::vec4 texOrigin; glm::vec4 texInfo;
                    glm::vec4 fineClip; };   ///< = VoxParams in vox.vert/.frag (fineClip: x radius, y 1 coarse / 2 fine)

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

    // ── Fine bricks: the world builds them (`VoxFineBricks`), here only their GPU side ──
    World::Vox::VoxFineBricks m_fine;
    struct GpuBrick {
        Renderer::ResourceId id = 0;
        uint64_t brickRev = 0;     ///< `VoxFineBricks::Brick::revision` published
        uint64_t storeRev = 0;     ///< the store's revision of that publish (uploaded when the GPU has it)
        bool     empty = true;
        double   dist = 0.0, halfDiag = 4.0;
    };
    std::unordered_map<World::Vox::VoxFineBricks::Key, GpuBrick, World::Vox::VoxFineBricks::KeyHash> m_gpuBricks;
    float      m_fineR = 0.0f;
    size_t     m_fineDrawn = 0;
    bool       m_fineOn = true;
    void updateFine(World::Vox::VoxWorld& world, const glm::dvec3& camRelPlanet);
    // The fine cut window (right part of the atlas).
    std::vector<unsigned char> m_fineCutPixels;
    glm::mat4  m_fineCutSpace{1.0f};
    glm::dvec3 m_fineCutCenter{0.0}, m_fineE1{1, 0, 0}, m_fineE2{0, 0, 1}, m_fineUp{0, 1, 0};
    uint64_t   m_fineCutVersion = ~0ull;
    bool       m_fineCutHave = false;
    /// Starts computing a new fine window in a job (`collectFineCut` installs it when it lands).
    void rebuildFineCut(World::Vox::VoxWorld& world, const glm::dvec3& camRelPlanet, double surfaceElevM);
    struct FineCut {
        std::vector<unsigned char> pixels;
        glm::dvec3 center{0.0}, e1{1, 0, 0}, e2{0, 0, 1}, up{0, 1, 0};
        uint64_t version = 0;
        size_t evaluated = 0, open = 0;
    };
    std::future<FineCut> m_fineCutJob;
    bool m_fineCutPending = false;
    bool collectFineCut(const glm::dvec3& camRelPlanet);
    void refreshFineCutSpace(const glm::dvec3& camRelPlanet);
    /// Rebuilds the cut texture from both windows' pixels (atlas when the fine one exists).
    void uploadCutAtlas();
};

} // namespace Haruka::Renderer
