#pragma once

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <nlohmann/json.hpp>
#include "core/modules.h"
#include "tools/math_types.h"
#include "core/scene/scene_manager.h"
#include "core/terrain/terrain_sample.h"     // WorldGenParams (terreno)
#include "tools/planetary_types.h"           // PlanetChunkKey (descarga diferida)
#include "core/lod_system.h"                  // LODUpdate (cache del último set)
#include "rhi/rhi_types.h"                    // TextureHandle (shadow map del pase de terreno)
#include "renderer/water_renderer.h"          // WaterRenderer::PassParams (params del pase de agua)

namespace Haruka { namespace Renderer { class Texture; } } using Haruka::Renderer::Texture;

namespace Haruka {

class ChunkCache;
class TerrainGenerator;
class TerrainRenderer;
class FloatingIslandRenderer;
class TerrainStreamingSystem;
class LODSystem;
class DeformationField;

class PlanetarySystem {
public:
    PlanetarySystem();
    ~PlanetarySystem();

    // Estructura limpia para un planeta
    struct Planet {
        std::string name;
        Haruka::WorldPos position;
        double radius;
        nlohmann::json terrainSettings; // Semilla, capas de ruido, etc.
        bool isHome = false;            // planeta del jugador (flags.originShiftingTarget)

        // --- ÓRBITA (Kepler ANALÍTICO): posición = función del tiempo → ESTABLE (sin deriva de
        // integración) y depurable. orbitParent = índice del cuerpo central (Sol) en m_planets, o -1
        // (estático). Plano orbital = base ortonormal (u,v); foco en el cuerpo padre. ---
        int    orbitParent = -1;          // índice del cuerpo central en m_planets (-1 = estático)
        double orbitA      = 0.0;         // semi-eje mayor (m)
        double orbitEcc    = 0.0;         // excentricidad [0,1) (0 = círculo)
        double orbitPeriod = 0.0;         // periodo (s); <=0 = no orbita
        double orbitPhase  = 0.0;         // anomalía media en t=0 (rad)
        glm::dvec3 orbitU  = glm::dvec3(1, 0, 0); // eje del periastro (plano orbital)
        glm::dvec3 orbitV  = glm::dvec3(0, 0, 1); // eje perpendicular en el plano (sentido del avance)
    };

    void init();

    /** @brief Actualiza órbitas, LOD y streaming. */
    void update(double dt, const glm::dvec3& cameraPos);

    void addPlanet(const Planet& planet);

    /** @brief Fija/activa la órbita (Kepler) de un planeta alrededor de otro, en RUNTIME (consola o
     *  escena). a/u/v/fase se derivan de la posición ACTUAL (arranca ahí, en el periastro). period<=0
     *  o parentName vacío/no-hallado = DETIENE la órbita (queda estático). Devuelve false si no existe. */
    bool setPlanetOrbit(const std::string& planetName, const std::string& parentName,
                        double periodSeconds, double ecc = 0.0);

    /** @brief Renderiza el terrain de un planeta (shader ya activo, UBO model ya subido). */
    void renderPlanetTerrain(const std::string& planetName, const glm::dvec3& cameraPos);

    /** @brief Renderiza el océano de un planeta (shader de agua ya activo). */
    void renderPlanetWater(const std::string& planetName, const glm::dvec3& cameraPos);

    /** @brief Renderiza las islas flotantes (reusa el shader de planeta, ya activo). */
    void renderPlanetIslands(const std::string& planetName, const glm::dvec3& cameraPos);

    /** @brief Datos del planeta v2 activo (el que tiene props/recursos) para que el JUEGO
     *  genere su sistema de recursos. center/radius/seed/relief. false si no hay. Los
     *  recursos (árboles/rocas/minerales) son del juego, no del motor. */
    bool getActivePlanet(glm::dvec3& center, double& radius, uint32_t& seed, float& reliefStrength) const;
    /** @brief Nombre del planeta ACTIVO (el hogar; ver getActivePlanet). Vacío si no hay. */
    std::string getActivePlanetName() const;

