#pragma once
/**
 * @file GroundScatter.h
 * @brief SIEMBRA EN LA GPU, CADA FRAME, ELEMENTOS FINOS Y REPETIDOS SOBRE EL SUELO DIBUJADO
 *        (`assets/shaders/ground_scatter.comp`) y los deja como instancias del stream de props, que
 *        dibuja `prop_inst.vert` con la malla del prototipo. No sabe qué siembra: hierba, flores,
 *        piedrecillas u hojarasca son un prototipo y una escala.
 *
 * ── POR QUÉ ASÍ ──────────────────────────────────────────────────────────────────────────────────
 *
 * "Hierba en cada cm" son cientos de miles de elementos cerca de la cámara. Ninguno tiene estado
 * propio: posición, escala y giro son un hash de (cara, celda, k) y la cota es la del nodo dibujado.
 * Así que la CPU no guarda NADA por elemento —solo decide qué grupos (celdas de ~1 m de la cara del
 * cubo) mira la cámara y cuántos hilos merece cada uno— y la GPU los genera y los cuenta. La memoria
 * es la del buffer de salida, acotada por `Config::capacity`, y nada crece con el mundo.
 *
 * ── EL DETALLE: PÍXELES Y MS, SIN RADIOS ─────────────────────────────────────────────────────────
 *
 * La ley es `PxLod` (los que quedan a distancia d son un PREFIJO y se ensanchan para conservar la
 * cobertura en pantalla). Su único mando, `thrPx`, lo mueve un `LoadController` contra los ms de GPU
 * del pase (compute + dibujo) y contra la capacidad del buffer (`over`). El alcance SALE de `thrPx`.
 *
 * ── LO QUE PASA POR ENCIMA ───────────────────────────────────────────────────────────────────────
 *
 * Tumbado: el campo de suelo (`Renderer::GroundField`, canal de empuje). Sin nada: cráteres, bocas de
 * cueva recortadas, zonas destruidas (`Frame::zones`). Todo se resuelve en el compute.
 */
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "Haruka/RHI/RHIResources.h"
#include "Haruka/Renderer/GroundField.h"
#include "Haruka/World/Planet/GroundView.h"
#include "Haruka/World/Props/LoadController.h"
#include "Haruka/World/Props/PxLod.h"

namespace Haruka { namespace RHI { class Context; class Device; } }

namespace Haruka::World::Props {

/// Una tanda de 64 hilos del compute: el grupo (cara, cx, cy) y desde qué elemento.
struct GroundScatterWork {
    uint32_t cx = 0, cy = 0;
    uint32_t faceK = 0;     ///< cara | (primer elemento << 3)
    uint32_t threads = 0;   ///< hilos que merece el GRUPO (los k ≥ esto salen)
};
static_assert(sizeof(GroundScatterWork) == 16, "GroundScatterWork = uvec4 de ground_scatter.comp");

/// Lo que el planificador necesita. GL-free: se prueba en CPU.
struct GroundScatterPlanInput {
    glm::dvec3 camPos{0.0}, planetC{0.0};
    double     planetR   = 6371000.0;
    double     groundM   = 0.0;     ///< cota del suelo bajo la cámara (m sobre el radio)
    PxLod      px;
    uint32_t   perGroup  = 24;      ///< elementos por grupo (K)
    int        level     = 23;      ///< grupos por lado de cara = 2^level
    /// Planos laterales del frustum por el ojo (normales hacia dentro). Todo cero = sin recorte.
    glm::vec3  planes[4] = { glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f) };
    uint32_t   maxWork   = 65535;   ///< tope de tandas (límite de un dispatch)
};
struct GroundScatterPlan {
    std::vector<GroundScatterWork> work;
    uint32_t groups  = 0;       ///< grupos visibles
    uint64_t threads = 0;       ///< hilos útiles pedidos (Σ por grupo)
    double   reachM  = 0.0;     ///< alcance que sale de `thrPx`
    bool     clipped = false;   ///< se llegó a `maxWork`: el que llama debe bajar el detalle
};

/** @brief Qué grupos mira la cámara y cuántos hilos merece cada uno (`pxGroupThreads` con la
 *  distancia HORIZONTAL más cercana del grupo: cota superior de lo que el shader se queda, porque el
 *  `keep` no crece con la distancia). Determinista y fijo al mundo. */
GroundScatterPlan planGroundScatter(const GroundScatterPlanInput& in);

/**
 * @brief EL ÍNDICE ESPACIAL de la tabla de nodos dibujados (`lib/ground_view.glsl`): una rejilla
 *        (lx, ly) de `dim`² celdas sobre la cara de la cámara, de `spanL` de lado, con la lista de
 *        nodos que toca cada celda. Lo comparten todos los que buscan la cota dibujada en la GPU (la
 *        siembra de la cubierta, el ajuste de los brotes) para no construirlo dos veces a mano.
 *        Fuera de la rejilla —u otra cara— el shader barre la tabla entera.
 */
struct GroundViewIndex {
    std::vector<uint32_t> off;    ///< dim² + 1 desplazamientos en `list`
    std::vector<uint32_t> list;   ///< índices de nodo (nunca vacía: un SSBO vacío no se ata)
    double x0 = 0.0, y0 = 0.0, cell = 0.0, span = 0.0;   ///< esquina mínima, lado de celda y total (lx, ly)
    int    dim = 0;
};
GroundViewIndex buildGroundViewIndex(const Planet::GroundView& g, const glm::dvec3& camPos,
                                     double reachM, int dim);

