#pragma once
/**
 * @file PropSystemBase.h
 * @brief LA BASE de los sistemas que pueblan el planeta con el scatter: lo que tienen en común
 *        `PropSystem` (árboles, rocas, casas, cúmulos) y `GrassSystem` (hierba en masa).
 *
 * ── POR QUÉ HAY UNA BASE Y DOS SISTEMAS ──────────────────────────────────────────────────────────
 *
 * Los dos colocan cosas del mismo modo —la misma ecología por celda mundial, determinista por
 * semilla, re-sembrando en un hilo worker al moverse— y las dibujan con las mismas mallas de
 * prototipo y el mismo material. Pero NO son el mismo sistema: un prop es una instancia con
 * identidad (colisiona, se rompe por partes, se guarda), y la hierba es un campo de millones de
 * matas sin collider que la GPU reconstruye. Meter la hierba como un caso más dentro de los props
 * mezclaba dos problemas en un archivo y ataba uno a las decisiones del otro.
 *
 * Así que lo común vive aquí y cada sistema HEREDA y rellena sus huecos:
 *   - `ownsLayer`        qué capas de la tabla son suyas (el scatter salta el resto por índice).
 *   - `configureScatter` bandas, densidades, topes.
 *   - `makeJob`          qué hace el worker con lo colocado (instancias, brotes…), con copias.
 *   - `applyScatter`     publicarlo en el hilo de render (filtros contra el vox VIVO, GPU).
 *   - `draw`             su pase.
 *   - `onRefresh`        lo que corre cada frame antes de decidir si re-sembrar.
 *
 * Lo que la base pone: el ciclo de re-siembra asíncrona (cuándo re-sembrar, lanzar, recoger,
 * descartar si cambió el planeta), el registro de prototipos de SUS capas, la caché de mallas
 * horneadas (`prototypeGpu`), el cull + LOD por píxel (`makeCull`) y el filtro de bocas/cráteres.
 *
 * Cada sistema re-siembra con SU scatter: el campo se muestrea una vez por sistema (en el worker,
 * fuera del frame). Es el precio de que sean sistemas de verdad y no uno colgado del otro.
 */
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <glm/glm.hpp>

#include "Haruka/RHI/RHITypes.h"
#include "Haruka/RHI/RHIResources.h"
#include "Haruka/World/Props/InstancedObject.h"
#include "Haruka/World/Props/PropGpu.h"
#include "Haruka/World/Props/PropScatter.h"
#include "Haruka/World/Vox/VoxWorld.h"

namespace Haruka { namespace RHI { class Context; class Device; } }
namespace Haruka::Core { class Camera; }
namespace Haruka::Physics { class PhysicsEngine; }
namespace Haruka::World { class WorldSystem; }
namespace Haruka::World::Planet { class PlanetarySystem; class TerrestrialPlanet; }
namespace Haruka::Tools::ProcGraph { struct TreeMeshData; }

namespace Haruka::World::Props {

class PropSystemBase {
public:
    /// Lo que el pase de color necesita del frame y no es suyo.
    struct DrawFrame {
        Core::Camera*            camera    = nullptr;
        Planet::PlanetarySystem* planets   = nullptr;
        const WorldSystem*       world     = nullptr;   ///< viento (nullptr = calma)
        RHI::BufferHandle        perFrameUBO{};         ///< view/proj + luces (binding 0)
        RHI::PipelineHandle      scenePSO{};            ///< se restaura al acabar (el cierre lo asume)
        float                    aspect    = 1.0f;
        int                      heightPx  = 1;
        glm::vec4                aerial{0.0f};          ///< perspectiva aerea (lib/aerial.glsl)
        /// CLIMA que pesa sobre la vegetación (SkyPass::weather): x = nieve acumulada · y = suelo
        /// mojado · z = lluvia · w = nevada.
        glm::vec4                weather{0.0f};
        double                   diagClock = 0.0;       ///< para espaciar las sondas de coste
    };
    /// Lo que suma el pase a las estadisticas del frame.
    struct DrawStats {
        int renderedDrawCalls = 0, renderedVertices = 0, renderedTriangles = 0;
        int totalDrawCalls = 0,    totalVertices = 0,    totalTriangles = 0;
    };

