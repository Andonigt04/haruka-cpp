#include "Haruka/Core/Modules.h"   // HARUKA_MOD_FLUIDS: el solver es del módulo de fluidos
#ifdef HARUKA_MOD_FLUIDS
#include "Haruka/World/Water/LiquidPatch.h"

#include "Haruka/Core/Camera.h"
#include "Haruka/Core/Logger.h"
#include "Haruka/Physics/PhysicsEngine.h"
#include "Haruka/Renderer/Shader.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/RHI/RHIGPUScope.h"
#include "Haruka/Tools/Profiler.h"
#include "Haruka/World/Planet/Planet.h"
#include "Haruka/World/Planet/PlanetarySystem.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace Haruka::World::Water {

namespace {
constexpr double kHeadroomM   = 1.0;     ///< aire sobre el reposo: lo que pueden subir olas y salpicadura
constexpr double kMaxDepthM   = 1.5;     ///< agua simulada bajo el reposo, como mucho: más hondo, un fondo
                                         ///< (las olas de hasta ~3 m de largo no lo notan); medido: con
                                         ///< 2,5 m el coste se iba en la columna y el parche era de 2,6 m
constexpr int    kMaxNy       = 40;      ///< celdas en vertical como mucho (lo hondo se corta con un fondo)
constexpr int    kPressureIt  = 40;
constexpr int    kMaxSubsteps = 2;       ///< más lento de lo que pide el frame → el agua va a cámara lenta
constexpr double kRebuildEvery = 2.0;    ///< s entre re-siembras, como mucho
constexpr double kRhoAirCd    = 1.225 * 1.3e-3;   ///< ρ_aire · C_d del esfuerzo del viento sobre el agua
} // namespace

bool LiquidPatch::ensureGpu() {
    if (m_gpuReady) return true;
    RHI::Device* dev = RHI::device();
    if (!dev) return false;
    if (!m_iso.init(dev, 1500000)) return false;
    const std::string base = Renderer::Shader::baseDir();
    const std::string vs = base + "shaders/iso_mesh.vert", fs = base + "shaders/water_surface.frag";
    RHI::PipelineDesc pd;
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.blend.enable = true;
    pd.depth.write = false;       // transparente: no tapa lo que se dibuje detrás después
    m_pipe = dev->createPipeline(pd);
    m_drawUbo = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(Renderer::IsoSurface::DrawUBO), nullptr, RHI::BufferMemory::Dynamic);
    m_gpuReady = RHI::valid(m_pipe) && RHI::valid(m_drawUbo);
    return m_gpuReady;
}

void LiquidPatch::disable(Planet::PlanetarySystem* planets) {
    if (!m_active && !m_sim) return;
    if (planets) if (auto* tp = planets->activeTerrestrialMut()) tp->setLiquidClip(false, {}, {1, 0, 0}, {0, 0, 1}, 0.0, 0.0);
    m_sim.reset();
    m_active = false;
    m_cells = 0; m_msSmooth = 0.0;
}

void LiquidPatch::shutdown() {
    m_sim.reset();
    m_iso.shutdown();
    if (RHI::Device* dev = RHI::device()) {
        if (RHI::valid(m_pipe)) dev->destroy(m_pipe);
        if (RHI::valid(m_drawUbo)) dev->destroy(m_drawUbo);
    }
    m_pipe = {}; m_drawUbo = {};
    m_gpuReady = m_active = false;
}

