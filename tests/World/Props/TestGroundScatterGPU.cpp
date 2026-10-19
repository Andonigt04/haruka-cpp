// ================================================================================================
// LA CUBIERTA DENSA EN LA GPU (World/Props/GroundScatter + assets/shaders/ground_scatter.comp).
//
// Montaje: un suelo dibujado a mano —UN nodo hoja bajo la cámara con una rampa de alturas conocida,
// una tabla de materiales de un solo material con hierba 1 y un campo base uniforme— y el compute
// sembrando sobre él. Se leen las instancias y se comprueba:
//   1. que hay elementos y que caben (lo pedido = lo escrito);
//   2. que cada uno está EN LA COTA DIBUJADA (la rampa leída con la bilineal del nodo) menos el
//      hundido de la raíz, al centímetro: si el shader leyera otro téxel u otro nodo, flotarían;
//   3. que el giro es unitario y la vertical del elemento es la del suelo (sin campo ni nieve);
//   4. que nada queda detrás de la cámara ni más allá del alcance que da el umbral;
//   5. que una ZONA destruida queda vacía y el resto no cambia;
//   6. que la siembra está FIJA AL MUNDO: con la cámara 0,3 m más allá, los elementos cercanos están
//      en el mismo sitio al milímetro.
// En GL y en Vulkan: el comando indirecto se escribe desde un SSBO y las instancias se leen como
// stream de vértices, que es exactamente lo que en Vulkan hay que pedir al crear el buffer.
// ================================================================================================
#include "RHI/Common/RHITestCommon.h"

#include "Haruka/World/Planet/GroundView.h"
#include "Haruka/World/Props/GroundScatter.h"
#include "Haruka/World/Props/MassGround.h"
#include "Haruka/World/Terrain/CubeSphere.h"
#include "Haruka/World/Terrain/TerrainNode.h"

#include <glm/gtc/packing.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
using namespace Haruka::World;

struct Inst { glm::vec3 pos; glm::vec4 q; glm::vec3 scale; };
std::vector<Inst> decode(const std::vector<uint32_t>& w) {
    std::vector<Inst> out(w.size() / 10);
    for (size_t i = 0; i < out.size(); ++i) {
        const uint32_t* p = &w[i * 10];
        std::memcpy(&out[i].pos, p, 12);
        const glm::vec2 xy = glm::unpackSnorm2x16(p[3]), zw = glm::unpackSnorm2x16(p[4]);
        out[i].q = glm::vec4(xy, zw);
        std::memcpy(&out[i].scale, p + 5, 12);
    }
    return out;
}
glm::vec3 quatRotate(const glm::vec4& q, const glm::vec3& v) {
    const glm::vec3 t = 2.0f * glm::cross(glm::vec3(q), v);
    return v + q.w * t + glm::cross(glm::vec3(q), t);
}
} // namespace

