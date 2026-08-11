#ifndef APPLICATION_H
#define APPLICATION_H

#include <SDL3/SDL.h>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <chrono>
#include <algorithm>
#include <unordered_set>

#ifdef HARUKA_NETWORK
    #include "include/dgs/client.h"
#endif

#include "tools/math_types.h"
#include "core/modules.h"      // HARUKA_MOD_* (gating de subsistemas opcionales)
#include "core/world_system.h"
#include "core/world_system_provider.h"  // adaptador IWorldProvider (cliente) para la física
#include "core/window.h"
#include "core/camera.h"
#include "rhi/rhi_device.h"
#include "core/scene/scene_manager.h"
#include "core/scene/scene_render_policy.h"
#include "renderer/shader.h"
#include "renderer/shadow.h"
#include "renderer/hdr.h"
#include "renderer/bloom.h"
#include "game/planet.h"        // TerrestrialPlanet::RenderStats (panel de performance)
#include "tools/profiler.h"
#include "renderer/ssao.h"
#include "renderer/ibl.h"
#include "renderer/point_shadow.h"
#include "renderer/precipitation_renderer.h"  // lluvia/nieve EN EL MUNDO (con depth)
#include "renderer/ground_stamp_renderer.h"   // HUELLAS en la capa granular (nieve/arena)
#include "core/ground_layer.h"                // la capa granular en sí (GL-free)
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
#include "game/instanced_object.h"

namespace Haruka { namespace Renderer { class MotorInstance; } } using Haruka::Renderer::MotorInstance;
namespace Haruka { namespace Renderer { class Model; class RenderTarget; } }
namespace Haruka { class MaterialComponent; }

namespace Haruka { namespace Core {

/** @brief Snapshot de debug de UNA instancia del scatter, para la jerarquía del editor. */
struct PropInstanceDebug {
    uint32_t seed    = 0;          ///< identidad determinista (celda del mundo)
    uint32_t state   = 0;          ///< InstancedObjectState (0=viva, 1=destruida, 2=rebrotando)
    bool     rendered = false;     ///< entró en el draw de este frame (Alive, dentro del tope y sin cull)
    bool     culled   = false;     ///< viva pero sin draw este frame (cull activo)
    uint8_t  cullReason = 0;       ///< 0 = ninguna (se dibujó), 1 = fuera del frustum, 2 = sub-pixel en pantalla
};

/** @brief Snapshot de debug de UN prototipo (GPUInstancing = un draw por prototipo). La jerarquía
 *  del editor lo usa como árbol de debug: instancias que no se renderizan → gris, y el nombre en
 *  ROJO + alerta si el prototipo NO tiene material per-pixel (solo color por vértice). */
struct PropPrototypeDebug {
    std::string name;                          ///< mesh/tipo ("tree", "rock", "house", …)
    bool        hasPerPixel = false;           ///< material con texturas (mask != 0) vs color por vértice
    int         totalInstances   = 0;          ///< instancias en el registro
    int         renderedInstances = 0;         ///< las que entraron en el draw este frame
    std::vector<PropInstanceDebug> instances;  ///< lista por instancia (para el árbol)
};

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

    /** @brief Activa/desactiva el scatter GLOBAL de props del motor (`scene.prop.instanced`).
     *  El juego (Survival) usa SU propio sistema de props (ResourceSystem) y lo desactiva con
     *  `setPropScatterEnabled(false)` para no duplicar árboles ni pagar su scatter de ~61k celdas. */
    void setPropScatterEnabled(bool enabled) { m_propScatterEnabled = enabled; }
    bool isPropScatterEnabled() const { return m_propScatterEnabled; }

