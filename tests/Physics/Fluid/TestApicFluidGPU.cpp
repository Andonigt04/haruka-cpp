// ================================================================================================
// UN LIQUIDO 3D QUE NO SABE QUE ES UNA OLA (Physics/Fluid/ApicFluid + assets/shaders/apic_fluid.comp)
//
// Andoni, 2026-10-19: "los rollos deberian de ser generados por fisicas normal no porque sean olas".
// Se monta un TANQUE DE OLAS de laboratorio: agua quieta, una pala que empuja una vez y una playa en
// pendiente. En el solver no hay rompiente, ni rollo, ni espuma: si el tubo sale, sale de la física.
//
//   1. Quieto: sin pala, el agua se queda quieta (sin corrientes inventadas por la presión) y no
//      pierde ni gana volumen.
//   2. Rompiente: una ola solitaria grande (H/h ≈ 0,4) en una playa 1:15 rompe en VOLUTA (es lo que
//      se mide en tanques: parámetro de pendiente S0 = 1,521·s/√(H/h) ≈ 0,16, voluta entre 0,025 y
//      0,3). Se busca un BOLSILLO DE AIRE bajo un labio de agua: columnas con agua encima de aire.
//   3. Contraprueba: la misma playa con una ola pequeña (H/h ≈ 0,1, S0 ≈ 0,32) sube sin volcar:
//      cero bolsillos. Si el medidor viera tubos en ruido numérico, aquí lo diría.
// ================================================================================================
#include "RHI/Common/RHITestCommon.h"

#include "Haruka/Physics/Fluid/ApicFluid.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
using Haruka::Physics::Fluid::ApicFluid;

/// Avanza `steps` pasos repartidos en frames (el anillo de descriptores de Vulkan es por frame).
void advance(ApicFluid& f, int steps) {
    const int perFrame = std::max(1, 1800 / f.dispatchesPerStep());
    while (steps > 0) {
        const int n = std::min(steps, perFrame);
        if (Context* c = g_dev->beginFrame()) { f.step(c, n); g_dev->endFrame(); pumpWindowEvents(); }
        steps -= n;
    }
}

/// Ocupación en el plano (x, y) sumando el través: celdas con al menos `minCount` partículas.
struct Occupancy {
    int nx = 0, ny = 0;
    std::vector<int> n;
    bool wet(int i, int j, int minCount) const { return i >= 0 && i < nx && j >= 0 && j < ny && n[(size_t)j * nx + i] >= minCount; }
};
Occupancy occupancy(const std::vector<ApicFluid::ParticleState>& ps, const ApicFluid::Config& c) {
    Occupancy o; o.nx = c.nx; o.ny = c.ny; o.n.assign((size_t)c.nx * c.ny, 0);
    for (const auto& p : ps) {
        const int i = (int)std::floor(p.x.x / c.dx), j = (int)std::floor(p.x.y / c.dx);
        if (i >= 0 && i < c.nx && j >= 0 && j < c.ny) ++o.n[(size_t)j * c.nx + i];
    }
    return o;
}

/// El tubo: en una columna, un tramo de AIRE (≥ 2 celdas) con AGUA (≥ 2 celdas) encima, empezando a
/// contar desde el lecho. Devuelve la mayor racha de columnas seguidas con bolsillo (m) y su x.
struct Pocket { double lengthM = 0.0, xM = 0.0, gapM = 0.0; };
Pocket findPocket(const Occupancy& o, const ApicFluid::Config& c, int minCount) {
    Pocket best; int run = 0; double runGap = 0.0;
    for (int i = 0; i < o.nx; ++i) {
        int j = 0;
        while (j < o.ny && (j + 0.5f) * c.dx < c.bed[(size_t)i]) ++j;   // primera celda sobre el lecho
        bool pocket = false; int gapCells = 0;
        while (j < o.ny) {
            while (j < o.ny && o.wet(i, j, minCount)) ++j;               // agua (o nada) de abajo
            int air = 0; while (j < o.ny && !o.wet(i, j, minCount)) { ++air; ++j; }
            int top = 0; while (j < o.ny && o.wet(i, j, minCount)) { ++top; ++j; }
            if (air >= 2 && top >= 2 && j <= o.ny) { pocket = true; gapCells = std::max(gapCells, air); }
            if (top == 0) break;
        }
        if (pocket) { ++run; runGap = std::max(runGap, gapCells * (double)c.dx); }
        else run = 0, runGap = 0.0;
        if (run * (double)c.dx > best.lengthM) best = { run * (double)c.dx, (i + 0.5) * c.dx, runGap };
    }
    return best;
}