    /**
     * @brief (F10) Altura DE LA MALLA que se está dibujando, en `dir` (planet-local, unitaria).
     *
     * ⚠️ Esta es LA fuente de verdad de la altura para todo lo que toca el suelo (colisión, props,
     * colocación). Antes cada consumidor RE-DERIVABA la altura con la fórmula analítica de CPU — y
     * esa fórmula y la malla NO siempre coinciden (la malla solo aplica el meso en chunks finos; un
     * chunk grueso, con vértices cada 200 m, no puede representar relieve de 40 m). Resultado: props
     * flotando hasta 60 m sobre el suelo que ves. Preguntarle a la malla elimina la discrepancia por
     * construcción: la altura que devuelve es, literalmente, la que se dibuja.
     *
     * Busca de FINO a GRUESO el primer chunk RESIDENTE que cubre `dir` (que es, por construcción, el
     * que el renderer dibuja ahí) y lo interpola bilinealmente.
     *
     * @return false si no hay ningún chunk residente que cubra el punto (lejos del jugador: spawn,
     *         IA, consultas planetarias). Ahí NO hay malla con la que chocar → el llamador usa el
     *         sampler analítico, que sigue siendo válido para eso.
     */
    bool meshHeightKmAt(const std::string& planetName, const glm::vec3& dir, float& outElevKm) const;

    /** @brief Sets camera-relative VP for frustum culling terrain+water chunks. */
    void setTerrainCullMatrix(const glm::mat4& camRelViewProj);

    /** @brief Parámetros del pase de agua (oleaje + costa per-píxel) → UBO WaterParams. */
    void setWaterPassParams(const WaterRenderer::PassParams& p);

    /** @brief Cota del AGUA (m sobre el radio del planeta) en un punto, o `kNoWater` si aquí NO hay
     *  agua. (F3) Sale del campo hidrológico: océano donde el terreno PROCEDURAL está bajo el mar, y
     *  lago donde la cuenca no drena. **Un agujero que cavas tierra adentro NO tiene agua**: no está
     *  conectado al mar. Antes la física preguntaba "¿estoy bajo el nivel del mar?" y cualquier hoyo
     *  profundo se convertía en océano (bug del "mar bajo tierra"). */
    static constexpr double kNoWater = -1e30;
    double sampleWaterLevel(const glm::dvec3& worldPos) const;

    /** @brief Niebla atmosférica del terreno on/off (consola: `fog`). */
    void setTerrainFog(bool on);
    /** @brief Shadow map del sol + matriz de luz para el terreno (vacío/off = sin sombras). */
    void setTerrainShadow(Haruka::RHI::TextureHandle map, const glm::mat4& lightSpace, bool on);

    int getGPUWaterChunkCount() const;

    const std::vector<Planet>& getPlanets() const { return m_planets; }
          std::vector<Planet>& getPlanets()       { return m_planets; }

    void syncFromScene(const SceneManager& scene);

    /** @brief Construye TODOS los cuerpos celestes desde la escena: planetas (los
     *  que tienen terrainSettings → terreno por chunks/LOD) y cuerpos con geometría
     *  PROCEDURAL (p.ej. la Luna: esfera + cráteres, vía properties.proceduralMesh).
     *  Punto de entrada único — toda la interpretación escena→cuerpos vive aquí, no
     *  en Application. Requiere contexto GL (genera mallas). */
    void buildFromScene(SceneManager& scene);

    int getGPUChunkCount()    const;
    int getPendingChunks()    const;  // chunks generating async (in-flight, few)
    int getQueuedChunks()     const;  // chunks still queued to generate (the real backlog)
    int getCachedChunks()     const;
    int getCacheMemoryMB()    const;
    int getCacheMaxMemoryMB() const;
    /** @brief Sets the chunk cache memory budget (MB). Evicts immediately if over. */
    void setCacheMaxMemoryMB(int mb);
    /** @brief Tunes terrain LOD detail (lower = fewer chunks = cheaper). */
    void setLODParams(double splitFactor, int maxLOD);

    // Batching de terreno (pool + glDrawElementsBaseVertex): reduce el coste CPU de
    // terrain.draw (rebind de VAO por chunk). Conmutable en runtime (consola: terrainpool).
    void   setTerrainBatching(bool on);
    bool   getTerrainBatching() const;

    // Superficie única de océano (test, `oceanshell`) en vez de mallas de agua por-chunk.
    void   setWaterSingleOcean(bool on);
    bool   getWaterSingleOcean() const;
    // F5.1: callback que el juego provee para que el océano único siga a la sim de fluidos cerca del jugador.
    void   setWaterFluidSampler(std::function<bool(const glm::dvec3&, float&, float&)> f);