    /** @brief Resultado de un golpe contra un prop del scatter. `partId` identifica QUÉ parte se
     *  rompió (0 = tronco → el prop entero cae; >0 = una rama) y `lengthM` es lo que medía ese
     *  segmento en el mundo, que es lo que decide si el golpe deja un palo aprovechable. */
    struct PropHit {
        bool        hit       = false;
        uint32_t    seed      = 0;      ///< identidad determinista de la instancia (celda del mundo)
        std::string prototype;          ///< nombre del prototipo ("tree", "rock", …)
        int         partId    = -1;
        bool        trunk     = false;  ///< true = se ha tumbado el prop entero
        double      lengthM   = 0.0;    ///< longitud del segmento roto (m), ya escalada
        glm::dvec3  pos{0.0};           ///< punto medio de la parte rota, en el mundo
    };
    /** @brief Rompe la parte de prop más cercana al golpe (esfera `center`+`radius`), si la hay.
     *  Sin raycast: la caja de la parte contra la esfera del hachazo, que es lo mismo que usaba el
     *  sistema de cosecha anterior. Marca el estado en el registro (tronco → `Destroyed`, rama →
     *  bit en `breakMask`) y REGENERA los colliders, para que lo que has roto deje de estorbar.
     *  Es PÚBLICA a propósito: el talado lo dispara el JUEGO (el hachazo), no el motor. */
    PropHit breakPropAt(const glm::dvec3& center, double radius);

    /** @brief Lo que le ha pasado a un prop concreto: talado y/o con ramas arrancadas. */
    struct PropStateDelta {
        uint32_t seed      = 0;
        uint32_t state     = 0;    ///< InstancedObjectState
        uint32_t breakMask = 0;    ///< partes rotas
        float    regrow    = 0.0f;
    };
    /** @brief Todo lo que el jugador ha roto, para guardarlo en la partida.
     *
     *  ⚠️ Vive FUERA del registro de instancias a propósito. El scatter regenera las instancias
     *  desde cero y solo conserva las que siguen cerca: si el estado viviera solo ahí, el árbol que
     *  talaste reaparecería en cuanto te alejaras lo bastante para que saliera del radio del
     *  scatter — que es a los pocos cientos de metros. El mapa es por SEMILLA (la celda del mundo),
     *  que es una identidad estable para siempre.
     *
     *  Solo guarda lo ROTO, no todos los props: un mundo entero de árboles intactos no se
     *  serializa, se regenera de la semilla. */
    std::vector<PropStateDelta> serializePropState() const;
    void restorePropState(const std::vector<PropStateDelta>& deltas);

    /** @brief Activa/desactiva el snapshot de DEBUG del scatter (`getPropScatterDebug`). La
     *  jerarquía del editor lo pide cuando muestra el árbol "Props (GPUInstancing)"; apagado por
     *  defecto para no pagar el barrido de instancias por frame en el motor sin editor. */
    void setPropDebugEnabled(bool enabled) { m_propDebugEnabled = enabled; }
    bool isPropDebugEnabled() const { return m_propDebugEnabled; }
    /** @brief Snapshot por prototipo (GPUInstancing) del pase de props del ÚLTIMO frame: cuántas
     *  instancias hay, cuántas se dibujaron, cuál es su estado y si el material es per-pixel. */
    const std::vector<PropPrototypeDebug>& getPropScatterDebug() const { return m_propScatterDebug; }

