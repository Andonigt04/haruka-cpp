#pragma once
/**
 * @file GrassSystem.h
 * @brief LA HIERBA (y flores, líquenes: todo el relleno sin collider) como SISTEMA PROPIO, hermano de
 *        `PropSystem`: los dos heredan de `PropSystemBase`.
 *
 * ── POR QUÉ ES OTRO SISTEMA ──────────────────────────────────────────────────────────────────────
 *
 * Un prop (árbol, roca, casa) es una INSTANCIA con identidad propia: colisiona, se rompe por partes,
 * se guarda uno a uno. La hierba no es nada de eso: es un campo de millones de matas sin collider
 * cuya única historia es "pisada" o "arrancada". Tratarla como props era lo que la limitaba —176 B y
 * un barrido por mata y por frame— y mezclaba en `PropSystem` dos problemas distintos.
 *
 * Aquí: sus capas (`isMassLayer`: libres y sin collider), su scatter (la base re-siembra solo esas
 * capas, con la MISMA ecología y los mismos hashes que si fuera un scatter conjunto), sus datos
 * (`MassBurst`, un registro por CELDA), su dibujo (un comando indirecto por brote; la GPU reconstruye
 * las matas en `prop_burst.vert`) y su estado (2 bits por mata, solo en los brotes tocados).
 *
 * ── LO QUE HEREDA DE LA BASE ─────────────────────────────────────────────────────────────────────
 *
 * El ciclo de re-siembra asíncrona, el registro de prototipos, las mallas horneadas y el material
 * (la hierba de un brote y un cúmulo de pradera son la misma planta, con el mismo `prop_inst.frag`),
 * el cull + LOD por píxel y el filtro de bocas/cráteres.
 *
 * ── CLIMA Y DESTRUCCIÓN ──────────────────────────────────────────────────────────────────────────
 *
 * Viento, peso de la nieve y del agua (DrawFrame::weather) en el vertex shader. Pisada efímera
 * (`flattenNear`), agua que la tumba (`onRefresh`), destrucción por zona (`destroyAt`: explosión,
 * fuego, siega) o por mata (`nearest` + `destroyElement`: coger una flor); las dos acaban en los
 * mismos bits y persisten por semilla de brote (`serialize`/`restore`).
 *
 * ── LA CUBIERTA DENSA: "CADA CM" ─────────────────────────────────────────────────────────────────
 *
 * Los brotes no llegan a cubrir el suelo (≤1024 matas por celda de 12 m, una cota por celda). Debajo
 * de ellos va la CUBIERTA (`GroundScatter`): el prototipo de hierba sembrado en la GPU cada frame
 * sobre el suelo DIBUJADO, con detalle por píxeles y un lazo de ms de GPU (sin radios), sin nada
 * guardado por elemento. Lo que la toca: el campo de suelo (`GroundField`, las pisadas la tumban),
 * los cráteres y bocas de cueva (no crece), y las ZONAS destruidas (`destroyAt` las guarda: una
 * esfera por evento, persistente con `serializeZones`). Su compute va en `prepareGpu`, antes del
 * pase de escena; su dibujo, en `draw`, con `prop_inst.vert/.frag` y la malla del prototipo.
 */
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

#include "Haruka/RHI/RHIResources.h"
#include "Haruka/Renderer/GroundField.h"
#include "Haruka/World/Props/GroundScatter.h"
#include "Haruka/World/Props/MassBurst.h"
#include "Haruka/World/Props/MassGround.h"
#include "Haruka/World/Props/PropSystemBase.h"

namespace Haruka::World::Props {

class GrassSystem final : public PropSystemBase {
public:
    /// Lo destruido de UN brote: sus palabras de estado (2 bits por mata, solo `Destroyed`).
    struct Delta {
        uint32_t seed = 0;
        std::vector<uint32_t> words;
    };
    /// Una mata concreta.
    struct Hit {
        bool       hit     = false;
        uint32_t   seed    = 0;       ///< brote (identidad estable)
        uint32_t   element = 0;       ///< mata dentro del brote
        int        proto   = -1;      ///< prototipo (índice del registro de props)
        glm::dvec3 pos{0.0};          ///< raíz de la mata, en el mundo
    };
    /// Una zona destruida (explosión, fuego, siega): ahí la cubierta densa no vuelve a salir.
    struct Zone {
        glm::dvec3 center{0.0};
        double     radius = 0.0;
    };
    /// Lo que la cubierta necesita del frame para su compute.
    struct GpuFrame {
        Core::Camera*            camera  = nullptr;
        Planet::PlanetarySystem* planets = nullptr;
        uint32_t                 viewW = 0, viewH = 0;   ///< el framebuffer que se dibuja
        float                    snow  = 0.0f;           ///< nieve acumulada 0..1
        float                    dt    = 0.016f;
    };
    GrassSystem() = default;
    ~GrassSystem() override;
    void shutdown() override;   ///< suelta la GPU (el device sigue vivo)

