/**
 * @file terrain_deform.h
 * @brief CAPA DE DEFORMACIÓN: lo que el jugador excava o levanta, encima del bake.
 *
 * ── POR QUÉ EXISTE ─────────────────────────────────────────────────────────────────────────────
 * Cavar no hacía nada. La cadena estaba rota en un punto concreto y documentado:
 *   1. `editTerrain` guardaba el desnivel.                                          ✅
 *   2. `rebuildWithEdits` reconstruía la MALLA con base+ediciones.                   ✅
 *   3. Pero no rehorneaba el bake.                                                   ❌
 *   4. Y el bake es quien manda: los dos tess eval leen `uHeightTex` y la física lee
 *      `m_heightCPU`. Las alturas de los vértices solo son respaldo si NO hay bake.
 *
 * Rehornear no es opción: el bake cuesta ~2 s y se cachea por hash en disco. Por palada, imposible.
 *
 * ── QUÉ ES ─────────────────────────────────────────────────────────────────────────────────────
 * Un DELTA en metros que se suma al bake, en una rejilla tangente anclada cerca del jugador — el
 * mismo patrón que el agua interior de ríos/lagos, y por la misma razón: es un campo local que
 * cambia por frame, así que se publica a la GPU como buffer y se muestrea con una bilineal a mano
 * que la CPU replica exactamente.
 *
 *     altura del suelo = bake(dir) + detalle(dir) + deform(dir)
 *
 * ⚠️ LOS DOS LADOS LEEN LO MISMO O NO SIRVE. Si el render suma el delta y la física no, el jugador
 * cava un hoyo y sigue caminando sobre el suelo que había — que es peor que no poder cavar, porque
 * falla en silencio. De ahí que esta cabecera y `lib/terrain_deform.glsl` se escriban juntas.
 *
 * ── Y ADEMÁS ES LA PROFUNDIDAD DE EXCAVACIÓN ───────────────────────────────────────────────────
 * Un delta NEGATIVO es material retirado. Su opuesto es cuánto se ha bajado respecto al terreno
 * original, o sea la PROFUNDIDAD a la que hay que preguntar los estratos (`depthKm` de la tabla de
 * materiales). Por eso cavar destapa granito sin declarar nada: es el mismo número.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cmath>
#include <algorithm>

namespace Haruka { namespace Planet {

/**
 * @brief Rejilla tangente de deltas de altura (m) alrededor de un ancla.
 *
 * Deliberadamente tonta: guarda, muestrea y publica. Quién la edita (una pala, una explosión, un
 * nivelado) es asunto de quien la use.
 */
struct TerrainDeform {
    static constexpr int   kN    = 129;      ///< celdas por lado (impar: hay celda central exacta)
    static constexpr float kSpan = 512.0f;   ///< lado del parche (m) → celda de 4 m, la del clipmap

    glm::dvec3 anchor{0.0};                  ///< centro del parche (mundo)
    glm::dvec3 tan{1, 0, 0}, bit{0, 0, 1};   ///< ejes del plano tangente
    glm::dvec3 up{0, 1, 0};
    std::vector<float> delta;                ///< kN·kN metros; 0 = sin tocar
    bool valid = false;

    void reset(const glm::dvec3& a, const glm::dvec3& t, const glm::dvec3& b, const glm::dvec3& u) {
        anchor = a; tan = t; bit = b; up = u;
        delta.assign((size_t)kN * kN, 0.0f);
        valid = true;
    }

    /// Celda (fraccionaria) de un punto del mundo. Devuelve false si cae fuera del parche.
    bool cellOf(const glm::dvec3& worldPos, float& fx, float& fy) const {
        if (!valid) return false;
        const glm::dvec3 rel = worldPos - anchor;
        const double u = glm::dot(rel, tan), v = glm::dot(rel, bit);
        fx = (float)((u + kSpan * 0.5) / kSpan) * (kN - 1);
        fy = (float)((v + kSpan * 0.5) / kSpan) * (kN - 1);
        return fx >= 0.0f && fy >= 0.0f && fx <= (float)(kN - 1) && fy <= (float)(kN - 1);
    }

    /**
     * @brief Delta en un punto (m). 0 fuera del parche o sin deformar.
     *
     * ⚠️ Bilineal A MANO, y su gemela GLSL hace la misma cuenta en el mismo orden. El filtrado del
     * hardware usa pesos de 8 bits en varias GPU: bastaría para que el borde de un hoyo se viera a
     * una altura por el render y otra por la física.
     */
    float sample(const glm::dvec3& worldPos) const {
        float fx, fy;
        if (!cellOf(worldPos, fx, fy)) return 0.0f;
        const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
        const int x1 = std::min(x0 + 1, kN - 1), y1 = std::min(y0 + 1, kN - 1);
        const float tx = fx - (float)x0, ty = fy - (float)y0;
        const float d00 = delta[(size_t)y0 * kN + x0], d10 = delta[(size_t)y0 * kN + x1];
        const float d01 = delta[(size_t)y1 * kN + x0], d11 = delta[(size_t)y1 * kN + x1];
        const float a = d00 + (d10 - d00) * tx;
        const float b = d01 + (d11 - d01) * tx;
        return a + (b - a) * ty;
    }

    /**
     * @brief Excava (o levanta) con caída suave. `amount` en metros: negativo excava.
     * @return true si tocó alguna celda (o sea, si el punto cae en el parche).
     */
    bool edit(const glm::dvec3& worldPos, float radiusM, float amount) {
        float fx, fy;
        if (!cellOf(worldPos, fx, fy)) return false;
        const float cell = kSpan / (float)(kN - 1);
        const int   r    = (int)std::ceil(radiusM / cell);
        const int   cx   = (int)std::lround(fx), cy = (int)std::lround(fy);
        bool touched = false;
        for (int j = std::max(0, cy - r); j <= std::min(kN - 1, cy + r); ++j)
            for (int i = std::max(0, cx - r); i <= std::min(kN - 1, cx + r); ++i) {
                const float dx = ((float)i - fx) * cell, dy = ((float)j - fy) * cell;
                const float d  = std::sqrt(dx * dx + dy * dy);
                if (d > radiusM) continue;
                // Caída coseno: un borde duro dejaría un cilindro, no un hoyo.
                const float w = 0.5f + 0.5f * std::cos(3.14159265f * d / std::max(radiusM, 1e-3f));
                delta[(size_t)j * kN + i] += amount * w;
                touched = true;
            }
        return touched;
    }
};

}} // namespace Haruka::Planet