    PropSystemBase() = default;
    PropSystemBase(const PropSystemBase&) = delete;
    PropSystemBase& operator=(const PropSystemBase&) = delete;
    virtual ~PropSystemBase();
    /** @brief Suelta la GPU (el device sigue vivo). Espera al worker. Quien herede y tenga GPU
     *  propia la suelta y llama a la base. */
    virtual void shutdown();

    /// Al empezar el frame.
    virtual void beginFrame() {}
    /** @brief Re-siembra por demanda (cuando la cámara cruza un tramo, cambia el campo o el sistema
     *  lo pide) y aplica lo que el worker haya terminado. ANTES de abrir ningún render pass. */
    void refreshScatter(Core::Camera* camera, Planet::PlanetarySystem* planets, Physics::PhysicsEngine* physics);
    /** @brief El pase de color, dentro del render pass de escena. */
    virtual void draw(RHI::Context* ctx, const DrawFrame& f, DrawStats& stats) = 0;

    void setEnabled(bool on) { m_enabled = on; }
    bool enabled() const { return m_enabled; }
    const InstancedObjectRegistry& registry() const { return m_registry; }
    /// Para tests y herramientas: prototipos (e instancias, en PropSystem) a mano.
    InstancedObjectRegistry& mutableRegistry() { return m_registry; }

protected:
    // ── Huecos que rellena cada sistema ────────────────────────────────────────────────────────
    /// Nombre para los logs.
    virtual const char* systemName() const = 0;
    /// ¿Es suya esta capa? El scatter de este sistema salta las demás (por índice).
    virtual bool ownsLayer(const PropLayer& L) const = 0;
    /// Bandas, densidades, topes. La base ya puso radio, semilla y `layerOwned`.
    virtual void configureScatter(PropScatterParams& params) = 0;

    /// Lo que el worker devuelve: cada sistema hereda y mete lo suyo.
    struct ScatterOutput {
        virtual ~ScatterOutput() = default;
        glm::dvec3 origin{0.0};   ///< cámara de lanzamiento: lo construido va relativo a ella
    };
    struct ScatterContext {
        glm::dvec3 camPos{0.0}, planetC{0.0};
        double     planetR = 0.0;
        uint32_t   planetSeed = 1u;
        /// nombre de prototipo → índice del registro (foto de lanzamiento)
        std::unordered_map<std::string, int> protoByName;
    };
    /// Corre en el WORKER: solo copias (nunca el registro, la GPU ni el vox vivo).
    using ScatterJob = std::function<std::shared_ptr<ScatterOutput>(std::vector<ScatteredProp>&& placed)>;
    /** @brief Prepara el trabajo del worker en el hilo de render (copiando lo que necesite). */
    virtual ScatterJob makeJob(const ScatterContext& ctx) = 0;
    struct ApplyContext {
        glm::dvec3                       planetC{0.0};
        double                           planetR = 0.0;
        Core::Camera*                    camera  = nullptr;
        Physics::PhysicsEngine*          physics = nullptr;
        const Planet::TerrestrialPlanet* planet  = nullptr;
    };
    /** @brief Publica el resultado del worker, en el hilo de render. */
    virtual void applyScatter(std::shared_ptr<ScatterOutput> out, const ApplyContext& ctx) = 0;
    /** @brief Cada `refreshScatter`, ANTES de decidir si re-sembrar (los early-return del tramo no
     *  lo saltan). Con planeta de superficie activo. */
    virtual void onRefresh(Physics::PhysicsEngine* /*physics*/) {}
    /// El sistema se apagó o no hay planeta con superficie: soltar lo publicado.
    virtual void onCleared() {}

