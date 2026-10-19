#pragma once
/**
 * @file ShellScatter.h
 * @brief SIEMBRA EN LA GPU, CADA FRAME, RACIMOS DE BULTOS SOBRE UNA CÁSCARA ESFÉRICA
 *        (`assets/shaders/shell_scatter.comp`) y los dibuja como mallas de un VOLUMEN (`prop_vol.vert` +
 *        `prop_vol.frag`: la silueta es la malla, la luz la de la envolvente del racimo). Cada racimo
 *        es UNA de `kVariants` mallas horneadas al arrancar (`BlobSurface`: sus bultos ya fundidos),
 *        en `kLods` niveles. No sabe qué siembra: la ley es `World/ShellClumps.h` y el campo (cobertura, base,
 *        techo, oscurecer) llega como textura por dirección. El cielo lo usa para los cúmulos.
 *
 * ── POR QUÉ ASÍ ──────────────────────────────────────────────────────────────────────────────────
 *
 * Una nube de malla no tiene estado propio: dónde está, cuánto mide y de qué bultos se hace son un
 * hash de su celda y el campo del clima en ese punto. La CPU solo decide qué bloques de 8×8 celdas
 * mira la cámara; la GPU genera los racimos, los recorta (imagen, tras el planeta, píxeles) y los
 * cuenta. La memoria es la del buffer de salida y nada crece con el mundo.
 *
 * ── EL DETALLE: PÍXELES Y MS, SIN RADIOS ─────────────────────────────────────────────────────────
 *
 * Nivel de malla de cada racimo por su ancho en píxeles (`lodPx*`), con los umbrales escalados por
 * `pxPerBlob / pxPerBlobMin` (1 a 9): eso y el tamaño mínimo de un racimo los mueve un
 * `LoadController` contra los ms de GPU del pase y contra la capacidad del buffer. La FORMA de una
 * nube no depende del detalle (antes sí, y crecía y menguaba sola). El alcance es el del horizonte de la cáscara, recortado por
 * píxeles: ninguna distancia fija decide el detalle.
 */
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "Haruka/RHI/RHIResources.h"
#include "Haruka/World/Props/LoadController.h"

namespace Haruka { namespace RHI { class Context; class Device; } }

namespace Haruka::World::Props {

/// Un bloque de 8×8 celdas de una cara: un grupo de 64 hilos del compute.
struct ShellScatterBlock {
    uint32_t face = 0, bx = 0, by = 0, pad = 0;
};
static_assert(sizeof(ShellScatterBlock) == 16, "ShellScatterBlock = uvec4 de shell_scatter.comp");

/// Lo que el planificador necesita. GL-free: se prueba en CPU.
struct ShellScatterPlanInput {
    glm::dvec3 camPos{0.0}, planetC{0.0};
    double     planetR  = 6371000.0;
    glm::dquat rot{1.0, 0.0, 0.0, 0.0};   ///< marco de la retícula → planeta
    uint32_t   cells    = 4096;           ///< celdas por lado de cara
    double     shellLoM = 600.0, shellHiM = 3000.0;   ///< cotas mínima y máxima de la cáscara (m sobre R)
    double     reachM   = 200000.0;       ///< hasta dónde (ya recortado por horizonte y píxeles)
    double     spillM   = 4000.0;         ///< cuánto puede salirse un racimo de su bloque (m)
    /// Planos laterales del frustum por el ojo (normales hacia dentro). Todo cero = sin recorte.
    glm::vec3  planes[4] = { glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f) };
    uint32_t   maxWork  = 16384;          ///< tope de bloques
};
struct ShellScatterPlan {
    std::vector<ShellScatterBlock> work;   ///< los más cercanos primero
    uint32_t candidates = 0;               ///< bloques dentro del alcance antes del tope
    bool     clipped = false;              ///< se llegó a `maxWork`: el que llama debe bajar el detalle
};

