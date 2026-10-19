#pragma once
/**
 * @file MassGround.h
 * @brief LAS MATAS DE LOS BROTES MASIVOS, SOBRE EL SUELO QUE SE DIBUJA. Planifica en CPU qué brotes
 *        se ajustan este frame y lanza `mass_ground.comp`, que escribe cuánto subir o bajar cada mata
 *        por la vertical de su brote. Lo lee `prop_burst.vert`.
 *
 * Genérico: vale para cualquier capa de `MassBurst` (hierba, flores, piedrecillas). Sin ajuste, un
 * brote planta todas sus matas en el plano tangente de su centro a la cota del CAMPO; con celdas de
 * hasta 12 m, en una ladera del 10 % las de los bordes flotaban o se enterraban medio metro
 * ("la hierba sobresale del terreno").
 *
 * ── QUÉ SE AJUSTA ────────────────────────────────────────────────────────────────────────────────
 * Los brotes más cercanos primero, hasta `budget` matas y dentro de `reachM`. El tope es de memoria
 * (4 B por mata); lo que queda fuera se dibuja en su plano, como antes.
 *
 * ── LA DIRECCIÓN EN DOBLE, EN LA CPU ─────────────────────────────────────────────────────────────
 * La inversa cubo↔esfera de GLSL es de float (~1 m a radio terrestre). Por brote se manda su
 * (cara, lx, ly) en doble y el jacobiano d(lx, ly)/d(u, v) de su plano: la mata `k` cae en
 * `l0 + J·offset`; la curvatura de la proyección dentro de una celda son micras.
 */
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include "Haruka/RHI/RHIResources.h"
#include "Haruka/World/Props/MassBurst.h"

namespace Haruka { namespace RHI { class Context; class Device; } }
namespace Haruka::World::Planet { struct GroundView; }

namespace Haruka::World::Props {

/// Un brote a ajustar. Gemelo de `SnapJob` de `mass_ground.comp` (std430, 64 B).
struct MassGroundJob {
    uint32_t a[4];   ///< índice del brote · base en el buffer de ajuste · matas · cara
    double   l[2];   ///< (lx, ly) del centro, en doble
    float    j[4];   ///< d lx/du, d lx/dv, d ly/du, d ly/dv (por metro)
    float    m[4];   ///< x = cota del centro (la del plano del brote)
};
static_assert(sizeof(MassGroundJob) == 64, "MassGroundJob: 64 B, gemelo de mass_ground.comp");

struct MassGroundPlanInput {
    const std::vector<MassBurst>* bursts = nullptr;
    glm::dvec3 planetC{0.0};
    double     planetR = 1.0;
    glm::dvec3 camPos{0.0};
    double     reachM = 250.0;
    uint32_t   budget = 1u << 20;   ///< matas como mucho
};
/// Los brotes a ajustar, los más cercanos primero, con su base en el buffer de ajuste.
std::vector<MassGroundJob> planMassGround(const MassGroundPlanInput& in, uint32_t& totalOut);

class MassGround {
public:
    bool init(RHI::Device* dev);
    void shutdown();
    bool ready() const { return m_ready; }

    /// Ajusta este frame los `jobs` sobre el suelo dibujado `g`. `bursts` = el SSBO de brotes que
    /// dibuja `prop_burst.vert` (`burstCount` entradas). Sin trabajos solo avanza el frame, así que
    /// los sellos viejos dejan de valer.
    bool dispatch(RHI::Context* ctx, const Planet::GroundView& g, const glm::dvec3& camPos, double reachM,
                  RHI::BufferHandle bursts, size_t burstCount, const std::vector<MassGroundJob>& jobs,
                  uint32_t totalElems, float sinkM = 0.0f);

    /// Lo que ata `prop_burst.vert` (bindings 4 y 5, siempre válidos) y el frame para `u_flags.w`.
    RHI::BufferHandle stamps() const { return m_stamps; }
    RHI::BufferHandle snap()   const { return m_snap; }
    float frameStamp() const { return (float)m_frame; }
    /// Para el banco: copia el ajuste (`elems` floats) y los sellos (`bursts` pares) a la CPU. Espera.
    void readBack(size_t elems, size_t bursts, std::vector<float>& snapOut, std::vector<uint32_t>& stampsOut);
    /// Un frame que no lleva ningún sello: el contador da la vuelta antes (`0xFFFFFF`). Exacto en float.
    static constexpr float kNone = 16777216.0f;

private:
    RHI::Device*        m_dev = nullptr;
    bool                m_ready = false;
    uint32_t            m_frame = 0;   ///< empieza en 1 al primer frame: un sello a 0 nunca vale
    RHI::PipelineHandle m_pipe{};
    RHI::BufferHandle   m_ubo{}, m_nodes{}, m_jobs{}, m_snap{}, m_stamps{}, m_idxOff{}, m_idxList{};
    size_t              m_nodesCap = 0, m_jobsCap = 0, m_snapCap = 0, m_stampsCap = 0, m_idxOffCap = 0, m_idxListCap = 0;
};

} // namespace Haruka::World::Props