    /** @brief El trabajo de GPU de la cubierta: campo de suelo + siembra. FUERA de cualquier render
     *  pass y DESPUÉS de `prepare` del planeta (necesita los nodos dibujados de este frame). */
    void prepareGpu(RHI::Context* ctx, const GpuFrame& f);
    const GroundScatter&         cover() const { return m_cover; }
    const Renderer::GroundField& field() const { return m_field; }

    /** @brief Un comando indirecto por brote visible, dentro del pase de escena (tras los props). */
    void draw(RHI::Context* ctx, const DrawFrame& f, DrawStats& stats) override;

    // ── Datos ──────────────────────────────────────────────────────────────────────────────────
    /** @brief Para tests y herramientas: fija los brotes a mano con el anclaje del planeta (dir +
     *  cota → mundo) y el ORIGEN al que van relativas las posiciones GPU. Lo normal es que lleguen
     *  de la re-siembra. El estado tocado se conserva: vive por semilla, fuera de los brotes. */
    void setBursts(std::vector<MassBurst> bursts, const glm::dvec3& planetC, double planetR, const glm::dvec3& origin);
    const std::vector<MassBurst>& bursts() const { return m_bursts; }
    uint64_t elementCount() const;                       ///< matas que representan los brotes
    size_t   touchedBursts() const { return m_state.size(); }
    size_t   hostBytes() const { return m_bursts.size() * (sizeof(MassBurst) + sizeof(MassBurstGpu)); }

    // ── Estado (API del juego) ─────────────────────────────────────────────────────────────────
    /** @brief DESTRUCCIÓN POR ZONA: todas las matas no destruidas cuya raíz cae en la esfera.
     *  Devuelve cuántas. Solo alcanza a los brotes cargados (cerca de la cámara). */
    int  destroyAt(const glm::dvec3& center, double radius);
    /** @brief La mata no destruida más cercana a `center` dentro de `radius`, SIN tocarla. */
    Hit  nearest(const glm::dvec3& center, double radius) const;
    /** @brief DESTRUCCIÓN POR MATA. false si ya estaba destruida o el brote no está cargado. */
    bool destroyElement(uint32_t seed, uint32_t element);
    /** @brief PISA LA HIERBA: las matas vivas de la esfera se tumban. Lo llaman el jugador, los
     *  objetos con física y los props derribados (`PropSystem::onGroundImpact`). Devuelve cuántas.
     *  ⚠️ NO es estado de juego: se levanta sola a los `kFlatDwellS` y no se guarda. */
    int  flattenNear(const glm::dvec3& center, double radius);
    /// Para tests: la pisada con reloj explícito, y su relevo.
    int  flattenAt(const glm::dvec3& center, double radius, double until);
    void sweepFlat(double now);
    /** @brief Lo destruido, para guardar/sincronizar (las pisadas no: son efímeras). */
    std::vector<Delta> serialize() const;
    void restore(const std::vector<Delta>& deltas);
    /** @brief Las zonas destruidas de la cubierta densa (las pone `destroyAt`). Una esfera dentro de
     *  otra no se guarda dos veces. */
    const std::vector<Zone>& zones() const { return m_zones; }
    std::vector<Zone> serializeZones() const { return m_zones; }
    void restoreZones(const std::vector<Zone>& zones) { m_zones = zones; }