/// Celdas por lado de cara para que una celda mida ~`cellM` a radio `R`.
uint32_t shellScatterCells(double R, double cellM);
/// Distancia al horizonte de la cáscara (lo más lejos que se ve algo a `shellHiM`) desde `camR`.
double shellHorizonM(double planetR, double camR, double shellHiM);
/** @brief Qué bloques mira la cámara: dentro del alcance angular, delante del frustum, sin repetir,
 *  los más cercanos primero y hasta `maxWork`. Determinista y fijo a la retícula. */
ShellScatterPlan planShellScatter(const ShellScatterPlanInput& in);

class ShellScatter {
public:
    static constexpr int kVariants = 16;   ///< nubes horneadas distintas (`BlobSurface`)
    static constexpr int kLods     = 4;    ///< niveles de malla de cada una
    static constexpr int kRegions  = kVariants * kLods;

    struct Config {
        double   cellM      = 1500.0;    ///< lado de una celda (m): ~un racimo por celda
        uint32_t capacity   = 4096;      ///< nubes por región (×64 regiones ×64 B)
        double   budgetMs   = 2.5;       ///< ms de GPU del pase (compute + dibujo): las bolitas del borde lo piden
        float    pxPerBlobMin = 10.0f;   ///< detalle 1: umbrales de malla tal cual (escala 1)
        float    pxPerBlobMax = 30.0f;   ///< detalle 0: umbrales ×3 (mallas más bastas)
        float    cullPxMin  = 0.8f, cullPxMax = 3.0f;   ///< racimo más pequeño que se dibuja (detalle 1 / 0)
        float    lodPx0     = 700.0f;    ///< ancho de una nube (px) por encima del cual va la malla fina
        float    lodPx1     = 200.0f;    ///< la segunda
        float    lodPx2     = 50.0f;     ///< la tercera (si no, la más basta)
        double   maxReachM  = 450000.0;  ///< tope del alcance (la cáscara se ve más lejos desde órbita)
        float    tint       = 0.97f;     ///< albedo de un bulto
        float    darkTint   = 0.50f;     ///< albedo con el campo a "oscurecer" 1 (nube de lluvia)
        float    rim        = 0.0f;      ///< ganancia del rim: 0, el contorno de CADA bulto los hacía globos
        float    translucency = 0.9f;    ///< el Sol a través del borde fino del racimo a contraluz
        /// Espesor óptico por unidad de camino ESCALADO por la envolvente (`lib/volume_light.glsl`):
        /// cruzarla entera son 2·opacity. Un cúmulo real tiene τ ≈ 20-40, pero la luz que sale es
        /// sobre todo dispersión múltiple: lo que se ve es un degradado ancho, no un corte.
        float    opacity    = 2.2f;
        float    multi      = 0.30f;     ///< parte de la luz por dispersión múltiple (la sombra no es negra)
        float    wrap       = 0.6f;      ///< cuánto rodea la luz a cada bulto
        /// ACABADO PINTADO (`lib/paint.glsl`): 0 = luz continua (se lee como globos de plástico),
        /// 1 = manchas de tono hechas de trazos y borde descuidado.
        float    paint      = 1.0f;
        float    brushPx    = 4.0f;
        // ── EL ASPECTO DEL TIPO DE NUBE (no dónde ni cuánta: eso lo dice el campo del clima) ──────
        float    aspectMul  = 1.0f;      ///< alto máximo respecto al ancho (1 cúmulo · menos, más plana)
        float    stretch    = 1.0f;      ///< estirado a lo largo del viento (cirro: fibras)
        float    alpha      = 1.0f;      ///< cuánto tapa: < 1 = translúcida a trazos (velo)
        bool     windStrokes = false;    ///< los trazos del pincel siguen el viento      ///< ancho del pincel en PÍXELES, el mismo en todas las nubes (el trazo mide 5 veces eso)
        uint32_t seed       = 0xC10Du;
    };
    struct Frame {
        glm::dvec3 camPos{0.0}, planetC{0.0};
        double     planetR = 6371000.0;
        glm::dquat rot{1.0, 0.0, 0.0, 0.0};
        glm::vec3  viewDir{0, 0, -1}, viewUp{0, 1, 0};
        float      tanHalfV = 0.577f, aspect = 1.0f;
        int        heightPx = 1080;
        RHI::TextureHandle field{}, fieldPrev{};   ///< (cobertura, base, techo, oscurecer) por dirección
        float      blend   = 1.0f;                 ///< peso de `field` frente a `fieldPrev`
        double     shellLoM = 600.0, shellHiM = 3000.0;
        double     now = 0.0;                      ///< reloj del controlador (s)
        glm::vec3  windDir{0.0f};                  ///< viento a la cota de la capa (unitario, marco del ojo; 0 = nada)
    };