void testGroundScatterGpu()
{
    BEGIN("cubierta densa: siembra en la cota dibujada, fija al mundo, zonas vacías (GL y Vulkan)");
    const double R = 6371000.0;
    const int    L = 13;                                   // nodo de ~1,2 km: el alcance cabe dentro
    using Haruka::Tools::PlanetFace;
    const PlanetFace face = PlanetFace::FRONT;
    const double lx0 = 0.123, ly0 = -0.087;
    const glm::dvec3 up = glm::normalize(Terrain::cubeFaceToDir(face, lx0, ly0));
    const int N1 = (int)Terrain::TERRAIN_NODE_TEXELS, cells = (int)Terrain::TERRAIN_NODE_CELLS;
    const double cellsL = std::ldexp(1.0, L);
    const int ni = (int)std::floor((lx0 + 1.0) * 0.5 * cellsL), nj = (int)std::floor((ly0 + 1.0) * 0.5 * cellsL);

    // La RAMPA del nodo: h = 12 + 0,02·tu + 0,01·tv (m). Suave (no la corta la pendiente) y asimétrica
    // (un eje cambiado se ve).
    auto rampAt = [&](double tu, double tv) { return 12.0 + 0.02 * tu + 0.01 * tv; };
    std::vector<float> heights((size_t)N1 * N1);
    for (int v = 0; v < N1; ++v) for (int u = 0; u < N1; ++u) heights[(size_t)v * N1 + u] = (float)rampAt(u, v);
    BufferHandle hBuf = g_dev->createBuffer(BufferUsage::Storage, heights.size() * 4, heights.data(), BufferMemory::Static);

    // Un material: hierba 1, bandas que lo cubren todo, ni agua ni lecho.
    struct GpuMat { glm::vec4 a, b, c, d, e, f, g; };
    struct MatTable { glm::vec4 count; GpuMat mats[16]; } mt{};
    mt.count = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    mt.mats[0].a = glm::vec4(-10.0f, 10.0f, -200.0f, 200.0f);
    mt.mats[0].b = glm::vec4(-1.0f, 2.0f, 0.0f, 0.0f);
    mt.mats[0].d = glm::vec4(1.0f, 0.05f, 0.0f, 0.0f);
    mt.mats[0].f = glm::vec4(-100.0f, 100.0f, 1.0f, 0.0f);
    mt.mats[0].g = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    BufferHandle matUbo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(mt), &mt, BufferMemory::Dynamic);

    // Campo base 2×2 × 6 caras: elevación 0, 15 °C, humedad 0,6.
    std::vector<float> fld(2 * 2 * 6 * 4);
    for (size_t i = 0; i < fld.size(); i += 4) { fld[i] = 0.0f; fld[i + 1] = 15.0f; fld[i + 2] = 0.6f; fld[i + 3] = 0.0f; }
    TextureDesc fd; fd.width = fd.height = 2; fd.layers = 6; fd.format = Format::RGBA32F;
    fd.filter = Filter::Nearest; fd.wrap = Wrap::ClampToEdge; fd.initialData = fld.data();
    TextureHandle fieldTex = g_dev->createTexture(fd);

    Planet::GroundView gv;
    gv.planetCenter = glm::dvec3(0.0); gv.planetRadiusM = R; gv.seaLevelM = -1000.0f;
    Terrain::TerrainNodeRenderer::NearNode nn{};
    nn.node[0] = (int32_t)face; nn.node[1] = L; nn.node[2] = ni; nn.node[3] = nj;
    gv.nodes.push_back(nn);
    gv.finestTexelM = (float)(R * 1.5707963267948966 / cellsL / cells);
    gv.heights = hBuf; gv.baseField = fieldTex; gv.materialUBO = matUbo;
    CHECK(valid(hBuf) && valid(matUbo) && valid(fieldTex), "los recursos del suelo de prueba se crean");

    Props::GroundScatter gs;
    Props::GroundScatter::Config cfg;
    cfg.capacity = 200000;
    CHECK(gs.init(g_dev, cfg), "GroundScatter se inicia (compute + buffers)");

    // La cota del nodo en una dirección: la MISMA bilineal que el shader, en double.
    auto drawnAt = [&](const glm::dvec3& dir) {
        PlanetFace f; double lx, ly;
        Terrain::dirToCubeFaceClosed(dir, f, lx, ly);
        const double fx = (lx + 1.0) * 0.5 * cellsL - ni, fy = (ly + 1.0) * 0.5 * cellsL - nj;
        return rampAt(fx * cells, fy * cells);   // la rampa es lineal: la bilineal la da exacta
    };
    const glm::dvec3 east = glm::normalize(glm::cross(up, glm::dvec3(0, 1, 0)));
    auto run = [&](const glm::dvec3& cam, std::vector<glm::vec4> zones, std::vector<uint32_t>& cmd) {
        Props::GroundScatter::Frame f;
        f.camPos = cam; f.viewDir = glm::vec3(east); f.viewUp = glm::vec3(up);
        f.tanHalfV = 0.577f; f.aspect = 1.0f; f.heightPx = 1080;
        f.groundM = drawnAt(up); f.ground = &gv; f.zones = std::move(zones);
        bool ok = false;
        if (Context* c = g_dev->beginFrame()) { ok = gs.dispatch(c, f, 3u); g_dev->endFrame(); }
        std::vector<uint32_t> words;
        if (ok) gs.readBack(cmd, words, cfg.capacity);
        return decode(words);
    };

    const glm::dvec3 cam = up * (R + drawnAt(up) + 1.7);
    std::vector<uint32_t> cmd;
    const std::vector<Inst> a = run(cam, {}, cmd);
    const uint32_t wanted = cmd.size() > 5 ? cmd[5] : 0u;
    std::printf("    %zu instancias (pedidas %u) · alcance %.1f m · %u grupos · %llu hilos\n",
                a.size(), wanted, gs.reachM(), gs.lastGroups(), (unsigned long long)gs.lastThreads());
    CHECK(a.size() > 500, "siembra cientos de elementos alrededor de la cámara");
    CHECK(cmd.size() > 1 && cmd[0] == 3u, "el comando lleva el indexCount de la malla");
    CHECK(wanted == (uint32_t)a.size(), "todo lo pedido cabe (bajo la capacidad)");

    double worstH = 0.0, worstUp = 1.0, worstQ = 0.0, farthest = 0.0, mostBehind = 0.0;
    for (const Inst& e : a) {
        const glm::dvec3 w = cam + glm::dvec3(e.pos);
        const double r = glm::length(w);
        worstH = std::max(worstH, std::abs(r - (R + drawnAt(w / r) - cfg.sinkM)));
        worstQ = std::max(worstQ, std::abs((double)glm::length(e.q) - 1.0));
        worstUp = std::min(worstUp, (double)glm::dot(glm::normalize(quatRotate(glm::normalize(e.q), glm::vec3(0, 1, 0))),
                                                     glm::vec3(w / r)));
        farthest = std::max(farthest, (double)glm::length(e.pos));
        mostBehind = std::min(mostBehind, (double)glm::dot(glm::vec3(e.pos), glm::vec3(east)));
    }
    std::printf("    peor cota %.4f m · peor |q|-1 %.5f · peor vertical %.4f · el más lejano %.1f m · el más atrás %.2f m\n",
                worstH, worstQ, worstUp, farthest, mostBehind);
    CHECK(worstH < 0.02, "cada elemento está en la cota DIBUJADA (−hundido) al centímetro");
    CHECK(worstQ < 1e-3, "el giro es unitario");
    CHECK(worstUp > 0.98, "sin campo ni nieve, el elemento crece hacia arriba");
    CHECK(farthest <= gs.reachM() + 0.5, "nada más allá del alcance que da el umbral");
    // Dentro de 3 m se siembra entero (lo que rodea los pies, que se ve al mirar abajo y da sombra).
    CHECK(mostBehind > -3.0, "nada detrás de la cámara salvo el corro de 3 m de los pies");

    // ── ZONA DESTRUIDA: 6 m alrededor de un punto 15 m por delante ───────────────────────────
    const glm::vec3 zc = glm::vec3(east * 15.0 - up * 1.7);
    const float zr = 6.0f;
    std::vector<uint32_t> cmdZ;
    const std::vector<Inst> z = run(cam, { glm::vec4(zc, zr) }, cmdZ);
    size_t inZoneA = 0, inZoneZ = 0;
    for (const Inst& e : a) inZoneA += glm::length(e.pos - zc) < zr;
    for (const Inst& e : z) inZoneZ += glm::length(e.pos - zc) < zr;
    std::printf("    zona: %zu elementos dentro antes · %zu después · total %zu → %zu\n", inZoneA, inZoneZ, a.size(), z.size());
    CHECK(inZoneA > 20, "la zona tenía hierba (si no, la prueba no prueba nada)");
    CHECK(inZoneZ == 0, "en la zona destruida no queda nada");
    CHECK(z.size() + inZoneA == a.size(), "fuera de la zona no cambia nada");

    // ── FIJA AL MUNDO ────────────────────────────────────────────────────────────────────────
    const glm::dvec3 cam2 = cam + east * 0.3;
    std::vector<uint32_t> cmd2;
    const std::vector<Inst> b = run(cam2, {}, cmd2);
    // Solo los que están bien DENTRO de la vista en las dos posiciones (4-10 m, a menos de 20° del
    // frente): en los bordes del frustum entran y salen elementos, y eso es recorte, no recolocación.
    size_t nearA = 0, matched = 0;
    for (const Inst& e : a) {
        const glm::vec3 hz = e.pos - glm::vec3(up) * glm::dot(e.pos, glm::vec3(up));
        const float dh = glm::length(hz);
        if (dh < 4.0f || dh > 10.0f || glm::dot(hz / dh, glm::vec3(east)) < std::cos(glm::radians(20.0f))) continue;
        ++nearA;
        const glm::dvec3 wa = cam + glm::dvec3(e.pos);
        for (const Inst& f : b)
            if (glm::length(wa - (cam2 + glm::dvec3(f.pos))) < 1e-3) { ++matched; break; }
    }
    std::printf("    fijo al mundo: %zu de %zu elementos cercanos en el mismo sitio con la cámara 0,3 m más allá\n", matched, nearA);
    CHECK(nearA > 50 && matched == nearA, "los elementos cercanos no se mueven al mover la cámara");

    // ── EN LOS TRIANGULOS DE LA MALLA, NO EN EL TEXEL FINO (17-10) ─────────────────────────────
    // "La hierba sobresale del terreno": se apoyaba en la bilineal del mapa en cada téxel, y el nodo
    // se dibuja con ZANCADA. Aquí la rampa lleva baches de 0,40 m en los téxeles IMPARES: con zancada
    // 2 los vértices de la malla caen solo en téxeles pares, así que lo que se DIBUJA es la rampa lisa
    // y la hierba tiene que estar en ella. CONTRAPRUEBA: el mismo nodo con zancada 1 dibuja los
    // baches, y la hierba los sigue.
    std::vector<float> bumpy((size_t)N1 * N1);
    for (int v = 0; v < N1; ++v) for (int u = 0; u < N1; ++u)
        bumpy[(size_t)v * N1 + u] = (float)rampAt(u, v) + (((u | v) & 1) ? 0.40f : 0.0f);
    BufferHandle bBuf = g_dev->createBuffer(BufferUsage::Storage, bumpy.size() * 4, bumpy.data(), BufferMemory::Static);
    gv.heights = bBuf;
    auto worstOffRamp = [&](int strideLog2) {
        gv.nodes[0].slot[1] = strideLog2 << 8;   // kUp 0 · zancada en los bits altos (ver `NearNode`)
        std::vector<uint32_t> c3;
        const std::vector<Inst> e3 = run(cam, {}, c3);
        double worst = 0.0;
        for (const Inst& e : e3) {
            const glm::dvec3 w = cam + glm::dvec3(e.pos);
            const double r = glm::length(w);
            worst = std::max(worst, std::abs(r - (R + drawnAt(w / r) - cfg.sinkM)));
        }
        return std::make_pair(worst, e3.size());
    };
    const auto s2 = worstOffRamp(1), s1 = worstOffRamp(0);
    std::printf("    baches de 0,40 m en los téxeles impares: con zancada 2 la hierba se separa de la rampa %.4f m (%zu) · con zancada 1 %.4f m (%zu)\n",
                s2.first, s2.second, s1.first, s1.second);
    CHECK(s2.second > 500 && s2.first < 0.02, "con zancada 2 la hierba está en la superficie DIBUJADA (la rampa), no en los baches");
    CHECK(s1.first > 0.1, "CONTRAPRUEBA: con zancada 1 la malla dibuja los baches y la hierba los sigue");
    gv.nodes[0].slot[1] = 0;
    g_dev->destroy(bBuf);

    gs.shutdown();
    g_dev->destroy(hBuf); g_dev->destroy(matUbo); g_dev->destroy(fieldTex);
}

