#pragma once
/**
 * @file ApicFluid.h
 * @brief UN LIQUIDO 3D EN LA GPU (APIC sobre rejilla MAC, `assets/shaders/apic_fluid.comp`).
 *
 * Andoni, 2026-10-19: "los rollos deberian de ser generados por fisicas normal no porque sean olas
 * ... asi son torvellinos etc ... que la fisica de liquidos sea una". Este solver no sabe qué es una
 * ola: rollos, remolinos y salpicaduras tienen que SALIR de gravedad + incompresibilidad + lecho +
 * lo que empuje el agua por el borde (aquí una pared móvil, la pala de un tanque de olas).
 *
 * El estado vive en la GPU y es la ÚNICA verdad: no hay gemelo en CPU. `readParticles` existe para
 * los tests y para quien necesite muestrear (física de cuerpos flotando), no para recalcular.
 *
 * Parche local: x a lo largo, y arriba, z de través (periódico). Coordenadas en metros, locales.
 */
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "Haruka/RHI/RHIResources.h"

namespace Haruka { namespace RHI { class Context; class Device; } }

namespace Haruka::Physics::Fluid {

class ApicFluid {
public:
    struct Config {
        int   nx = 64, ny = 32, nz = 4;   ///< celdas
        float dx = 0.05f;                 ///< lado de celda (m)
        float dt = 0.005f;                ///< paso (s)
        float gravity = 9.81f;
        int   perCell = 8;                ///< partículas por celda al sembrar (2×2×2)
        float densityGain = 0.0f;         ///< expansión de lo sobre-comprimido, fracción por paso (con
                                          ///< cuentas por celda infla el agua quieta: 0,05 → +20 % en 2 s)
        float sor = 1.7f;                 ///< ω del Gauss-Seidel rojo-negro
        int   pressureIters = 60;
        /// Pared móvil en x: x(t) = stroke/2·(1 + tanh((t − t0)/tau)). stroke 0 = pared quieta en 0.
        float wallStroke = 0.0f, wallT0 = 1.0f, wallTau = 0.3f;
        /// Cota del lecho por columna (nx·nz, índice k·nx + i), en m desde el fondo del parche.
        std::vector<float> bed;
        float fillLevel = 0.5f;           ///< se siembra líquido entre el lecho y esta cota
        bool  periodicZ = false;          ///< través periódico (tanque de prueba) o con paredes
        /// Esponja de los bordes ABIERTOS (x siempre; z si no es periódico): el agua de fuera está en
        /// reposo y lo que llega se frena en vez de rebotar contra la pared del parche. 0 = sin esponja.
        float spongeWidth = 0.0f, spongeRate = 0.0f;
    };

    /// Un sólido que se mueve y empuja el agua (un cuerpo, el jugador): esfera en coordenadas locales.
    struct Sphere { glm::vec3 center; float radius; glm::vec3 velocity; };
    static constexpr int kMaxSpheres = 16;

    struct ParticleState { glm::vec3 x; glm::vec3 v; };

    ApicFluid() = default;
    ~ApicFluid() { shutdown(); }
    ApicFluid(const ApicFluid&) = delete;
    ApicFluid& operator=(const ApicFluid&) = delete;

    bool init(RHI::Device* dev, const Config& cfg);
    void shutdown();

    /// Lo que empuja el agua desde fuera, por frame (antes de `step`). `windAccel`: aceleración de
    /// la capa de superficie (m/s², x y z locales) = esfuerzo del viento / (ρ agua · dx).
    void setForcing(const glm::vec2& windAccel, const std::vector<Sphere>& spheres);

    /// Graba `steps` pasos en `ctx` (fuera de un render pass). Cada paso son ~10 + 2·iteraciones
    /// dispatches: quien llama reparte los pasos entre frames (`dispatchesPerStep`).
    void step(RHI::Context* ctx, int steps = 1);
    int  dispatchesPerStep() const { return 9 + 2 * m_cfg.pressureIters; }

    /// Campo de DENSIDAD en los nodos (≈ 1 dentro, 0 fuera) para dibujar la superficie: nodos
    /// `fieldDims()` (x más rápido, z periódico), separados `dx`. Fuera del paso: se llama al dibujar.
    void buildField(RHI::Context* ctx);
    RHI::BufferHandle field() const { return m_field; }
    glm::ivec3 fieldDims() const { return { m_cfg.nx + 1, m_cfg.ny + 1, m_cfg.nz + (m_cfg.periodicZ ? 0 : 1) }; }

    /// Copia las partículas a la CPU (dos frames: copia + espera). Para tests y muestreo.
    bool readParticles(std::vector<ParticleState>& out);

    int particleCount() const { return m_numParticles; }
    double simTime() const { return m_time; }
    const Config& config() const { return m_cfg; }

private:
    void dispatchPass(RHI::Context* ctx, int pass, int color, uint32_t threads);

    RHI::Device* m_dev = nullptr;
    Config m_cfg;
    int m_numParticles = 0;
    int m_perCellSeeded = 8;
    double m_time = 0.0;
    RHI::PipelineHandle m_pipe{};
    void bindAll(RHI::Context* ctx);
    void uploadParams();

    // Un UBO por pase (0..11) + el segundo color de la presión (12).
    static constexpr int kPassUbos = 13;
    RHI::BufferHandle m_params{}, m_passUbo[kPassUbos]{}, m_particles{}, m_accum{}, m_faces{}, m_count{},
                      m_type{}, m_pressure{}, m_rhs{}, m_bed{}, m_state{}, m_nodeAcc{}, m_field{}, m_solids{};
    glm::vec2 m_wind{ 0.0f };
    int m_numSpheres = 0;
};

} // namespace Haruka::Physics::Fluid
