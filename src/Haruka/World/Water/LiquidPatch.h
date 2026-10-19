#pragma once
/**
 * @file World/Water/LiquidPatch.h
 * @brief EL LIQUIDO 3D EN EL MUNDO: un parche de `Physics::Fluid::ApicFluid` anclado al planeta bajo
 *        la cámara, con su superficie dibujada como agua (`Renderer::IsoSurface` + water_surface.frag).
 *
 * Andoni, 2026-10-19: el parche "debería ser por ms, no por distancia". Nada aquí es un radio:
 *   · el LADO DE CELDA sale de los PÍXELES (una celda ≈ `kCellPx` px a la altura del ojo sobre el agua);
 *   · la EXTENSIÓN sale del PRESUPUESTO: se miden los ms de GPU del líquido (paso + superficie +
 *     dibujo) y el número de celdas se escala hasta gastar `kBudgetMs` (3 ms).
 *
 * Qué lo mueve, y solo lo que se queda en el diseño final ("quitar lo que no se vaya a utilizar"):
 *   · la gravedad sobre el lecho DE VERDAD (el suelo que se pisa, `TerrainSampler::heightAt`);
 *   · el VIENTO del clima tirando de la superficie;
 *   · los CUERPOS de la física como esferas sólidas que lo empujan;
 *   · bordes que ABSORBEN (esponja): fuera del parche el mar está en reposo, aún no hay mar lejano
 *     en el mismo solver que le mande su oleaje.
 *
 * Convive con el agua de siempre (Andoni: "conviven para comparar"): no la recorta. Se apaga con el
 * ajuste `GraphicsSettings::liquid` (`Liquid=`, consola `liquido 0|1`).
 *
 * ⚠️ Al RECOLOCARSE (la cámara se fue lejos del centro, o el controlador pidió otro tamaño) el parche
 * se re-siembra en reposo: el agua de dentro se pierde. Con histéresis y como mucho cada 2 s.
 */
#include <memory>
#include <glm/glm.hpp>

#include "Haruka/Physics/Fluid/ApicFluid.h"
#include "Haruka/Renderer/IsoSurface.h"

namespace Haruka { namespace Core { class Camera; } }
namespace Haruka { namespace RHI { class Context; } }
namespace Haruka::Physics { class PhysicsEngine; }
namespace Haruka::World::Planet { class PlanetarySystem; }

namespace Haruka::World::Water {

class LiquidPatch {
public:
    struct Frame {
        Core::Camera*            camera  = nullptr;
        Planet::PlanetarySystem* planets = nullptr;
        Physics::PhysicsEngine*  physics = nullptr;   ///< cuerpos = esferas sólidas (puede ser null)
        glm::vec3 wind{ 0.0f };                       ///< viento del clima (m/s, marco del planeta)
        float dt = 0.016f;
        int   viewH = 1080;
        float aspect = 16.0f / 9.0f;
    };

    static constexpr double kBudgetMs = 3.0;
    static constexpr float  kCellPx = 6.0f;           ///< una celda ≈ 6 px a la altura del ojo
    static constexpr int    kMaxCells = 1500000;      ///< tope de memoria
    static constexpr int    kMinCells = 16 * 16 * 12;
    static constexpr int    kStartCells = 120000;     ///< se arranca pequeño; el lazo lo lleva al presupuesto
    static constexpr double kSettleS = 1.0;           ///< tras re-sembrar, s sin creerse el tiempo medido

    ~LiquidPatch() { shutdown(); }

    /// Compute, FUERA del render pass: coloca/re-siembra, empuja y avanza el líquido, y extrae la malla.
    void prepare(RHI::Context* ctx, const Frame& f);
    /// En el pase de escena (al final, transparente).
    void draw(RHI::Context* ctx, const Frame& f);
    void shutdown();
    /// Apagado por el ajuste: suelta el recorte del agua de siempre (si no, quedaría un agujero).
    void disable(Planet::PlanetarySystem* planets);

    bool active() const { return m_active; }

private:
    bool ensureGpu();
    bool rebuild(const Frame& f, float dx, int nCells);

    std::unique_ptr<Physics::Fluid::ApicFluid> m_sim;
    Renderer::IsoSurface m_iso;
    bool m_gpuReady = false, m_active = false;
    RHI::PipelineHandle m_pipe{};
    RHI::BufferHandle m_drawUbo{};
    double m_msSmooth = 0.0;      ///< ms de GPU del parche ACTUAL (media), 0 = aún sin medida

    // El marco del parche (mundo, m): origen = esquina (0,0,0) de la rejilla; Y = vertical.
    glm::dvec3 m_origin{ 0.0 }, m_axisX{ 1, 0, 0 }, m_axisY{ 0, 1, 0 }, m_axisZ{ 0, 0, 1 };
    float  m_dx = 0.0f;
    int    m_cells = 0;
    double m_depthM = 0.0;        ///< agua simulada bajo el nivel de reposo (m)
    double m_lastBuild = -1e9, m_acc = 0.0, m_lastLog = -1e9, m_clock = 0.0;
};

} // namespace Haruka::World::Water
