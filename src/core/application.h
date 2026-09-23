#ifndef APPLICATION_H
#define APPLICATION_H

/**
 * @file application.h
 * @brief `Haruka::Core::Application`: el dueño del proceso, la ventana y el frame.
 *
 * Application es el objeto que `main()` construye y del que cuelga todo lo demás:
 * la @ref Haruka::Core::Window, el @ref Haruka::RHI::Device, la escena activa, la
 * cámara, el @ref Haruka::WorldSystem, el @ref Haruka::PlanetarySystem, el
 * @ref PhysicsEngine y los recursos del renderer. Es también quien decide el
 * backend gráfico y quien corre el bucle principal.
 *
 * La implementación está partida en cuatro traducciones porque son cuatro
 * responsabilidades con ritmos distintos:
 *
 * | Fichero | Qué vive ahí |
 * |---------|--------------|
 * | `application.cpp`         | ciclo de vida: arranque, carga de escena, ajustes, bucle |
 * | `application_render.cpp`  | el frame: cola de render y todos los pases |
 * | `application_assets.cpp`  | cachés de mallas, texturas y materiales |
 * | `application_network.cpp` | puente con el servidor autoritativo (DGS) |
 *
 * @note La clase es grande y sus métodos también. Para leer uno concreto no hace
 * falta seguirlo a mano: en su página de documentación, el botón
 * *Flujo de ejecución* despliega lo que llama, por qué ramas pasa y qué variables
 * escribe, sacado del AST del compilador (@ref flujo_interactivo).
 */

#include <SDL3/SDL.h>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <chrono>
#include <future>     // el horneado del cielo corre en un hilo (134 ms medidos: 8 frames)
#include <algorithm>
#include <cstdlib>   // getenv/atof: HARUKA_RAIN (ver m_rainOverride)
#include <unordered_set>
#include <unordered_map>

#ifdef HARUKA_NETWORK
    #include "include/dgs/client.h"
    #include "net/entity_sync.h"
#endif

#include "tools/math_types.h"
#include "core/modules.h"      // HARUKA_MOD_* (gating de subsistemas opcionales)
#include "world/world_system.h"
#include "world/world_system_provider.h"  // adaptador IWorldProvider (cliente) para la física
#include "core/window.h"
#include "core/camera.h"
#include "renderer/cloud_pass.h"   // el pase volumetrico de nubes (modulo)
#include "renderer/post_pass.h"    // el post-proceso (modulo)
#include "renderer/collision_wire.h"   // alambre de la malla de colision (modulo)
#include "renderer/sky_pass.h"         // cielo + clima del frame (modulo)
#include "renderer/scene_pass.h"       // objetos de la escena + construccion (modulo)
#include "renderer/frame_lights.h"     // sol, luna, ambiente del cielo (modulo)
#include "world/vox/vox_system.h"      // cuevas → Jolt y anillo del suelo cercano (modulo)
#include "renderer/shadow_pass.h"      // sombra del sol + mascara cenital (modulo)
#ifdef HARUKA_MOD_FLUIDS
#include "world/water/fluid_bridge.h"   // el enganche del agua dinamica al planeta
#endif
#include "rhi/rhi_device.h"
#include "core/scene/scene_manager.h"
#include "core/scene/scene_render_policy.h"
#include "renderer/shader.h"
#include "renderer/shadow.h"
#include "renderer/hdr.h"
#include "renderer/bloom.h"
#include "world/planet/planet.h"        // TerrestrialPlanet::RenderStats (panel de performance)
#include "tools/profiler.h"
#include "renderer/ssao.h"
#include "renderer/ibl.h"
#include "renderer/point_shadow.h"
#include "renderer/precipitation_renderer.h"  // lluvia/nieve EN EL MUNDO (con depth)
#include "renderer/ground_stamp_renderer.h"   // HUELLAS en la capa granular (nieve/arena)
#include "world/ground_layer.h"                // la capa granular en sí (GL-free)
#include "renderer/render_target.h"
#include "renderer/simple_mesh.h"
#include "renderer/gpu_instancing.h"
#include "renderer/cascaded_shadow.h"
#ifdef HARUKA_MOD_FLUIDS
#include "renderer/fluid_host.h"
#endif
#include "tools/error_reporter.h"
#include "io/asset_streamer.h"
#include "tools/debug_overlay.h"
#include "physics/raycast_simple.h"
#include "physics/physics_engine.h"
#include "core/scene/scene_loader.h"
#include "core/game_interface.h"
#include "world/props/instanced_object.h"
#include "world/props/prop_system.h"

namespace Haruka { namespace Renderer { class MotorInstance; } } using Haruka::Renderer::MotorInstance;
namespace Haruka { namespace Renderer { class Model; class RenderTarget; } }
namespace Haruka { class MaterialComponent; }

namespace Haruka { namespace Core {

struct RenderFrame;   // lo que comparten los pases del frame (application_render.cpp)

// Los props son un modulo (world/props/prop_system.h); estos nombres siguen valiendo para el editor.
using PropInstanceDebug  = Haruka::World::PropInstanceDebug;
using PropPrototypeDebug = Haruka::World::PropPrototypeDebug;

/**
 * @brief Haruka runtime application orchestrator.
 *
 * Responsibilities:
 * - initialize render/scene systems
 * - render frames for the active scene
 * - maintain camera and global render state
 *
 * Non-responsibilities:
 * - window creation/ownership in editor-driven embedding mode
 * - high-level editor UI orchestration
 */
class Application {
public:
    Application();
    ~Application();
    