/// Nivel de la retícula para que una celda mida ~`cellM` en un planeta de radio `R`.
int groundScatterLevel(double planetR, double cellM);

class GroundScatter {
public:
    struct Config {
        float    elemPerM2  = 30.0f;     ///< elementos por m² a detalle completo
        float    cellM      = 1.0f;      ///< lado objetivo de un grupo (m)
        float    scaleMin   = 0.18f, scaleMax = 0.32f;   ///< escala del prototipo
        float    widthM     = 0.03f;     ///< anchura del elemento que mide la ley de píxeles (m)
        float    thrMinPx   = 0.6f;      ///< umbral a detalle 1 (fino y lejos)
        float    thrMaxPx   = 8.0f;      ///< umbral a detalle 0 (basto y cerca)
        float    widenMax   = 3.5f;
        double   budgetMs   = 2.0;       ///< ms de GPU del pase (compute + dibujo)
        uint32_t capacity   = 262144;    ///< instancias del buffer de salida (×40 B)
        float    density    = 1.0f;      ///< multiplica a la hierba del material (0..1)
        float    maxSlope   = 0.30f;     ///< 1 − n·up: ~46°
        float    bendRad    = 1.35f;     ///< cuánto tumba un empuje entero
        float    sinkM      = 0.04f;     ///< la raíz bajo el suelo (el error del morph del nodo)
        uint32_t seed       = 0x6A55u;
    };
    struct Frame {
        glm::dvec3 camPos{0.0};
        glm::vec3  viewDir{0, 0, -1}, viewUp{0, 1, 0};
        float      tanHalfV = 0.577f, aspect = 1.0f;
        int        heightPx = 1080;
        double     groundM  = 0.0;                 ///< cota del suelo bajo la cámara
        const Planet::GroundView* ground = nullptr;
        RHI::TextureHandle        fieldTex{};      ///< campo de suelo (empuje en rg); {} = sin campo
        Renderer::GroundField::View field;
        float      snow = 0.0f;                    ///< nieve acumulada 0..1
        std::vector<glm::vec4> zones;              ///< zonas destruidas: centro (mundo−cámara) + radio
        double     now = 0.0;                      ///< reloj del controlador (s)
    };

    GroundScatter() = default;
    GroundScatter(const GroundScatter&) = delete;
    GroundScatter& operator=(const GroundScatter&) = delete;
    ~GroundScatter() { shutdown(); }

    bool init(RHI::Device* dev, const Config& cfg);
    void shutdown();
    bool ready() const { return m_ready; }
    const Config& config() const { return m_cfg; }

    /** @brief Planifica y lanza el compute (FUERA de cualquier render pass). `indexCount` = índices
     *  de la malla que se dibujará (va al comando indirecto). false = este frame no hay nada. */
    bool dispatch(RHI::Context* ctx, const Frame& f, uint32_t indexCount);

    /// Lo que lee el dibujo: instancias (binding 1 del layout de props) y el comando indirecto.
    RHI::BufferHandle instances() const { return m_inst; }
    RHI::BufferHandle command()   const { return m_cmd; }
    bool              hasWork()   const { return m_lastWork > 0; }

    /** @brief Un tiempo de GPU medido del pase (ms; ≤0 = sin medida) para el lazo de detalle. */
    void feedGpuMs(double ms, double now);

    float    detail()     const { return m_ctl.detail; }
    double   gpuMs()      const { return m_ctl.msSmooth; }   ///< media de lo medido (0 = sin medida)
    float    thrPx()      const;
    double   reachM()     const { return m_lastReach; }
    uint32_t lastGroups() const { return m_lastGroups; }
    uint64_t lastThreads() const { return m_lastThreads; }
    uint32_t lastCount()  const { return m_lastCount; }      ///< instancias escritas (lectura atrasada)
    uint32_t lastWanted() const { return m_lastWanted; }     ///< las que quisieron entrar
    size_t   gpuBytes()   const;

    /// Para el banco: copia el comando y las instancias a CPU (espera a la GPU).
    uint32_t readBack(std::vector<uint32_t>& cmdOut, std::vector<uint32_t>& instOut, uint32_t maxInst);

private:
    RHI::Device*        m_dev = nullptr;
    Config              m_cfg;
    bool                m_ready = false;
    RHI::PipelineHandle m_pipe{};
    RHI::BufferHandle   m_ubo{}, m_nodes{}, m_work{}, m_idxOff{}, m_idxList{}, m_inst{}, m_cmd{}, m_readback{};
    RHI::TextureHandle  m_dummy2D{};   ///< para la ventana de recorte o el campo cuando no llegan
    size_t              m_nodesCap = 0, m_workCap = 0, m_idxListCap = 0;
    LoadController      m_ctl;
    bool                m_over = false;
    uint64_t            m_frame = 0;
    double              m_lastReach = 0.0;
    uint32_t            m_lastGroups = 0, m_lastWork = 0, m_lastCount = 0, m_lastWanted = 0;
    uint64_t            m_lastThreads = 0;
};

} // namespace Haruka::World::Props