    // LOD v3 F1: split por error en pantalla (conmutable). Ver docs/guides/PLAN_LOD_V3.md.
    void   setLODScreenSpace(bool on);
    bool   getLODScreenSpace() const;
    void   setLODScreenK(double k);   // px por (mundo/dist=1); desde la cámara, por frame
    void   setLODTargetPx(double px); // subdivide si el chunk proyecta > px
    double getLODTargetPx() const;

    /** @brief LOD ADAPTATIVO por presupuesto de frame. El targetPx se ajusta solo: AFINA
     *  (mejor calidad) mientras el frame va sobrado de tiempo y solo ENGORDA (menos detalle)
     *  si el frame se pasa del presupuesto. Así se da la mejor calidad que el hardware sostiene
     *  y el LOD solo degrada bajo carga. budgetMs = presupuesto de frame del preset (1000/fpsObjetivo);
     *  [minPx,maxPx] = cota de calidad (min = más fino/mejor, max = más grueso/seguro). budgetMs<=0 desactiva. */
    void   setLODBudget(double budgetMs, double minPx, double maxPx);
    void   setAdaptiveLOD(bool on) { m_adaptiveLOD = on; }
    bool   getAdaptiveLOD() const  { return m_adaptiveLOD; }

    /** @brief Ejecuta el recompute del LOD (updatePlanetLOD ~ el pico de CPU del frame:
     *  recursiveProcess + balanceo 2:1) en un HILO WORKER en vez de en el hilo de render.
     *  El worker es puro CPU (el LOD no hace llamadas GL) y trabaja sobre una instancia
     *  propia de LODSystem + snapshots (residencia, cámara, config) → CERO estado compartido,
     *  sin locks en el hot path. El main consume el resultado (doble-buffer) 1–2 recomputes
     *  después y hace TODO el trabajo GL (streaming/subidas/agua/unload) como siempre. Solapa
     *  el cálculo con la GPU ociosa. Conmutable (consola: `lodasync 0|1`); OFF = recompute
     *  inline (idéntico al comportamiento previo). Validar en el juego: pop-in/parpadeo del
     *  terreno = síntoma de un problema en el async → volver a OFF. */
    void setAsyncLOD(bool on);
    bool getAsyncLOD() const { return m_asyncLOD.load(std::memory_order_relaxed); }

    struct TerrainDrawStats { int draws = 0; int vertices = 0; int triangles = 0; };
    TerrainDrawStats getTerrainDrawStats() const;

    // Telemetría de los pools de geometría del terreno (blindaje del fix "buffer fijo"). Para el
    // banco de pruebas: asertar VRAM acotada y growCount==0. Espejo de TerrainRenderer::PoolStats.
    struct TerrainPoolStats {
        int      poolCount = 0;
        size_t   totalSlots = 0, usedSlots = 0, vramBytes = 0;
        uint32_t maxPoolCap = 0;
        uint64_t growCount = 0;
    };
    TerrainPoolStats getTerrainPoolStats() const;

    /** @brief Valida los invariantes del LOD este frame (cobertura sin huecos/solapes,
     *  balance 2:1) y devuelve un resumen legible. Para el comando de consola `lodcheck`. */
    std::string validateLOD() const;
    /** @brief Modo vigilancia: si está activo, update() valida cada frame y avisa por
     *  stderr SOLO cuando el LOD es inválido (throttled). Para `lodcheck watch`. */
    void setLODValidateWatch(bool on) { m_lodWatch = on; }
    bool getLODValidateWatch() const { return m_lodWatch; }

    /** @brief DIAGNÓSTICO/bench: cronometra buildDrawSet (el cuello CPU #1 del frame) `iters`
     *  veces sobre el estado actual y devuelve el mediano de ns/llamada. `outDrawn` recibe el
     *  tamaño del draw-set (chunks dibujados). Solo para el banco de pruebas. */
    double benchDrawSet(int iters, int* outDrawn) const;

    /** @brief DIAGNÓSTICO/test: compara el buildDrawSet OPTIMIZADO con el de REFERENCIA sobre el
     *  estado actual. Devuelve true si producen el MISMO conjunto (garantía de equivalencia). Los
     *  medianos ns/llamada de cada uno salen por outNsOpt/outNsRef si != null. */
    bool checkDrawSetMatchesReference(int iters, double* outNsOpt, double* outNsRef, int* outDrawn) const;

    /** @brief DIAGNÓSTICO/test del gating de invalidación: ¿el draw-set CACHEADO que se dibuja
     *  coincide con un rebuild fresco? Delata si noteResidencyChange se saltó un bump necesario
     *  (cache rancio → huecos). Llamar en punto quiescente. */
    bool checkCachedDrawSetFresh() const;