    /** @name Accessors */
    ///@{
    Camera* getCamera() { return _camera.get(); }
    Haruka::SceneManager* getCurrentScene() { return _currentScene; }
    RaycastSimple* getRaycastSystem() { return _raycastSystem.get(); }
    Haruka::PlanetarySystem* getPlanetarySystem() { return _planetarySystem.get(); }
    /** @brief El host de fluidos del motor (ríos/lagos + splash). Es el ÚNICO dueño de la sim de
     *  aguas someras: el juego le pide la suya en vez de crear otra. Puede ser null antes del
     *  primer frame de render. */
    Haruka::FluidHost* getFluidHost() { return _fluidHost.get(); }
    /** @brief Vista de depuración del terreno (0=normal, 1=elevación, 2=zonas, 3=bioma,
     *  4=temperatura, 5=humedad, 6=capas, 10+i=máscara de capa i). La usa el editor. */
    void setPlanetDebugView(int view) {
        if (_planetarySystem) _planetarySystem->setDebugView(view);
    }
    /** @brief Nombres de las capas de textura del terreno (para el selector de capas del editor). */
    std::vector<std::string> getPlanetTerrainLayerNames() const {
        if (_planetarySystem) return _planetarySystem->activeTerrainLayerNames();
        return {};
    }
    /** @brief Nombres de las CAPAS DE PROPS (para pintar el área de spawn de cada capa). */
    std::vector<std::string> getPlanetPropLayerNames() const {
        if (_planetarySystem) return _planetarySystem->activePropLayerNames();
        return {};
    }
    /** @brief Regenera el planeta `name` leyendo SU surfaceConfig actual de la escena.
     *  Camino del editor: cambias parámetros en el inspector y el planeta se reconstruye. */
    void regeneratePlanet(const std::string& name) {
        if (_planetarySystem && _currentScene) {
            _planetarySystem->updatePlanetFromScene(*_currentScene, name);
        }
    }
    /** @brief Fuerza la lluvia (0..1) para pruebas; <0 vuelve al modo AUTO (según el clima). */
    void setRainOverride(float r) { m_rainOverride = r; }

    // ── PROPS DEL MUNDO: el modulo es `World::PropSystem`; esto solo reenvia (la API que usa
    //    Survival no cambia). Ver world/props/prop_system.h.
    void setPropScatterEnabled(bool enabled) { m_props.setEnabled(enabled); }
    bool isPropScatterEnabled() const { return m_props.enabled(); }
    using PropHit = Haruka::World::PropSystem::Hit;
    /** @brief Rompe la parte de prop mas cercana al golpe. La dispara el JUEGO (el hachazo). */
    PropHit breakPropAt(const glm::dvec3& center, double radius);
    const Haruka::InstancedObjectRegistry& propRegistry() const { return m_props.registry(); }
    Haruka::World::PropSystem& props() { return m_props; }


    using PropStateDelta = Haruka::World::PropSystem::StateDelta;
    /** @brief Todo lo que el jugador ha roto, para guardarlo en la partida (ver PropSystem). */
    std::vector<PropStateDelta> serializePropState() const { return m_props.serializeState(); }
    void restorePropState(const std::vector<PropStateDelta>& deltas) { m_props.restoreState(deltas); }
    void setPropDebugEnabled(bool enabled) { m_props.setDebugEnabled(enabled); }
    bool isPropDebugEnabled() const { return m_props.debugEnabled(); }
    const std::vector<PropPrototypeDebug>& getPropScatterDebug() const { return m_props.scatterDebug(); }

    /** @brief LA CAPA GRANULAR (nieve/arena/barro): aquí se registran las HUELLAS. Cualquier cosa con
     *  collider puede pisar — el jugador por zancada, una criatura, una rueda; el motor solo necesita
     *  dónde y con qué huella. Ver `GroundLayer` para por qué esto NO pasa por `DeformationField`. */
    Haruka::GroundLayer& getGroundLayer() { return m_groundLayer; }
    /** @brief Espesor de NIEVE acumulada [0,1] aquí y ahora. El juego lo usa para decidir si una
     *  pisada deja marca, cuánto te hundes y cuánto frena. */
    float getSnowAccum() const { return m_sky.weather().snowAccum; }
    /** @brief Agua acumulada en el suelo [0,1] (mojado; baja el agarre). */
    float getGroundWetness() const { return m_sky.weather().groundWetness; }
#ifdef HARUKA_MOD_PHYSICS
    Haruka::PhysicsEngine* getPhysicsEngine() { return _physicsEngine.get(); }
    Haruka::PhysicsEngine* getPhysicsEngineOrNull() { return _physicsEngine.get(); }
#else
    Haruka::PhysicsEngine* getPhysicsEngineOrNull() { return nullptr; }
#endif

    /** @brief AABB de un modelo (lo carga/cachea si hace falta). Para colisión de props
     *  colocados: caja ajustada al modelo. Devuelve false si no se pudo. */
    bool getModelBounds(const std::string& path, glm::vec3& outMin, glm::vec3& outMax);

    /** @brief Modelo cargado (carga PEREZOSA como getModelBounds), o nullptr. Da acceso a sus mallas
     *  —cuyos vértices están también en CPU— para que el juego pueda construir geometría a partir de un
     *  `.glb` sin pasar por el pipeline de render. Lo usa el PREVIEW 3D del inventario. */
    Haruka::Renderer::Model* getModel(const std::string& path);

    /**
     * @brief Radio de la esfera que ENVUELVE al objeto, en metros de mundo.
     *
     * Existe para que un editor pueda encuadrar cualquier objeto sin saber qué es: el IDE trata todo
     * como `SceneObject` genérico y no sabe que un planeta mide 6371 km mientras un prop mide 40 cm.
     * Esa diferencia —de siete órdenes de magnitud— la resuelve el motor, que sí lo sabe:
     *
     *  1. Planeta (por nombre, en `PlanetarySystem`) → su radio real.
     *  2. Modelo `.glb`/`.obj` → media diagonal de su AABB local × la escala del objeto.
     *  3. Lo demás (primitivas, luces, spawns) → las primitivas son unitarias y centradas en el
     *     origen, así que media diagonal del cubo unidad × la escala.
     *
     * Nunca devuelve 0: encuadrar con radio 0 pondría la cámara EXACTAMENTE sobre el objeto.
     */
    double getObjectBoundingRadius(const Haruka::SceneObject& obj);