    ShellScatter() = default;
    ShellScatter(const ShellScatter&) = delete;
    ShellScatter& operator=(const ShellScatter&) = delete;
    ~ShellScatter() { shutdown(); }

    bool init(RHI::Device* dev, const Config& cfg);
    void shutdown();
    bool ready() const { return m_ready; }
    const Config& config() const { return m_cfg; }

    /** @brief Planifica y lanza el compute (FUERA de cualquier render pass). false = nada que sembrar. */
    bool dispatch(RHI::Context* ctx, const Frame& f);
    /** @brief Dibuja lo sembrado DENTRO del pase de escena (depth de la escena atado). `perFrameUBO` es
     *  el UBO de frame (binding 0); `restorePSO` se vuelve a atar al acabar (válido o no). */
    void draw(RHI::Context* ctx, RHI::BufferHandle perFrameUBO, RHI::PipelineHandle restorePSO, float time);
    bool hasWork() const { return m_lastWork > 0; }

    /** @brief Un tiempo de GPU medido del pase (ms; ≤0 = sin medida) para el lazo de detalle. */
    void feedGpuMs(double ms, double now);
    float  detail() const { return m_ctl.detail; }
    double gpuMs()  const { return m_ctl.msSmooth; }
    float  pxPerBlob() const;
    float  cullPx() const;
    uint32_t lastBlocks() const { return m_lastWork; }
    uint32_t lastCount()  const { return m_lastCount; }    ///< instancias escritas (lectura atrasada)
    uint32_t lastWanted() const { return m_lastWanted; }   ///< las que quisieron entrar
    double   lastReach()  const { return m_lastReach; }
    /// Lo que usó el último `dispatch` (para el careo del banco con el gemelo de CPU).
    struct LastUsed { ShellScatterPlanInput plan; uint32_t cells = 0; double cellM = 0.0; float focalPx = 0.0f, pxPerBlob = 0.0f, cullPx = 0.0f; };
    const LastUsed& lastUsed() const { return m_lastUsed; }

    /// Para el banco: copia los comandos y las instancias de cada región a CPU (espera a la GPU).
    /// `inst[r]` = instancias de la región r (10 palabras cada una). Devuelve el total escrito.
    uint32_t readBack(std::vector<uint32_t>& cmdOut, std::vector<std::vector<uint32_t>>& inst, uint32_t maxPerRegion);

    /// Las mallas (variante × nivel), para los tests.
    uint32_t indexCount(int variant, int lod) const { return m_meshIdx[variant * kLods + lod]; }

private:
    bool buildMeshes();

    RHI::Device*        m_dev = nullptr;
    Config              m_cfg;
    bool                m_ready = false;
    RHI::PipelineHandle m_pipe{}, m_drawPSO{};
    RHI::BufferHandle   m_ubo{}, m_params{}, m_work{}, m_inst{}, m_vol{}, m_cmd{}, m_readback{};
    RHI::BufferHandle   m_vbo[kRegions] = {}, m_ebo[kRegions] = {};
    uint32_t            m_meshIdx[kRegions] = {};
    RHI::TextureHandle  m_dummy{};
    size_t              m_workCap = 0;
    LoadController      m_ctl;
    bool                m_over = false;
    uint64_t            m_frame = 0;
    double              m_lastReach = 0.0;
    LastUsed            m_lastUsed;
    float               m_pixAngle = 0.001f;   ///< ángulo de un píxel (rad) del último `dispatch`
    glm::vec3           m_windDir{0.0f};
    uint32_t            m_lastWork = 0, m_lastCount = 0, m_lastWanted = 0;
};

} // namespace Haruka::World::Props