    /** @brief DIAGNÓSTICO del gating: fracción de cambios de residencia que SÍ invalidaron el
     *  draw-set (estaban en la clausura). Menos = más reconstrucciones evitadas al moverse. */
    void residencyGatingStats(uint64_t& changes, uint64_t& bumps) const;
    // Diagnóstico del delta-splice del draw-set: frames aplicados por splice vs rebuild completo.
    void drawSetDeltaStats(uint64_t& applied, uint64_t& full) const;
    // Micro-bench: coste de un rebuild COMPLETO vs un splice de 1 nodo sobre el draw-set actual.
    void benchDrawSetSplice(int iters, double* nsFull, double* nsSplice, size_t* drawn) const;

    /**
     * @brief Applies a terrain edit (dig/crater/build) at a world position and
     *        regenerates the affected chunks so the mesh updates immediately.
     * @param worldPos centre of the edit
     * @param radius   metres of influence
     * @param strength metres of displacement (positive); dig=true subtracts.
     */
    void editTerrain(const glm::dvec3& worldPos, double radius, double strength, bool dig);

    /**
     * @brief Nivela (aplana) el terreno hacia una altura objetivo en un radio. El centro
     *        queda exactamente a targetHeightM y los bordes mezclan suavemente.
     * @param targetHeightM elevación destino en metros sobre la esfera de referencia.
     */
    void levelTerrain(const glm::dvec3& worldPos, double radius, double targetHeightM);

    /**
     * @brief Nivela con la HUELLA (caja orientada) de un objeto en vez de un círculo:
     *        el área aplanada tiene el tamaño/forma/orientación del objeto colocado.
     * @param halfExtents medias extensiones de la huella (m), en el espacio de rot.
     * @param rot         orientación de la caja (columnas = ejes locales del objeto).
     * @param band        ancho de transición del borde (m).
     */
    void levelTerrainBox(const glm::dvec3& center, const glm::dvec3& halfExtents,
                         const glm::dmat3& rot, double targetHeightM, double band);

    /** @brief The terrain-edit field (null until init, or always null if the
     *  DEFORM module is compiled out). For save/restore of edits. */
    DeformationField* deformationField() {
#ifdef HARUKA_MOD_DEFORM
        return m_deform.get();
#else
        return nullptr;
#endif
    }

    /** @brief Drops all GPU terrain chunks so they regenerate (e.g. after bulk
     *  restoring deformation brushes from a save). */
    void invalidateAllChunks();

    /**
     * @brief Returns the terrain height (in metres) above the reference sphere
     *        surface at the given world position, for the nearest planet.
     *        Returns 0 if no planet is found or terrain settings are missing.
     */
    double sampleTerrainHeight(const glm::dvec3& worldPos) const;

    /** @brief (F3) Muestreo CANÓNICO de superficie en una posición de mundo: devuelve
     *  el TerrainSample completo (elev, agua, wetness, clima…) del planeta más cercano,
     *  con deform aplicado. Colisión, puntería, agua y siembra deben usar ESTO. */
    Haruka::TerrainSample sampleSurface(const glm::dvec3& worldPos) const;

    /** @brief (F3) Params EXACTOS del generador para el planeta ACTIVO (home): seed +
     *  reliefStrength + profile + radio. Fuente única de params → el juego no re-deriva
     *  los suyos (evita divergencias, p.ej. el profile). Devuelve false si no hay planeta. */
    bool getActivePlanetParams(Haruka::WorldGenParams& out, double& outRadius) const;

    /**
     * @brief Mean sea surface for the nearest planet (sea level = planet radius).
     * @return true if a planet was found.
     */
    bool getSeaSurface(const glm::dvec3& worldPos, glm::dvec3& outCenter, double& outSeaRadius) const;

private:
    // Invalida/regenera los chunks que toca una edición de terreno (centro + radio).
    void invalidateEditedChunks(const glm::dvec3& center, double radius);

    // Componentes del motor de terreno (Los "músculos")
    std::unique_ptr<ChunkCache> m_cache;
    std::unique_ptr<TerrainGenerator> m_generator;
    std::unique_ptr<TerrainRenderer> m_renderer;
    std::unique_ptr<WaterRenderer> m_waterRenderer;
    std::unique_ptr<FloatingIslandRenderer> m_islandRenderer;
    bool       m_islandsGenerated = false;
    glm::dvec3 m_islandGenCamDir{0.0}; // dirección cámara en la última (re)generación de islas