// ================================================================================================
// LAS MATAS DE LOS BROTES, SOBRE EL SUELO DIBUJADO (`MassGround` + mass_ground.comp, 17-10).
//
// "La hierba sobresale del terreno": un brote plantaba sus matas en el plano tangente de su centro, a
// la cota del CAMPO en ese centro, sobre celdas de 12 m. Aquí el nodo es una RAMPA (5 %) con baches de
// 0,40 m en los téxeles impares, dibujado con ZANCADA 2: lo que se ve es la rampa lisa. Los brotes
// tienen la cota de la rampa en su centro (como los pone el scatter). Cada mata, con su ajuste, tiene
// que caer EN la rampa: en el plano del brote, las de los bordes se separaban hasta ~0,3 m.
// CONTRAPRUEBA: con zancada 1 la malla dibuja los baches y el ajuste los sigue.
void testMassGroundGpu()
{
    BEGIN("brotes masivos: cada mata sobre el suelo DIBUJADO, no en el plano del brote (GL y Vulkan)");
    const double R = 6371000.0;
    const int    L = 13;
    using Haruka::Tools::PlanetFace;
    const PlanetFace face = PlanetFace::FRONT;
    const double lx0 = 0.123, ly0 = -0.087;
    const glm::dvec3 up = glm::normalize(Terrain::cubeFaceToDir(face, lx0, ly0));
    const int N1 = (int)Terrain::TERRAIN_NODE_TEXELS, cells = (int)Terrain::TERRAIN_NODE_CELLS;
    const double cellsL = std::ldexp(1.0, L);
    const int ni = (int)std::floor((lx0 + 1.0) * 0.5 * cellsL), nj = (int)std::floor((ly0 + 1.0) * 0.5 * cellsL);
    const double texelM = R * 1.5707963267948966 / cellsL / cells;
    // La rampa, en metros por téxel: 5 % de pendiente en `tu`, 2 % en `tv`.
    auto rampAt = [&](double tu, double tv) { return 30.0 + 0.05 * texelM * tu + 0.02 * texelM * tv; };
    std::vector<float> heights((size_t)N1 * N1);
    for (int v = 0; v < N1; ++v) for (int u = 0; u < N1; ++u)
        heights[(size_t)v * N1 + u] = (float)rampAt(u, v) + (((u | v) & 1) ? 0.40f : 0.0f);
    BufferHandle hBuf = g_dev->createBuffer(BufferUsage::Storage, heights.size() * 4, heights.data(), BufferMemory::Static);
    auto rampDir = [&](const glm::dvec3& dir) {
        PlanetFace f; double lx, ly;
        Terrain::dirToCubeFaceClosed(dir, f, lx, ly);
        return rampAt(((lx + 1.0) * 0.5 * cellsL - ni) * cells, ((ly + 1.0) * 0.5 * cellsL - nj) * cells);
    };

    Planet::GroundView gv;
    gv.planetCenter = glm::dvec3(0.0); gv.planetRadiusM = R; gv.seaLevelM = -1000.0f;
    Terrain::TerrainNodeRenderer::NearNode nn{};
    nn.node[0] = (int32_t)face; nn.node[1] = L; nn.node[2] = ni; nn.node[3] = nj;
    gv.nodes.push_back(nn);
    gv.finestTexelM = (float)texelM;
    gv.heights = hBuf;

    // Brotes alrededor de la cámara, con la cota de la rampa en su centro.
    const glm::dvec3 e1 = glm::normalize(glm::cross(up, glm::dvec3(0, 1, 0))), e2 = glm::cross(up, e1);
    std::vector<Props::MassBurst> bursts;
    for (int i = 0; i < 40; ++i) {
        const double a = i * 2.39996, r = 9.0 * std::sqrt((double)i);
        Props::MassBurst b;
        b.dir = glm::vec3(glm::normalize(up * R + e1 * (r * std::cos(a)) + e2 * (r * std::sin(a))));
        b.heightM = (float)rampDir(glm::normalize(glm::dvec3(b.dir)));
        b.cellM = 12.0f; b.seed = 777u + (uint32_t)i; b.count = 200; b.scaleMin = 0.8f; b.scaleMax = 1.2f;
        bursts.push_back(b);
    }
    std::vector<Props::MassBurstGpu> gpu(bursts.size());
    for (size_t i = 0; i < bursts.size(); ++i) {
        uint32_t cellBits; std::memcpy(&cellBits, &bursts[i].cellM, 4);
        gpu[i].a = glm::vec4(0.0f, 0.0f, 0.0f, bursts[i].scaleMin);
        gpu[i].b = glm::vec4(bursts[i].dir, bursts[i].scaleMax);
        gpu[i].c = glm::uvec4(bursts[i].seed, bursts[i].count, Props::kMassNoState, cellBits);
    }
    BufferHandle bBuf = g_dev->createBuffer(BufferUsage::Storage, gpu.size() * sizeof(gpu[0]), gpu.data(), BufferMemory::Static);
    CHECK(valid(hBuf) && valid(bBuf), "los recursos de prueba se crean");

    Props::MassGround mg;
    CHECK(mg.init(g_dev), "MassGround se inicia (compute + buffers)");
    const glm::dvec3 cam = up * (R + rampDir(up) + 1.7);
    Props::MassGroundPlanInput in;
    in.bursts = &bursts; in.planetC = gv.planetCenter; in.planetR = R; in.camPos = cam; in.reachM = 250.0;
    uint32_t total = 0;
    const std::vector<Props::MassGroundJob> jobs = Props::planMassGround(in, total);

    // Para cada mata: cuánto se separa de la RAMPA con el ajuste (y, de referencia, sin él).
    auto measure = [&](int strideLog2, double& withSnap, double& planeOnly, bool& stampsOk) {
        gv.nodes[0].slot[1] = strideLog2 << 8;
        bool ok = false;
        if (Context* c = g_dev->beginFrame()) { ok = mg.dispatch(c, gv, cam, 266.0, bBuf, bursts.size(), jobs, total); g_dev->endFrame(); }
        std::vector<float> snap; std::vector<uint32_t> stamps;
        if (ok) mg.readBack(total, bursts.size(), snap, stamps);
        withSnap = 0.0; planeOnly = 0.0; stampsOk = ok && snap.size() == total && stamps.size() == bursts.size() * 2;
        if (!stampsOk) return;
        for (const Props::MassGroundJob& j : jobs) {
            const Props::MassBurst& b = bursts[j.a[0]];
            stampsOk = stampsOk && stamps[j.a[0] * 2] == j.a[1] && stamps[j.a[0] * 2 + 1] == (uint32_t)mg.frameStamp();
            glm::vec3 u, v; Props::massTangents(b.dir, u, v);
            const glm::dvec3 c = glm::normalize(glm::dvec3(b.dir)) * (R + (double)b.heightM);   // como `rebuildGpu`
            for (uint32_t k = 0; k < j.a[2]; ++k) {
                const Props::MassElement e = Props::massElement(b.seed, k, b.cellM, b.scaleMin, b.scaleMax);
                const glm::dvec3 root = c + glm::dvec3(u) * (double)e.offset.x + glm::dvec3(v) * (double)e.offset.y;
                const double planeH = glm::length(root) - R;                    // la raíz en el plano del brote
                const double groundH = rampDir(glm::normalize(root));           // lo que se dibuja
                withSnap  = std::max(withSnap,  std::abs(planeH + snap[j.a[1] + k] - groundH));
                planeOnly = std::max(planeOnly, std::abs(planeH - groundH));
            }
        }
    };
    double s2, p2, s1, p1; bool ok2, ok1;
    measure(1, s2, p2, ok2);
    measure(0, s1, p1, ok1);
    std::printf("    %zu brotes, %u matas · separación de la rampa: en el plano del brote %.3f m · con el ajuste (zancada 2) %.4f m · con zancada 1 (baches) %.3f m\n",
                jobs.size(), total, p2, s2, s1);
    CHECK(ok2 && ok1, "el compute escribe el ajuste y el sello de cada brote (base y frame)");
    CHECK(p2 > 0.1, "sin ajuste, las matas de los bordes se separan del suelo (si no, la escena no prueba nada)");
    CHECK(s2 < 0.02, "con el ajuste, cada mata está en el suelo DIBUJADO (la rampa lisa de la zancada 2)");
    CHECK(s1 > 0.1, "CONTRAPRUEBA: con zancada 1 la malla dibuja los baches y el ajuste los sigue");

    mg.shutdown();
    g_dev->destroy(hBuf); g_dev->destroy(bBuf);
}
