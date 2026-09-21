#pragma once
/**
 * @file world/props/prop_system.h
 * @brief LOS PROPS DEL MUNDO como modulo: scatter global (arboles/rocas/casas por celda del mundo),
 *        registro de instancias, mallas y materiales de prototipo, pase instanciado de color y de
 *        sombra, colliders por parte, rotura (talar / arrancar una rama) y el estado persistente
 *        de lo roto. Todo esto vivia en `Application` (application_render.cpp, ~900 lineas y 18
 *        miembros `m_prop*`); Andoni (21-09): "los modulos todos estan en application_*".
 *
 * Dos ganchos por frame, como cualquier modulo del render:
 *   · `refreshScatter()` ANTES de abrir ningun render pass (re-siembra por demanda, colliders).
 *   · `draw()` dentro del pase de escena, y `drawShadows()` dentro del pase de sombras.
 * Y la API que usa el JUEGO (`breakAt`, `serializeState`, `registry`) — `Application` la reenvia
 * en una linea para no cambiar a Survival.
 */
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

#include "rhi/rhi_types.h"
#include "rhi/rhi_resources.h"
#include "world/props/instanced_object.h"
#include "renderer/gpu_instancing.h"

namespace Haruka { namespace Core { class Camera; } class PlanetarySystem; class WorldSystem;
                   namespace Physics { class PhysicsEngine; } namespace RHI { class Context; } }

namespace Haruka::World {

/** @brief Snapshot de debug de UNA instancia del scatter, para la jerarquia del editor. */
struct PropInstanceDebug {
    uint32_t seed    = 0;          ///< identidad determinista (celda del mundo)
    uint32_t state   = 0;          ///< InstancedObjectState (0=viva, 1=destruida, 2=rebrotando)
    bool     rendered = false;     ///< entro en el draw de este frame (Alive, dentro del tope y sin cull)
    bool     culled   = false;     ///< viva pero sin draw este frame (cull activo)
    uint8_t  cullReason = 0;       ///< 0 = ninguna (se dibujo), 1 = fuera del frustum, 2 = sub-pixel
};

/** @brief Snapshot de debug de UN prototipo (un draw por prototipo). La jerarquia del editor lo usa
 *  como arbol de debug: instancias que no se renderizan → gris, y el nombre en ROJO + alerta si el
 *  prototipo NO tiene material per-pixel (solo color por vertice). */
struct PropPrototypeDebug {
    std::string name;                          ///< mesh/tipo ("tree", "rock", "house", …)
    bool        hasPerPixel = false;           ///< material con texturas (mask != 0) vs color por vertice
    int         totalInstances   = 0;          ///< instancias en el registro
    int         renderedInstances = 0;         ///< las que entraron en el draw este frame
    std::vector<PropInstanceDebug> instances;  ///< lista por instancia (para el arbol)
};

class PropSystem {
public:
    /** @brief Resultado de un golpe contra un prop del scatter. `partId` identifica QUE parte se
     *  rompio (0 = tronco → el prop entero cae; >0 = una rama) y `lengthM` es lo que media ese
     *  segmento en el mundo, que es lo que decide si el golpe deja un palo aprovechable. */
    struct Hit {
        bool        hit       = false;
        uint32_t    seed      = 0;      ///< identidad determinista de la instancia (celda del mundo)
        std::string prototype;          ///< nombre del prototipo ("tree", "rock", …)
        int         partId    = -1;
        bool        trunk     = false;  ///< true = se ha tumbado el prop entero
        double      lengthM   = 0.0;    ///< longitud del segmento roto (m), ya escalada
        // RADIOS reales del tronco de cono roto, ya escalados. Con la longitud dan el VOLUMEN, y el
        // volumen por la densidad del material da los KILOS — que es lo que decide cuanto material
        // sacas. Sin esto solo se sabia "cuanto media", y una rama fina y un tronco gordo del mismo
        // largo rendian igual.
        double      rBottomM  = 0.0;
        double      rTopM     = 0.0;
        glm::dvec3  pos{0.0};           ///< punto medio de la parte rota, en el mundo
    };

