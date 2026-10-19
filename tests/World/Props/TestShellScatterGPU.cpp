// ================================================================================================
// LOS RACIMOS EN LA GPU (World/Props/ShellScatter + assets/shaders/shell_scatter.comp): los cúmulos de
// malla.
//
// Montaje: un planeta de radio terrestre, un campo de clima UNIFORME (cobertura 0,35, base 1 500 m,
// techo 2 700 m) y la retícula GIRADA (como la deja el viento). Se siembra y se comprueba:
//   1. CAREO con el gemelo de CPU (`World/ShellClumps.h` + el mismo plan): cada nube que escribe la
//      GPU es una que la ley de CPU pone ahí, con su variante horneada, su escala y su nivel de malla.
//      Si el compute leyera otra celda, otra rotación u otro campo, las nubes no casarían;
//   2. que la panza es PLANA: cada nube apoya su base en la del campo (1 500 m sobre el radio, ±40 m);
//   3. que nada queda detrás de la cámara ni bajo el horizonte;
//   4. EL DIBUJO con el pase de props: hay nube en el cielo, con el Sol detrás de la cámara la cara
//      que se ve está iluminada (si el orden de los triángulos estuviera al revés se verían las caras
//      de DENTRO, a la sombra) y es BLANCA, no gris.
// En GL y en Vulkan: comandos indirectos escritos por el compute y regiones por `baseInstance`.
// ================================================================================================
#include "RHI/Common/RHITestCommon.h"

#include "Haruka/Core/Camera.h"
#include "Haruka/World/Props/ShellScatter.h"
#include "Haruka/World/ShellClumps.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace Haruka::World;
using Props::ShellScatter;

/// Una nube escrita: su base (la instancia), su escala y su región (variante·4 + nivel de malla).
struct Blob { glm::vec3 pos; glm::vec3 scale; int region; };

std::vector<Blob> decodeRegion(const std::vector<uint32_t>& w, int region) {
    std::vector<Blob> out(w.size() / 10);
    for (size_t i = 0; i < out.size(); ++i) {
        std::memcpy(&out[i].pos, &w[i * 10], 12);
        std::memcpy(&out[i].scale, &w[i * 10 + 5], 12);
        out[i].region = region;
    }
    return out;
}

/// El compute, en CPU, con la ley gemela: lo que DEBERÍA haber escrito con lo que usó.
std::vector<Blob> cpuTwin(const ShellScatter::LastUsed& u, const ShellScatter::Config& cfg,
                          const glm::vec4& field, const glm::vec3* planes) {
    std::vector<Blob> out;
    const Props::ShellScatterPlan plan = Props::planShellScatter(u.plan);
    const glm::mat3 rot = glm::mat3(glm::mat3_cast(u.plan.rot));
    const glm::vec3 center = glm::vec3(u.plan.planetC - u.plan.camPos);
    const float R = (float)u.plan.planetR;
    for (const Props::ShellScatterBlock& b : plan.work)
        for (uint32_t li = 0; li < 64; ++li) {
            const uint32_t i = b.bx * 8u + (li & 7u), j = b.by * 8u + (li >> 3);
            if (i >= u.cells || j >= u.cells) continue;
            const uint32_t h = clumpCellHash((int)b.face, i, j, cfg.seed);
            const glm::vec3 dirC = clumpSiteDir((int)b.face, i, j, u.cells, h);
            const glm::vec3 dir = glm::normalize(rot * dirC);
            ClumpField cf; cf.cover = field.x; cf.baseM = field.y; cf.topM = field.z;
            const Clump c = clumpAt(cf, h, (float)u.cellM, -1.0f, cfg.aspectMul);
            if (!c.present) continue;
            const glm::vec3 C = center + dir * (R + field.y + c.baseOffsetM);
            const float H = c.heightM, rw = 0.5f * c.widthM;
            const float d = std::max(glm::length(C), 1.0f);
            const float px = c.widthM * u.focalPx / d;
            if (px < u.cullPx) continue;
            const glm::vec3 mid = C + dir * (0.5f * H);
            const float bound = std::max(rw, 0.5f * H) * 1.2f;
            bool out_ = false;
            for (int p = 0; p < 4; ++p) out_ = out_ || glm::dot(mid, planes[p]) < -bound;
            if (out_) continue;
            {
                const glm::vec3 T = C + dir * H;
                const float tl = glm::length(T);
                const glm::vec3 rd = T / tl;
                const float bq = glm::dot(-center, rd);
                const float cc = glm::dot(center, center) - R * R;
                const float disc = bq * bq - cc;
                if (cc > 0.0f && disc > 0.0f) { const float t0 = -bq - std::sqrt(disc); if (t0 > 0.0f && t0 < tl) continue; }
            }
            // El nivel de malla por el ancho en píxeles (umbrales × el mando) y la variante por el hash.
            const float ls = std::max(u.pxPerBlob / std::max(cfg.pxPerBlobMin, 1e-3f), 1.0f);
            const int lod = (px > cfg.lodPx0 * ls) ? 0 : ((px > cfg.lodPx1 * ls) ? 1 : ((px > cfg.lodPx2 * ls) ? 2 : 3));
            const int variant = (int)((h >> 8) % (uint32_t)ShellScatter::kVariants);
            const float st = std::max(cfg.stretch, 1.0f);
            out.push_back({ C, glm::vec3(rw * st, H / kClumpBakeH, rw / std::sqrt(st)), variant * ShellScatter::kLods + lod });
        }
    return out;
}
} // namespace