    /**
     * @brief Identificador GL de una textura de material, cargándola y cacheándola si hace falta.
     *
     * Es la MISMA caché que usa el pase de escena, no una copia: la miniatura que enseña el editor
     * y lo que se ve sobre el objeto salen del mismo píxel. Con una caché propia, el IDE podría
     * enseñar un PNG que el render no encontró (o al revés) y nadie lo notaría.
     * Devuelve 0 si la ruta no se pudo cargar (el fallo también se cachea).
     */
    unsigned int getMaterialTextureGL(const std::string& path);

    /**
     * @brief Dibuja una ESFERA con ese material en el target dado. Preview de material del editor.
     *
     * Vive en el motor porque el sombreado es suyo: los PSO, el UBO per-frame y `final.frag` no son
     * públicos, y una preview pintada con otro shader mentiría — enseñaría un material que no es el
     * que verás en la escena. Iluminación fija de tres cuartos para que dos materiales se puedan
     * comparar entre sí sin que la hora del día del mundo cambie el resultado.
     */
    void renderMaterialPreview(const Haruka::MaterialComponent& material,
                               Haruka::Renderer::RenderTarget& target);
    Haruka::WorldSystem* getWorldSystem() { return _worldSystem.get(); }
    ///@}

    /** @brief Sets the render quality preset. */
    static void setRenderQualityPreset(int preset) { s_renderQualityPreset = std::clamp(preset, 0, 3); }
    static int getRenderQualityPreset() { return s_renderQualityPreset; }
    static void setRenderFeatureHDR(bool enabled) { s_enableHDR = enabled; }
    static bool getRenderFeatureHDR() { return s_enableHDR; }
    static void setRenderFeatureBloom(bool enabled) { s_enableBloom = enabled; }
    static bool getRenderFeatureBloom() { return s_enableBloom; }
    static void setRenderFeatureSSAO(bool enabled) { s_enableSSAO = enabled; }
    static bool getRenderFeatureSSAO() { return s_enableSSAO; }
    static void setRenderFeatureIBL(bool enabled) { s_enableIBL = enabled; }
    static bool getRenderFeatureIBL() { return s_enableIBL; }
    static void setRenderFeatureShadows(bool enabled) { s_enableShadows = enabled; }
    static bool getRenderFeatureShadows() { return s_enableShadows; }

    /** @brief Reads GraphicsSettings from SettingsManager and applies to all engine systems. */
    void applyGraphicsSettings();

    /** @brief Sets the maximum distance for a render layer. */
    static void setLayerMaxDistance(int layer, float distance) {
        if (layer < 1 || layer > 5) return;
        s_layerMaxDistance[layer] = std::max(0.0f, distance);
    }
    /** @brief Gets the maximum distance for a render layer. */
    static float getLayerMaxDistance(int layer) {
        if (layer < 1 || layer > 5) return 0.0f;
        return s_layerMaxDistance[layer];
    }
    
    int getRenderedVertices()      const { return _iRenderedVertices; }
    int getRenderedTriangles()     const { return _iRenderedTriangles; }
    int getRenderedDrawCalls()     const { return _iRenderedDrawCalls; }
    int getTotalVertices()         const { return _iTotalVertices; }
    int getTotalTriangles()        const { return _iTotalTriangles; }
    int getTotalDrawCalls()        const { return _iTotalDrawCalls; }
    /// Duración REAL del último frame en ms, SIN el recorte de 100 ms del bucle (application.cpp).
    /// Para medidores (F5, sonda, `HARUKA_FRAMELOG`): la física sigue usando `deltaTime` recortado.
    float getFrameTimeMs()         const { return _lastFrameTimeMs; }

    // Stats de geometría del TERRENO del último frame (malla base + clipmap + agua), para el
    // panel de performance del editor. Sustituye al "Chunk Streaming" legacy (no existe streaming
    // por chunks: el planeta es malla fija + clipmap, así que el panel muestra el desglose real).
    Haruka::Planet::TerrestrialPlanet::RenderStats getTerrainStats() const { return m_terrainStats; }

    /** @brief Árbol CPU del ÚLTIMO FRAME del hilo de render (ms inclusivos por etapa, anidado):
     *  renderFrameContent → scene.objects.draw, scene.prop.instanced, simple_planet.draw →
     *  planet.base/clipmap/water.draw, etc. Los scopes son THREAD-LOCAL; este getter devuelve el
     *  árbol del hilo que llama (el del editor = el de render, ya que el imgui corre dentro del
     *  frame). Se añade `Haruka::Profiler::get().add()` con el tiempo GPU cuando exista. */
    const std::vector<Haruka::Profiler::Node>& profilerNodes() const {
        return Haruka::Profiler::get().nodes();
    }
    /** @brief Terrain height (metres above reference sphere) at a world position. */
    double getTerrainHeightAt(const glm::dvec3& worldPos) const {
        return _planetarySystem ? _planetarySystem->sampleTerrainHeight(worldPos) : 0.0;
    }
    /** @brief ¿Está el SUELO recortado aquí (boca de cueva, lo picado)? Si sí, la altura del
     *  heightfield NO es suelo: debajo hay aire y el jugador puede estar más abajo que ella.
     *  ⚠️ Es lo que el jugador tiene que consultar antes de "subirse al suelo" como red de
     *  seguridad: sin esto, dentro de un foso el script lo empujaba cada frame a la cota del
     *  heightfield — "flota" sobre la boca (Andoni lo vio). */
    bool isTerrainCutAt(const glm::dvec3& worldPos) const {
        if (!_planetarySystem) return false;
        const auto s = _planetarySystem->terrainSampler(worldPos);
        return s && s.surfaceCutAt(worldPos);
    }

