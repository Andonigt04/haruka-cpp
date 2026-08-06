#pragma once

// ===========================================================================
// Decisor ecológico de props (primera plantilla: ÁRBOLES). GL-free, CPU-only.
//
// REGLA DE ORO del PLAN_TERRENO_V3 (§3.5): un prop NO se coloca con una fórmula
// propia — PREGUNTA al campo. Aquí el campo es `Haruka::FieldSample`
// (planet_fields.h): elevación erosionada, caudal, lago, temperatura, humedad.
//
// `treeSuitability(...)`: acepta o rechaza un punto (pendiente, humedad,
// temperatura, caudal, lago) y devuelve un factor [0,1] + un `species`/`scale`
// determinista.
//
// `treePoissonCandidate(dir, cellHits)`: distribución Poisson-disk anclada al
// mundo por hash → separación mínima entre troncos SIN patrón de rejilla
// visible, misma en cliente y servidor (determinista, como toda la generación).
// ===========================================================================

#include "proc_noise.h"          // hash32
#include "core/terrain/planet_fields.h"  // FieldSample

#include <algorithm>
#include <cmath>

namespace Haruka { namespace Tools { namespace ProcGraph {

// ===========================================================================
// Regla ecológica de un árbol en un punto del campo.
// ===========================================================================
struct TreeSuit {
    bool  ok       = false;                      // ¿sitio aceptable?
    float density  = 0.0f;                       // factor de densidad/peso [0,1]
    float heightMul = 1.0f;                      // variación de altura por especie
    float scale    = 1.0f;                       // tamaño relativo del árbol
    float hueMix   = 0.0f;                       // 0 = corteza, 1 = follaje (para matices)
};

// ===========================================================================
// Valora un punto del campo. `slope` = pendiente local (rad) derivada por el
// llamador (el campo no la expone aún; ver planet_fields.h → FieldSample).
// `seedMix = seedBase + (hash de la celda)` para no repetir el árbol idéntico.
// ===========================================================================
inline TreeSuit treeSuitability(const FieldSample& fs, float slopeRad,
                               uint32_t sampleSeed) {
    TreeSuit s;

    // --- humedad: el árbol necesita un mínimo; desiertos/tundra NO. ----------
    // Campo típico: 0 = desierto · 0.5 = pastizal · 1 = selva.
    if (fs.humidity < 0.35f) return s;          // demasiado seco
    if (fs.humidity > 0.85f) return s;          // demasiado húmedo (selva → otra veget.)

    // --- temperatura: rango templado/mesotérmico ----------------------------
    if (fs.tempC < -2.0f) return s;             // helada
    if (fs.tempC > 32.0f) return s;             // tórrida (savas/asavana)

    // --- caudal: NO en el cauce de un río ------------------------------------
    if (fs.flow > Haruka::PlanetFields::kRiverFlow) return s;   // dentro del río
    if (fs.isLake) return s;                                     // en el lago

    // --- pendiente: ángulo frontal del suelo (< ~30° ≈ 0.52 rad) ------------
    if (slopeRad > 0.6f) return s;                 // vertiente muy pronunciada

    // --- densidad por humedad (a más humedad, más bosque) --------------------
    float t = (fs.humidity - 0.35f) / (0.85f - 0.35f);   // 0..1
    s.density = t;

    // --- variadores deterministas por celda: altura/escala --------------------
    float r1 = WhiteNode::hashFloat((int)sampleSeed, 100, 0, sampleSeed);
    float r2 = WhiteNode::hashFloat((int)sampleSeed, 101, 0, sampleSeed);
    s.heightMul = 0.8f + 0.4f * r1;
    s.scale     = 0.9f + 0.5f * r2;
    s.hueMix    = r2;               // reuse

    s.ok = true;
    return s;
}

// ===========================================================================
// Poisson-disk por hash: dada una celda y su semilla, el jitter determinista
// y el radio mínimo entre puntos. Los puntos MÍNIMOS separados por `radius`.
// `inside` usa la regla ecológica (basta para saber que existe tree aquí).
// ===========================================================================
struct PoissonPoint {
    glm::vec2 uv  = glm::vec2(0.0f);   // desplazamiento dentro de la celda [0,1]
    glm::vec3 pos = glm::vec3(0.0f);   // posición (anclada al mundo) en km
    uint32_t seed = 0;
};

// Candidato de Poisson para una celda; `gridScale` = lado de celda (m).
inline PoissonPoint treePoisson(uint32_t cellHash, float gridScale,
                                float radiusUnit, uint32_t baseSeed) {
    // --- jitter determinista por hash de la celda -------------------------
    uint32_t hx = hash32(cellHash ^ 0x9e3779b1u);
    uint32_t hz = hash32(cellHash ^ 0x7042a51u) ;
    float fx = (float)((double)hx / (double)UINT32_MAX);
    float fz = (float)((double)hz / (double)UINT32_MAX);

    // centro "Poisson": célula ancla casa un punto que busca separación
    // mínima `radius` con sus vecinas → subdivide la celda en una rejilla interna
    // de n; tomamos el centro de una subcelda (evita colapso al usar [0,1]).
    int n = std::max(1, (int)std::floor(1.0f / std::max(radiusUnit, 1e-3f)));
    float sub = 1.0f / (float)n;
    int ix = (int)std::floor(fx * (float)0.999f);
    int iz = (int)std::floor(fz * (float)0.999f);
    ix = std::min(ix, n - 1);
    iz = std::min(iz, n - 1);

    // Jitter fino dentro de la sub-celda (no al centro exacto).`
    float jx = (float)((double)hash32(cellHash ^ 0x1234567fu) / (double)UINT32_MAX);
    float jz = (float)((double)hash32(cellHash ^ 0x7654321fu) / (double)UINT32_MAX);
    float px = (float(ix) + 0.2f + 0.6f * jx) * sub;
    float pz = (float(iz) + 0.2f + 0.6f * jz) * sub;

    // Marcador ancla la posición del punto con su propia semilla.
    uint32_t seed = hash32(baseSeed ^ cellHash);

    return { glm::vec2(px, pz), glm::vec3(px, 0.0f, pz), seed };
}

}}} // namespace Haruka::Tools::ProcGraph