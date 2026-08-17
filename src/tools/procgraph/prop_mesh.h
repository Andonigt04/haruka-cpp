#pragma once

// ===========================================================================
// Mallas PROCEDURALES de props no-árbol: roca y casa.
//
// El scatter global solo sabía hornear árboles (`bakeTreeMesh`), así que la
// capa "house"/"rock" salía con un árbol. Estos bakes generan geometría
// determinista por seed en el MISMO formato (`TreeMeshData`) y con el MISMO
// convenio: base en y=0 en el origen, normales hacia fuera, color de vértice.
//
// Determinista: misma seed + parámetros ⇒ misma malla exacta. La variedad la
// da la seed de la celda (escala/tinte aparte), como en el árbol.
// ===========================================================================

#include "proc_noise.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "tree_mesh.h"   // TreeMeshData (formato de salida compartido)

namespace Haruka { namespace Tools { namespace ProcGraph {

// ===========================================================================
// Roca: cluster de blobs achatados (un "guijarro de montaña").
// `scale` multiplica el tamaño total; `squashY` aplasta el conjunto.
// ===========================================================================
// ────────────────────────────────────────────────────────────────────────────────────────────────
// ENVOLVENTES: las MISMAS constantes con las que se construye la malla.
//
// ⚠️ Existen porque el collider las tenía ESCRITAS A MANO y no coincidían: la roca se colisionaba
// con una caja de y=0 a y=1.44 cuando la malla va de -0,79 a +0,79 CENTRADA EN EL ORIGEN — o sea
// una caja flotando entera por encima de la piedra. Un collider derivado aparte de la malla acaba
// separándose de ella; es el mismo argumento que llevó a extraer `TreeSkeleton`.
struct PropBoxExtent {
    glm::vec3 center{0.0f};   ///< centro de la caja, en el marco local del prop
    glm::vec3 half{0.0f};     ///< semiejes
};

/** @brief Envolvente de la ROCA. El bloque principal es un blob en el ORIGEN de radio `1.1·scale`
 *  achatado por `squashY`; los peñones sobresalen algo pero no dominan. */
inline PropBoxExtent rockExtent(float scale = 1.0f, float squashY = 0.72f) {
    const float mainR = 1.1f * scale;
    return { glm::vec3(0.0f), glm::vec3(mainR, mainR * squashY, mainR) };
}

/** @brief Envolvente del CUERPO de la casa (muros; el tejado sobresale y no se colisiona aparte).
 *  La altura varía por semilla, igual que en el bake, así que hay que pasarla. */
inline PropBoxExtent houseExtent(int seed, float scale = 1.0f) {
    const float w = 3.0f * scale;
    const float h = 2.4f * scale * (0.95f + 0.10f * WhiteNode::hashFloat(0, 0, 0, (uint32_t)seed));
    return { glm::vec3(0.0f, h * 0.5f, 0.0f), glm::vec3(w * 0.5f, h * 0.5f, w * 0.5f) };
}

/**
 * @param cullFrac  fracción del radio principal bajo la cual un peñón se considera ENTERRADO y no
 *                  se genera (ver la nota larga en el bucle de peñones). 0,83 = la esfera inscrita
 *                  en el poliedro lat4×lon7, que es el valor correcto.
 *                  **0 lo desactiva** — es la malla de referencia contra la que el banco compara.
 *                  Valores > 1 recortan peñones VISIBLES: solo existen para la contraprueba.
 */
inline TreeMeshData bakeRockMesh(int seed, float scale = 1.0f, float squashY = 0.72f,
                                 float cullFrac = 0.83f) {
    TreeMeshData m;

    struct Ctx {
        TreeMeshData& m;
        std::vector<glm::vec3> hints;

        size_t addV(const glm::vec3& p, const glm::vec3& hint,
                    const glm::vec2& uv, unsigned char mat, const glm::vec3& color) {
            m.positions.push_back(p);
            m.normals.push_back(glm::vec3(0.0f));
            m.uvs.push_back(uv);
            m.materialId.push_back(mat);
            m.colors.push_back(color);
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

    const float kPi = 3.14159265358979323846f;
    const glm::vec3 rockCol(0.55f, 0.55f, 0.57f);

    auto appendBlob = [&](const glm::vec3& center, float radius, float sy,
                          const glm::vec3& color) {
        int lat = 4, lon = 7;
        size_t topV = ctx.addV(center + glm::vec3(0, radius * sy, 0),
                               glm::vec3(0, 1, 0), glm::vec2(0.5f, 1.0f), 0, color);
        std::vector<std::vector<size_t>> rings(lat - 1);
        for (int i = 1; i < lat; ++i) {
            float theta = kPi * (float)i / (float)lat;
            float r = radius * std::sin(theta);
            float y = radius * std::cos(theta) * sy;
            for (int j = 0; j < lon; ++j) {
                float phi = 2.0f * kPi * (float)j / (float)lon;
                glm::vec3 p(center.x + std::cos(phi) * r,
                            center.y + y,
                            center.z + std::sin(phi) * r);
                glm::vec3 hint = glm::normalize(p - center);
                glm::vec2 uv((float)j / (float)lon, (float)i / (float)lat);
                rings[i - 1].push_back(ctx.addV(p, hint, uv, 0, color));
            }
        }
        size_t botV = ctx.addV(center - glm::vec3(0, radius * sy, 0),
                               glm::vec3(0, -1, 0), glm::vec2(0.5f, 0.0f), 0, color);
        for (int j = 0; j < lon; ++j) {
            int j2 = (j + 1) % lon;
            ctx.addTri(topV, rings[0][j2], rings[0][j]);
            for (int i = 1; i < lat - 1; ++i)
                ctx.addQuad(rings[i - 1][j], rings[i - 1][j2], rings[i][j2], rings[i][j]);
            ctx.addTri(botV, rings[lat - 2][j], rings[lat - 2][j2]);
        }
    };

    // Blob principal achatado + 2-3 peñones encima/pegados → silueta irregular.
    const float mainR = rockExtent(scale, squashY).half.x;   // misma constante que el collider
    appendBlob(glm::vec3(0, 0, 0), mainR, squashY, rockCol);
    int nCrags = 2 + (int)(WhiteNode::hashFloat(0, 0, 0, seed) * 2.0f);   // 2..3
    for (int c = 0; c < nCrags; ++c) {
        float ang = 2.0f * kPi * WhiteNode::hashFloat(c, 1, 0, seed);
        float rad = mainR * (0.30f + WhiteNode::hashFloat(c, 2, 0, seed) * 0.45f);
        float y   = mainR * squashY * (0.2f + WhiteNode::hashFloat(c, 3, 0, seed) * 0.6f);
        float br  = mainR * (0.30f + WhiteNode::hashFloat(c, 4, 0, seed) * 0.25f);
        glm::vec3 pos(std::cos(ang) * rad, y, std::sin(ang) * rad);
        const float sy = squashY * (0.8f + 0.4f * WhiteNode::hashFloat(c, 6, 0, seed));

        // ── PEÑONES COMPLETAMENTE ENTERRADOS: no se generan ─────────────────────────────────────
        //
        // Los peñones se colocan por sorteo (ángulo, radio, altura, tamaño) y una buena parte cae
        // ENTERA dentro del blob principal: 36 vértices y 42 triángulos que no se ven desde ningún
        // ángulo, pero que se transforman, se rasterizan y se descartan por el z-buffer en cada
        // instancia de cada roca del mundo. Es overdraw puro.
        //
        // ⚠️ EL TEST NO ES CONTRA EL ELIPSOIDE, y esa es la trampa. El blob principal se teseló con
        // lat=4 × lon=7, así que lo que se dibuja es un poliedro INSCRITO: sus caras son cuerdas que
        // pasan por dentro del elipsoide. Un peñón "dentro del elipsoide" puede perfectamente asomar
        // por el centro de una faceta plana. El radio seguro es el de la esfera INSCRITA en el
        // poliedro: cos(π/7)·cos(π/8) = 0,901 · 0,924 ≈ 0,83 del radio nominal.
        //
        // El elipsoide se vuelve esfera dividiendo la Y por `squashY`; ahí la contención es la
        // comparación de siempre: distancia al centro + radio del peñón ≤ radio seguro.
        if (cullFrac > 0.0f) {
            const glm::vec3 posS(pos.x, pos.y / squashY, pos.z);
            const float cragMax = std::max(br, br * sy / squashY);   // semieje mayor del peñón
            const float safeR   = mainR * cullFrac;                  // esfera inscrita en el poliedro
            if (glm::length(posS) + cragMax <= safeR) continue;       // invisible: no se genera
        }

        // Tinte sutil por peñón (misma roca, luz distinta del sol no hay en el vértice).
        glm::vec3 col = rockCol * (0.92f + 0.16f * WhiteNode::hashFloat(c, 5, 0, seed));
        appendBlob(pos, br, sy, col);
    }

    for (size_t i = 0; i < m.normals.size(); ++i) {
        float l = glm::length(m.normals[i]);
        if (l > 1e-6f) m.normals[i] /= l;
        else m.normals[i] = glm::normalize(ctx.hints[i]);
    }
    return m;
}

// ===========================================================================
// Casa: caja (muros) + tejado a dos aguas. Determinista por seed (pequeñas
// variaciones de proporción), base en y=0, en el origen. Las normales se
// orientan con la dirección exterior explícita de cada cara (la geometría es
// convexa: centro → centroide de la cara), no con hints por vértice.
// ===========================================================================
inline TreeMeshData bakeHouseMesh(int seed, float scale = 1.0f) {
    TreeMeshData m;

    struct Ctx {
        TreeMeshData& m;
        std::vector<glm::vec3> hints;
        glm::vec3 outward = glm::vec3(0, 1, 0);   // dirección exterior de la cara actual

        size_t addV(const glm::vec3& p, const glm::vec3& hint,
                    const glm::vec2& uv, unsigned char mat, const glm::vec3& color) {
            m.positions.push_back(p);
            m.normals.push_back(glm::vec3(0.0f));
            m.uvs.push_back(uv);
            m.materialId.push_back(mat);
            m.colors.push_back(color);
            hints.push_back(hint);
            return m.positions.size() - 1;
        }

        // Orientación por outward explícito (robusto en geometría convexa).
        void addTriOut(size_t a, size_t b, size_t c) {
            const glm::vec3& va = m.positions[a];
            const glm::vec3& vb = m.positions[b];
            const glm::vec3& vc = m.positions[c];
            glm::vec3 n = glm::cross(vb - va, vc - va);
            if (glm::dot(n, outward) < 0.0f) {
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

        void addQuadOut(size_t a, size_t b, size_t c, size_t d) {
            addTriOut(a, b, c);
            addTriOut(a, c, d);
        }
    } ctx{m};

    const glm::vec3 wallCol (0.85f, 0.80f, 0.70f);
    const glm::vec3 roofCol(0.50f, 0.28f, 0.20f);
    const glm::vec3 doorCol(0.38f, 0.26f, 0.16f);

    // Proporciones deterministas: caja [w × w × h], tejado con voladizo.
    float w = 3.0f * scale;
    float h = 2.4f * scale * (0.95f + 0.10f * WhiteNode::hashFloat(0, 0, 0, seed));
    float roofH = 1.6f * scale;
    float over  = 0.45f * scale;
    const glm::vec3 center(0.0f, h * 0.5f, 0.0f);   // para orientar caras

    // UV PLANAR sobre la caja completa (proyección x-y extendida al voladizo). La textura de
    // material del prototipo es un detalle repetible, así que no hace falta un desplegado por
    // cara; con (0,0) fijo la casa entera resolvía a un SOLO téxel y el material parecía no
    // aplicarse (misma razón que los UV por proyección de caja de mesh.cpp).
    const float ext = w * 0.5f + over;
    const float uvMax = h + roofH;
    auto planarUv = [&](const glm::vec3& p) {
        return glm::vec2((p.x + ext) / (2.0f * ext), glm::clamp(p.y / uvMax, 0.0f, 1.0f));
    };

    auto addV = [&](const glm::vec3& p, unsigned char mat, const glm::vec3& color) {
        return ctx.addV(p, glm::vec3(0.0f), planarUv(p), mat, color);
    };
    // Cara = quad con outward = normalize(centroide - centro del volumen).
    auto quad = [&](size_t a, size_t b, size_t c, size_t d) {
        ctx.outward = glm::normalize((m.positions[a] + m.positions[b] +
                                      m.positions[c] + m.positions[d]) * 0.25f - center);
        ctx.addQuadOut(a, b, c, d);
    };
    auto tri  = [&](size_t a, size_t b, size_t c) {
        ctx.outward = glm::normalize((m.positions[a] + m.positions[b] +
                                      m.positions[c]) * (1.0f / 3.0f) - center);
        ctx.addTriOut(a, b, c);
    };

    // ---- muros: 4 caras + tapa (la base queda abierta, enterrada) ----------
    float hw = w * 0.5f;
    size_t c0 = addV(glm::vec3(-hw, 0, -hw), 0, wallCol);
    size_t c1 = addV(glm::vec3( hw, 0, -hw), 0, wallCol);
    size_t c2 = addV(glm::vec3( hw, 0,  hw), 0, wallCol);
    size_t c3 = addV(glm::vec3(-hw, 0,  hw), 0, wallCol);
    size_t t0 = addV(glm::vec3(-hw, h, -hw), 0, wallCol);
    size_t t1 = addV(glm::vec3( hw, h, -hw), 0, wallCol);
    size_t t2 = addV(glm::vec3( hw, h,  hw), 0, wallCol);
    size_t t3 = addV(glm::vec3(-hw, h,  hw), 0, wallCol);
    // Frontal (+Z): puerta recortada como color distinto sobre la cara.
    size_t d0 = addV(glm::vec3(-hw * 0.28f, 0, hw), 0, doorCol);
    size_t d1 = addV(glm::vec3( hw * 0.28f, 0, hw), 0, doorCol);
    size_t d2 = addV(glm::vec3( hw * 0.28f, h * 0.55f, hw), 0, doorCol);
    size_t d3 = addV(glm::vec3(-hw * 0.28f, h * 0.55f, hw), 0, doorCol);
    quad(t0, t1, t2, t3);          // tapa
    quad(t0, c0, c1, t1);          // -Z
    quad(t1, c1, c2, t2);          // +X
    quad(t3, c3, c0, t0);          // -X
    // +Z con puerta: cuatro trozos alrededor del vano.
    quad(t3, c3, d0, d3);          // izquierda del vano
    quad(d2, d1, c2, t2);          // derecha del vano
    quad(d3, d2, t2, t3);          // arriba del vano
    quad(d3, d0, d1, d2);          // vano (puerta, cara +Z)

    // ---- tejado a dos aguas sobre la caja, con voladizo --------------------
    // Cumbrera a lo largo del eje Z; dos faldones (+X / -X) y dos aguas (+Z/-Z).
    float x0 = -hw - over, x1 = hw + over;
    float z0 = -hw - over, z1 = hw + over;
    size_t r0 = addV(glm::vec3(x0, h, z0), 1, roofCol);
    size_t r1 = addV(glm::vec3(x1, h, z0), 1, roofCol);
    size_t r2 = addV(glm::vec3(x1, h, z1), 1, roofCol);
    size_t r3 = addV(glm::vec3(x0, h, z1), 1, roofCol);
    size_t ridge0 = addV(glm::vec3(0, h + roofH, z0), 1, roofCol);
    size_t ridge1 = addV(glm::vec3(0, h + roofH, z1), 1, roofCol);
    quad(r0, ridge0, ridge1, r3);   // falda -X
    quad(r2, r1, ridge0, ridge1);   // falda +X
    tri(r3, ridge1, r2);            // agua +Z
    tri(r1, ridge0, r0);            // agua -Z

    // ---- normalización final de normales -----------------------------------
    for (size_t i = 0; i < m.normals.size(); ++i) {
        float l = glm::length(m.normals[i]);
        if (l > 1e-6f) m.normals[i] /= l;
        else m.normals[i] = glm::vec3(0, 1, 0);
    }
    return m;
}

}}} // namespace Haruka::Tools::ProcGraph
