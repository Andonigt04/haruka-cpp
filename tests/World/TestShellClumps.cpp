// ================================================================================================
// LOS RACIMOS SOBRE LA CÁSCARA, LA PARTE DE CPU (World/ShellClumps.h): la ley que siembra los cúmulos.
//
// Lo que este banco fija
// ----------------------
//  1. LA LEY DEL TAMAÑO nace en cero (un racimo nuevo crece, no aparece) y su E[f²] es la constante
//     que usa el ancho.
//  2. LA COBERTURA SE CONSERVA: la fracción de suelo que tapan los bultos vista desde arriba es la que
//     pide el campo (c), de 0,1 a 0,9. Es lo que hacía el umbral del campo de ruido de la nube
//     volumétrica, y la razón de `kClumpFill`.
//  3. LA PANZA ES PLANA: ningún bulto baja de la base y los domos la apoyan en ella; nada pasa del
//     techo del racimo.
//  4. SIN TIRONES: si la cobertura sube un poco, cada racimo cambia un poco (ninguno salta de 0 a su
//     tamaño) — el clima se funde entre horneados y eso tiene que verse como crecer, no como aparecer.
//  5. EL PREFIJO del detalle: de lejos menos bultos, y el domo central crece hacia la envolvente.
// ================================================================================================
#include "Common/TestCommon.h"
#include "Haruka/World/ShellClumps.h"
#include "Haruka/World/Props/BlobSurface.h"

#include <glm/gtc/packing.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
using namespace Haruka::World;

/// El racimo de la celda (i, j) de una retícula PLANA de `cellM` (cerca del centro de una cara la
/// retícula del cubo es esto): posición del sitio en metros y su racimo.
struct FlatSite { glm::vec2 pos; Clump c; uint32_t h; };
FlatSite flatSite(int i, int j, float cellM, const ClumpField& f, float widthCells = -1.0f) {
    FlatSite s;
    s.h = clumpCellHash(0, (uint32_t)i, (uint32_t)j, 0x51A7u);
    const float jx = (1.0f - kClumpJitter) * 0.5f + kClumpJitter * clumpHashF(s.h, 1u);
    const float jy = (1.0f - kClumpJitter) * 0.5f + kClumpJitter * clumpHashF(s.h, 2u);
    s.pos = glm::vec2(((float)i + jx) * cellM, ((float)j + jy) * cellM);
    s.c = clumpAt(f, s.h, cellM, widthCells);
    return s;
}
/// ¿Tapa el bulto `b` (racimo en `site`) el punto `p` del plano, visto desde arriba?
bool blobCovers(const glm::vec2& site, const ClumpBlob& b, const glm::vec2& p) {
    glm::vec2 d = p - (site + glm::vec2(b.center.x, b.center.z));
    const float cy = std::cos(b.yaw), sy = std::sin(b.yaw);
    const glm::vec2 l(d.x * cy + d.y * sy, -d.x * sy + d.y * cy);
    const float ex = l.x / b.radii.x, ez = l.y / b.radii.z;
    return ex * ex + ez * ez <= 1.0f;
}

/// Fracción tapada de un parche de `n`×`n` celdas, muestreada a 1/8 de celda.
double coverageOf(const ClumpField& f, float cellM, int n, double* fillOut = nullptr, float widthCells = -1.0f) {
    std::vector<FlatSite> sites;
    const int pad = 4;
    for (int j = -pad; j < n + pad; ++j)
        for (int i = -pad; i < n + pad; ++i) sites.push_back(flatSite(i, j, cellM, f, widthCells));
    const int W = n + 2 * pad;
    auto at = [&](int i, int j) -> const FlatSite& { return sites[(size_t)(j + pad) * W + (i + pad)]; };
    long hit = 0, tot = 0;
    const int sub = 8;
    for (int y = 0; y < n * sub; ++y)
        for (int x = 0; x < n * sub; ++x) {
            const glm::vec2 p(((float)x + 0.5f) * cellM / sub, ((float)y + 0.5f) * cellM / sub);
            const int ci = x / sub, cj = y / sub;
            bool in = false;
            for (int dj = -pad; dj <= pad && !in; ++dj)
                for (int di = -pad; di <= pad && !in; ++di) {
                    const FlatSite& s = at(ci + di, cj + dj);
                    if (!s.c.present) continue;
                    for (int k = 0; k < s.c.blobs && !in; ++k)
                        in = blobCovers(s.pos, clumpBlob(s.c, s.h, k), p);
                }
            hit += in; ++tot;
        }
    if (fillOut) {
        // Lo que tapa cada racimo de SU disco de ancho w (sin vecinos): la `fill` de la ley.
        double fs = 0.0; long fn = 0;
        for (const FlatSite& s : sites) {
            if (!s.c.present || fn > 400) continue;
            const float rw = 0.5f * s.c.widthM;
            long h = 0, t = 0;
            for (int y = -20; y <= 20; ++y)
                for (int x = -20; x <= 20; ++x) {
                    const glm::vec2 o((float)x / 20.0f * rw, (float)y / 20.0f * rw);
                    if (glm::dot(o, o) > rw * rw) continue;
                    bool in = false;
                    for (int k = 0; k < s.c.blobs && !in; ++k) in = blobCovers(s.pos, clumpBlob(s.c, s.h, k), s.pos + o);
                    h += in; ++t;
                }
            fs += (double)h / (double)std::max(t, 1L); ++fn;
        }
        *fillOut = fn ? fs / (double)fn : 0.0;
    }
    return (double)hit / (double)std::max(tot, 1L);
}
} // namespace