void testShellScatterGpu()
{
    BEGIN("cúmulos de malla: la GPU siembra la ley de CPU, panza plana, y se dibujan blancos (GL y Vulkan)");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(0.31, 0.83, -0.46));
    const glm::dvec3 cam = pc + up0 * (R + 2.0);
    const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 1, 0), up0));
    const double pitch = glm::radians(12.0);
    const glm::dvec3 fwd = glm::normalize(east * std::cos(pitch) + up0 * std::sin(pitch));   // al horizonte, algo arriba
    const glm::dvec3 vUp = glm::normalize(up0 - fwd * glm::dot(up0, fwd));

    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const glm::vec4 fieldV(0.35f, 1500.0f, 2700.0f, 0.0f);
    std::vector<float> fieldPx(8 * 4 * 4);
    for (int i = 0; i < 8 * 4; ++i) std::memcpy(&fieldPx[(size_t)i * 4], &fieldV, 16);
    TextureDesc td; td.width = 8; td.height = 4; td.format = Format::RGBA32F;
    td.filter = Filter::Linear; td.wrap = Wrap::Repeat; td.mipmaps = false; td.initialData = fieldPx.data();
    TextureHandle fieldTex = g_dev->createTexture(td);

    ShellScatter s;
    ShellScatter::Config cfg;
    const bool inited = s.init(g_dev, cfg);
    CHECK(inited && valid(fieldTex), "el pase de racimos se crea (compute + pipeline de props + mallas)");
    if (!inited) { if (valid(fieldTex)) g_dev->destroy(fieldTex); return; }

    ShellScatter::Frame f;
    f.camPos = cam; f.planetC = pc; f.planetR = R;
    f.rot = glm::angleAxis(0.37, glm::normalize(glm::dvec3(1.0, 2.0, 3.0)));   // como la deja el viento
    f.viewDir = glm::vec3(fwd); f.viewUp = glm::vec3(vUp);
    f.tanHalfV = (float)std::tan(glm::radians(30.0)); f.aspect = (float)w / (float)h; f.heightPx = h;
    f.field = fieldTex; f.fieldPrev = fieldTex; f.blend = 1.0f;
    f.shellLoM = 1500.0; f.shellHiM = 2700.0 * 1.2;

    // ── 1. LA SIEMBRA, CONTRA EL GEMELO ──────────────────────────────────────────────────────────
    bool dispatched = false;
    if (Context* c = g_dev->beginFrame()) { dispatched = s.dispatch(c, f); g_dev->endFrame(); }
    CHECK(dispatched, "hay bloques que sembrar");
    std::vector<uint32_t> cmd; std::vector<std::vector<uint32_t>> inst;
    const uint32_t total = s.readBack(cmd, inst, cfg.capacity);
    std::vector<Blob> gpu;
    uint32_t wanted = 0;
    for (int r = 0; r < ShellScatter::kRegions; ++r) {
        const auto v = decodeRegion(inst[(size_t)r], r);
        gpu.insert(gpu.end(), v.begin(), v.end());
        if (cmd.size() >= 6u * ShellScatter::kRegions) wanted += cmd[5 * ShellScatter::kRegions + r];
    }
    const ShellScatter::LastUsed& lu = s.lastUsed();
    const std::vector<Blob> cpu = cpuTwin(lu, cfg, fieldV, lu.plan.planes);
    std::printf("    %u bloques · %u celdas por cara (%.0f m) · alcance %.0f km · mando %.1f\n",
                s.lastBlocks(), lu.cells, lu.cellM, lu.plan.reachM / 1000.0, (double)lu.pxPerBlob);
    int perLod[ShellScatter::kLods] = {}, perLodCpu[ShellScatter::kLods] = {};
    for (const Blob& b : gpu) ++perLod[b.region % ShellScatter::kLods];
    for (const Blob& b : cpu) ++perLodCpu[b.region % ShellScatter::kLods];
    std::printf("    nubes GPU %u (pedidas %u) · CPU %zu · por nivel de malla: GPU %d %d %d %d · CPU %d %d %d %d\n",
                total, wanted, cpu.size(), perLod[0], perLod[1], perLod[2], perLod[3],
                perLodCpu[0], perLodCpu[1], perLodCpu[2], perLodCpu[3]);
    CHECK(total > 200, "siembra cientos de nubes (cobertura 0,35 hasta el horizonte)");
    CHECK(total == wanted, "caben (lo pedido = lo escrito)");
    // Cada nube de la GPU tiene su gemelo en CPU: misma región, a menos de 2 m (float a 6e6 m) y
    // con la misma escala.
    std::vector<char> used(cpu.size(), 0);
    int matched = 0; double worstPos = 0.0, worstScale = 0.0;
    for (const Blob& g : gpu) {
        double best = 1e30; int bi = -1;
        for (size_t k = 0; k < cpu.size(); ++k) {
            if (used[k] || cpu[k].region != g.region) continue;
            const double d = glm::length(glm::dvec3(cpu[k].pos - g.pos));
            if (d < best) { best = d; bi = (int)k; }
        }
        if (bi >= 0 && best < 2.0) {
            used[(size_t)bi] = 1; ++matched;
            worstPos = std::max(worstPos, best);
            const glm::vec3 rs = g.scale / glm::max(cpu[(size_t)bi].scale, glm::vec3(1e-3f));
            worstScale = std::max(worstScale, (double)std::max({ std::abs(rs.x - 1.0f), std::abs(rs.y - 1.0f), std::abs(rs.z - 1.0f) }));
        }
    }
    const double frac = gpu.empty() ? 0.0 : (double)matched / (double)std::max(gpu.size(), cpu.size());
    std::printf("    careo: %d de %zu casan (%.2f %%) · peor posición %.3f m · peor escala %.4f relativa\n",
                matched, gpu.size(), 100.0 * frac, worstPos, worstScale);
    CHECK(frac > 0.995, "la GPU siembra lo que dice la ley de CPU (≥ 99,5 % de las nubes casan; el resto son umbrales de píxel en el filo)");
    CHECK(worstScale < 5e-3, "con la misma escala (< 0,5 %)");

    // ── 2-3. PANZA PLANA, DELANTE Y SOBRE EL HORIZONTE ──────────────────────────────────────────
    const glm::vec3 centerRel = glm::vec3(pc - cam);
    double worstBase = 0.0; int behind = 0;
    for (const Blob& g : gpu) {
        const glm::vec3 fromC = g.pos - centerRel;
        const double alt = glm::length(glm::dvec3(fromC)) - R;
        // La instancia ES la base de la nube (la malla horneada tiene la panza en y = 0): ±40 m por racimo.
        worstBase = std::max(worstBase, std::abs(alt - 1500.0));
        if (glm::dot(glm::dvec3(g.pos), fwd) < -5000.0) ++behind;
    }
    std::printf("    peor separación de la base de una nube a la del campo %.2f m · detrás de la cámara %d\n", worstBase, behind);
    CHECK(worstBase < 0.5 * kClumpBaseJitterM + 1.0, "cada nube apoya la panza en la base del campo (±40 m por racimo): la panza es PLANA");
    CHECK(behind == 0, "nada detrás de la cámara");

    // ── 4. EL DIBUJO ─────────────────────────────────────────────────────────────────────────────
    const glm::mat4 proj = Haruka::Core::Camera(Haruka::Tools::WorldPos(0.0, 0.0, 0.0)).getProjectionMatrix((float)w / (float)h);
    const glm::mat4 view = glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vUp))));
    struct PerFrame {
        float view[16], proj[16];
        float camPos[4], sunDir[4], sunColor[3]; float ambientStrength;
        int   hdr, bloom, ssao, ibl, shadows, p3a, p3b, p3c;
        float moonDir[3]; float moonIntensity;
        float moonColor[3]; float pad4;
    } pf{};
    static_assert(sizeof(PerFrame) == 240, "PerFrame debe casar con PerFrameUBOData (std140)");
    std::memcpy(pf.view, &view[0][0], 64); std::memcpy(pf.proj, &proj[0][0], 64);
    // El Sol DETRÁS de la cámara y alto: la cara de las nubes que se ve es la iluminada.
    const glm::dvec3 sunDir = glm::normalize(-fwd * 0.6 + up0 * 0.8);
    pf.sunDir[0] = (float)sunDir.x; pf.sunDir[1] = (float)sunDir.y; pf.sunDir[2] = (float)sunDir.z;
    pf.sunColor[0] = pf.sunColor[1] = pf.sunColor[2] = 1.0f;
    BufferHandle pfUbo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(pf), &pf, BufferMemory::Dynamic);
    setTestSkyLight(pc - cam, R, sunDir);

    // EL TARGET DE ESCENA DEL MOTOR: color + D32F con Z invertida. En el backbuffer de GL (D24) la
    // profundidad de una nube a 150 km redondea a 0 y no pasa el GREATER: faltaban todas las lejanas.
    RenderTargetDesc rd{};
    rd.width = w; rd.height = h;
    rd.colorFormats = { Format::RGBA8 };
    rd.hasDepth = true; rd.depthFormat = Format::D32F;
    RenderPassHandle rt = g_dev->createRenderTarget(rd);
    CHECK(valid(rt), "target de escena D32F creado");
    std::vector<uint8_t> px((size_t)w * h * 4, 0);
    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    for (int frame = 0; frame < 2 && valid(rt); ++frame) {
        Context* c = g_dev->beginFrame();
        if (!c) break;
        s.dispatch(c, f);
        ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
        cv.color[0] = 0.0f; cv.color[1] = 0.0f; cv.color[2] = 1.0f; cv.color[3] = 1.0f;   // azul puro: nada del motor sale así
        c->beginRenderPass(rt, cv);
        c->setViewport(0, 0, w, h);
        s.draw(c, pfUbo, PipelineHandle{}, 0.0f);
        c->endRenderPass();
        ClearValues bb; bb.clearColor = true; bb.clearDepth = true; bb.depth = 0.0f;
        c->beginRenderPass({}, bb);
        c->endRenderPass();
        c->blitColor(rt, {}, w, h);
        if (!isVk && frame == 1) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        g_dev->endFrame(); pumpWindowEvents();
        if (isVk && frame == 1) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
    }
    long cloud = 0; double lum = 0.0, sat = 0.0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = &px[((size_t)y * w + x) * 4];
            // El clear (la copia de Vulkan al swapchain BGRA cruza rojo y azul: vale cualquiera).
            if (p[1] < 8 && std::min(p[0], p[2]) < 8 && std::max(p[0], p[2]) > 247) continue;
            ++cloud;
            lum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
            sat += (double)(std::max({ p[0], p[1], p[2] }) - std::min({ p[0], p[1], p[2] }));
        }
    const double cover = (double)cloud / ((double)w * h);
    const double meanLum = cloud ? lum / cloud : 0.0, meanSat = cloud ? sat / cloud : 0.0;
    std::printf("    dibujo: %.1f %% de la imagen es nube · luminancia media %.0f/255 · saturación media %.0f\n",
                100.0 * cover, meanLum, meanSat);
    recordShot("cumulos de malla", w, h, px);
    CHECK(cover > 0.02, "hay nubes en el cielo");
    // (Era > 150 para cazar triángulos al revés con la luz toon por normal. Con la luz de VOLUMEN el
    //  brillo depende de cuánta nube hay hacia el Sol, y mirando a 12° se ven panzas y flancos: da
    //  148-151 bien orientada. El orden de los triángulos lo mide `blob_surface_winding` en CPU.)
    CHECK(meanLum > 120.0, "con el Sol detrás de la cámara la nube es CLARA (no la cara de dentro, a oscuras)");
    CHECK(meanSat < 60.0, "y blanca, no de color");

    resetTestSkyLight();
    if (valid(rt)) g_dev->destroy(rt);
    g_dev->destroy(pfUbo);
    g_dev->destroy(fieldTex);
    s.shutdown();
}