    // ── Lo común ───────────────────────────────────────────────────────────────────────────────
    /// La malla horneada de un prototipo (la hornea la primera vez). nullptr si el bake falló.
    const PrototypeGpu* prototypeGpu(const InstancedPrototype& proto);
    /// La malla CPU del prototipo a un detalle (1 = la de siempre). La misma que se sube a la GPU:
    /// quien necesite la geometría (los colliders de PropSystem) la pide aquí y no la rehornea distinta.
    static Haruka::Tools::ProcGraph::TreeMeshData bakePrototypeMesh(const InstancedPrototype& proto, float detail);
    /// Ata los cinco slots de material (en Vulkan un slot sin atar es basura).
    static void bindPrototypeMaterial(RHI::Context* ctx, const PrototypeGpu& pg);
    /** @brief UBO de parámetros del prototipo `proto` (binding 6), uno por prototipo: en Vulkan los
     *  draws se graban y se ejecutan después, y un UBO compartido daría a todos el último material. */
    RHI::BufferHandle paramsUBO(RHI::Device* dev, int proto);
    /// `PropParams` rellenos con lo del frame y el material del prototipo.
    static PropParams makeParams(const PrototypeGpu& pg, const glm::vec3& wind, float time,
                                 const glm::vec3& originRel, const DrawFrame& f);
    /// Viento del clima y reloj del balanceo (de pared; `HARUKA_PROP_TIME` lo fija para capturas).
    static glm::vec3 frameWind(const DrawFrame& f);
    static float     frameTime();

    /** @brief Cull de frustum (esfera vs 4 planos) + descarte sub-píxel + LOD por RADIO EN PÍXELES.
     *  `lodPx0/lodPx1`: por encima de cuántos px se dibuja a nivel 0 / 1. Dentro de `kNearM`
     *  siempre nivel 0. Devuelve 0 = visible (y `lod`), 1 = fuera del frustum, 2 = sub-píxel. */
    struct Cull {
        glm::vec3 camF{0.0f}, camU{0.0f}, camR{0.0f};
        float tanV = 0.5f, tanH = 0.5f;
        float pixPerM2 = 1.0f, near2 = 0.0f, lod02 = 1.0f, lod12 = 1.0f;
        int   forceLod = -1;
        uint8_t operator()(const glm::vec3& posF, float radius, int& outLod) const;
    };
    static Cull makeCull(const DrawFrame& f, float lodPx0, float lodPx1);
    static constexpr float kNearM = 45.0f;   ///< PRIORIDAD CERCANA: dentro, SIEMPRE nivel 0

    /** @brief ¿Retirar lo que caería en `dir`? Boca de cueva (el vox dice aire) o cuenco de cráter.
     *  Se construye UNA vez por aplicar (colecta las columnas con hueco) y es de solo lectura. */
    struct SurfaceFilter {
        const Planet::TerrestrialPlanet* planet = nullptr;
        std::unordered_set<Vox::VoxKey, Vox::VoxKeyHash> cutCols;
        bool removes(const glm::vec3& dir) const;
    };
    static SurfaceFilter makeSurfaceFilter(const Planet::TerrestrialPlanet* planet);

    /// `HARUKA_NOPROPS=1` apaga el camino entero (scatter y dibujo), como bisección.
    static bool propsDisabled();

    // ── Estado común ───────────────────────────────────────────────────────────────────────────
    InstancedObjectRegistry m_registry;     ///< prototipos de SUS capas (e instancias, en PropSystem)
    std::unordered_map<std::string, PrototypeGpu> m_protoMesh;
    std::vector<RHI::BufferHandle> m_paramsUBOs;
    /// Anclaje del planeta: `dir + heightM` → mundo sin pedir el PlanetarySystem.
    glm::dvec3  m_planetC{0.0};
    double      m_planetR = 6371000.0;
    bool        m_enabled = true;
    /// El sistema pide re-sembrar aunque la cámara no se haya movido (p. ej. cambió su radio).
    bool        m_rescatter = false;

private:
    // Re-siembra ASINCRONA (ver `refreshScatter`).
    std::future<std::shared_ptr<ScatterOutput>> m_scatterFut;
    bool        m_scatterBusy = false;
    std::string m_scatterPlanet;              ///< planeta con el que se lanzó el worker
    glm::dvec3  m_scatterCam{0.0};            ///< posición de lanzamiento
    glm::dvec3  m_lastCam{1e300, 1e300, 1e300};
    std::string m_planet;
    uint64_t    m_voxVersion = 0;
    std::chrono::steady_clock::time_point m_lastT = std::chrono::steady_clock::now();   ///< reloj del tramo adaptivo
};

} // namespace Haruka::World::Props