    /** @brief Lo que le ha pasado a un prop concreto: talado y/o con ramas arrancadas. */
    struct StateDelta {
        uint32_t seed      = 0;
        uint32_t state     = 0;    ///< InstancedObjectState
        uint32_t breakMask = 0;    ///< partes rotas
        float    regrow    = 0.0f;
    };

    /// Lo que el pase de color necesita del frame y no es suyo.
    struct DrawFrame {
        Core::Camera*            camera    = nullptr;
        PlanetarySystem*         planets   = nullptr;
        const WorldSystem*       world     = nullptr;   ///< viento (nullptr = calma)
        RHI::BufferHandle        perFrameUBO{};         ///< view/proj + luces (binding 0)
        RHI::PipelineHandle      scenePSO{};            ///< se restaura al acabar (el cierre lo asume)
        float                    aspect    = 1.0f;
        int                      heightPx  = 1;
        glm::vec4                aerial{0.0f};          ///< perspectiva aerea (lib/aerial.glsl)
        double                   diagClock = 0.0;       ///< para espaciar las sondas de coste
    };
    /// Lo que suma el pase a las estadisticas del frame.
    struct DrawStats {
        int renderedDrawCalls = 0, renderedVertices = 0, renderedTriangles = 0;
        int totalDrawCalls = 0,    totalVertices = 0,    totalTriangles = 0;
    };

    ~PropSystem();
    void shutdown();   ///< suelta la GPU (el device sigue vivo)

    // ── Por frame ──────────────────────────────────────────────────────────────────────────────
    /// Al empezar el frame: el anillo de instancias empieza de cero (ver GPUInstancing::kRing).
    void beginFrame() { if (m_instancing) m_instancing->beginFrame(); }
    /** @brief Re-siembra por demanda (cuando la camara cruza un tramo o cambia el campo) y
     *  actualiza los colliders. ANTES de abrir ningun render pass. */
    void refreshScatter(Core::Camera* camera, PlanetarySystem* planets, Physics::PhysicsEngine* physics);
    /** @brief Pase instanciado de color. Dentro del render pass de escena. */
    void draw(RHI::Context* ctx, const DrawFrame& f, DrawStats& stats);
    /** @brief Solo profundidad, en el mapa de sombras (culling por la caja de la LUZ). */
    void drawShadows(RHI::Context* ctx, const glm::mat4& lightSpace, Core::Camera* camera,
                     PlanetarySystem* planets);

    // ── API del juego ──────────────────────────────────────────────────────────────────────────
    /** @brief Rompe la parte de prop mas cercana al golpe (esfera `center`+`radius`), si la hay.
     *  Marca el estado en el registro (tronco → `Destroyed`, rama → bit en `breakMask`) y REGENERA
     *  los colliders. La dispara el JUEGO (el hachazo), no el motor. */
    Hit breakAt(const glm::dvec3& center, double radius, Core::Camera* camera,
                PlanetarySystem* planets, Physics::PhysicsEngine* physics);
    /** @brief Todo lo que el jugador ha roto, para guardarlo en la partida.
     *
     *  ⚠️ Vive FUERA del registro de instancias a proposito. El scatter regenera las instancias
     *  desde cero y solo conserva las que siguen cerca: si el estado viviera solo ahi, el arbol que
     *  talaste reapareceria en cuanto te alejaras lo bastante para que saliera del radio del
     *  scatter. El mapa es por SEMILLA (la celda del mundo), que es una identidad estable. Solo
     *  guarda lo ROTO: un mundo entero de arboles intactos se regenera de la semilla. */
    std::vector<StateDelta> serializeState() const;
    void restoreState(const std::vector<StateDelta>& deltas);

    /** @brief Activa/desactiva el scatter GLOBAL (`scene.prop.instanced`). Survival lo apagaba
     *  cuando usaba su propio ResourceSystem. */
    void setEnabled(bool on) { m_enabled = on; }
    bool enabled() const { return m_enabled; }
    /** @brief Snapshot de DEBUG del scatter para la jerarquia del editor (apagado por defecto:
     *  el barrido por instancia cuesta). */
    void setDebugEnabled(bool on) { m_debugEnabled = on; }
    bool debugEnabled() const { return m_debugEnabled; }
    const std::vector<PropPrototypeDebug>& scatterDebug() const { return m_scatterDebug; }