    /** @brief Altura DEL SUELO DEL JUEGO en una dirección planet-local (km): la MISMA superficie que
     *  pisa el jugador (el SimplePlanet/TerrestrialPlanet, como `sampleTerrainHeight`). La
     *  ReferenceSurface (esfera lisa en esta rama) solo se usa como fallback. Segura desde un hilo
     *  worker (`sampleHeight` lee solo `m_heightCPU` inmutable). Devuelve false si no hay suelo. */
    bool groundHeightKmAtDir(const glm::dvec3& dir, float& outElevKm) const {
        if (!_planetarySystem) return false;
        return _planetarySystem->groundHeightKmAtDir(dir, outElevKm);
    }
    /** @brief Igual, con la dirección en float. ⚠️ PIERDE PRECISIÓN: en la Tierra un float solo
     *  resuelve ~0.5 m de posición, así que sobre pendiente real son centímetros de altura — la misma
     *  pérdida que costó 6.4 cm cuando `meshHeightKmAt` tomaba `vec3`. Usar la de `dvec3` siempre que
     *  se pueda. */
    bool groundHeightKmAtDir(const glm::vec3& dir, float& outElevKm) const {
        return groundHeightKmAtDir(glm::dvec3(dir), outElevKm);
    }

    /// Dynamic terrain editing: delegate to PlanetarySystem.
    void editTerrain(const glm::dvec3& worldPos, double radius, double step, bool dig) {
        if (_planetarySystem) _planetarySystem->editTerrain(worldPos, radius, step, dig);
    }
    /// Flatten terrain to target height.
    /** @brief EL CAMPO del mundo en un punto: > 0 roca, < 0 aire (m). Es `min(terreno, cueva)`:
     *  el heightfield más todo lo picado/construido. Lo que apunta el pico se busca contra ESTO,
     *  no contra la altura del heightfield — si no, desde dentro de una cueva no se puede picar. */
    double worldFieldAt(const glm::dvec3& worldPos) const {
        if (!_planetarySystem) return 1e9;
        const auto* tp = _planetarySystem->activeTerrestrial();
        if (!tp) return 1e9;
        return (double)tp->vox().density(worldPos - tp->position());
    }
    /** @brief Rayo contra el campo del mundo (la misma marcha que usa el editor: `VoxWorld::raycast`).
     *  Coordenadas de mundo. @return false sin planeta, desde dentro de la roca, o sin impacto. */
    bool worldRaycast(const glm::dvec3& o, const glm::dvec3& dir, double maxM, glm::dvec3& outHit) const {
        if (!_planetarySystem) return false;
        const auto* tp = _planetarySystem->activeTerrestrial();
        if (!tp) return false;
        glm::dvec3 h;
        if (!tp->vox().raycast(o - tp->position(), dir, maxM, h)) return false;
        outHit = h + tp->position();
        return true;
    }
    /** @brief Carpeta de la PARTIDA para lo que el jugador cambia del mundo (trazos, cuevas
     *  colocadas). Sin ella, lo picado se pierde al salir. La fija el juego al cargar/crear la
     *  partida. Idempotente. */
    void setWorldEditsDir(const std::string& dir) {
        if (_planetarySystem)
            if (auto* tp = _planetarySystem->activeTerrestrialMut()) tp->setVoxEditsDir(dir);
        m_worldEditsDir = dir;
    }
    const std::string& worldEditsDir() const { return m_worldEditsDir; }
    /** @brief Guarda los trazos del campo (llamar al guardar la partida). */
    bool saveWorldEdits() {
        if (!_planetarySystem) return false;
        auto* tp = _planetarySystem->activeTerrestrialMut();
        return tp && tp->vox().saveEdits();
    }
    void levelTerrain(const glm::dvec3& worldPos, double radius, double targetHeight) {
        if (_planetarySystem) _planetarySystem->levelTerrain(worldPos, radius, targetHeight);
    }

    /** @brief Cota del agua (m sobre el radio del planeta), o `PlanetarySystem::kNoWater` si aquí
     *  no hay agua. Ver PlanetarySystem::sampleWaterLevel — un hoyo cavado en tierra NO es mar. */
    double getWaterLevelAt(const glm::dvec3& worldPos) const {
        return _planetarySystem ? _planetarySystem->sampleWaterLevel(worldPos)
                                : Haruka::PlanetarySystem::kNoWater;
    }

    /** @brief Mean sea surface for the nearest planet. Returns false if none. */
    bool getSeaSurfaceAt(const glm::dvec3& worldPos, glm::dvec3& outCenter, double& outSeaRadius) const {
        return _planetarySystem ? _planetarySystem->getSeaSurface(worldPos, outCenter, outSeaRadius) : false;
    }

    /** @brief Current framebuffer height in pixels (editor viewport or window). */
    int getWindowHeight() const {
        if (m_editorViewportH > 0) return m_editorViewportH;
        return _window ? (int)_window->getHeight() : 0;
    }

    /** @brief Current framebuffer width in pixels (editor viewport or window). */
    int getWindowWidth() const {
        if (m_editorViewportW > 0) return m_editorViewportW;
        return _window ? (int)_window->getWidth() : 0;
    }

    CascadedShadowMap* getCascadedShadowMap() { return _cascadedShadow.get(); }
    Shader* getCascadedShadowShader() { return _cascadeShadowShader.get(); }

    void setImGuiRenderCallback(std::function<void()> cb) { _imguiCallback = std::move(cb); }

    /** @brief Requests a clean (no-HUD) PNG screenshot of the next rendered frame.
     *  Empty path → screenshots/shot_<timestamp>.png. Standalone runtime only. */
    void requestScreenshot(const std::string& path = "");
    
    /**
     * @brief Callback invoked by `MotorInstance` when active scene changes.
     * @note Performs internal copy into owned scene storage.
     */
    void onSceneChanged(Haruka::SceneManager* scene) {
        _currentScene = scene;
    }
    /**
     * @brief Callback invoked by `MotorInstance` when viewport camera changes.
     * @note Copies camera state into local owned camera instance.
     */
    void onCameraChanged(Camera* cam) {
        if (!cam) {
            _camera.reset();
            return;
        }
        _camera = std::make_unique<Camera>(cam->position);
        _camera->orientation = cam->orientation;
        _camera->zoom = cam->zoom;
        _camera->speed = cam->speed;
        _camera->sensitivity = cam->sensitivity;
    }

#ifdef HARUKA_NETWORK
    /// Lo que este jugador dice de si mismo, en UNA llamada. `data` es el payload del juego (vida,
    /// inventario, lo que sea); `size = 0` significa "sin cambios", no "vacio".
    void sendPlayerState(uint32_t uuid, const Haruka::WorldPos& pos, const Haruka::Rotation& rot,
                         const uint8_t* data = nullptr, uint16_t size = 0);
    /// ⚠️ WITHOUT THIS THE PLAYER IS CAPPED AT ~20 m/s AND NOBODY IS TOLD. The zone's S1 filter allows
    /// `maxSpeed * dt + 1 m` between updates and believes the most recent packet, and a transform that
    /// declares nothing declares zero — leaving the 1 m of slack as the only thing letting anyone move.
    /// At 20 Hz that is 72 km/h; a sprint, a vehicle or a descent is silently discarded by the server,
    /// which from the client looks exactly like packet loss. Call it whenever the movement mode changes.