    /** @brief LA CAPA GRANULAR (nieve/arena/barro): aquí se registran las HUELLAS. Cualquier cosa con
     *  collider puede pisar — el jugador por zancada, una criatura, una rueda; el motor solo necesita
     *  dónde y con qué huella. Ver `GroundLayer` para por qué esto NO pasa por `DeformationField`. */
    Haruka::GroundLayer& getGroundLayer() { return m_groundLayer; }
    /** @brief Espesor de NIEVE acumulada [0,1] aquí y ahora. El juego lo usa para decidir si una
     *  pisada deja marca, cuánto te hundes y cuánto frena. */
    float getSnowAccum() const { return m_snowAccum; }
    /** @brief Agua acumulada en el suelo [0,1] (mojado; baja el agarre). */
    float getGroundWetness() const { return m_groundWetness; }
#ifdef HARUKA_MOD_PHYSICS
    Haruka::PhysicsEngine* getPhysicsEngine() { return _physicsEngine.get(); }
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
    void sendPlayerTransform(uint32_t uuid, const Haruka::WorldPos& pos, const Haruka::Rotation& rot);
    void sendPlayerChat(uint32_t uuid, const std::string& username, const std::string& text);
    std::vector<DGS::ChatMessage> pollPlayerChats();
    bool connectDGS(const std::string& headHost, int headPort,
                    const std::string& email,    const std::string& password,
                    const std::string& apiHost = "", int apiPort = 0);
    bool isNetworkConnected() const;
    int  getGhostCount()      const;
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
    void renderFrame();
    /** @brief Frame rendering body (logic-only path). */
    void renderFrameContent();
    /** @brief Releases allocated runtime resources. */
    void cleanup();

private:
    friend class Haruka::Renderer::MotorInstance;
    
#ifdef HARUKA_NETWORK
    DGS::Client m_dgs;
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
    std::unique_ptr<Shader> _mainShader;
    /** @brief Cielo atmosférico de fondo (pase 3 migrado a PSO/Context). Sin VBO ni VAO propios:
     *  el triángulo fullscreen sale de gl_VertexID y el VAO (vacío) lo aporta el pipeline. */
    Haruka::RHI::PipelineHandle m_skyPSO;
    Haruka::RHI::BufferHandle   m_skyUBO;   // SkyParams (binding 5)
    Haruka::RHI::PipelineHandle m_rainPSO;  // pase de LLUVIA (fullscreen sobre la escena, blend)
    Haruka::RHI::BufferHandle   m_rainUBO;  // RainParams (binding 5)
    float                       m_rainAmount = 0.0f;  // 0..1, lo fija el pase de cielo desde el clima
    float                       m_snowAmount = 0.0f;  // 0..1, ídem (nieve: la misma precipitación, otra forma)
    float                       m_rainOverride = -1.0f; // <0 = auto (clima); >=0 = forzado (consola `rain`)
    float                       m_windSlant = 0.0f;   // inclinación de la lluvia = viento del clima (mismo vector que las nubes)
    glm::vec3                   m_windVec{0.0f};      // viento del clima (m/s, mundo): nubes, lluvia y follaje van con ESTE
    /** @brief Lluvia/nieve como GEOMETRÍA en el pase de escena (con depth), no como filtro de pantalla. */
    Haruka::PrecipitationRenderer m_precip;

    /** @brief MÁSCARA DE EXPOSICIÓN AL CIELO: un shadow map con la "luz" en el CÉNIT → dice qué
     *  tienes ENCIMA. El depth de la escena descarta lo que tiene algo DELANTE; esto descarta lo que
     *  está bajo cubierto, que es otra pregunta y necesita su propio pase. */
    std::unique_ptr<Haruka::Renderer::Shadow> m_skyMask;
    glm::mat4                   m_skySpace{1.0f};     // matriz de la máscara cenital
    bool                        m_skyMaskOn = false;  // se rellenó este frame (solo cuando precipita)
    /** @brief Resolución y alcance de la máscara. ±48 m a 1024² = 9.4 cm/téxel: de sobra para un
     *  alero o una copa (lo que se pregunta es "¿hay algo?", no su silueta exacta). */
    static constexpr unsigned   kSkyMaskRes = 1024u;
    static constexpr double     kSkyMaskExtentM = 48.0;
    /** @brief Agua acumulada en el suelo [0,1]. Se INTEGRA (subir rápido, secar lento) → el suelo
     *  sigue mojado tras la lluvia. Es, de hecho, el canal "mojado" de la capa granular. */
    float                       m_groundWetness = 0.0f;
    /** @brief NIEVE acumulada [0,1]: cuaja nevando y FUNDE con la temperatura. Es una CAPA sobre el
     *  suelo, no un bioma — el material del terreno no cambia porque haga frío. */
    float                       m_snowAccum = 0.0f;
    /** @brief Las HUELLAS: la lista (GL-free, compartible con el servidor) y el pase que las pinta
     *  en su ventana cenital para que el terreno las muestree. */
    Haruka::GroundLayer         m_groundLayer;
    Haruka::GroundStampRenderer m_stampRenderer;
    /** @brief The lamp shader instance. */
    std::unique_ptr<Shader> _lampShader;
    /** @brief The shadow shader instance. */
    std::unique_ptr<Shadow> _shadow;
    /** @brief The HDR shader instance. */
    std::unique_ptr<HDR> _hdr;
    /** @brief The bloom shader instance. */
    std::unique_ptr<Bloom> _bloom;
    /** @brief The SSAO shader instance. */
    std::unique_ptr<SSAO> _ssao;
    /** @brief Offscreen HDR scene target for the standalone post-processing stack
     *  (render-scale source + bloom/fxaa input). Sized to renderScale*window. */
    std::unique_ptr<HDR> _postScene;
    int m_postW = 0, m_postH = 0;          // current _postScene dimensions
    bool m_postActive = false;             // standalone post stack engaged this frame
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