void test_clump_size_law() {
    beginTest("clump_size_law");
    bool born = true, mono = true;
    for (float pres : { 0.05f, 0.2f, 0.6f, 1.0f }) {
        born = born && clumpSizeOf(pres, pres) == 0.0f && clumpSizeOf(pres - 1e-6f, pres) < 1e-3f;
        float prev = 1e9f;   // más rango, menos fuerza: el tamaño no crece con u
        for (float u = 0.0f; u < pres; u += pres / 500.0f) { const float f = clumpSizeOf(u, pres); mono = mono && f <= prev + 1e-6f; prev = f; }
    }
    CHECK(born, "un racimo justo en su rango mide 0: nace pequeño, no aparece");
    CHECK(mono, "los de rango bajo son los grandes");
    // E[(min + span·s)²] cerrada frente a la media numérica de los presentes (sin la rampa de nacer).
    double worst = 0.0;
    for (float pres : { 0.05f, 0.15f, 0.25f, 0.5f, 1.0f }) {
        double e2 = 0.0; const int N = 100000;
        for (int i = 0; i < N; ++i) {
            const float u = ((float)i + 0.5f) / (float)N * pres;
            const float sv = 1.0f - u / std::max(pres, kClumpPresFloor);
            const double g = kClumpSizeMin + kClumpSizeSpan * sv; e2 += g * g;
        }
        e2 /= N;
        worst = std::max(worst, std::abs(e2 - clumpSizeE2(pres)) / e2);
    }
    std::printf("    E[f^2] cerrada contra numerica: peor %.2e relativo\n", worst);
    CHECK(worst < 1e-3, "clumpSizeE2 es la integral de la ley");
}

void test_clump_coverage() {
    beginTest("clump_coverage");
    const float cellM = 1500.0f;
    bool ok = true; double fillSum = 0.0; int fillN = 0;
    for (float c : { 0.05f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f }) {
        ClumpField f; f.cover = c; f.baseM = 1500.0f; f.topM = 2600.0f;
        double fill = 0.0;
        const double got = coverageOf(f, cellM, 40, &fill);
        fillSum += fill; ++fillN;
        // CALIBRACION: la tabla que daría EXACTAMENTE c. Con el modelo booleano la cobertura va como
        // 1 − exp(−x), así que la entrada que falta es la de ahora × ln(1−c)/ln(1−tapado). Es de donde
        // salen `kClumpLatticeFix` (y `kClumpFill`, la media de "su disco").
        const double need = (double)clumpLatticeFix(c) * std::log(1.0 - c) / std::log(1.0 - std::min(got, 0.999));
        std::printf("    pide %.2f -> tapa %.3f · su disco %.3f · ancho %.2f celdas · tabla %.3f (la exacta %.3f)\n",
                    c, got, fill, (double)clumpWidthCells(c), (double)clumpLatticeFix(c), need);
        ok = ok && std::abs(got - c) < 0.04;
    }
    std::printf("    fill medido (media) %.3f · constante de la ley %.3f\n", fillSum / fillN, (double)kClumpFill);
    CHECK(ok, "la fracción tapada desde arriba es la que pide el campo (±0,04 de 0,05 a 0,9)");
    CHECK(std::abs(fillSum / fillN - kClumpFill) < 0.03, "kClumpFill es lo que tapan de verdad los bultos de su disco");
}

void test_clump_flat_base() {
    beginTest("clump_flat_base");
    bool noneBelow = true, domesOnBase = true, underTop = true, hasCore = true;
    int clumps = 0, tall = 0;
    for (float c : { 0.2f, 0.6f, 0.95f })
        for (float depth : { 300.0f, 1200.0f, 6000.0f })
            for (int i = 0; i < 400; ++i) {
                ClumpField f; f.cover = c; f.baseM = 1000.0f; f.topM = 1000.0f + depth;
                const uint32_t h = clumpCellHash(1, (uint32_t)i, 7u, 99u);
                const Clump cl = clumpAt(f, h, 1500.0f);
                if (!cl.present) continue;
                ++clumps; tall += cl.heightM > 2.0f * cl.widthM;
                for (int k = 0; k < cl.blobs; ++k) {
                    const ClumpBlob b = clumpBlob(cl, h, k);
                    const float bottom = b.dome ? b.center.y : b.center.y - b.radii.y;
                    noneBelow   = noneBelow && bottom >= -1e-3f;
                    domesOnBase = domesOnBase && (!b.dome || (b.center.y >= 0.0f && b.center.y <= kClumpDomeStep * cl.heightM * kClumpMaxBlobs + 1e-3f));
                    underTop    = underTop && b.center.y + b.radii.y <= cl.heightM * 1.001f + 1e-3f;
                    if (k == 0) hasCore = hasCore && b.dome;
                }
            }
    std::printf("    %d racimos, %d torres (alto > 2 anchos)\n", clumps, tall);
    CHECK(clumps > 1000, "hay racimos que mirar");
    CHECK(noneBelow, "ningún bulto baja de la base");
    CHECK(domesOnBase, "los domos apoyan la tapa EN la base (escalonada unos metros contra el z-fighting): la panza es plana");
    CHECK(underTop, "nada pasa del techo del racimo");
    CHECK(hasCore, "el bulto 0 es el domo central");
    CHECK(tall > 0, "con una capa convectiva de 6 km salen torres");
}