    /// The cluster's world clock (see `Haruka::Network::hasWorldTime`). When there is one, the
    /// planetary simulation is DRIVEN by it instead of accumulating the local frame delta, which is
    /// what puts every player in the same day and the same storm.
    /// Pide al cluster colocar una pieza (ver `Haruka::Network::sendPlacePiece`).
    void sendPlacePiece(uint32_t actor, uint16_t typeId,
                        const Haruka::WorldPos& pos, const glm::dquat& orient);
    /// Pide al cluster tirar un objeto al mundo (ver `Haruka::Network::sendDropItem`).
    /// @return el `requestId` con el que reconocer el veredicto, o 0 si no se pudo pedir.
    uint32_t sendDropItem(uint32_t actor, const Haruka::WorldPos& pos, const std::string& kind);
    /// Los veredictos de acciones que han llegado desde la ultima vez. Un juego que coloca al
    /// instante necesita esto para poder DESHACERLO cuando el servidor dice que no.
    std::vector<DGS::ActionAck> pollActionResults();
    uint32_t networkSessionUuid() const;
    bool   networkHasWorldTime() const;
    /// Lo que pasa por el cable + RTT al head (ver DGS::Client::stats). Para el panel de depuracion.
    struct NetLinkStats { uint64_t txBytes = 0, rxBytes = 0; float headMs = -1, headMinMs = -1, headAvgMs = -1, headMaxMs = -1;
                          float zoneMs = -1, zoneMinMs = -1, zoneAvgMs = -1, zoneMaxMs = -1; uint32_t pingsLost = 0; std::string zone; };
    NetLinkStats networkLinkStats();
    /// Jugadores / npcs / objetos del mundo que el feed tiene ahora en la escena.
    struct NetCounts { int players = 0, npcs = 0, worldObjects = 0; };
    NetCounts networkEntityCounts() const { const auto c = m_entitySync.counts(); return { c.players, c.npcs, c.worldObjects }; }
    double networkWorldTimeSeconds() const;

    /// Which uuid is US, so the world feed does not spawn a second copy of the local player: the zone
    /// broadcasts the sender's own entity back along with everyone else's. Set it before connecting.
#ifdef HARUKA_NETWORK
    void setLocalPlayerUuid(uint32_t uuid) { m_entitySync.setLocalUuid(uuid); }
#else
    void setLocalPlayerUuid(uint32_t) {}
#endif

    /// "Este objeto del mundo YA LO TENGO YO": el juego lo colocó al pedirlo, con su física, y el
    /// servidor lo devuelve por el feed como cualquier otro. Sin esto aparecen DOS — el tuyo, que
    /// cae y rueda, y el del servidor, clavado donde lo pediste y sin físicas. Medido soltando una
    /// mesa: la copia quieta es la que se ve, y parece que el objeto se ha "estancado".
    /// Se llama con el uuid que trae el acuse de la acción; si la entidad ya había llegado antes
    /// que el acuse, retira la copia que se creó.
#ifdef HARUKA_NETWORK
    void adoptWorldObject(uint32_t uuid) { m_entitySync.adoptWorldObject(uuid, _currentScene); }
#endif

    /// The engine's opaque per-entity payload (an inventory, typically). The wire carries `dataSize`,
    /// so an empty one costs nothing; there was simply no way to reach it from a game.

    /// `channel` decides which wire it takes, because it decides who may hear it: `CHAT_LOCAL` goes
    /// over the UDP link to the ZONE and is filtered by the same interest radius as the world itself;
    /// guild and global go to the social node. Defaults to global.
    void sendPlayerChat(uint32_t uuid, const std::string& username, const std::string& text,
                        uint8_t channel = DGS::CHAT_GLOBAL);
    std::vector<DGS::ChatMessage> pollPlayerChats();
    bool connectDGS(const std::string& headHost, int headPort,
                    const std::string& email,    const std::string& password,
                    const std::string& apiHost = "", int apiPort = 0);
    bool isNetworkConnected() const;
    int  getGhostCount()      const;
    /// ⚠️ "¿LA ZONA ME OYE?" — la prueba que no existia (ver `EntitySync::selfEchoAgeS`). Edad en
    /// segundos del ultimo eco que la zona devolvio de TU transform; -1 mientras no llegue ninguno.
    /// El panel de depuracion lo dibuja como "zona te oye: si/no".
    double networkSelfEchoAgeS() const { return m_entitySync.selfEchoAgeS(); }
#endif

    /** @brief Attaches a game interface — run() will call onInit/onUpdate/onShutdown automatically. */
    void setGameInterface(Haruka::GameInterface* gi) { _gameInterface = gi; }