    // (Los recursos —árboles/rocas/minerales/cosecha— se movieron al juego: Survival
    //  ResourceSystem. El motor solo expone el planeta activo vía getActivePlanet.)

    // Texturas de bioma del terreno (OPCIONALES, declaradas en la escena
    // terrainSettings.textures). Sin config → terreno procedural. El tier de
    // resolución se elige por TextureQuality. Cargadas/bindeadas aquí (no en el engine core).
    std::unique_ptr<Texture> m_texSandAlbedo,  m_texSandNormal;
    std::unique_ptr<Texture> m_texGrassAlbedo;
    std::unique_ptr<Texture> m_texLandAlbedo,  m_texLandNormal;
    int         m_texTier = -1;        // tier cargado (px); -1 = aún no
    std::string m_texDir;              // dir base de la última carga
    void bindTerrainTextures(const Planet& planet); // carga diferida + bind + uniforms
    std::unique_ptr<TerrainStreamingSystem> m_streaming;
    std::unique_ptr<LODSystem> m_lod;

    // --- LOD ASÍNCRONO (offload del recompute al worker; ver setAsyncLOD) ------------------
    // El worker corre updatePlanetLOD sobre m_lodAsync, una instancia SEPARADA de LODSystem
    // que SOLO el worker toca → el estado interno del LOD (m_lastFrameChunks, config) nunca se
    // comparte con el main. El main pasa un job-snapshot (cámara + residencia + config) bajo
    // m_lodMx y recoge el resultado igual. m_lod (main) solo se usa como fuente de config/
    // getters; en modo async NO recomputa.
    std::unique_ptr<LODSystem>  m_lodAsync;             // instancia exclusiva del worker
    std::thread                 m_lodThread;
    std::mutex                  m_lodMx;                // protege job/result/flags de abajo
    std::condition_variable     m_lodCv;               // el worker duerme aquí hasta tener job
    // ✅ ON POR DEFECTO (2026-07-14). El recompute del LOD (5-8 ms: el PICO de CPU del frame) va a un
    // WORKER, solapado con la GPU —que está OCIOSA—. Suite completa en verde con async ON (57/57).
    //
    // ⚠️ EL BUG QUE LO TENÍA APAGADO (y que despistó): el hilo worker lo arrancaba `setAsyncLOD(true)`,
    // que empieza con `if (on == m_asyncLOD) return;`. Con el flag ya en `true` por defecto, esa
    // llamada NO HACÍA NADA → el worker nunca arrancaba → el main esperaba resultados que nadie
    // producía → CERO chunks a GPU. Por eso `init()` arranca el worker explícitamente.
    // (Los "huecos" que se le achacaron al async eran en realidad de la caché de disco sirviendo
    //  chunks de una generación anterior. Ver ChunkCache::kGenVersion.)
    //
    // Fallback: consola `lodasync off` → recompute inline (comportamiento previo, idéntico).
    std::atomic<bool>           m_asyncLOD{true};       // ON = recompute en el worker

    // Cola de chunks de AGUA pendientes de subir, por planeta. Se rellena al cambiar las hojas y se
    // drena con presupuesto → el catch-up cuesta O(lo que sube), no O(todos los chunks del mundo)
    // (era 5.5 ms ×3 por barrer `chunksToKeep` entero cada 4 frames).
    std::vector<std::deque<PlanetChunkKey>> m_waterBacklog;
    bool                        m_lodStop     = false;  // señal de cierre del worker
    bool                        m_lodJobReady = false;  // hay un job esperando al worker
    bool                        m_lodBusy     = false;  // el worker está recomputando (no re-encolar)
    bool                        m_lodResultReady = false; // el worker publicó resultado sin consumir