/// Cota de la superficie en la columna i (la celda mojada más alta + 1).
double surfaceAt(const Occupancy& o, const ApicFluid::Config& c, int i, int minCount) {
    for (int j = o.ny - 1; j >= 0; --j) if (o.wet(i, j, minCount)) return (j + 1) * (double)c.dx;
    return 0.0;
}

ApicFluid::Config beachTank(float stroke) {
    ApicFluid::Config c;
    c.periodicZ = true;
    c.dx = 0.025f; c.nx = 500; c.ny = 40; c.nz = 4;      // 12,5 m × 1 m × 0,1 m (través periódico)
    c.dt = 0.004f;
    c.fillLevel = 0.5f;                                    // h = 0,5 m en la parte llana
    c.bed.assign((size_t)c.nx * c.nz, 0.0f);
    for (int k = 0; k < c.nz; ++k) for (int i = 0; i < c.nx; ++i) {
        const float x = (i + 0.5f) * c.dx;
        c.bed[(size_t)k * c.nx + i] = x > 2.5f ? (x - 2.5f) / 15.0f : 0.0f;   // playa 1:15
    }
    // Pala de ola solitaria (Goring, aproximada): x(t) = S/2·(1 + tanh(k·c·(t − t0))).
    // Para H/h = 0,4: k = √(3H/4h³) = 1,10 /m, c = √(g(h+H)) = 2,62 m/s → τ = 1/(k c) = 0,35 s;
    // carrera S = 2H/(k h) = 0,73 m. Una carrera menor da una ola menor con la misma pala.
    c.wallStroke = stroke; c.wallT0 = 1.2f; c.wallTau = 0.35f;
    if (const char* e = std::getenv("APIC_IT")) c.pressureIters = std::atoi(e);
    return c;
}

struct BeachRun { Pocket best; double bestT = 0.0; double maxEta = 0.0; int lost = 0; double secs = 0.0; };
BeachRun runBeach(float stroke, double seconds) {
    BeachRun r;
    ApicFluid f;
    const ApicFluid::Config cfg = beachTank(stroke);
    if (!f.init(g_dev, cfg)) return r;
    const int minCount = 8;   // 4 celdas de través × 8 por celda = 32 en reposo
    const int perSample = (int)std::lround(0.05 / cfg.dt);
    const int samples = (int)std::lround(seconds / 0.05);
    const int probeI = (int)(2.0 / cfg.dx);
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<ApicFluid::ParticleState> ps;
    for (int s = 0; s < samples; ++s) {
        advance(f, perSample);
        if (!f.readParticles(ps)) break;
        const Occupancy o = occupancy(ps, cfg);
        r.maxEta = std::max(r.maxEta, surfaceAt(o, cfg, probeI, minCount) - cfg.fillLevel);
        const Pocket p = findPocket(o, cfg, minCount);
        if (p.lengthM > r.best.lengthM) { r.best = p; r.bestT = f.simTime(); }
        // `APIC_DUMP=<carpeta>`: el perfil (x, y) de cada muestra en PGM, para mirarlo a ojo.
        if (const char* d = std::getenv("APIC_DUMP")) {
            char path[512]; std::snprintf(path, sizeof path, "%s/apic_%.2f_%03d.pgm", d, stroke, s);
            if (FILE* fp = std::fopen(path, "wb")) {
                std::fprintf(fp, "P5 %d %d 255\n", o.nx, o.ny);
                for (int j = o.ny - 1; j >= 0; --j) for (int i = 0; i < o.nx; ++i) {
                    const bool bed = (j + 0.5f) * cfg.dx < cfg.bed[(size_t)i];
                    const unsigned char v = bed ? 90 : o.wet(i, j, minCount) ? 255 : 0;
                    std::fputc(v, fp);
                }
                std::fclose(fp);
            }
            std::printf("      t %.2f  bolsillo %.3f m en x %.2f\n", f.simTime(), p.lengthM, p.xM);
        }
    }
    r.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (const auto& p : ps)
        if (!std::isfinite(p.x.x) || p.x.y < -0.01f || p.x.x < -0.01f || p.x.x > cfg.nx * cfg.dx + 0.01f) ++r.lost;
    return r;
}
} // namespace

