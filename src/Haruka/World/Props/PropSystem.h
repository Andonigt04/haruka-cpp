#pragma once
#include "Haruka/World/Props/PropCollider.h"
/**
 * @file World/Props/PropSystem.h
 * @brief LOS PROPS DEL MUNDO como sistema (hereda de `PropSystemBase`, hermano de `GrassSystem`):
 *        las capas que COLISIONAN o reclaman sitio —arboles/rocas/casas por celda del mundo—,
 *        registro de instancias, mallas y materiales de prototipo, pase instanciado de color y de
 *        sombra, colliders por parte, rotura (talar / arrancar una rama) y el estado persistente
 *        de lo roto. Todo esto vivia en `Application` (ApplicationRender.cpp, ~900 lineas y 18
 *        miembros `m_prop*`); Andoni (21-09): "los modulos todos estan en application_*".
 *
 * Dos ganchos por frame, como cualquier modulo del render:
 *   · `refreshScatter()` ANTES de abrir ningun render pass (re-siembra por demanda, colliders).
 *   · `draw()` dentro del pase de escena, y `drawShadows()` dentro del pase de sombras.
 * Y la API que usa el JUEGO (`breakAt`, `serializeState`, `registry`) — `Application` la reenvia
 * en una linea para no cambiar a Survival.
 */
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

#include "Haruka/RHI/RHITypes.h"
#include "Haruka/RHI/RHIResources.h"
#include "Haruka/World/Props/InstancedObject.h"
#include "Haruka/World/Props/PropScatter.h"   // ScatteredProp/PropScatterStats: el worker async del scatter
#include "Haruka/World/Props/PropSystemBase.h" // la base: re-siembra, prototipos, cull (hermano: GrassSystem)
#include "Haruka/Renderer/GPUInstancing.h"

namespace Haruka { namespace Core { class Camera; } namespace Physics { class PhysicsEngine; } namespace RHI { class Context; } }
namespace Haruka::World::Planet { class PlanetarySystem; }
namespace Haruka::World { class WorldSystem; namespace Planet { class TerrestrialPlanet; } }

namespace Haruka::World::Props {

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

class PropSystem final : public PropSystemBase {
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


    ~PropSystem() override;
    void shutdown() override;   ///< suelta la GPU (el device sigue vivo)

    // ── Por frame ──────────────────────────────────────────────────────────────────────────────
    /// Al empezar el frame: el anillo de instancias empieza de cero (ver GPUInstancing::kRing).
    void beginFrame() override { if (m_instancing) m_instancing->beginFrame(); }
    /** @brief Pase instanciado de color. Dentro del render pass de escena. */
    void draw(RHI::Context* ctx, const DrawFrame& f, DrawStats& stats) override;
    /** @brief Solo profundidad, en el mapa de sombras (culling por la caja de la LUZ). */
    void drawShadows(RHI::Context* ctx, const glm::mat4& lightSpace, Core::Camera* camera,
                     Planet::PlanetarySystem* planets);

    // ── API del juego ──────────────────────────────────────────────────────────────────────────
    /** @brief Rompe la parte de prop mas cercana al golpe (esfera `center`+`radius`), si la hay.
     *  Marca el estado en el registro (tronco → `Destroyed`, rama → bit en `breakMask`) y REGENERA
     *  los colliders. La dispara el JUEGO (el hachazo), no el motor. */
    Hit breakAt(const glm::dvec3& center, double radius, Core::Camera* camera,
                Planet::PlanetarySystem* planets, Physics::PhysicsEngine* physics);
    /** @brief Una parte de prop cercana, SIN romperla: para quien decide si romper por su cuenta (una
     *  criatura enorme lo hace por energía, no por un golpe de hacha). */
    struct NearPart {
        uint32_t    seed = 0;
        std::string prototype;
        int         partId = -1;
        bool        trunk = false;
        PropWorldOBB box;
    };
    /** @brief Las partes rompibles de los props vivos a menos de `radius` de `center`. UNA pasada por
     *  el registro: para no recorrerlo una vez por cada pieza que pregunta. */
    std::vector<NearPart> partsNear(const glm::dvec3& center, double radius, Planet::PlanetarySystem* planets) const;
    /** @brief Rompe UNA parte (tronco = el prop entero) SIN rehacer los colliders: quien rompa varias
     *  en un frame los rehace una vez al final con `refreshCollidersAround`. false si ya estaba rota. */
    bool breakPart(uint32_t seed, int partId, bool trunk, const glm::dvec3& pos, const std::string& prototype,
                   double rBottomM, Planet::PlanetarySystem* planets);
    void refreshCollidersAround(Core::Camera* camera, Planet::PlanetarySystem* planets, Physics::PhysicsEngine* physics);
    /** @brief Todo lo que el jugador ha roto, para guardarlo en la partida.
     *
     *  ⚠️ Vive FUERA del registro de instancias a proposito. El scatter regenera las instancias
     *  desde cero y solo conserva las que siguen cerca: si el estado viviera solo ahi, el arbol que
     *  talaste reapareceria en cuanto te alejaras lo bastante para que saliera del radio del
     *  scatter. El mapa es por SEMILLA (la celda del mundo), que es una identidad estable. Solo
     *  guarda lo ROTO: un mundo entero de arboles intactos se regenera de la semilla. */
    std::vector<StateDelta> serializeState() const;