    /** @brief Starts runtime using a scene path bootstrap. */
    void run(const std::string& startScenePath, bool headless = false);
    /** @brief Initializes systems from a scene instance. */
    void init(Haruka::SceneManager& scene);
    /** @brief Recreates all size-dependent FBOs when the viewport is resized. */
    void recreateFBOs(int newWidth, int newHeight);
    /** @brief Sets the viewport-owned FBO that the editor render path writes into.
     *  Must be called after recreateRenderTarget() on the viewport side. */
    void setEditorTarget(RenderTarget* rt) { _editorTarget = rt; }
    /** @brief Sets the render size used in editor mode (when no Window exists). */
    void setEditorViewportSize(int w, int h);
    /** @brief Scans current scene and initialises PlanetarySystem with planet objects. */
    void initPlanetarySystem();
    /** @brief Loads scene data from disk path. */
    void loadScene(const std::string& scenePath);
    /** @brief Builds the render queue with frustum culling and LOD management. */
    void buildRenderQueue();
    /** @brief Renders one frame and updates timing state. */
    /// Drena lo que el cluster ha mandado y lo convierte en objetos de la escena (ver la nota larga
    /// en la definicion). Es SIMULACION, no render: el bucle del juego la llama antes de dibujar.
    void renderFrame();
    /** @brief Frame rendering body (logic-only path). */
    void renderFrameContent();   // LA LISTA de pases (ver application_render.cpp)
    // Los pases, en el orden de la lista. Cada uno con su trozo; `RenderFrame` con lo compartido.
    void frameBegin(RenderFrame& f);
    void passCompute(RenderFrame& f);
    void passSky(RenderFrame& f);
    void passSceneSetup(RenderFrame& f);
    void passSceneObjects(RenderFrame& f);
    void passProps(RenderFrame& f);
    void passPlanetUpdate(RenderFrame& f);
    void passShadows(RenderFrame& f);
    void passPlanet(RenderFrame& f);
    void passGameWorld(RenderFrame& f);
    void passPrecipitation(RenderFrame& f);
    void passFluid(RenderFrame& f);
    void passClouds(RenderFrame& f);
    void frameEnd(RenderFrame& f);
    /** @brief Releases allocated runtime resources. */
    void cleanup();

private:
    friend class Haruka::Renderer::MotorInstance;
    
#ifdef HARUKA_NETWORK
    DGS::Client m_dgs;
    /// Las entidades remotas en la escena (crear/mover/caducar): net/entity_sync.h.
    Haruka::Net::EntitySync m_entitySync;
    std::vector<Haruka::SceneObject> m_ghostObjects;

    enum class LoginState { Idle, Show, Connecting, Failed, Done };
    LoginState  m_loginState   = LoginState::Idle;
    std::string m_loginEmail;
    std::string m_loginUsername;
    std::string m_loginError;

    void renderLoginScreen();
#endif

    /** @brief The main application window. */
    std::unique_ptr<Haruka::Core::Window> _window = nullptr;
    /** @brief RHI device (backend gráfico). Se crea tras la Window; declarado después para
     *  destruirse ANTES que la Window (libera el contexto GL mientras la ventana aún existe). */
    std::unique_ptr<Haruka::RHI::Device> _device = nullptr;
    /** @brief The currently active scene. */
    Haruka::SceneManager* _currentScene = nullptr;
    /** @brief The owned scene instance. */
    std::unique_ptr<Haruka::SceneManager> _ownedScene;
    /** @brief The active camera instance. */
    std::unique_ptr<Camera> _camera;
    
    /** @brief The main shader instance. */
    /** @brief Cielo atmosférico de fondo (pase 3 migrado a PSO/Context). Sin VBO ni VAO propios:
     *  el triángulo fullscreen sale de gl_VertexID y el VAO (vacío) lo aporta el pipeline. */
    /// EL CIELO Y EL CLIMA DEL FRAME (renderer/sky_pass.h): lluvia, nieve, viento, mojado, aire.
    Haruka::Renderer::SkyPass   m_sky;
    float                       m_rainOverride = [] {
        const char* e = std::getenv("HARUKA_RAIN");
        return (e && e[0]) ? std::max(-1.0f, std::min(1.0f, (float)std::atof(e))) : -1.0f;
    }();
    /** @brief Lluvia/nieve como GEOMETRÍA en el pase de escena (con depth), no como filtro de pantalla. */
    Haruka::PrecipitationRenderer m_precip;

    /** @brief MÁSCARA DE EXPOSICIÓN AL CIELO: un shadow map con la "luz" en el CÉNIT → dice qué
     *  tienes ENCIMA. El depth de la escena descarta lo que tiene algo DELANTE; esto descarta lo que
     *  está bajo cubierto, que es otra pregunta y necesita su propio pase. */
    /// SOMBRA DEL SOL + MASCARA CENITAL (renderer/shadow_pass.h).
    Haruka::Renderer::ShadowPass m_shadows;
    /** @brief Agua acumulada en el suelo [0,1]. Se INTEGRA (subir rápido, secar lento) → el suelo
     *  sigue mojado tras la lluvia. Es, de hecho, el canal "mojado" de la capa granular. */
    /** @brief Las HUELLAS: la lista (GL-free, compartible con el servidor) y el pase que las pinta
     *  en su ventana cenital para que el terreno las muestree. */
    Haruka::GroundLayer         m_groundLayer;
    Haruka::GroundStampRenderer m_stampRenderer;
    /// Las paredes del campo volumétrico → cuerpos de Jolt. Por frame, sólo lo que cambió de revisión.
    /// Las paredes del campo volumetrico → Jolt, y el anillo del suelo cercano: world/vox/vox_system.h.
    Haruka::World::VoxSystem m_vox;
    void syncVoxColliders() { Haruka::World::VoxSystem::Frame vf; vf.camera = _camera.get(); vf.planets = _planetarySystem.get(); vf.physics = getPhysicsEngineOrNull(); m_vox.syncColliders(vf); }
    std::string m_worldEditsDir;
    /** @brief The lamp shader instance. */
    std::unique_ptr<Shader> _lampShader;
    /** @brief The shadow shader instance. */
    /** @brief The HDR shader instance. */
    std::unique_ptr<HDR> _hdr;
    /** @brief The bloom shader instance. */
    std::unique_ptr<Bloom> _bloom;
    /** @brief The SSAO shader instance. */
    std::unique_ptr<SSAO> _ssao;
    /** @brief Offscreen HDR scene target for the standalone post-processing stack
     *  (render-scale source + bloom/fxaa input). Sized to renderScale*window. */
    /// EL POST-PROCESO (target a escala de render, bloom, composite FXAA+upscale): renderer/post_pass.h.
    Haruka::Renderer::PostPass m_post;
    bool m_postActive = false;             // `m_post.active()` de este frame (lo leen varios pases)
    // Bloom ping-pong targets (own FBOs — the Bloom class isn't ping-pong shaped).
    // === MIGRADO A PSO (primer pase de la ruta Vulkan) ===
    // Los draws van por el Context del RHI (beginRenderPass/bindPipeline/bindVertexBuffer/
    // bindUniformBuffer/bindTexture/draw) en vez de glUseProgram+glBindVertexArray+glDrawArrays.
    // Los uniforms SUELTOS (threshold/horizontal) — que NO existen en Vulkan — viven ahora en el
    // UBO BloomParams (binding 2). Ver assets/shaders/bloom_extract.frag / bloom_blur.frag.
    // (F1) COPIA de la profundidad de la escena, para que el pase de AGUA la muestree y sepa cuánta
    // agua atraviesa el rayo (reversed-Z: dist = near / z). Es una copia y no el propio depth de la
    // escena porque leer una textura ATADA al FBO activo es un feedback loop (comportamiento
    // indefinido). El blit cuesta poco y la GPU está ociosa.
    Haruka::RHI::RenderPassHandle m_sceneDepthCopy;
    int m_depthCopyW = 0, m_depthCopyH = 0;