void testApicFluidStill() {
    BEGIN("liquido APIC: el agua quieta se queda quieta y no pierde volumen (GL y Vulkan)");
    ApicFluid::Config c;
    c.periodicZ = true;
    c.dx = 0.05f; c.nx = 40; c.ny = 24; c.nz = 4; c.dt = 0.005f;
    c.fillLevel = 0.6f;
    if (const char* e = std::getenv("APIC_DG")) c.densityGain = (float)std::atof(e);
    if (const char* e = std::getenv("APIC_IT")) c.pressureIters = std::atoi(e);
    c.bed.assign((size_t)c.nx * c.nz, 0.0f);
    for (int k = 0; k < c.nz; ++k) for (int i = 0; i < c.nx; ++i)
        c.bed[(size_t)k * c.nx + i] = i > 25 ? (i - 25) * c.dx * 0.5f : 0.0f;   // con rampa: el lecho no es solo el suelo
    ApicFluid f;
    const bool ok = f.init(g_dev, c);
    CHECK(ok, "solver creado (pipeline + buffers)");
    if (!ok) return;
    std::vector<ApicFluid::ParticleState> ps0, ps;
    f.readParticles(ps0);
    const Occupancy o0 = occupancy(ps0, c);
    int wet0 = 0; for (int v : o0.n) wet0 += v >= 8;
    advance(f, 400);   // 2 s
    f.readParticles(ps);
    double vmax = 0.0, ymax = 0.0;
    for (const auto& p : ps) { vmax = std::max(vmax, (double)glm::length(p.v)); ymax = std::max(ymax, (double)p.x.y); }
    const Occupancy o = occupancy(ps, c);
    int wet = 0; for (int v : o.n) wet += v >= 8;
    double sMin = 1e9, sMax = 0.0;
    for (int i = 2; i < 24; ++i) { const double s = surfaceAt(o, c, i, 8); sMin = std::min(sMin, s); sMax = std::max(sMax, s); }
    std::printf("    %d partículas · |v| max %.3f m/s · cota max %.3f m · superficie %.3f..%.3f m · celdas mojadas %d -> %d (%.1f %%)\n",
                f.particleCount(), vmax, ymax, sMin, sMax, wet0, wet, 100.0 * (wet - wet0) / std::max(1, wet0));
    CHECK(vmax < 0.15, "a los 2 s nada se mueve más de 0,15 m/s");
    CHECK(ymax < c.fillLevel + 3 * c.dx, "nada salta por encima de la superficie");
    CHECK(sMax - sMin <= 2 * c.dx, "la superficie está plana (±1 celda)");
    CHECK(std::abs(wet - wet0) <= wet0 * 0.05, "el volumen mojado se conserva (±5 %)");
}

void testApicFluidBreaker() {
    BEGIN("liquido APIC: una ola grande en una playa 1:15 rompe en TUBO sin ninguna regla de ola");
    const BeachRun big = runBeach(0.73f, 6.0);
    std::printf("    ola grande: altura en x = 2 m %.3f m (H/h %.2f) · bolsillo %.3f m de largo, %.3f m de alto, en x = %.2f m, t = %.2f s · %.1f s de GPU\n",
                big.maxEta, big.maxEta / 0.5, big.best.lengthM, big.best.gapM, big.best.xM, big.bestT, big.secs);
    CHECK(big.lost == 0, "ninguna partícula se escapa del tanque ni se vuelve NaN");
    CHECK(big.maxEta > 0.12, "la pala levanta una ola grande (H > 0,12 m sobre h = 0,5 m)");
    CHECK(big.best.lengthM >= 0.075, "hay TUBO: agua sobre un bolsillo de aire en >= 3 columnas seguidas");
    CHECK(big.best.xM > 2.5, "el tubo está sobre la playa, no en la parte llana");

    const BeachRun small = runBeach(0.18f, 6.0);
    std::printf("    ola pequeña: altura %.3f m (H/h %.2f) · bolsillo %.3f m · %.1f s de GPU\n",
                small.maxEta, small.maxEta / 0.5, small.best.lengthM, small.secs);
    CHECK(small.lost == 0, "contraprueba: nada se escapa");
    CHECK(small.maxEta > 0.02 && small.maxEta < big.maxEta * 0.5, "contraprueba: la ola es claramente menor");
    CHECK(small.best.lengthM < 0.075, "contraprueba: la ola pequeña NO forma tubo (el medidor no ve tubos en ruido)");
}

