#pragma once
/**
 * @file renderer/grass_renderer.h
 * @brief HIERBA DENSA GENERADA EN LA GPU, apoyada en el suelo que se dibuja, que se APLASTA al paso.
 *
 * ── LO QUE HACE, EN TRES PASES ──────────────────────────────────────────────────────────────────
 *
 *  1. `prepare()` (fuera del render pass, en la fase de compute del frame):
 *       a. EL MAPA DE PRESION (`grass_press.frag`): 512² texeles de 25 cm anclados al mundo, que
 *          siguen a la camara a saltos de texel entero. Cada frame decae (la hierba se levanta en
 *          `recoverS` segundos) y estampa lo que se ha movido (`addStamp`: jugador, criaturas,
 *          objetos con fisica). Un pase de pantalla completa: sin geometria de sellos.
 *       b. LA GENERACION (`grass_gen.comp`): cada hilo es (celda del cubo, brizna); hash del mundo,
 *          altura del MAPA DEL NODO DIBUJADO (no del campo), densidad del MATERIAL del suelo,
 *          culling de frustum y LOD por distancia. Escribe `Blade[]` y el `instanceCount` del draw.
 *  2. `draw()` (dentro del pase de escena, tras el terreno): UN `drawIndexedIndirect` de 15 indices
 *     por brizna (7 vertices procedurales, `grass.vert`), doblada por viento + presion.
 *
 * ── POR QUE ASI ────────────────────────────────────────────────────────────────────────────────
 *
 * Andoni (21-09): «hierba bastante, pero de manera realista: que si un objeto pasa se aplasta, y
 * así, pero muy optimizado». Medio millon de briznas no se pueden colocar en CPU cada frame, y una
 * malla de matas estatica no puede tumbarse bajo un pie. La GPU genera lo que la camara ve, sobre
 * el suelo que la GPU dibuja, y el aplastado es un mapa que cuesta lo mismo con un jugador que
 * con sesenta.
 *
 * ── LO QUE NO ES ───────────────────────────────────────────────────────────────────────────────
 *
 * No sustituye a la capa de props `grass` (matas hasta 1 km): esto es el primer plano, `radiusM`
 * alrededor de la camara. No proyecta sombra ni la recibe (v1). No colisiona. Y no se ve desde una
 * camara a caballo de dos caras del cubo en la esquina lejana de la otra cara (ver el .comp).
 *
*  Mandos para medir sin recompilar: `HARUKA_GRASS=0` (apaga), `HARUKA_GRASS_DENSITY=<0..1>`,
 *  `HARUKA_GRASS_RADIUS=<m>`, `HARUKA_GRASS_BIS=press|gen|draw` (salta ese pase: atribucion de
 *  coste y de fugas de estado), `HARUKA_GRASS_DEBUG=k` (el compute no descarta desde la etapa k),
 *  `HARUKA_GRASS_FULL=1` (regenera cada frame, para medir cuanto cuesta `v5.hierba.gen`).
 *
 *  ── CUANDO NO CAMBIA NADA ────────────────────────────────────────────────────────────────────
 *
 *  La generacion son ~2,7 M de hilos (rejilla de ~55×55 celdas de ~2,4 m, `kCellBits=22`) y se
 *  relanza cada frame aunque la camara no se mueva. El generador siembra por hash del mundo y no
 *  mete viento ni presion (eso lo hace el draw), asi que si la camara no ha movido > ~0,25 m, no
 *  hay sellos y los nodos son los mismos, el dispatch anterior es valido y se REAPROVECHA: `prepare`
 *  se salta el compute y `draw` reusa el contador. Parado, `v5.hierba.gen` pasa de 7,4 ms a nada.
 *  (Ademas, la CPU manda una LISTA de celdas con el centro dentro del disco en vez de la rejilla
 *  cuadrada —las esquinas del cuadrado mueren antes de pagar su fp64— y un INDICE ESPACIAL de la
 *  tabla de nodos `uNodes`: en vez de barrer los N nodos por brizna, el compute solo prueba los
 *  candidatos de su celda, en el mismo orden de tabla y con la misma comprobacion.)
 */