    /// ⚠️ UN UBO POR DRAW, NO UNO COMPARTIDO. En OpenGL cada draw se ejecuta al vuelo y reescribir
    /// el mismo buffer entre draws funciona; en Vulkan los comandos se GRABAN y todos acaban leyendo
    /// el ULTIMO valor escrito. El bloom hace 1 + 2xN draws con parametros distintos, asi que en
    /// Vulkan el extract se quedaba con `threshold = 0` —la escena ENTERA entraba al bloom— y el
    /// desenfoque horizontal se volvia vertical. Medido: el frame salia **1,48x mas claro** que en GL,
    /// y con el bloom apagado los dos backends daban la MISMA imagen hasta la decima.
    // Present/composite (pase 2 migrado): FXAA + bloom + upscale a pantalla.
    /** @brief Bright-pass + separable blur of a scene color texture. Toma y devuelve HANDLES RHI
     *  (no ids GL): toda la cadena bloom→composite dibuja por el Context (ruta PSO). Handle
     *  inválido = los pipelines no se pudieron crear → componer sin bloom. */
    /** @brief The IBL shader instance. */
    std::unique_ptr<IBL> _ibl;
    /** @brief The point shadow shader instance. */
    std::unique_ptr<PointShadow> _pointShadow;
    /** @brief The GPU instancing instance. */
    /** @brief The cascaded shadow map instance. */
    std::unique_ptr<CascadedShadowMap> _cascadedShadow;
    /** @brief The raycast system instance. */
    std::unique_ptr<RaycastSimple> _raycastSystem;
    /** @brief The world system instance. */
    std::unique_ptr<Haruka::WorldSystem> _worldSystem;
    /** @brief The planetary system instance. */
    std::unique_ptr<Haruka::PlanetarySystem> _planetarySystem;
#ifdef HARUKA_MOD_FLUIDS
    /** @brief Hito 2: host del stack de fluido (ríos/lagos/splash) frente al jugador. */
    std::unique_ptr<Haruka::FluidHost> _fluidHost;
#endif
    /** @brief The physics engine instance. */
#ifdef HARUKA_MOD_PHYSICS
    std::unique_ptr<Haruka::PhysicsEngine>      _physicsEngine;
    std::unique_ptr<Haruka::WorldSystemProvider> _worldProvider; // adaptador mundo→física (vive con el motor)
#endif
    // Editor viewport target — set explicitly by viewport, bypasses MotorInstance singleton split.
    RenderTarget* _editorTarget = nullptr;
    // Editor viewport size (used when no _window exists).
    int m_editorViewportW = 0;
    int m_editorViewportH = 0;
    /** @brief True when running with --headless (no visible window, no ImGui). */
    bool m_headless = false;
    /** @brief True cuando el impl de ImGui GL está activo (solo backend OpenGL). Con Vulkan no hay
     *         impl (fase 7 → ImGui_ImplVulkan) y no se pinta la UI; la escena 3D es RHI y sí corre. */
    bool m_imguiGL = false;
    /** @brief True cuando el impl de ImGui Vulkan está activo (fase 7: ImGui_ImplVulkan). La UI se
     *         graba en el command buffer del frame ANTES de presentar (VKDevice::drawImgui). */
    bool m_imguiVK = false;
    /** @brief Factor físico/lógico del swapchain (DPI). Si > 1, el viewport de ImGui y el atlas de
     *         fuentes se escalan a esa resolución (texto nítido); DisplaySize se mantiene lógico. */
    float m_imguiFbScale = 1.0f;

    // Resets to 0 on each init(); renderFrameContent logs the first 5 frames per init.
    int _diagFramesLeft = 0;

    // Render targets
    
    // Primitives (LOD spheres for celestial bodies)
    std::unique_ptr<SimpleMesh> sphereLOD[4];
    
    // Shaders (cached to avoid recreation every frame)
    std::unique_ptr<Shader> _geomShader;
    std::unique_ptr<Shader> _ssaoShader;
    std::unique_ptr<Shader> _lightShader;
    // (_compositeShader eliminado: el present dibuja por PSO/Context — m_presentPSO.)
    std::unique_ptr<Shader> _flatShader;
    std::unique_ptr<Shader> _cascadeShadowShader;
    // (_bloomExtractShader/_bloomBlurShader eliminados: el bloom ya dibuja por PSO/Context —
    //  el shader vive dentro del PipelineHandle, no en un objeto Shader suelto.)
    // (`_pointShadowShader` y `_instancingShader` vivían aquí: dos unique_ptr que NUNCA se
    //  construían — su único rastro era el `.reset()` del destructor. Sus shaders
    //  (point_shadow.*, instancing.*) se han borrado con ellos; el instancing real de las piezas de
    //  construcción va por GPUInstancing + construction_inst.*.)
    // Pase de ESCENA (objetos) migrado a PSO/Context. Dos variantes de fragment (final/preview)
    // → el pipeline se recrea si cambia useFinalLook.