// ── 3D DE VERDAD: una playa con un CANAL ─────────────────────────────────────────────────────────
// Un metro de través (40 celdas): el fondo ondula de través, banco en los bordes y canal en medio
// (±7 cm). Un canal estrecho NO sirve: la ola (~3 m) lo rodea y el frente sigue recto — medido.
// Lo que solo sale en 3D: la misma ola rompe ANTES (más mar adentro) sobre los bancos que en el
// canal, porque allí el fondo es más alto. Ninguna parte del solver sabe que hay un canal.
namespace {
void waitGpu() {
    FenceHandle f{};
    if (Context* c = g_dev->beginFrame()) { c->memoryBarrier(); f = c->signalFence(); g_dev->endFrame(); }
    if (Context* c2 = g_dev->beginFrame()) { if (valid(f)) { c2->waitFence(f, 10000000000ull); c2->deleteFence(f); } g_dev->endFrame(); }
}
} // namespace

void testApicFluidChannel() {
    BEGIN("liquido APIC 3D: con bancos y canal de traves la ola rompe antes en los bancos que en el canal");
    ApicFluid::Config c;
    c.periodicZ = true;
    c.dx = 0.025f; c.nx = 460; c.ny = 40; c.nz = 40;          // 11,5 m × 1 m × 1 m
    c.dt = 0.004f;
    c.fillLevel = 0.5f;
    if (const char* e = std::getenv("APIC_IT")) c.pressureIters = std::atoi(e);
    const float zc = 0.5f;
    // Fondo ondulado de través: banco en z = 0 (y 1, periódico), canal en z = 0,5. ±0,07 m.
    auto channel = [&](float z) { return 0.07f * (1.0f - std::cos(6.2831853f * z)) - 0.07f; };
    c.bed.assign((size_t)c.nx * c.nz, 0.0f);
    for (int k = 0; k < c.nz; ++k) for (int i = 0; i < c.nx; ++i) {
        const float x = (i + 0.5f) * c.dx, z = (k + 0.5f) * c.dx;
        const float slope = x > 1.5f ? (x - 1.5f) / 15.0f : 0.0f;
        c.bed[(size_t)k * c.nx + i] = std::max(0.0f, slope - (x > 1.5f ? channel(z) * std::min(1.0f, (x - 1.5f) / 3.0f) : 0.0f));
    }
    c.wallStroke = 0.73f; c.wallT0 = 1.2f; c.wallTau = 0.35f;
    ApicFluid f;
    const bool ok = f.init(g_dev, c);
    CHECK(ok, "solver 3D creado");
    if (!ok) return;

    // Coste por paso, sin lecturas por medio.
    waitGpu();
    const auto t0 = std::chrono::steady_clock::now();
    advance(f, 100);
    waitGpu();
    const double msStep = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 100.0;

    // Hasta el tramo de rompiente sin leer; luego cada 0,05 s, por bandas de través.
    advance(f, (int)std::lround(3.0 / c.dt) - 100);
    // `edge`: la banda da la vuelta por el borde periódico (z < z1 o z > 1 − z1).
    struct Band { const char* name; bool edge; float z0, z1; double firstX = -1.0, firstT = -1.0, best = 0.0; };
    Band bands[2] = { { "bancos", true, 0.0f, 0.1f }, { "canal", false, 0.4f, 0.6f } };
    std::vector<ApicFluid::ParticleState> ps;
    int lost = 0;
    for (int s = 0; s < 50; ++s) {
        advance(f, (int)std::lround(0.05 / c.dt));
        if (!f.readParticles(ps)) break;
        // `APIC_DUMP=<carpeta>`: vista CENITAL (x, z) de la cota de la superficie, en PGM.
        if (const char* d = std::getenv("APIC_DUMP")) {
            std::vector<float> top((size_t)c.nx * c.nz, 0.0f);
            for (const auto& p : ps) {
                const int i = (int)(p.x.x / c.dx), k = (int)(p.x.z / c.dx);
                if (i >= 0 && i < c.nx && k >= 0 && k < c.nz) top[(size_t)k * c.nx + i] = std::max(top[(size_t)k * c.nx + i], p.x.y);
            }
            char path[512]; std::snprintf(path, sizeof path, "%s/apic3d_%03d.pgm", d, s);
            if (FILE* fp = std::fopen(path, "wb")) {
                std::fprintf(fp, "P5 %d %d 255\n", c.nx, c.nz);
                for (int k = 0; k < c.nz; ++k) for (int i = 0; i < c.nx; ++i) {
                    const float h = top[(size_t)k * c.nx + i] - c.bed[(size_t)k * c.nx + i];
                    std::fputc((int)std::clamp(h / 0.5f * 255.0f, 0.0f, 255.0f), fp);
                }
                std::fclose(fp);
            }
        }
        for (Band& b : bands) {
            const int nzBand = (int)std::lround((b.z1 - b.z0) / c.dx);
            std::vector<ApicFluid::ParticleState> sel;
            for (const auto& p : ps) {
                const float z = p.x.z;
                const bool in = b.edge ? (z < b.z1 || z > 1.0f - b.z1) : (z >= b.z0 && z < b.z1);
                if (in) sel.push_back(p);
            }
            ApicFluid::Config cb = c;   // perfil 2D de la banda: lecho de su columna central
            cb.bed.assign((size_t)c.nx, 0.0f);
            const int kc = b.edge ? 0 : (int)(zc / c.dx);
            for (int i = 0; i < c.nx; ++i) cb.bed[(size_t)i] = c.bed[(size_t)kc * c.nx + i];
            const int bandCells = b.edge ? 2 * nzBand : nzBand;
            const Pocket p = findPocket(occupancy(sel, cb), cb, std::max(4, bandCells * 2));
            b.best = std::max(b.best, p.lengthM);
            if (std::getenv("APIC_TRACE")) std::printf("      t %.2f %-6s n %zu bolsillo %.3f en x %.2f\n", f.simTime(), b.name, sel.size(), p.lengthM, p.xM);
            if (b.firstX < 0.0 && p.lengthM >= 0.075) { b.firstX = p.xM; b.firstT = f.simTime(); }
        }
    }
    for (const auto& p : ps) if (!std::isfinite(p.x.x) || p.x.y < -0.01f) ++lost;
    std::printf("    %d partículas · %.2f ms por paso (%dx%dx%d celdas, %d iteraciones de presión)\n",
                f.particleCount(), msStep, c.nx, c.ny, c.nz, c.pressureIters);
    for (const Band& b : bands)
        std::printf("    %-6s: primer tubo en x = %.2f m, t = %.2f s · tubo mayor %.3f m\n", b.name, b.firstX, b.firstT, b.best);
    CHECK(lost == 0, "nada se escapa ni se vuelve NaN");
    CHECK(bands[0].firstX > 0.0 && bands[1].firstX > 0.0, "hay tubo en los bancos y en el canal");
    CHECK(bands[0].firstX > 0.0 && bands[1].firstX > bands[0].firstX + 0.1,
          "en el canal (más hondo) rompe más cerca de la orilla que en los bancos: el 3D sale de la física");
}