    Haruka::RHI::RenderPassHandle m_bloomPass[2];   // render targets RHI
    Haruka::RHI::TextureHandle    m_bloomTexH[2];   // handles de color (Context::bindTexture)
    Haruka::RHI::PipelineHandle   m_bloomExtractPSO, m_bloomBlurPSO; // horneados 1 vez (shader+layout+estado)
    Haruka::RHI::BufferHandle     m_bloomUBO;       // BloomParams (binding 2)
    int m_bloomW = 0, m_bloomH = 0;
    // Present/composite (pase 2 migrado): FXAA + bloom + upscale a pantalla.
    Haruka::RHI::PipelineHandle   m_presentPSO;
    Haruka::RHI::BufferHandle     m_presentUBO;     // PresentParams (binding 3)
    /** @brief Bright-pass + separable blur of a scene color texture. Toma y devuelve HANDLES RHI
     *  (no ids GL): toda la cadena bloom→composite dibuja por el Context (ruta PSO). Handle
     *  inválido = los pipelines no se pudieron crear → componer sin bloom. */
    Haruka::RHI::TextureHandle renderBloom(Haruka::RHI::TextureHandle srcColorTex);
    /** @brief The IBL shader instance. */
    std::unique_ptr<IBL> _ibl;
    /** @brief The point shadow shader instance. */
    std::unique_ptr<PointShadow> _pointShadow;
    /** @brief The GPU instancing instance. */
    std::unique_ptr<GPUInstancing> _instancing;
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
    std::unique_ptr<RenderTarget> _lightingTarget;
    std::unique_ptr<RenderTarget> _bloomExtractTarget;
    std::unique_ptr<RenderTarget> _bloomPing;
    std::unique_ptr<RenderTarget> _bloomPong;
    
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
    bool _mainShaderUsesFinalLook = false;
    // Pase de ESCENA (objetos) migrado a PSO/Context. Dos variantes de fragment (final/preview)
    // → el pipeline se recrea si cambia useFinalLook.
    Haruka::RHI::PipelineHandle m_scenePSO;
    bool m_scenePSOFinalLook = false;

    // Preview de MATERIAL del editor: siempre el look final, sin depender de los ajustes de render
    // del usuario (ver renderMaterialPreview).
    Haruka::RHI::PipelineHandle m_matPreviewPSO;

    // Pase INSTANCIADO de piezas de construcción: mismo Vertex (binding 0) + stream de instancia
    // (binding 1). Un draw por modelo. Ver application_render.cpp (recolección + dibujo).
    Haruka::RHI::PipelineHandle m_constInstPSO;