    // Preview de MATERIAL del editor: siempre el look final, sin depender de los ajustes de render
    // del usuario (ver renderMaterialPreview).
    Haruka::RHI::PipelineHandle m_matPreviewPSO;

    // Pase INSTANCIADO de piezas de construcción: mismo Vertex (binding 0) + stream de instancia
    // (binding 1). Un draw por modelo. Ver application_render.cpp (recolección + dibujo).

    /// LOS PROPS DEL MUNDO (scatter, registro, pase instanciado, sombras, colliders, rotura).
    Haruka::World::PropSystem m_props;
    /// LAS NUBES VOLUMETRICAS (horneado del cielo, marcha reducida, composicion): renderer/cloud_pass.h.
    Haruka::Renderer::CloudPass     m_clouds;
    /// Perspectiva aérea del frame (lib/aerial.glsl): x = 1/L (por metro) · y = día. Sale del clima
    /// en la cámara al montar el cielo y va al terreno, a los props y a las nubes: un solo aire.

    /// TEXTURA DE RELLENO 1x1 blanca para los slots de material que un objeto NO tiene.
    /// ⚠️ No es cosmética: en OpenGL un sampler sin atar lee negro y el guard `hasTex()` del shader
    /// lo hace inofensivo, pero en Vulkan el descriptor queda INDEFINIDO y muestrearlo da basura —
    /// los props salían GRISES en vez de con su color. Se ata algo válido a TODOS los slots que el
    /// shader declara; el shader sigue ignorándolos por la máscara.

    /// PropParams (binding 6), UNO POR PROTOTIPO.


    /// ALAMBRE DE LA MALLA DE COLISION (diagnostico): renderer/collision_wire.h.
    Haruka::Renderer::CollisionWire m_wire;
public:
    /** @brief Activa el alambre de la malla de colisión (F-tecla del juego / editor). */
    void setCollisionWireframe(bool on) { m_wire.setEnabled(on, getPhysicsEngineOrNull()); }
    bool isCollisionWireframe() const { return m_wire.enabled(); }
private:


    // ImGui injection callback (set by editor viewport)
    std::function<void()> _imguiCallback;

    // Optional game interface — used by standalone runtime (not editor)
    Haruka::GameInterface* _gameInterface = nullptr;
    bool m_cleanedUp = false;

    // Screenshot: captured at the end of the 3D pass (before ImGui) for a clean
    // world frame with no HUD. See requestScreenshot() / captureScreenshotIfPending().
    bool m_screenshotPending = false;
    std::string m_screenshotPath;
    void captureScreenshotIfPending(int width, int height);

    float _exposure = 1.0f;

    /** @brief Timing state for frame time and FPS calculation. Updated in renderFrame(). */
    std::chrono::time_point<std::chrono::high_resolution_clock> _frameStart;
    float _lastFrameTimeMs = 0.0f;
    float _lastFps         = 0.0f;
    uint64_t _fpsFrameCount  = 0;
    double   _fpsLastTime    = 0.0;

    /** @brief The time elapsed since the last frame. */
    float deltaTime = 0.0f;

    /** @brief Reloj de los diagnósticos periódicos del render (segundos acumulados). Es ESTADO DE
     *  LA APLICACIÓN, no una estática de función: dos Application (editor + juego, o un test) tienen
     *  cada una su frame y su cadencia de log. */
    double m_diagClock   = 0.0;
    
    /** @brief The vertex array object for the screen quad. */
    Haruka::RHI::BufferHandle m_uboPerFrameH;
    /// LAS LUCES DEL FRAME: renderer/frame_lights.h.
    Haruka::Renderer::FrameLights m_lights;
    /// EL PASE DE ESCENA (objetos + construccion instanciada): renderer/scene_pass.h.
    Haruka::Renderer::ScenePass m_scene;
    /** @brief Sets up the screen quad for post-processing. */

    /** @brief The render quality preset. */
    inline static int s_renderQualityPreset = 2; // 0=Low,1=Medium,2=High,3=Ultra
    inline static bool s_enableHDR = true;
    inline static bool s_enableBloom = true;
    inline static bool s_enableSSAO = true;
    inline static bool s_enableIBL = true;
    inline static bool s_enableShadows = true;

    inline static float s_layerMaxDistance[6] = {
        0.0f,
        1.0e9f,  // layer 1: always
        1200.0f, // layer 2: lowest details
        3500.0f, // layer 3: medium details / clouds
        900.0f,  // layer 4: buildings
        300.0f   // layer 5: small props
    };

    /** @brief Statistics for rendered geometry. */
    // Cached static render queue (rebuilt only when the scene object set changes).
    // Nº de comandos de la parte ESTÁTICA de la cola (todo menos los fantasmas de red, que se
    // añaden y quitan por frame). Sustituye a la copia entera que se hacía cada frame.
    std::size_t m_staticQueueCount = 0;
    // Qué objetos YA están clasificados en la cola, y buffer de los presentes en la escena: permiten
    // reconstruirla de forma INCREMENTAL (clasificar solo lo nuevo) en vez de rehacerla entera.
    std::unordered_set<uint64_t> m_queuedObjects, m_presentObjects;   // por UID, no por puntero
    size_t m_renderQueueObjCount = (size_t)-1;
    bool   m_renderQueueDirty = true;

    int _iRenderedVertices      = 0;
    int _iRenderedTriangles     = 0;
    int _iRenderedDrawCalls     = 0;
    int _iTotalVertices         = 0;
    int _iTotalTriangles        = 0;
    int _iTotalDrawCalls        = 0;

    // Último desglose de geometría del terreno (lo rellena el render tras el pase del planeta).
    Haruka::Planet::TerrestrialPlanet::RenderStats m_terrainStats;
};

}} // namespace Haruka::Core

using Haruka::Core::Application;                 // back-compat alias (migration)
namespace Haruka { using Core::Application; }
#endif