    /// Raíz de la mata `k` en el mundo: la MISMA cuenta que `prop_burst.vert`, en double.
    glm::dvec3 elementWorld(const MassBurst& b, uint32_t k) const;

protected:
    // ── Los huecos de la base ──
    const char* systemName() const override { return "GrassSystem"; }
    bool ownsLayer(const PropLayer& L) const override { return isMassLayer(L); }
    void configureScatter(PropScatterParams& params) override;
    struct GrassOutput final : ScatterOutput { std::vector<MassBurst> bursts; };
    ScatterJob makeJob(const ScatterContext& ctx) override;
    void applyScatter(std::shared_ptr<ScatterOutput> out, const ApplyContext& ctx) override;
    /// Cada frame: levanta las pisadas vencidas y deja que el agua tumbe lo que cubre.
    void onRefresh(Physics::PhysicsEngine* physics) override;
    void onCleared() override { m_bursts.clear(); m_gpu.clear(); m_dirty = true; }

private:
    static constexpr double kFlatDwellS        = 5.0;    ///< cuanto queda tendida una pisada (s)
    static constexpr double kWaterSweepS       = 0.12;   ///< pausa entre vueltas del agua (s)
    static constexpr double kWaterSweepBudgetMs = 1.0;   ///< ms por frame de la vuelta del agua
    /// EL AGUA TUMBA LA HIERBA: un brote cuyo pie queda bajo la superficie (la COTA con la ola que
    /// usa la física) se tumba entero y no se levanta mientras lo cubra. Una consulta por BROTE,
    /// repartida en frames (la cota con olas no es gratis y hay miles de brotes).
    void sweepWater(const std::function<double(const glm::dvec3&)>& waterSurfaceAt, double now);
    double m_lastWaterSweep = -1e300;

    struct StateEntry {
        std::vector<uint32_t> words;          ///< 2 bits por mata (MassState)
        double flatUntil = -1.0;              ///< reloj de la pisada; <0 = sin pisada
    };
    glm::dvec3 centerWorld(const MassBurst& b) const {
        return m_planetC + glm::dvec3(b.dir) * (m_planetR + (double)b.heightM);
    }
    void rebuildGpu();
    void upload(RHI::Device* dev);
    /// Cambia a `to` las matas de la esfera cuyo estado está en `fromMask` (bits 1 << MassState).
    int  setStateIn(const glm::dvec3& center, double radius, uint32_t fromMask, MassState to, double flatUntil);

    std::vector<MassBurst>    m_bursts;
    std::vector<MassBurstGpu> m_gpu;          ///< copia GPU, relativa a `m_origin`
    std::unordered_map<uint32_t, StateEntry> m_state;   ///< por semilla de brote: lo tocado
    glm::dvec3 m_origin{0.0};                 ///< origen del pase: cámara de lanzamiento
    bool       m_dirty   = true;              ///< re-subir brotes/estado a la GPU
    size_t     m_waterCursor = 0;             ///< por dónde va la vuelta del agua

    RHI::PipelineHandle m_pso{};
    RHI::BufferHandle   m_burstSSBO{}, m_stateSSBO{}, m_cmdBuf{};
    size_t m_burstCap = 0, m_stateCap = 0, m_cmdCap = 0;   ///< bytes

    // ── La cubierta densa ──
    static constexpr size_t kMaxZones = 4096;   ///< zonas guardadas (las más viejas se olvidan)
    /// Nivel de malla del prototipo que dibuja la cubierta: la ley de píxeles ya ralea y ensancha,
    /// así que no hace falta la mata completa en cada instancia.
    static constexpr int    kCoverLod = 1;
    /// Hasta dónde se posan las matas de los brotes en el suelo dibujado, y cuántas como mucho (4 B
    /// cada una: 4 MB). Más allá, medio metro de ladera son menos de un par de píxeles.
    static constexpr double   kGroundSnapReachM = 250.0;
    static constexpr uint32_t kGroundSnapBudget = 1u << 20;
    /// El prototipo que siembra la cubierta: la primera mata de hierba de SUS capas (-1 = ninguna).
    int  coverPrototype() const;
    void drawCover(RHI::Context* ctx, const DrawFrame& f, DrawStats& stats);
    GroundScatter         m_cover;
    Renderer::GroundField m_field;
    bool                  m_coverTried = false;   ///< init intentado (no reintentar cada frame)
    /// Las matas de los brotes, sobre el suelo DIBUJADO (`MassGround`, `mass_ground.comp`).
    MassGround            m_ground;
    bool                  m_groundTried = false;
    RHI::PipelineHandle   m_coverPSO{};
    std::vector<Zone>     m_zones;
    double                m_coverClock = 0.0;
};

} // namespace Haruka::World::Props