#include "rhi/rhi_device.h"
#include "world/terrain/terrain_node_renderer.h"   // TerrainNodeRenderer::NearNode

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace Haruka::Renderer {

class GrassRenderer {
public:
    struct Config {
        float radiusM      = 60.0f;   ///< hasta donde se genera (mas alla: las matas de props)
        /// MATAS por m² a los pies con `density = 1`; cada mata son 3 briznas (grass.vert), o sea
        /// ~300 briznas/m². Andoni (21-09): "la hierba es basta, cada milimetro": con 24 sueltas el
        /// suelo se veia entre ellas. El LOD las ralea desde 8 m y las que quedan se ensanchan.
        float bladesPerM2  = 160.0f;
        float density      = 1.0f;    ///< mando global 0..1 (HARUKA_GRASS_DENSITY)
        float heightM      = 0.26f;   ///< altura media de brizna
        float widthM       = 0.018f;  ///< anchura media en la base (x1,5 sobre 1,2 cm; Andoni, 21-09)
        float lodStartM    = 8.0f;    ///< desde donde ralea
        float lodKeep      = 0.10f;   ///< fraccion que queda en el radio (las demas se ensanchan)
        int   pressRes     = 512;     ///< lado del mapa de presion (texeles)
        float pressTexelM  = 0.25f;   ///< metros por texel
        float recoverS     = 4.0f;    ///< segundos para que la hierba aplastada se levante
        size_t maxBlades   = 900000;  ///< tope del buffer de MATAS (32 B cada una)
    };

    /** @brief Todo lo que el frame necesita: lo rellena el planeta, que es quien tiene el terreno. */
    struct Frame {
        glm::dvec3 camPos{0.0}, planetCenter{0.0};
        double     planetRadiusM = 1.0;
        glm::vec3  viewDir{0, 0, -1};
        glm::vec3  viewUp{0, 1, 0};          ///< arriba de la camara: con viewDir da los 4 planos del frustum
        float      tanHalfV = 0.577f;        ///< tan(fovY/2)
        float      aspect   = 1.777f;
        float      coneHalfAngle = 1.0f;     ///< semiangulo del cono (solo el descarte temprano)
        float      seaLevelM = 0.0f;
        glm::vec3  sunDir{0, 1, 0};
        float      sunIntensity = 1.0f, ambient = 0.35f;
        glm::vec3  wind{0.0f};
        float      time = 0.0f, dt = 1.0f / 60.0f;
        glm::vec4  aerial{0.0f};
        RHI::BufferHandle  heights{};        ///< `TerrainNodeRenderer::heightsBuffer()`
        RHI::TextureHandle baseField{};      ///< clima (binding 15), con relleno si no hay
        RHI::BufferHandle  materialUBO{};    ///< la tabla de materiales del terreno (binding 12)
        // VENTANA DE RECORTE DE LOS VOX (binding 16): donde el terreno se salta el suelo para
        // enseñar la boca, tampoco crece hierba — la brizna quedaría clavada en el aire (ver la
        // nota del `.comp`). Rellena la hierba la misma matriz que el pase de terreno usa para
        // `uVoxCut`; sin ventana (no hay vox cargados/cortados) llega vacia y no se descarta nada.
        RHI::TextureHandle cutTex{};
        glm::mat4          cutSpace{1.0f};
        const std::vector<Haruka::Terrain::TerrainNodeRenderer::NearNode>* nodes = nullptr;
        float      finestTexelM = 1.0f;      ///< texel del nivel mas fino de `nodes`
    };

    bool init(RHI::Device* dev, const std::string& shaderDir, const Config& cfg);
    bool init(RHI::Device* dev, const std::string& shaderDir) { return init(dev, shaderDir, Config{}); }
    void shutdown();
    bool ready() const { return m_ready; }
    static bool enabled();                   ///< HARUKA_GRASS (por defecto ON)

    /** @brief Algo se ha movido por aqui este frame: se estampa en el mapa de presion.
     *  `velocity` da la direccion en que se tumba la hierba (m/s; con 0 se tumba hacia fuera). */
    void addStamp(const glm::dvec3& worldPos, float radiusM, const glm::vec3& velocity, float strength = 1.0f);

    /** @brief Pase de presion + compute de briznas. FUERA del render pass de la escena. */
    void prepare(RHI::Context* ctx, const Frame& f);
    /** @brief El draw indirecto. DENTRO del pase de escena, tras el terreno. `rotVP` = proyeccion ·
     *  rotacion de la vista: las briznas ya son relativas al ojo. */
    void draw(RHI::Context* ctx, const glm::mat4& rotVP);

    const Config& config() const { return m_cfg; }
    /** @brief Briznas dibujadas en el ultimo frame del que se leyo (se lee 1 de cada 60: cuesta un readback). */
    size_t lastBladeCount() const { return m_lastCount; }
    size_t lastDispatched() const { return m_lastThreads; }
    RHI::TextureHandle pressureTexture() const;   ///< el mapa actual (para las sondas del banco)
    /// El marco del mapa de presion (ancla en el mundo, ejes E/N), para que una sonda sepa donde cae un sello.
    void pressureFrame(glm::dvec3& anchor, glm::dvec3& E, glm::dvec3& N) const { anchor = m_pressAnchor; E = m_pressE; N = m_pressN; }
    /// SONDA: el contador y las primeras `n` briznas del ultimo dispatch, leidos AHORA (tras endFrame,
    /// con la GPU ociosa). Cuesta dos copias sincronas: solo para el banco.
    size_t readBack(size_t n, std::vector<float>& bladesOut);

private:
    struct Stamp { glm::dvec3 pos; float radius; glm::vec3 vel; float strength; };
    void updatePressure(RHI::Context* ctx, const Frame& f);
    void dispatchBlades(RHI::Context* ctx, const Frame& f);

    RHI::Device*       m_dev = nullptr;
    Config             m_cfg;
    bool               m_ready = false;
    RHI::PipelineHandle m_genPipe{}, m_drawPipe{}, m_pressPipe{};
    RHI::BufferHandle  m_genUBO{}, m_drawUBO{}, m_pressUBO{};
    RHI::TextureHandle m_cutDummy{};     // 1x1 para el binding 16 cuando no hay ventana (Vulkan: sin descriptor sin atar)
    RHI::BufferHandle  m_nodesSSBO{}, m_bladesSSBO{}, m_cmd{}, m_ib{};
    RHI::BufferHandle  m_cellPairs{};   // ivec2[]: las celdas del disco que recorre el compute
    RHI::BufferHandle  m_idxOff{}, m_idxList{};   // índice espacial de `drawnHeightAt` (uints)
    size_t             m_nodesCap = 0, m_cellCap = 0;
    RHI::RenderPassHandle m_pressRT[2]{};
    RHI::TextureHandle m_pressTex[2]{};
    int                m_pressCur = 0;
    // El ancla del mapa de presion: un punto del mundo y su marco E/N; salta de texel en texel.
    bool               m_pressAnchored = false;
    glm::dvec3         m_pressAnchor{0.0}, m_pressE{1, 0, 0}, m_pressN{0, 0, 1}, m_pressUp{0, 1, 0};
    std::vector<Stamp> m_stamps;
    Frame              m_frameData;       ///< lo que `prepare` recibio; `draw` lo usa (sin `nodes`)
    // El ultimo `instanceCount`, leido de vuelta de tarde en tarde para el log.
    size_t             m_lastCount = 0, m_lastThreads = 0;
    uint32_t           m_frame = 0;
    RHI::BufferHandle  m_readback{};
    // Ultimo estado que DETERMINO la generacion (lo rellena el final de `dispatchBlades`, no cada
    // frame): si el frame actual es igual a eso dentro de umbrales, `prepare` reutiliza el dispatch
    // anterior (y `m_lastThreads` conserva su valor para el draw). `m_lastCam` es la camara del
    // BUFFER de briznas: el draw ancla el campo al mundo con `camShift = m_lastCam - camara actual`.
    glm::dvec3         m_lastCam{0.0}, m_lastPlanet{0.0};
    glm::vec3          m_lastViewDir{0, 0, -1}, m_lastViewUp{0, 1, 0};
    float              m_lastTh = 0.0f, m_lastAspect = 0.0f;
    const void*        m_lastNodes = nullptr;
    size_t             m_lastNodesN = 0;
    // Ventana de recorte del ULTIMO dispatch: si cambia (el `VoxRenderer` rehace la textura o
    // reancla la matriz) hay que volver a generar, aunque la camara no se mueva.
    RHI::TextureHandle m_lastCutTex{};
    bool               m_first = true;
};

} // namespace Haruka::Renderer