    /** @brief AVISO: algo de este sistema ha caído al suelo en `center` (un árbol talado, una roca
     *  partida, un cúmulo derribado) y aplasta lo que haya en `radius`. PropSystem no sabe qué hay
     *  en el suelo —la hierba es de `GrassSystem`—: quien monta los sistemas (la app) lo conecta. */
    std::function<void(const glm::dvec3& center, double radius)> onGroundImpact;
    void restoreState(const std::vector<StateDelta>& deltas);




    /** @brief Snapshot de DEBUG del scatter para la jerarquia del editor (apagado por defecto:
     *  el barrido por instancia cuesta). */
    void setDebugEnabled(bool on) { m_debugEnabled = on; }
    bool debugEnabled() const { return m_debugEnabled; }
    const std::vector<PropPrototypeDebug>& scatterDebug() const { return m_scatterDebug; }

    /** @brief Rebuilds the physics colliders of the living props within 96 m of `camPos`: the exact
     *  path a re-seed runs. Each part is its prop's real mesh (trunk, branches; never the canopy). */
    void refreshColliders(const glm::dvec3& planetC, double planetR, const glm::dvec3& camPos,
                          Physics::PhysicsEngine* physics);
    /// Ids de las mallas de colision por (prototipo → parte) y su copia CPU: el alambre de
    /// depuracion dibuja EXACTAMENTE la geometria que usa la fisica.
    const std::unordered_map<int, std::vector<int>>& meshShapes() const { return m_meshShapes; }
    const std::unordered_map<int, std::vector<std::vector<glm::vec3>>>& meshCpu() const { return m_meshCpu; }
    size_t hostBytes() const {
        return m_gpu.size() * sizeof(Renderer::InstanceDataFloat) + (m_instancing ? m_instancing->hostBytes() : 0);
    }

private:
    // Cache GPU de prototipos: por nombre → malla (VBO/EBO por nivel) + material PER-PIXEL.
    void rebuildGpu(const glm::dvec3& planetC, double planetR, Core::Camera* camera);
    const std::vector<int>& meshShapesFor(int protoIdx, Physics::PhysicsEngine* physics);
    // ── Los huecos de la base ──
    const char* systemName() const override { return "PropSystem"; }
    /// Las capas que NO son de la capa masiva: las que colisionan o reclaman sitio.
    bool ownsLayer(const PropLayer& L) const override { return !isMassLayer(L); }
    void configureScatter(PropScatterParams& params) override;
    /// Lo que devuelve el worker: las INSTANCIAS finales (prototipo, yaw determinista, estado
    /// inicial) y SUS INSTANCIAS GPU ya construidas (relativas a `origin`): la mitad de la
    /// re-siembra queda fuera del hilo de render. El filtro de boca NO va aquí: `surfaceCut` lee el
    /// vox viviente, que solo el hilo de render puede tocar.
    struct PropOutput final : ScatterOutput {
        std::vector<InstancedObject>             objs;   // paralela a `gpu`
        std::vector<Renderer::InstanceDataFloat> gpu;    // relativas a `origin`
    };
    ScatterJob makeJob(const ScatterContext& ctx) override;
    /// Parte FINAL de una re-siembra, en el hilo de render: sincroniza el estado roto, FILTRA las
    /// bocas contra el vox VIVIENTE y publica registro + GPU + colliders.
    void applyScatter(std::shared_ptr<ScatterOutput> out, const ApplyContext& ctx) override;
    void onCleared() override { m_scatterDebug.clear(); }