void test_clump_no_pop() {
    beginTest("clump_no_pop");
    // La cobertura sube dc. Entre dos horneados un texel cambia hasta 0,05 (medido, HARUKA_DIAG) y se
    // funde en 2 s: 0,002 son ~5 frames. Lo que separa CRECER de APARECER es la continuidad: con un
    // paso 4 veces menor el salto tiene que ser ~4 veces menor (una aparición mediría lo mismo).
    const float cellM = 1500.0f;
    auto worstJump = [&](float dc) {
        double worst = 0.0;
        for (float c = 0.02f; c < 0.95f; c += 0.01f)
            for (int i = 0; i < 300; ++i) {
                const uint32_t h = clumpCellHash(2, (uint32_t)i, 3u, 5u);
                ClumpField a; a.cover = c; a.baseM = 1000.0f; a.topM = 2200.0f;
                ClumpField b = a; b.cover = c + dc;
                const Clump ca = clumpAt(a, h, cellM), cb = clumpAt(b, h, cellM);
                worst = std::max(worst, (double)std::abs(cb.widthM - ca.widthM));
                worst = std::max(worst, (double)std::abs(cb.heightM - ca.heightM));
            }
        return worst;
    };
    const double big = worstJump(0.002f), small = worstJump(0.0005f);
    std::printf("    peor salto de ancho/alto: %.1f m con +0,002 de cobertura · %.1f m con +0,0005 (celda %.0f m)\n",
                big, small, (double)cellM);
    CHECK(big < 0.12 * cellM, "en ~5 frames de fundido ningún racimo cambia más de una décima de celda");
    CHECK(small < big / 3.0, "y es CONTINUO: con un paso 4 veces menor, el salto es ~4 veces menor (crecen, no aparecen)");
}

void test_blob_surface_winding() {
    beginTest("blob_surface_winding");
    // Las nubes HORNEADAS (`World/Props/BlobSurface`): una superficie cerrada, con los triángulos en
    // sentido antihorario vistos desde FUERA (la normal de cara apunta como el gradiente de la
    // distancia), que es lo que el pase recorta con Back. Y la panza plana: nada por debajo de y = 0.
    using namespace Haruka::World::Props;
    int bad = 0, tris = 0, below = 0;
    for (int v = 0; v < 4; ++v) {
        const uint32_t h = clumpHash(0xC10D0000u + (uint32_t)v * 7919u);
        Clump c; c.present = true; c.widthM = 2.0f; c.heightM = kClumpBakeH; c.strength = 1.0f;
        clumpSetShape(c, h);
        std::vector<ClumpBlob> blobs;
        for (int k = 0; k < c.blobs; ++k) blobs.push_back(clumpBlob(c, h, k));
        BlobSurfaceParams prm; prm.seed = h; prm.cells = 28;
        std::vector<PropVertex> verts; std::vector<uint32_t> idx;
        CHECK(bakeBlobSurface(blobs, prm, verts, idx), "se hornea la nube");
        for (const PropVertex& pv : verts) below += pv.pos.y < -1e-5f;
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            const glm::vec3 a = verts[idx[t]].pos, b = verts[idx[t + 1]].pos, cc = verts[idx[t + 2]].pos;
            const glm::vec3 fn = glm::cross(b - a, cc - a);
            if (glm::dot(fn, fn) < 1e-14f) continue;   // degenerado
            const glm::vec3 vn = glm::vec3(glm::unpackSnorm3x10_1x2(verts[idx[t]].normal))
                               + glm::vec3(glm::unpackSnorm3x10_1x2(verts[idx[t + 1]].normal))
                               + glm::vec3(glm::unpackSnorm3x10_1x2(verts[idx[t + 2]].normal));
            ++tris; bad += glm::dot(fn, vn) < 0.0f;
        }
    }
    std::printf("    %d triángulos · %d al revés · %d vértices bajo la base\n", tris, bad, below);
    CHECK(tris > 1000, "hay triángulos que mirar");
    CHECK(bad == 0, "todos antihorarios vistos desde fuera");
    CHECK(below == 0, "nada por debajo de la base plana");
}

HARUKA_REGISTER_TESTS(
    { "blob_surface_winding", test_blob_surface_winding },
    { "clump_size_law", test_clump_size_law },
    { "clump_coverage", test_clump_coverage },
    { "clump_flat_base", test_clump_flat_base },
    { "clump_no_pop", test_clump_no_pop });
