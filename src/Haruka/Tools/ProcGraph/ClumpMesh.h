#pragma once

// ===========================================================================
// CÚMULO de lejanía: un "prop ya fundido" en una sola malla, para CUALQUIER forma (árbol,
// hierba, roca y casa son el mismo scatter; el individuo lejano es sub-píxel y el cull lo tira,
// así que el horizonte se quedaba en hierba rala y rocas que se desvanecían a 3 km). El cúmulo
// sustituye a TODO lo individual de una celda (40-120 m según banda) por una masa de su MISMO
// tipo: un bosque (20-28 árboles), un pedregal (cantos medio enterrados), una aldea (casitas con
// tejado) o una pradera (matas bajas). A esa distancia la masa lee como la forma ya pobladísima.
//
// DETALLE: la masa debe leer como un DESCALADO DE RESOLUCIÓN de la forma real, no como un bloque
// liso — copas pequeñas separadas (cielo entre medias), techo con jitter (dentellado), tinte por
// individuo (ruido de color), y coníferas como mástiles/agujas por no parecer un pan verde. La
// silueta lejana (tier 3) es esto mismo a detalle bajo: menos individuos, sin satélites.
//
// Determinista (mismo seed ⇒ misma masa, paridad con el resto del grafo) y APLANADO a propósito:
// la escala de instancia es uniforme (`io.scale`), y quien la escribe estira la celda por el
// ancho usando el footprint nominal `kClumpFootprintM`; si el bake fuera alto también, un cúmulo
// de 40 m de celda parecería un rascacielos. El bake nace bajo y ancho.
//
// ⚠️ NO ES un prop individual: no tiene esqueleto rompible por partes y no depende de
// `TreeMesh.h` (que está clavado por el test de compatibilidad del LOD).
// ===========================================================================