bool LiquidPatch::rebuild(const Frame& f, float dx, int nCells) {
    m_active = false;
    glm::dvec3 pc; double R = 0.0;
    if (!f.planets || !f.planets->getActivePlanet(pc, R) || R <= 0.0) return false;
    const glm::dvec3 cam(f.camera->position);
    const glm::dvec3 up = glm::normalize(cam - pc);
    // El NIVEL DE REPOSO: un lago horneado bajo la cámara (su lámina, > 0), o el mar CON SU MAREA.
    // Con la cota 0 fija, en marea baja el parche era un bloque de agua 0,5 m sobre la arena seca.
    const auto s = f.planets->sampleSurface(cam);
    const bool lake = s.waterType != decltype(s.waterType)::None && std::isfinite(s.waterLevelKm) && s.waterLevelKm > 0.0f;
    const double level = lake ? (double)s.waterLevelKm * 1000.0 : (double)f.planets->oceanState().seaLevelM;

    const int ny = std::clamp((int)std::ceil((kHeadroomM + kMaxDepthM) / dx), 12, kMaxNy);
    const double depth = ny * (double)dx - kHeadroomM;          // agua que cabe bajo el reposo
    const int n = std::clamp((int)std::sqrt((double)nCells / ny), 16, 400);
    const double side = n * (double)dx;

    // El marco: Y = vertical del sitio, X y Z en el plano tangente. A 400 celdas de 0,6 m la
    // curvatura baja 2 cm en el borde: se ignora.
    const glm::dvec3 ref = std::abs(up.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 X = glm::normalize(glm::cross(ref, up));
    const glm::dvec3 Z = glm::cross(X, up);
    const glm::dvec3 foot = pc + up * (R + level);
    const glm::dvec3 origin = foot - X * (side * 0.5) - Z * (side * 0.5) - up * depth;

    Physics::Fluid::ApicFluid::Config c;
    c.nx = n; c.nz = n; c.ny = ny; c.dx = dx;
    c.dt = std::min(1.0f / 60.0f, 0.5f * dx / 3.0f);            // CFL 0,5 con ~3 m/s de agua
    c.pressureIters = kPressureIt;
    c.periodicZ = false;
    c.spongeWidth = (float)(side * 0.15); c.spongeRate = 3.0f;
    c.fillLevel = (float)depth;
    c.bed.assign((size_t)n * n, 0.0f);
    const auto sampler = f.planets->terrainSampler(foot);
    if (!sampler) return false;
    int wetColumns = 0;
    for (int k = 0; k < n; ++k) for (int i = 0; i < n; ++i) {
        const glm::dvec3 p = origin + X * ((i + 0.5) * dx) + Z * ((k + 0.5) * dx) + up * depth;
        const double h = sampler.heightAt(p, dx) - (level - depth);   // cota del suelo sobre el fondo del parche
        const float b = (float)std::clamp(h, 0.0, ny * (double)dx);
        c.bed[(size_t)k * n + i] = b;
        if (b < c.fillLevel - dx) ++wetColumns;
    }
    if (wetColumns == 0) { m_lastBuild = m_clock; return false; }   // aquí no hay agua: nada que simular

    if (!m_sim) m_sim = std::make_unique<Physics::Fluid::ApicFluid>();
    if (!m_sim->init(RHI::device(), c)) { m_sim.reset(); return false; }
    m_origin = origin; m_axisX = X; m_axisY = up; m_axisZ = Z;
    m_dx = dx; m_cells = n * n * ny; m_depthM = depth;
    m_lastBuild = m_clock; m_acc = 0.0;
    m_active = true;
    HARUKA_LOGI("Liquido", "parche %dx%dx%d celdas de %.2f m (%.0f m de lado, %.1f m de agua bajo el reposo) · %d partículas · %d columnas mojadas",
                n, ny, n, dx, side, depth, m_sim->particleCount(), wetColumns);
    return true;
}

void LiquidPatch::prepare(RHI::Context* ctx, const Frame& f) {
    if (!ctx || !f.camera || !f.planets) return;
    if (!ensureGpu()) return;
    HARUKA_PROFILE("liquid.prepare");
    m_clock += std::min((double)f.dt, 0.1);

    // ── EL LAZO: ms de GPU del líquido entero → celdas ──────────────────────────────────────────
    // MULTIPLICATIVO y solo con medidas DEL PARCHE QUE HAY: tras re-sembrar se espera `kSettleS`
    // antes de creerse el tiempo. El `LoadController` aditivo oscilaba aquí (95² → 22² → 110² en 6 s):
    // el error llega a +180 % y su paso lineal saltaba al mínimo; y medía aún el parche anterior.
    if (RHI::Device* dev = RHI::device()) {
        double ms = 0.0;
        for (const auto& s : dev->gpuScopes())
            if (s.name == "liquid.step" || s.name == "liquid.surface" || s.name == "liquid.draw") ms += s.ms;
        if (m_active && ms > 0.0 && m_clock - m_lastBuild > kSettleS)
            m_msSmooth = (m_msSmooth <= 0.0) ? ms : m_msSmooth + (ms - m_msSmooth) * 0.1;
    }

    // ── EL TAMAÑO DE CELDA, POR PÍXELES: lo que mide una celda a la altura del ojo sobre el agua ──
    glm::dvec3 pc; double R = 0.0;
    if (!f.planets->getActivePlanet(pc, R)) return;
    const glm::dvec3 cam(f.camera->position);
    const double alt = std::max(2.0, glm::length(cam - pc) - R);
    const double radPerPx = glm::radians((double)f.camera->zoom) / std::max(1, f.viewH);
    const float dxWant = (float)std::clamp(kCellPx * alt * radPerPx, 0.12, 0.6);
    // Lo que cuesta va ~lineal con lo simulado: se escala por (presupuesto/medido)^0,7 (amortiguado).
    int cellsWant = m_cells > 0 ? m_cells : kStartCells;
    if (m_active && m_msSmooth > 0.0)
        cellsWant = (int)std::clamp(m_cells * std::pow(kBudgetMs / m_msSmooth, 0.7), m_cells * 0.5, m_cells * 2.0);
    cellsWant = std::clamp(cellsWant, kMinCells, kMaxCells);

    // ¿Recolocar? Lejos del centro, o el tamaño que se pide difiere bastante del que hay.
    bool want = !m_active;
    if (m_active) {
        const glm::dvec3 center = m_origin + (m_axisX + m_axisZ) * (0.5 * m_dx * m_sim->config().nx) + m_axisY * m_depthM;
        const glm::dvec3 d = cam - center;
        const double horiz = glm::length(d - m_axisY * glm::dot(d, m_axisY));
        const double side = m_dx * m_sim->config().nx;
        want = horiz > 0.25 * side
            || std::abs(dxWant - m_dx) > 0.25f * m_dx
            || cellsWant > m_cells * 1.25 || cellsWant < m_cells * 0.8;
    }
    if (want && m_clock - m_lastBuild > kRebuildEvery) { if (rebuild(f, dxWant, cellsWant)) m_msSmooth = 0.0; }
    // UN AGUA EN CADA SITIO: dentro del parche no se dibuja la del pase de nodos.
    if (auto* tp = f.planets->activeTerrestrialMut()) {
        const double side = m_active ? m_dx * (double)m_sim->config().nx : 0.0;
        tp->setLiquidClip(m_active, m_origin, m_axisX, m_axisZ, side, side);
    }
    if (!m_active) return;

    // ── LO QUE LO EMPUJA ──
    const auto& cfg = m_sim->config();
    const glm::dvec3 w(f.wind);
    const glm::dvec2 U(glm::dot(w, m_axisX), glm::dot(w, m_axisZ));
    const glm::vec2 windAccel = glm::vec2(kRhoAirCd * glm::length(U) * U / (1000.0 * cfg.dx));
    std::vector<Physics::Fluid::ApicFluid::Sphere> spheres;
    if (f.physics) {
        const double side = cfg.nx * (double)cfg.dx, height = cfg.ny * (double)cfg.dx;
        for (const auto& b : f.physics->getBodies()) {
            if (!b || b->anchored || b->radius <= 0.0) continue;
            const glm::dvec3 rel = b->position - m_origin;
            const glm::dvec3 l(glm::dot(rel, m_axisX), glm::dot(rel, m_axisY), glm::dot(rel, m_axisZ));
            const double r = b->radius;
            if (l.x < -r || l.z < -r || l.x > side + r || l.z > side + r || l.y < -r || l.y > height + r) continue;
            spheres.push_back({ glm::vec3(l), (float)r,
                                glm::vec3((float)glm::dot(b->velocity, m_axisX), (float)glm::dot(b->velocity, m_axisY),
                                          (float)glm::dot(b->velocity, m_axisZ)) });
            if ((int)spheres.size() >= Physics::Fluid::ApicFluid::kMaxSpheres) break;
        }
    }
    m_sim->setForcing(windAccel, spheres);

    // ── EL PASO: tiempo real, con tope (si no da, cámara lenta en vez de hundir el frame) ──
    m_acc += std::min((double)f.dt, 0.1);
    int steps = (int)std::floor(m_acc / cfg.dt);
    if (steps > kMaxSubsteps) { steps = kMaxSubsteps; m_acc = 0.0; }
    else m_acc -= steps * (double)cfg.dt;
    { HARUKA_GPU_SCOPE("liquid.step"); if (steps > 0) m_sim->step(ctx, steps); }
    { HARUKA_GPU_SCOPE("liquid.surface");
      m_sim->buildField(ctx);
      Renderer::IsoSurface::Grid g;
      g.nodes = m_sim->fieldDims(); g.periodicZ = false; g.cell = cfg.dx; g.iso = 0.5f;
      g.openSides = true;   // el agua sigue fuera del parche: sin paredes de superficie en x/z
      m_iso.extract(ctx, m_sim->field(), g); }

    if (m_clock - m_lastLog > 5.0 && Core::diagLogs()) {
        m_lastLog = m_clock;
        const int64_t verts = m_iso.pollCount(ctx);
        HARUKA_LOGDIAG("Liquido", "%.2f ms GPU (media, presupuesto %.1f) · celda %.2f m · %d celdas · %d partículas · %d esferas · %d pasos · superficie %lld vértices (de una lectura anterior)",
                       m_msSmooth, kBudgetMs, m_dx, m_cells, m_sim->particleCount(), (int)spheres.size(), steps, (long long)verts);
    }
}

void LiquidPatch::draw(RHI::Context* ctx, const Frame& f) {
    if (!ctx || !m_active || !f.camera) return;
    HARUKA_PROFILE("liquid.draw"); HARUKA_GPU_SCOPE("liquid.draw");
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    const glm::dvec3 cam(f.camera->position);
    Renderer::IsoSurface::DrawUBO u;
    const glm::mat4 view = f.camera->getViewMatrix();
    u.viewProj = f.camera->getProjectionMatrix(f.aspect) * glm::mat4(glm::mat3(view));
    u.originEye = glm::vec4(glm::vec3(m_origin - cam), 0.0f);
    u.axisX = glm::vec4(glm::vec3(m_axisX), 0.0f);
    u.axisY = glm::vec4(glm::vec3(m_axisY), 0.0f);
    u.axisZ = glm::vec4(glm::vec3(m_axisZ), 0.0f);
    static const bool s_debug = std::getenv("HARUKA_LIQUID_DEBUG") != nullptr;
    u.misc = glm::vec4((float)m_depthM, s_debug ? 1.0f : 0.0f, 0.0f, 0.0f);
    dev->updateBuffer(m_drawUbo, 0, sizeof(u), &u);
    m_iso.draw(ctx, m_pipe, m_drawUbo);
}

} // namespace Haruka::World::Water

#endif // HARUKA_MOD_FLUIDS