    // Snapshot de un planeta para el recompute en el worker (todo lo que updatePlanetLOD lee).
    struct LODAsyncPlanet {
        size_t     index = 0;
        std::string name;
        glm::dvec3 position{0.0};
        double     radius = 1.0;
        glm::dvec3 camPos{0.0};
        uint16_t   bodyId = 0;
        std::function<double(const glm::dvec3&)> coastFn; // bias de costa de ESTE planeta (o vacía)
        double     coastRefine = 0.4;
    };
    // Job = un lote de planetas + la config/residencia comunes de este recompute.
    struct LODAsyncJob {
        std::vector<LODAsyncPlanet> planets;
        std::unordered_set<uint64_t> resident;          // snapshot de residencia (lock-free en el worker)
        // config del LOD (espejo de m_lod → aplicada a m_lodAsync antes de recomputar)
        double splitFactor = 1.0, targetPx = 320.0, screenK = 935.0;
        int    maxLOD = 20, minLOD = 4;
        bool   screenSpace = true;
        glm::mat4 cullVP{1.0f}; bool hasCull = false; // el bias de costa va PER-PLANET (LODAsyncPlanet)
        std::vector<PlanetChunkKey> forgetKeys;           // forgetChunk acumulados
    };
    LODAsyncJob                 m_lodJob;                 // job en curso (protegido por m_lodMx)
    std::vector<std::pair<size_t, LODUpdate>> m_lodResults; // salida del worker (protegida por m_lodMx)
    std::vector<PlanetChunkKey> m_pendingForget;          // forgetChunk acumulados para el próximo job
    // Config "actual" que el main mantiene para snapshotear al job (sin getters en LODSystem).
    double                      m_curScreenK   = 935.0;
    std::function<double(const glm::dvec3&)> m_curCoastFn; // última coastFn (o vacía)
    double                      m_curCoastRefine = 0.4;

    void lodWorkerLoop();                                  // bucle del hilo worker
    void startLODWorker();                                 // arranca el hilo (idempotente)
    void stopLODWorker();                                  // señala cierre y join (idempotente)
    // Último set deseado por planeta. Cuando el LOD está en THROTTLE (cámara quieta)
    // NO recalculamos, pero SÍ reprocesamos este último set para SUBIR a GPU lo que
    // se haya generado mientras tanto (si no, los chunks no aparecen hasta moverte).
    std::vector<LODUpdate> m_lastUpdates;
    // Ventana de "catch-up": solo subimos lo recién generado mientras haya generación
    // activa (+1 s de margen). Cuando todo está cargado y la cámara quieta, el
    // catch-up se apaga → sin el pico periódico de subida.
    int m_catchupGrace = 0;
    // Techo de caché resuelto al arrancar (AUTO o manual). El vigilante de memoria baja el techo real
    // cuando el SISTEMA se queda sin RAM y lo devuelve hasta AQUÍ (nunca más) cuando hay holgura.
    size_t m_cacheBudgetMB = 0;
#ifdef HARUKA_MOD_DEFORM
    std::unique_ptr<DeformationField> m_deform; // player terrain edits
#endif

    // Datos del universo
    std::vector<Planet> m_planets;
    double m_simulationTime = 0.0;
    const double G = 6.67430e-11;

    // LOD throttle: skip the (allocating) quadtree rebuild when the camera has
    // barely moved since the last recompute. Per-planet last cam pos; force on
    // first frame / new planet.
    std::vector<glm::dvec3> m_lastLODCamPos;
    std::vector<int>        m_lastLODFrame; // frame del último recompute por planeta (throttle temporal)
    glm::mat4 m_curCullVP{1.0f};            // cull VP del frame actual (para detectar giro de cámara)
    glm::mat4 m_lastRecomputeCullVP{0.0f};  // cull VP con que se hizo el último recompute (giro → recompute)
    bool m_forceLOD = true;
    bool m_lodWatch = false; // valida el LOD cada frame y avisa al volverse inválido

    // LOD ADAPTATIVO por presupuesto de frame (ver setLODBudget).
    bool   m_adaptiveLOD   = true;
    double m_lodBudgetMs   = 0.0;    // 0 = desactivado (targetPx fijo)
    double m_lodMinPx      = 200.0;  // cota fina (mejor calidad)
    double m_lodMaxPx      = 900.0;  // cota gruesa (seguro bajo carga)
    double m_lodEwmaMs     = 0.0;    // tiempo de frame suavizado (EWMA)
    double m_lodAdjustAccum = 0.0;   // temporizador entre ajustes

    // Predicción de movimiento: velocidad suavizada de la cámara para PEDIR chunks
    // por delante del jugador (lookahead) → menos pop-in al moverse/volar rápido. El
    // render usa la cámara REAL; solo el LOD/streaming mira el punto adelantado.
    glm::dvec3 m_prevCamPos{0.0};
    glm::dvec3 m_camVel{0.0};
    bool       m_havePrevCam = false;

    void updateOrbits(double dt);
};

}