    // Pase INSTANCIADO de PROPS del mundo (árboles/rocas/objetos). Prototipo compartido por tipo
    // (malla con color de vértice + material del node graph) + stream de instancia (binding 1).
    // El REGISTRO es de la Application (decisión de arquitectura): el scatter lo rellena, este
    // pase lo lee por frame. Ver application_render.cpp (pase "scene.prop.instanced").
    Haruka::InstancedObjectRegistry m_propRegistry;
    Haruka::RHI::PipelineHandle     m_propInstPSO;
    /// Solo profundidad, instanciado: mete los props del motor en el mapa de sombras. Sin esto el
    /// pase de sombras solo contenía lo que dibuja el hook del juego y ningún árbol proyectaba.
    Haruka::RHI::PipelineHandle     m_propShadowPSO;
    Haruka::RHI::BufferHandle       m_propShadowUBO;   ///< la matriz de luz del pase
    // Pase VOLUMÉTRICO de nubes. El cúmulo bajo dejó de pintarse en el shader de CIELO (fondo, sin
    // profundidad, antes que la escena) y pasó a ser un medio que se RECORRE, dibujado después de
    // toda la geometría. Ese cambio es lo que hace posible atravesar una nube: un fondo no tiene
    // interior, así que la única alternativa habría sido fingirlo con un efecto de pantalla.
    Haruka::RHI::PipelineHandle     m_cloudPSO;
    Haruka::RHI::BufferHandle       m_cloudUBO;
    /// Copia del depth de la escena: no se puede samplear la profundidad del MISMO target al que se
    /// dibuja (realimentación). Mismo patrón que usa el fluido para que sus partículas se ocluyan.
    Haruka::RHI::RenderPassHandle   m_cloudDepthRT;
    int  m_cloudDepthW = 0, m_cloudDepthH = 0;
    bool m_volumetricClouds = true;   ///< off = solo el cielo de fondo (nubes planas, no atravesables)

    Haruka::RHI::BufferHandle       m_propParamsUBO;   // PropParams (binding 6) del pase de props
    Haruka::RHI::BufferHandle       m_constParamsUBO;  // ConstParams (binding 6) del pase de construcción
    /// Estado PERSISTENTE de los props rotos, por semilla de celda. Ver `serializePropState` para
    /// por qué no puede vivir dentro de `m_propRegistry`. Crece solo con lo que el jugador rompe.
    std::unordered_map<uint32_t, PropStateDelta> m_propState;
    bool m_propScatterEnabled = true;                  // off = el juego gestiona sus props (Survival)
    bool m_propDebugEnabled   = false;                 // snapshot de debug para la jerarquía del editor
    std::vector<PropPrototypeDebug> m_propScatterDebug; // por frame, cuando m_propDebugEnabled
    // Cache GPU de prototipos: por nombre de prototipo → malla (VBO/EBO) + material PER-PIXEL
    // (albedo/normal/metallic/roughness/ao horneados con el node material graph y subidos a GPU).
    // Sin material (mask == 0) → solo color por vértice (el caso del editor de debug "⚠ sin per-pixel").
    struct PropPrototypeGpu {
        /// NIVELES DE DETALLE de la malla. Existen porque, medido con RenderDoc, los árboles del
        /// scatter son el **85,8 % de los triángulos del frame** (6320 instancias × 512 = 3,24 M) y el
        /// instancing no lo arregla: instanciar ahorra draw calls, no trabajo de vértices — la GPU
        /// ejecuta el vertex shader por instancia, vaya en un comando o en 6320.
        ///
        /// El material (texturas) es COMPARTIDO por los niveles: es el mismo prototipo, solo cambia la
        /// densidad de la malla. Cada nivel es un draw instanciado propio, así que 4 prototipos × 3
        /// niveles son 12 draws como máximo — sigue siendo nada al lado de los 3,24 M de triángulos.
        static constexpr int kLods = 3;
        Haruka::RHI::BufferHandle vbo[kLods] = {}, ebo[kLods] = {};
        uint32_t vertexCount[kLods] = {0, 0, 0};
        uint32_t indexCount[kLods]  = {0, 0, 0};
        Haruka::RHI::TextureHandle albedo = {}, normal = {}, metallic = {}, roughness = {}, ao = {};
        float   metallicS   = 0.0f;     ///< escalar fallback (si el bit no está en la máscara)
        float   roughnessS  = 0.5f;
        float   aoS         = 1.0f;
        uint32_t mask       = 0u;       ///< bits de texturas presentes (mismo esquema que prop_inst.frag)
    };
    std::unordered_map<std::string, PropPrototypeGpu> m_propProtoMesh;