// ── LA SUPERFICIE PARA DIBUJAR (Renderer/IsoSurface + assets/shaders/iso_surface.comp) ───────────
// El líquido se dibuja con la malla que sale de SU campo de densidad, no con una ola calculada aparte.
// Agua quieta a 0,6 m sobre un fondo con rampa: la malla tiene que estar a esa cota, mirar arriba,
// no tener cara pegada al lecho (el campo se prolonga bajo él) y con triángulos hacia FUERA.
#include "Haruka/Renderer/IsoSurface.h"

void testApicFluidSurface() {
    BEGIN("liquido APIC: la superficie dibujable sale del campo del propio liquido, a su cota");
    ApicFluid::Config c;
    c.periodicZ = true;
    c.dx = 0.05f; c.nx = 40; c.ny = 24; c.nz = 4; c.dt = 0.005f;
    c.fillLevel = 0.6f;
    c.bed.assign((size_t)c.nx * c.nz, 0.0f);
    for (int k = 0; k < c.nz; ++k) for (int i = 0; i < c.nx; ++i)
        c.bed[(size_t)k * c.nx + i] = i > 25 ? (i - 25) * c.dx * 0.5f : 0.0f;
    ApicFluid f;
    Haruka::Renderer::IsoSurface iso;
    const bool ok = f.init(g_dev, c) && iso.init(g_dev, 300000);
    CHECK(ok, "solver y extractor creados");
    if (!ok) return;
    advance(f, 40);
    Haruka::Renderer::IsoSurface::Grid g;
    g.nodes = f.fieldDims(); g.periodicZ = true; g.cell = c.dx; g.iso = 0.5f;
    if (Context* ctx = g_dev->beginFrame()) { f.buildField(ctx); iso.extract(ctx, f.field(), g); g_dev->endFrame(); }
    std::vector<Haruka::Renderer::IsoSurface::Vertex> v;
    uint32_t n = 0;
    iso.readBack(v, n);
    int up = 0, upOff = 0, down = 0, outward = 0, tris = 0;
    double worst = 0.0;
    for (uint32_t t = 0; t + 2 < n; t += 3) {
        const glm::vec3 a(v[t].p), b(v[t + 1].p), cc(v[t + 2].p);
        const glm::vec3 nn = glm::normalize(glm::vec3(v[t].n) + glm::vec3(v[t + 1].n) + glm::vec3(v[t + 2].n));
        const glm::vec3 fn = glm::cross(b - a, cc - a);
        ++tris; if (glm::dot(fn, nn) > 0.0f) ++outward;
        else if (std::getenv("APIC_TRACE")) std::printf("      mal: a (%.3f %.3f %.3f) b (%.3f %.3f %.3f) c (%.3f %.3f %.3f) n (%.2f %.2f %.2f)\n", a.x, a.y, a.z, b.x, b.y, b.z, cc.x, cc.y, cc.z, nn.x, nn.y, nn.z);
    }
    for (uint32_t i = 0; i < n; ++i) {
        const glm::vec3 p(v[i].p), nn(v[i].n);
        const bool interior = p.x > 3 * c.dx && p.x < (c.nx - 3) * c.dx;
        if (!interior) continue;
        if (nn.y > 0.8f) { ++up; const double e = std::abs(p.y - c.fillLevel); worst = std::max(worst, e); if (e > 0.5 * c.dx) ++upOff; }
        if (nn.y < -0.5f && p.y < c.fillLevel - 2 * c.dx) ++down;
    }
    std::printf("    %u vértices (%d triángulos) · %d mirando arriba, peor cota %.4f m (%d a más de media celda) · %d caras bajo el agua mirando abajo · %d/%d triángulos hacia fuera\n",
                n, tris, up, worst, upOff, down, outward, tris);
    CHECK(n > 0 && n < 300000, "hay malla y cabe");
    CHECK(up > 0 && upOff == 0, "la superficie está a la cota del agua (±media celda)");
    CHECK(down == 0, "no hay cara pegada al lecho");
    CHECK(outward >= tris * 99 / 100, "los triángulos miran hacia fuera (el lado de la normal)");
}

