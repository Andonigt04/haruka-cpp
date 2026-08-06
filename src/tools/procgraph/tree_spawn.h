#pragma once

// ===========================================================================
// Spawner de árboles (Etapa 4 del pipeline de props). GL-free, CPU-only.
//
// Reúne ETAPA 1 (Poisson-disk + regla ecológica de tree_prop.h) sobre una
// RETÍCULA ANCLADA AL MUNDO: por cada celda decide si hay árbol, y dónde
// exactamente, de forma determinista (paridad cliente/servidor).
//
// El campo se inyecta como interfaz pura ON/OFF GPU: `sampleAt(px,pz)` debe
// responder con la ecología (FieldSample) y la cota local (heightM) del punto.
// El motor real lo implementa muestreando PlanetFields/PlanetarySystem; los
// tests lo simulan para validar determinismo, reglas y separación Poisson.
//
// La salida es una lista de `PlacedTree` local_a_la_retícula: el pegamento que
// lo sube a un SceneObject (bakeTreeMesh → MeshRendererComponent + bakeTreeTextures
// → MaterialComponent) vive fuera de aquí y ya usa el patrón resultante.
// ===========================================================================

#include "proc_noise.h"                 // hash32
#include "tree_prop.h"                  // treeSuitability, treePoisson, TreeSuit
#include "core/terrain/planet_fields.h" // FieldSample

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Haruka { namespace Tools { namespace ProcGraph {

// ===========================================================================
// Árbol colocado: todo lo que el ensamblado a objeto necesita.
// Local a la retícula (px/pz sobre el terreno, heightM = cota).
// ===========================================================================
struct PlacedTree {
    glm::vec3 localPos;    // (px, 0, pz) sobre la superficie
    float     heightM  = 0.0f;        // cota del suelo en el punto (m)
    uint32_t  meshSeed = 0;           // seed de la MÓDULO (TreeMeshNode/bake)
    float     heightMul = 1.0f;       // variación de altura
    float     scale    = 1.0f;        // tamaño
};

// ===========================================================================
// Interfaz del campo (ecología + cota). La implementa el MOTOR; los tests la
// simulan. No depende de un esférico concreto para mantenerse GL-free reversible.
// ===========================================================================
class IPropField {
public:
    virtual ~IPropField() = default;

    /// Ecología del punto (humedad/temp/caudal/lago…).
    virtual FieldSample sampleAt(float px, float pz) const = 0;
    /// Cota del suelo (m) en el punto, como ver para asentar el árbol.
    virtual float heightAt(float px, float pz) const = 0;
    /// Distribución por mapa [0,1] en el punto (1 = sin mapa de distribución).
    virtual float mapDensityAt(float px, float pz, const std::string& mapPath) const { return 1.0f; }
    /// Zona del zoneMap en el punto: nombre del material asignado (vacío = sin zona/mapa).
    virtual std::string zoneAt(float, float) const { return {}; }
    /// MATERIAL del terreno en el punto (p.ej. "sand", "forest"). Distinto de la zona nombrada:
    /// `layerAt` es lo que el terreno pinta, `zoneAt` la zona del autor que lo recubre.
    virtual std::string layerAt(float, float) const { return {}; }
};

// ===========================================================================
// Pendiente local por diferencias finitas sobre el campo (el campo no la
// expone aún). EPS en metros en el espacio de la retícula.
// ===========================================================================
inline float slopeAt(const IPropField& f, float px, float pz, float eps = 2.0f) {
    float h0 = f.heightAt(px,      pz);
    float hx = f.heightAt(px + eps, pz);
    float hz = f.heightAt(px,      pz + eps);
    float dx = (hx - h0) / eps;
    float dz = (hz - h0) / eps;
    return std::atan(std::sqrt(dx * dx + dz * dz));   // rad
}

// ===========================================================================
// Spawner: recorre la retícula [0,span]² en `cellsPerSide`² celdas.
// En cada celda: jitter Poisson determinista ≡ separación mínima; pregunta al
// campo (≈ habitación real) y coloca el árbol si la regla ecológica acepta.
// ===========================================================================
struct PropGrid {
    float spanM      = 1000.0f;   // lado de la región (m)
    int   cellsPerSide = 16;      // celdas por lado (resolución del Poisson)
    float minSeparation = 0.25f;  // fracción de celda: radio mínimo entre troncos
    uint32_t seed = 0;            // semilla del patch
};

inline std::vector<PlacedTree> spawnTrees(const IPropField& field,
                                          const PropGrid& g) {
    std::vector<PlacedTree> out;
    float cellM = g.spanM / (float)g.cellsPerSide;

    for (int iy = 0; iy < g.cellsPerSide; ++iy) {
        for (int ix = 0; ix < g.cellsPerSide; ++ix) {
            // Celda ancla con hash del par.
            uint32_t cellKey = hash32((uint32_t)ix ^ (uint32_t)iy * 1664525u);
            cellKey = hash32(cellKey ^ g.seed * 2654435761u);

            // Jitter Poisson dentro de la celda → offset [0,1] del punto.
            PoissonPoint pp = treePoisson(cellKey, cellM, g.minSeparation, g.seed);
            float px = ((float)ix + pp.uv.x) * cellM;
            float pz = ((float)iy + pp.uv.y) * cellM;

            // REGLA ECOLÓGICA (campo): ¿hay un árbol aquí?
            FieldSample fs = field.sampleAt(px, pz);
            float slope  = slopeAt(field, px, pz);

            TreeSuit suit = treeSuitability(fs, slope, pp.seed);
            if (!suit.ok) continue;

            PlacedTree t;
            t.localPos  = glm::vec3(px, 0.0f, pz);
            t.heightM   = field.heightAt(px, pz);
            t.meshSeed  = pp.seed;
            t.heightMul = suit.heightMul;
            t.scale     = suit.scale;
            out.push_back(t);
        }
    }
    return out;
}

}}} // namespace Haruka::Tools::ProcGraph