    // Scatter GLOBAL de props (Todo 5): rellena `m_propRegistry` desde el campo del planeta activo.
    // El refresh es por demanda: solo se re-enumera cuando la cámara cruza `kPropScatterCellM`/2
    // desde la última posición — el scatter determinista por CELDA MUNDIAL no necesita correr cada
    // frame (las instancias se mantienen estables hasta que el jugador se mueve un tramo).
    void refreshPropScatter();
    /** @brief Vuelca los props del scatter cercanos al jugador como cuerpos estáticos de la física.
     *  Los colliders salen del ESQUELETO del prop (tronco + ramas por separado, ver
     *  `prop_collider.h`), no de una caja envolvente: así se camina bajo las ramas y cada parte
     *  tiene identidad para poder romperla. La llama `refreshPropScatter`, que es quien cambia el
     *  conjunto de instancias. */
    void refreshPropColliders(const glm::dvec3& planetC, double planetR, const glm::dvec3& camPos);
    /// Dibuja los props del motor en el mapa de sombras (culling por la caja de la LUZ, no por la
    /// cámara: el volumen está centrado en ella e incluye lo que queda detrás).
    void renderPropShadows(Haruka::RHI::Context* ctx, const glm::mat4& lightSpace);

    /**
     * @brief ALAMBRE DE LA MALLA DE COLISIÓN encima del terreno dibujado.
     *
     * La disparidad del terreno se veía a ojo y no la detectaba ninguna medida: la sonda de paridad da
     * 2 cm, los pies quedan a centímetros del suelo, y el clipmap dibuja con quads de 4 m — la misma
     * retícula que colisiona. Con todo coherente y el problema visible, la salida es dibujar la malla
     * que la física TIENE de verdad sobre lo que el render pinta: si el alambre flota o se hunde, ya no
     * es una impresión.
     */
    void renderCollisionWireframe(Haruka::RHI::Context* ctx, const glm::mat4& viewProjRotOnly);
public:
    /** @brief Activa el alambre de la malla de colisión (F-tecla del juego / editor). */
    void setCollisionWireframe(bool on);
    bool isCollisionWireframe() const { return m_collisionWireOn; }
private:
    bool m_collisionWireOn = false;
    Haruka::RHI::PipelineHandle m_dbgLinePSO{};
    Haruka::RHI::BufferHandle   m_dbgLineVB{}, m_dbgLineUBO{};
    uint32_t                    m_dbgLineVerts = 0;
    uint64_t                    m_dbgLineRev   = ~0ull;

    // EL SUELO CERCANO DIBUJADO DESDE LA COLISIÓN. Dentro de ±192 m el suelo que se dibuja son los
    // MISMOS vértices y los MISMOS triángulos que Jolt colisiona, con el material del terreno de
    // siempre; el clipmap deja el hueco. ENCENDIDO por defecto (`HARUKA_NEAR_RING=0` lo apaga).
    // Los buffers de GPU los posee `TerrestrialPlanet` (los dibuja con su propio material); aquí
    // solo se recuerda QUÉ revisión se le entregó y desde qué ancla, para no re-subir por frame.
    uint64_t                    m_nearRingRev     = ~0ull;
    glm::dvec3                  m_nearRingAnchor{0.0};
    void updateNearGroundRing();
    glm::dvec3 m_propScatterLastCam{1e300, 1e300, 1e300};   // última posición muestreada
    std::string m_propScatterPlanet;                        // planeta del último scatter (reset al cambiar)

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
    double m_lastStarLog = -1e9;
    
    /** @brief The vertex array object for the screen quad. */
    Haruka::RHI::BufferHandle m_quadBuf, m_uboPerFrameH, m_uboPerObjectH;
    /** @brief Sets up the screen quad for post-processing. */
    void setupQuad();

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