// ── Y SE DIBUJA COMO AGUA ────────────────────────────────────────────────────────────────────────
// La malla del líquido con `iso_mesh.vert` + `water_surface.frag` (la luz del mar): un tanque de
// 2×2 m visto desde arriba llena el cuadro de agua: su color (sin mezcla) es azul y no negro, y con
// la mezcla (60 cm de agua a plomo dejan ver el fondo: opacidad ~0,1, la física del material) tiñe el
// fondo hacia el azul. Contraprueba: el mismo frame sin el dibujo queda del color de limpieza.
#include "Haruka/Core/Camera.h"
#include <glm/gtc/matrix_transform.hpp>

void testApicFluidDraw() {
    BEGIN("liquido APIC: su superficie se dibuja con el material agua (GL y Vulkan)");
    ApicFluid::Config c;
    c.periodicZ = true;
    c.dx = 0.05f; c.nx = 40; c.ny = 20; c.nz = 40; c.dt = 0.005f;
    c.fillLevel = 0.6f;
    c.bed.assign((size_t)c.nx * c.nz, 0.0f);
    ApicFluid f;
    Haruka::Renderer::IsoSurface iso;
    bool ok = f.init(g_dev, c) && iso.init(g_dev, 600000);
    const std::string base = Haruka::Renderer::Shader::baseDir();
    const std::string vs = base + "shaders/iso_mesh.vert", fs = base + "shaders/water_surface.frag";
    PipelineDesc pd; pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    PipelineHandle pipeOpaque = g_dev->createPipeline(pd);
    pd.blend.enable = true;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    ok = ok && valid(pipe) && valid(pipeOpaque);
    CHECK(ok, "solver, extractor y pipeline del agua creados");
    if (!ok) { if (valid(pipe)) g_dev->destroy(pipe); if (valid(pipeOpaque)) g_dev->destroy(pipeOpaque); return; }
    advance(f, 20);

    const int cw = 256, ch = 256;
    const glm::vec3 cam(1.0f, 2.0f, 1.0f);           // 1,4 m sobre el agua, en el centro, mirando abajo
    const double R = 6371000.0;
    setTestSkyLight(glm::dvec3(0.0, -(R + cam.y), 0.0), R, glm::normalize(glm::dvec3(0.3, 0.9, 0.2)));
    const glm::mat4 proj = Haruka::Core::Camera(Haruka::Tools::WorldPos(0.0, 0.0, 0.0)).getProjectionMatrix((float)cw / (float)ch);
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1));
    Haruka::Renderer::IsoSurface::DrawUBO du;
    du.viewProj = proj * view;
    du.originEye = glm::vec4(-cam, 0.0f);
    du.misc = glm::vec4(0.6f, 0, 0, 0);
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(du), &du, BufferMemory::Static);
    Haruka::Renderer::IsoSurface::Grid g;
    g.nodes = f.fieldDims(); g.periodicZ = true; g.cell = c.dx; g.iso = 0.5f;

    auto frame = [&](PipelineHandle drawIt, std::vector<uint8_t>& px) {
        const bool isVk = (g_dev->backend() == Backend::Vulkan);
        px.assign((size_t)cw * ch * 4, 0);
        if (Context* ctx = g_dev->beginFrame()) {
            f.buildField(ctx);
            iso.extract(ctx, f.field(), g);
            ClearValues cv; cv.color[0] = 0.8f; cv.color[1] = 0.1f; cv.color[2] = 0.1f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            if (valid(drawIt)) iso.draw(ctx, drawIt, ubo);
            ctx->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk) g_dev->readPixels(0, 0, cw, ch, Format::RGBA8, px.data());
        }
    };
    auto mean = [&](const std::vector<uint8_t>& px, int ch0) {
        double a = 0.0; int n = 0;
        for (int y = ch / 2 - 40; y < ch / 2 + 40; ++y) for (int x = cw / 2 - 40; x < cw / 2 + 40; ++x) { a += px[((size_t)y * cw + x) * 4 + ch0]; ++n; }
        return a / n;
    };
    std::vector<uint8_t> opaque, on, off;
    frame(pipeOpaque, opaque);
    frame(pipe, on);
    frame(PipelineHandle{}, off);
    const double r1 = mean(opaque, 0), g1 = mean(opaque, 1), b1 = mean(opaque, 2);
    const double rb = mean(on, 0), bb = mean(on, 2);
    const double r0 = mean(off, 0), b0 = mean(off, 2);
    std::printf("    color del agua rgb (%.0f %.0f %.0f) · mezclada sobre el fondo rojo rg-b (%.0f · %.0f) · sin dibujo (%.0f · %.0f)\n",
                r1, g1, b1, rb, bb, r0, b0);
    recordShot("apic_water_top", cw, ch, opaque);
    CHECK(b1 > r1 + 10.0, "el color del agua es azul (más azul que rojo)");
    CHECK(r1 + g1 + b1 > 30.0, "y no negro: le llega la luz del frame");
    CHECK(bb > b0 + 2.0 && rb < r0 - 2.0, "mezclada, tiñe el fondo hacia el azul");
    CHECK(r0 > 150.0 && b0 < 60.0, "contraprueba: sin el dibujo queda el fondo rojo");
    resetTestSkyLight();
    g_dev->destroy(ubo); g_dev->destroy(pipe); g_dev->destroy(pipeOpaque);
}