    const InstancedObjectRegistry& registry() const { return m_registry; }
    /// Ids de las mallas de colision por (prototipo → parte) y su copia CPU: el alambre de
    /// depuracion dibuja EXACTAMENTE la geometria que usa la fisica.
    const std::unordered_map<int, std::vector<int>>& meshShapes() const { return m_meshShapes; }
    const std::unordered_map<int, std::vector<std::vector<glm::vec3>>>& meshCpu() const { return m_meshCpu; }
    size_t hostBytes() const { return m_gpu.size() * sizeof(InstanceDataFloat) + (m_instancing ? m_instancing->hostBytes() : 0); }

private:
    // Cache GPU de prototipos: por nombre → malla (VBO/EBO por nivel) + material PER-PIXEL.
    struct PrototypeGpu {
        /// NIVELES DE DETALLE. Medido con RenderDoc, los arboles del scatter eran el 85,8 % de los
        /// triangulos del frame y el instancing no lo arregla: ahorra draw calls, no vertices. El
        /// material es COMPARTIDO por los niveles (mismo prototipo, solo cambia la densidad).
        static constexpr int kLods = 3;
        RHI::BufferHandle vbo[kLods] = {}, ebo[kLods] = {};
        uint32_t vertexCount[kLods] = {0, 0, 0};
        uint32_t indexCount[kLods]  = {0, 0, 0};
        RHI::TextureHandle albedo = {}, normal = {}, metallic = {}, roughness = {}, ao = {};
        float    metallicS  = 0.0f, roughnessS = 0.5f, aoS = 1.0f;
        uint32_t mask       = 0u;       ///< bits de texturas presentes (mismo esquema que prop_inst.frag)
    };
    const PrototypeGpu* prototypeGpu(const InstancedPrototype& proto);   // hornea si falta
    void rebuildGpu(const glm::dvec3& planetC, double planetR, Core::Camera* camera);
    void refreshColliders(const glm::dvec3& planetC, double planetR, const glm::dvec3& camPos,
                          Physics::PhysicsEngine* physics);
    const std::vector<int>& meshShapesFor(int protoIdx, Physics::PhysicsEngine* physics);

    InstancedObjectRegistry m_registry;
    /// Instancias YA TRANSFORMADAS (mismo indice que el registro), relativas a `m_origin`. Se
    /// construyen en el scatter, NO por frame: 180 000 matrices por frame costaban 31 ms.
    std::vector<InstanceDataFloat> m_gpu;
    glm::dvec3 m_origin{0.0};
    std::unique_ptr<Renderer::GPUInstancing> m_instancing;
    RHI::PipelineHandle m_instPSO{}, m_shadowPSO{};
    RHI::BufferHandle   m_shadowUBO{};
    /// UNO POR PROTOTIPO: en Vulkan los draws se graban y se ejecutan despues; un buffer
    /// compartido hacia que todos leyeran el ultimo material escrito (props blancos).
    std::vector<RHI::BufferHandle> m_paramsUBOs;
    std::unordered_map<std::string, PrototypeGpu> m_protoMesh;
    std::unordered_map<uint32_t, StateDelta> m_state;      ///< lo roto, por semilla
    std::unordered_map<int, std::vector<int>> m_meshShapes;
    std::unordered_map<int, std::vector<std::vector<glm::vec3>>> m_meshCpu;
    std::vector<PropPrototypeDebug> m_scatterDebug;
    // buckets del pase (reusados entre frames)
    std::vector<std::vector<uint32_t>> m_buckets, m_shadowBuckets;
    std::vector<int> m_aliveCounts, m_drawnCounts;
    // scatter por demanda
    glm::dvec3  m_lastCam{1e300, 1e300, 1e300};
    std::string m_planet;
    uint64_t    m_voxVersion = 0;
    bool m_enabled = true, m_debugEnabled = false;
};

} // namespace Haruka::World