    /// Instancias YA TRANSFORMADAS (mismo indice que el registro), relativas a `m_origin`. Se
    /// construyen en el scatter, NO por frame: 180 000 matrices por frame costaban 31 ms.
    std::vector<Renderer::InstanceDataFloat> m_gpu;
    glm::dvec3 m_origin{0.0};
    std::unique_ptr<Renderer::GPUInstancing> m_instancing;
    /// Tope duro del arena de instancias. NO se fija en el primer frame (entonces el registro aún
    /// está vacío: el scatter llega en el frame de gen y fijarlo antes congelaba 2×0+256k, con el
    /// arena llenándose y RECORTANDO los props: "faltan las rocas y los árboles"). Se actualiza al
    /// MÁXIMO HISTÓRICO del anillo: solo crece cuando el mundo toca un nuevo máximo (los reallocs
    /// caen en frames de gen, ya de por sí gigantescos); en juego estable jamás se reasigna.
    size_t m_arenaHiRing = 0;
    RHI::PipelineHandle m_instPSO{}, m_shadowPSO{};
    RHI::BufferHandle   m_shadowUBO{};
    /// UNO POR PROTOTIPO: en Vulkan los draws se graban y se ejecutan despues; un buffer
    /// compartido hacia que todos leyeran el ultimo material escrito (props blancos).
    std::unordered_map<uint32_t, StateDelta> m_state;      ///< lo roto, por semilla
    std::unordered_map<int, std::vector<int>> m_meshShapes;
    std::unordered_map<int, std::vector<std::vector<glm::vec3>>> m_meshCpu;
    std::vector<PropPrototypeDebug> m_scatterDebug;
    // ── LOD POR CARGA ──
    // `m_propDetail` = 1 → sin LOD por distancia (todo nivel 0 por encima del sub-pixel) · 0 →
    // umbrales historicos (90/25 px) · NEGATIVO → mas agresivo que el historico (umbrales por
    // encima de 90/25): el controlador baja AQUI cuando ni el config historico alcanza el
    // presupuesto ("no saturar la GPU" manda sobre conservar el detalle de siempre). Solo manda
    // cuando `m_loadDriven` (tenemos la medicion GPU del pase = Vulkan); en GL se queda en false y
    // los umbrales son los fijos de siempre.
    bool   m_loadDriven   = false;
    float  m_propDetail   = 1.0f;
    double m_propMsSmooth = 0.0;    ///< EMA del ms del pase de props (frame anterior)
    double m_lastLoadStep = -1e18;  ///< reloj diag del último paso del controlador
    static constexpr double       kPropBudgetMs = 6.0;   ///< presupuesto del pase de props (ms GPU)
    static constexpr double       kLoadStepS    = 0.5;   ///< cada cuanto ajusta el controlador (s)
    static constexpr double       kLoadKi       = 0.25;  ///< paso de detalle por unidad de error
    static constexpr const char*  kPropScope    = "scene.prop.instanced";
    static constexpr float        kDetailMin    = -1.0f; ///< suelo del detalle (por debajo del historico)
    // ── RADIO DEL SCATTER POR CARGA ──
    // El borde exterior del scatter (donde los props acaban) se estira hasta su techo a su propio
    // ritmo, DESACOPLADO del detalle: las bandas lejanas son sub-pixel (cull por tamaño, no se
    // dibujan) o LOD2 de 40 tris → coste GPU ~nulo, asi que una GPU saturada por el tramo cercano
    // no debe negar "dibuja mas de lejos". El coste real del radio es CPU (barrido O(N), arena) y
    // lo limita la cota `maxProps` del scatter. Solo se RECORTA como ultimo recurso —cuando ni en
    // el suelo de detalle se llega al presupuesto— para aliviar el barrido. Cada cambio fuerza una
    // re-siembra (`m_rescatter`).
    float m_ringM     = kRingFloorM;   ///< borde exterior actual del scatter (m)
    static constexpr float kRingFloorM = 6000.0f;    ///< radio historico (probado): nunca se baja
    static constexpr float kRingMaxM   = 24000.0f;   ///< techo del radio (m)
    static constexpr float kRingStepM  = 3000.0f;    ///< paso del radio por tick de control (m)
    static constexpr float kRingFadeM  = 300.0f;     ///< crossfade entre bandas (m); las rampas se compensan → sin aro vacío
    static constexpr double kRingPaceS = 1.5;        ///< cada cuanto mueve el radio (s)
    // buckets del pase (reusados entre frames)
    std::vector<std::vector<uint32_t>> m_buckets, m_shadowBuckets;
    std::vector<int> m_aliveCounts, m_drawnCounts;
    bool m_debugEnabled = false;
};

} // namespace Haruka::World::Props