// ── UN CUERPO QUE CRUZA EL AGUA (esfera sólida móvil, paredes en z, esponja) ─────────────────────
// Lo que un jugador o una piedra le hacen al agua sale del mismo solver: una esfera de 0,3 m que
// avanza a 1,5 m/s semihundida levanta agua DELANTE (proa) y la mueve, y no queda agua dentro de
// ella. Contraprueba: el mismo tanque (paredes y esponja, sin través periódico) sin esfera, quieto.
void testApicFluidBody() {
    BEGIN("liquido APIC: una esfera que cruza el agua la empuja (proa) y no la atraviesa; sin ella, quieta");
    auto run = [&](bool withSphere, double& vmax, double& bow, int& inside) {
        ApicFluid::Config c;
        c.dx = 0.05f; c.nx = 60; c.ny = 20; c.nz = 30; c.dt = 0.005f;   // 3 × 1 × 1,5 m
        c.fillLevel = 0.5f; c.periodicZ = false;
        c.spongeWidth = 0.3f; c.spongeRate = 4.0f;
        c.bed.assign((size_t)c.nx * c.nz, 0.0f);
        ApicFluid f;
        if (!f.init(g_dev, c)) return false;
        const float r = 0.3f, speed = 1.5f;
        glm::vec3 center(0.6f, 0.5f, 0.75f);
        std::vector<ApicFluid::ParticleState> ps;
        vmax = 0.0; bow = 0.0; inside = 0;
        for (int s = 0; s < 120; ++s) {                       // 0,6 s
            std::vector<ApicFluid::Sphere> sp;
            if (withSphere) sp.push_back({ center, r, glm::vec3(speed, 0.0f, 0.0f) });
            f.setForcing(glm::vec2(0.0f), sp);
            if (Context* ctx = g_dev->beginFrame()) { f.step(ctx, 1); g_dev->endFrame(); pumpWindowEvents(); }
            center.x += speed * c.dt;
        }
        f.readParticles(ps);
        // La cota del agua justo DELANTE de la esfera (de 1 a 2 radios), en el plano central.
        double top = 0.0;
        for (const auto& p : ps) {
            vmax = std::max(vmax, (double)glm::length(p.v));
            if (withSphere && glm::distance(p.x, center) < r - 0.5f * c.dx) ++inside;
            const float ahead = p.x.x - center.x;
            if (ahead > r && ahead < 2.0f * r && std::abs(p.x.z - center.z) < 0.1f) top = std::max(top, (double)p.x.y);
        }
        bow = top - c.fillLevel;
        return true;
    };
    double v1, bow1, v0, bow0; int in1, in0;
    const bool ok = run(true, v1, bow1, in1) && run(false, v0, bow0, in0);
    CHECK(ok, "solver creado (paredes en z, esponja, esferas)");
    if (!ok) return;
    std::printf("    con esfera: |v| max %.2f m/s · proa %.3f m sobre el nivel · %d partículas dentro · sin esfera: |v| max %.3f, proa %.3f m\n",
                v1, bow1, in1, v0, bow0);
    CHECK(v1 > 0.5, "la esfera mueve el agua (más de 0,5 m/s)");
    CHECK(bow1 > 0.05, "levanta agua DELANTE (proa > 5 cm)");
    CHECK(in1 == 0, "no queda agua dentro de la esfera");
    CHECK(v0 < 0.1 && std::abs(bow0) < 0.05, "contraprueba: sin esfera, con paredes y esponja, el agua sigue quieta");
}