#include "TreeMesh.h"   // TreeMeshData, TreeStyle
#include "ProcNoise.h"  // WhiteNode::hashFloat

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Haruka::Tools {  namespace ProcGraph {

/** @brief Ancho nominal de la masa (m). Quien la instancia escala `celda/este`.
 *  Lo usa también el collider del cúmulo (una sola caja envolvente). */
inline constexpr float kClumpFootprintM = 30.0f;

/**
 * @brief Hornea la masa del cúmulo, base en y=0 en el origen.
 *
 * Parámetros de la vegetación base (los que `propTreeParams` deriva del nombre):
 * `style` silueta, `heightM` alto del árbol individual, `trunkR` grosor de fuste
 * y `canopy` factor de copa. `footprintM` gobierna la extensión horizontal. El
 * detalle recorta individuos y densidad (el LOD por píxel del prop).
 *
 * `shapeKind` = la forma del prop BASE (0 árbol/hierba, 1 roca, 2 casa): cada
 * forma hornea su propia masa (bosque, pedregal, aldea/racimo de tejados). Es
 * como `PropShapeKind`, pero con un int: esta cabecera (herramienta) no depende
 * del mundo.
 */
inline TreeMeshData bakeClumpMesh(uint32_t seed, TreeStyle style, float heightM,
                                  float trunkR, float canopyScale, float footprintM,
                                  float detail, int shapeKind = 0) {
    TreeMeshData m;
    const float d   = std::clamp(detail, 0.05f, 1.0f);
    const float kPi = 3.14159265358979323846f;

    struct Ctx {
        TreeMeshData& m;
        std::vector<glm::vec3> hints;   // referencia radial (igual que el árbol)
        size_t addV(const glm::vec3& p, const glm::vec3& hint,
                    const glm::vec2& uv, unsigned char mat, const glm::vec3& color) {
            m.positions.push_back(p);
            m.normals.push_back(glm::vec3(0.0f));
            m.uvs.push_back(uv);
            m.materialId.push_back(mat);
            m.colors.push_back(color);
            m.partId.push_back(0);
            hints.push_back(hint);
            return m.positions.size() - 1;
        }
        void addTri(size_t a, size_t b, size_t c) {
            const glm::vec3& va = m.positions[a];
            const glm::vec3& vb = m.positions[b];
            const glm::vec3& vc = m.positions[c];
            glm::vec3 n = glm::cross(vb - va, vc - va);
            glm::vec3 hintSum = hints[a] + hints[b] + hints[c];
            glm::vec3 hint = glm::length(hintSum) > 1e-6f ? glm::normalize(hintSum)
                                                         : glm::vec3(0, 1, 0);
            if (glm::dot(n, hint) < 0.0f) {
                std::swap(b, c);
                n = -n;
            }
            m.indices.push_back((unsigned)a);
            m.indices.push_back((unsigned)b);
            m.indices.push_back((unsigned)c);
            m.normals[a] += n;
            m.normals[b] += n;
            m.normals[c] += n;
        }
        void addQuad(size_t a, size_t b, size_t c, size_t d) {
            addTri(a, b, c);
            addTri(a, c, d);
        }
    } ctx{m};

    // Misma paleta que el árbol de la capa: que el cúmulo sea LA MASA de esa vegetación.
    const glm::vec3 baseBark(0.42f, 0.30f, 0.20f);
    const glm::vec3 baseFoliage(0.24f, 0.42f, 0.17f);
    glm::vec3 barkCol = baseBark, foliageCol = baseFoliage;
    switch (style) {
        case TreeStyle::Conifer: foliageCol = glm::vec3(0.14f, 0.32f, 0.16f); barkCol = glm::vec3(0.36f, 0.24f, 0.16f); break;
        case TreeStyle::Palm:    foliageCol = glm::vec3(0.30f, 0.50f, 0.18f); barkCol = glm::vec3(0.50f, 0.40f, 0.26f); break;
        case TreeStyle::Shrub:   foliageCol = glm::vec3(0.30f, 0.46f, 0.18f); barkCol = glm::vec3(0.30f, 0.40f, 0.20f); break;
        case TreeStyle::Grass:   foliageCol = glm::vec3(0.36f, 0.56f, 0.20f); break;
        case TreeStyle::Dead:
        case TreeStyle::Cactus:
        default: break;
    }

    // LOD por píxel: recorta árboles y enjaula la esfera y el cono (igual que el árbol).
    const int segsLod = std::max(3, (int)std::lround(6.0f * d));
    const int lat     = std::max(2, (int)std::lround(3.0f * d));
    const int lon     = std::max(3, segsLod);

    auto appendCone = [&](const glm::vec3& base, const glm::vec3& top,
                          float baseR, float topR,
                          unsigned char mat, const glm::vec3& color) {
        glm::vec3 dd = top - base;
        float len = glm::length(dd);
        if (len < 1e-6f) return;
        glm::vec3 axis = dd / len;
        glm::vec3 ref = (std::fabs(axis.y) < 0.99f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        glm::vec3 u = glm::normalize(glm::cross(ref, axis));
        glm::vec3 v = glm::cross(axis, u);
        std::vector<size_t> ringA, ringB;
        for (int j = 0; j < segsLod; ++j) {
            float ang = 2.0f * kPi * (float)j / (float)segsLod;
            float co = std::cos(ang), sn = std::sin(ang);
            glm::vec3 pa = base + (u * co + v * sn) * baseR;
            glm::vec3 pb = top  + (u * co + v * sn) * topR;
            ringA.push_back(ctx.addV(pa, glm::normalize(pa - base), glm::vec2(0.0f), mat, color));
            ringB.push_back(ctx.addV(pb, glm::normalize(pb - top),  glm::vec2(1.0f), mat, color));
        }
        for (int j = 0; j < segsLod; ++j) {
            int j2 = (j + 1) % segsLod;
            ctx.addQuad(ringA[j], ringA[j2], ringB[j2], ringB[j]);
        }
    };

    auto appendSphere = [&](const glm::vec3& center, float radius,
                            float squashY, unsigned char mat,
                            const glm::vec3& color) {
        size_t topV = ctx.addV(center + glm::vec3(0, radius * squashY, 0),
                               glm::vec3(0, 1, 0), glm::vec2(0.5f, 1.0f), mat, color);
        std::vector<std::vector<size_t>> rings(std::max(1, lat - 1));
        for (int i = 1; i < lat; ++i) {
            float theta = kPi * (float)i / (float)lat;
            float r = radius * std::sin(theta);
            float y = radius * std::cos(theta) * squashY;
            for (int j = 0; j < lon; ++j) {
                float phi = 2.0f * kPi * (float)j / (float)lon;
                glm::vec3 p(center.x + std::cos(phi) * r, center.y + y,
                            center.z + std::sin(phi) * r);
                glm::vec3 hint = glm::normalize(p - center);
                rings[i - 1].push_back(ctx.addV(p, hint, glm::vec2(0.5f, 0.5f), mat, color));
            }
        }
        size_t botV = ctx.addV(center - glm::vec3(0, radius * squashY, 0),
                               glm::vec3(0, -1, 0), glm::vec2(0.5f, 0.0f), mat, color);
        for (int j = 0; j < lon; ++j) {
            int j2 = (j + 1) % lon;
            ctx.addTri(topV, rings[0][j2], rings[0][j]);
            for (int i = 1; i < lat - 1; ++i)
                ctx.addQuad(rings[i - 1][j], rings[i - 1][j2], rings[i][j2], rings[i][j]);
            ctx.addTri(botV, rings[lat - 2][j], rings[lat - 2][j2]);
        }
    };

    auto appendBlade = [&](const glm::vec3& root, const glm::vec3& tip, const glm::vec3& side,
                           float widthRoot, float widthTip, unsigned char mat,
                           const glm::vec3& color) {
        const glm::vec3 n = glm::normalize(glm::cross(tip - root, side));
        for (int face = 0; face < 2; ++face) {
            const glm::vec3 hint = face == 0 ? n : -n;
            const size_t a = ctx.addV(root - side * (widthRoot * 0.5f), hint, glm::vec2(0, 0), mat, color);
            const size_t b = ctx.addV(root + side * (widthRoot * 0.5f), hint, glm::vec2(1, 0), mat, color);
            const size_t c = ctx.addV(tip  + side * (widthTip  * 0.5f), hint, glm::vec2(1, 1), mat, color);
            const size_t d = ctx.addV(tip  - side * (widthTip  * 0.5f), hint, glm::vec2(0, 1), mat, color);
            ctx.addQuad(a, b, c, d);
        }
    };

    // ── MASAS NO VEGETALES ──────────────────────────────────────────────────
    // ROCA → PEDREGAL: la forma lejana de un campo de rocas es un montículo de cantos medio
    // enterrados con tintes de roca+musgo, no un bosque. CASA → ALDEA: un racimo de casitas con
    // techo de pirámide — la silueta de un asentamiento lejano son sus tejados.
    //
    // Mismo contrato que la vegetación: determinista por seed, aplanado, detalle = LOD por píxel,
    // y un solo partId 0 (la masa entera se rompe como bloque; la de hierba, sin collider).
    const float discR = footprintM * 0.55f;
    auto finishMesh = [&]() -> TreeMeshData {
        for (size_t i = 0; i < m.normals.size(); ++i) {
            const float l = glm::length(m.normals[i]);
            m.normals[i] = (l > 1e-6f) ? m.normals[i] / l : glm::normalize(ctx.hints[i]);
        }
        return m;
    };

    if (shapeKind == 1) {   // roca → pedregal
        const int nRock = 5 + (int)(WhiteNode::hashFloat(0, 20, 0, seed) * 4.0f);   // 5..8
        const glm::vec3 rockBase(0.52f, 0.50f, 0.46f);
        const glm::vec3 mossCol(0.30f, 0.42f, 0.22f);
        for (int t = 0; t < nRock; ++t) {
            const float ang = 2.0f * kPi * WhiteNode::hashFloat(t, 21, 0, seed);
            const float rr  = discR * (0.10f + 0.80f * WhiteNode::hashFloat(t, 22, 0, seed));
            const glm::vec3 c(std::cos(ang) * rr, 0.0f, std::sin(ang) * rr);
            const float R      = footprintM * (0.07f + 0.06f * WhiteNode::hashFloat(t, 23, 0, seed));
            const float squish = 0.62f + 0.30f * WhiteNode::hashFloat(t, 24, 0, seed);
            // Enterrados a distinta profundidad: silueta de morrena, no canicas en el suelo.
            const float eyed = R * squish * (0.35f + 0.55f * WhiteNode::hashFloat(t, 25, 0, seed));
            const float moss = WhiteNode::hashFloat(t, 26, 0, seed);
            const glm::vec3 tint = rockBase * (0.80f + 0.45f * WhiteNode::hashFloat(t, 27, 0, seed));
            appendSphere(c + glm::vec3(0.0f, eyed, 0.0f), R, squish, 0,
                         moss < 0.30f ? tint : glm::mix(tint, mossCol, 0.40f));
        }
        return finishMesh();
    }

    if (shapeKind == 2) {   // casa → aldea
        auto appendHouse = [&](const glm::vec3& center, float w, float h,
                               const glm::vec3& wall, const glm::vec3& roofCol) {
            const glm::vec3 cc(center.x, center.y + h * 0.5f, center.z);   // suelo en y=0
            const glm::vec3 hw(w * 0.5f, h * 0.5f, w * 0.5f);
            auto v = [&](float x, float y, float z) {
                const glm::vec3 p = cc + glm::vec3(x, y, z) * hw;
                return ctx.addV(p, glm::normalize(p - cc), glm::vec2(0.5f, 0.5f), 0, wall);
            };
            size_t A0 = v(-1, -1, -1), A1 = v(1, -1, -1), A2 = v(1, -1, 1), A3 = v(-1, -1, 1);
            size_t B0 = v(-1,  1, -1), B1 = v(1,  1, -1), B2 = v(1,  1, 1), B3 = v(-1,  1, 1);
            ctx.addQuad(A1, A0, B0, B1);   // -z (sin suelo: lo tapa el terreno)
            ctx.addQuad(A3, A2, B2, B3);   // +z
            ctx.addQuad(A0, A3, B3, B0);   // -x
            ctx.addQuad(A2, A1, B1, B2);   // +x
            const size_t ap = ctx.addV(cc + glm::vec3(0.0f, h * 0.5f + w * 0.30f, 0.0f),
                                       glm::vec3(0, 1, 0), glm::vec2(0.5f, 1.0f), 0, roofCol);
            const size_t corn[4] = {B0, B1, B2, B3};
            for (int e = 0; e < 4; ++e) ctx.addTri(corn[e & 3], corn[(e + 1) & 3], ap);
        };
        const int nHouse = 3 + (int)(WhiteNode::hashFloat(0, 30, 0, seed) * 3.0f);   // 3..5
        for (int t = 0; t < nHouse; ++t) {
            const float ang = 2.0f * kPi * WhiteNode::hashFloat(t, 31, 0, seed);
            const float rr  = discR * (0.10f + 0.80f * WhiteNode::hashFloat(t, 32, 0, seed));
            const glm::vec3 c(std::cos(ang) * rr, 0.0f, std::sin(ang) * rr);
            const float w   = footprintM * (0.11f + 0.05f * WhiteNode::hashFloat(t, 33, 0, seed));
            const float h   = w * (0.50f + 0.25f * WhiteNode::hashFloat(t, 34, 0, seed));
            const float tint = 0.85f + 0.30f * WhiteNode::hashFloat(t, 35, 0, seed);
            const bool straw = WhiteNode::hashFloat(t, 36, 0, seed) > 0.6f;   // un tejado de paja
            appendHouse(c, w, h,
                        glm::vec3(0.72f, 0.62f, 0.46f) * tint,
                        (straw ? glm::vec3(0.74f, 0.66f, 0.42f) : glm::vec3(0.42f, 0.22f, 0.16f)) * tint);
        }
        return finishMesh();
    }

    // ── el bosque ya fundido ────────────────────────────────────────────────
    // 20-28 árboles en un disco de ~footprint; el detalle reduce un poco el número
    // y quita satélites (la silueta lejana del tier 3 es esto mismo a d bajo).
    //
    // ⚠️ Tiene que leer como un DESCALADO DE IMAGEN del bosque, no como un bloque:
    //    · copas PEQUEÑAS y separadas → queda cielo/fustes entre ellas.
    //    · altura de cada copa con jitter → el techo dentellado, no un domo plano.
    //    · tinte por individuo con ruido fuerte → "pixelado", no una mancha solida.
    //    · conífera = mástiles/agujas (un pinar lejano es dentado, no un pan verde).
    // La masa sigue APLANADA: la altura meta es ~0.55 del árbol para que la
    // escala de celda no la dispare.
    const int nFull = 20 + (int)(WhiteNode::hashFloat(0, 0, 0, seed) * 9.0f);  // 20..28
    const int n     = std::max(1, (int)std::lround((float)nFull * (0.70f + 0.30f * d)));

    const float metaH = heightM * 0.55f;
    const bool  bladeStyle = (style == TreeStyle::Palm || style == TreeStyle::Grass);
    const bool  spireStyle = (style == TreeStyle::Conifer);
    const int   puffsLod   = (d >= 0.75f) ? 1 : 0;   // satélites (harrapan el contorno) solo de cerca

    for (int t = 0; t < n; ++t) {
        const float ang = 2.0f * kPi * WhiteNode::hashFloat(t, 1, 0, seed);
        const float rr  = discR * (0.12f + 0.78f * WhiteNode::hashFloat(t, 2, 0, seed));
        const float cx  = std::cos(ang) * rr;
        const float cz  = std::sin(ang) * rr;
        const float sh  = 0.45f + 0.55f * WhiteNode::hashFloat(t, 3, 0, seed);   // alto relativo
        const float h   = metaH * sh;
        const float tint = 0.70f + 0.55f * WhiteNode::hashFloat(t, 4, 0, seed);  // ruido de color
        const glm::vec3 fb = barkCol * tint, ff = foliageCol * tint;

        if (bladeStyle) {
            const int blades = std::max(2, (int)std::lround(5.0f * d));
            for (int b = 0; b < blades; ++b) {
                const float wa = kPi * (float)b / (float)blades + 0.3f * WhiteNode::hashFloat(b, 8, 0, (uint32_t)(seed + t * 131u));
                const glm::vec3 side(std::cos(wa), 0.0f, std::sin(wa));
                const glm::vec3 lean = glm::vec3(-side.z, 0.0f, side.x) * (0.12f * (WhiteNode::hashFloat(b, 9, 0, seed) - 0.5f));
                // La hierba nace de varios puntos (no de la raíz común de una palmera): cada brizna
                // del mechón con su propia raíz alrededor del centro.
                glm::vec3 root(cx, 0, cz);
                if (style == TreeStyle::Grass) {
                    const float ra = h * 0.35f * std::sqrt(WhiteNode::hashFloat(b, 12, 0, (uint32_t)(seed + t * 131u)));
                    const float rt = 2.0f * kPi * WhiteNode::hashFloat(b, 13, 0, (uint32_t)(seed + t * 131u));
                    root += glm::vec3(std::cos(rt) * ra, 0.0f, std::sin(rt) * ra);
                }
                appendBlade(root, root + glm::vec3(0, h, 0) + lean * h,
                            side, h * 0.22f, h * 0.04f, 1, ff);
            }
            continue;
        }
        if (style == TreeStyle::Dead || style == TreeStyle::Cactus) {
            // Madera muerta: tocos secos (altos para el cactus). La masa lee como
            // matorral árido y la malla nunca queda vacía.
            const float stubH = h * (style == TreeStyle::Cactus ? 0.85f : 0.35f);
            appendCone(glm::vec3(cx, 0, cz), glm::vec3(cx, stubH, cz),
                       trunkR * (style == TreeStyle::Cactus ? 1.0f : 0.8f) * sh, 0.01f, 0, fb);
            if (style == TreeStyle::Dead && d >= 0.28f) {
                const float dr = footprintM * 0.05f * (0.7f + 0.4f * WhiteNode::hashFloat(t, 5, 0, seed));
                appendSphere(glm::vec3(cx, stubH * 0.6f, cz), dr, 0.9f, 1, ff);
            }
            continue;
        }

        // Fuste corto solo si hay detalle; la silueta lejana no lo necesita.
        if (d >= 0.55f) {
            const float stubH = h * 0.20f;
            appendCone(glm::vec3(cx, 0, cz), glm::vec3(cx, stubH, cz),
                       trunkR * 0.8f * sh, 0.01f, 0, fb);
        }

        const float br = footprintM * (0.048f + 0.034f * canopyScale)
                       * (0.70f + 0.45f * WhiteNode::hashFloat(t, 5, 0, seed));
        // La copa CRECE al bajar el detalle: de cerca copas pequeñas y separadas
        // (textura "descalada" que pediste); de lejos copas GRANDES y solapadas
        // (masa CONTINUA — un bosque a un km es una mancha sólida, no motas sueltas
        // que se desvanecen al moverte). La mancha debe persistir hasta el cull.
        const float brL = br * (1.9f - 0.85f * d);   // 1.05 de cerca → ~1.5 lejos
        // Centro de la copa CON jitter vertical: un techo plano es lo que al
        // descalar lee como "bloque". Cada copa cuelga a distinta altura.
        const float cy = h * (0.30f + 0.55f * WhiteNode::hashFloat(t, 6, 0, seed));

        if (spireStyle) {
            // Conífera = AGUJA: un mástil fino que rompe el techo, no un blob.
            const float spireH = h * (1.05f + 0.35f * WhiteNode::hashFloat(t, 7, 0, seed));
            appendCone(glm::vec3(cx, cy - br * 0.4f, cz), glm::vec3(cx, cy + spireH, cz),
                       br * 0.28f, 0.01f, 1, ff);
            continue;
        }

        // Frondosa: copa principal + satélite(s) pequeños que harrapan el contorno
        // (depende del detalle). Copas separadas = cielo/fustes entre árboles.
        appendSphere(glm::vec3(cx, cy, cz), brL, 0.80f, 1, ff);
        for (int p = 0; p < puffsLod; ++p) {
            const float pa = ((float)p * 2.39996f + 1.0f) * kPi;
            const glm::vec3 off(std::cos(pa), 0.30f + 0.60f * WhiteNode::hashFloat(t, 8, p, seed),
                                std::sin(pa));
            appendSphere(glm::vec3(cx, cy, cz) + off * brL * 0.9f,
                         brL * (0.42f + 0.25f * WhiteNode::hashFloat(t, 9, p, seed)), 0.80f, 1, ff);
        }
    }

    // Copas muertas/cactus a medias: normalizar y salir.
    return finishMesh();
}

}} // namespace Haruka::Tools   // Haruka::Tools::ProcGraph