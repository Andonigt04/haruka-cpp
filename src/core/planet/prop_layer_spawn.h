/**
 * @file prop_layer_spawn.h
 * @brief Pase de colocación ORDENADO de las capas de props, con RESOLUCIÓN DE CLAIMS.
 *
 * GL-free, CPU-only. Dado un campo (IPropField: ecología + cota) y una retícula (PropGrid),
 * coloca las capas de `PropLayerTable` UNA POR UNA en el ORDEN en que están en la tabla.
 *
 * ── EL ORDEN ES LA PRIORIDAD DE CONSTRUCCIÓN ──────────────────────────────────────────────
 * Cada capa que instala deja sus CLAIMS (claimRadius por instancia). Las capas posteriores
 * DESCARTAN cualquier spot de Poisson que caiga dentro de un claim previo. Así:
 *
 *     capa 0 "house" — coloca casa y reclama su solar (radius 6 m)
 *     capa 1 "path"  — el camino no cruza el solar de la casa
 *     capa 2 "tree"  — un árbol cuyo spot cayó en el solar de la casa NO se instancia
 *
 * Es la regla "se construyó una casa, se pusieron los caminos después, y el árbol o la roca
 * que tenían ese spot designado ya no se instancian" — propiedad del ORDEN, no un parche.
 *
 * Las posiciones salen del MISMO Poisson determinista de tree_prop (anclado a la celda), así
 * la colocación es reproducible cliente/servidor. `density` de cada capa pincha contra un hash
 * de la celda con su peso para dejar huecos sin patrón de rejilla.
 */
#pragma once
#include <vector>
#include <algorithm>
#include <climits>
#include <glm/glm.hpp>

#include "core/planet/prop_layer.h"                 // PropLayerTable, PlacedProp
#include "tools/procgraph/tree_spawn.h"             // IPropField, PropGrid, slopeAt
#include "tools/procgraph/tree_prop.h"              // treePoisson, PoissonPoint
#include "tools/procgraph/proc_noise.h"             // hash32, WhiteNode

namespace Haruka { namespace Planet {
namespace PG = Haruka::Tools::ProcGraph;

/** @brief Ancla el Poisson de una capa en la celda de la retícula.
 *  Reutiliza `treePoisson` (tree_prop.h): jitter determinista → offset [0,1] dentro de la celda. */
inline PG::PoissonPoint cellPoisson(int layerIndex, int ix, int iy,
                                    float cellM, float minSep, uint32_t seed) {
    uint32_t key = PG::hash32((uint32_t)ix ^ (uint32_t)iy * 1664525u ^ layerIndex * 2654435761u);
    return PG::treePoisson(key, cellM, minSep, seed);
}

/** @brief Un claim: disco de exclusión (centro + radio en el plano local de la retícula). */
struct PropClaim {
    glm::vec2 pos;                // (px, pz)
    float     radius = 0.0f;      // m
    int       layer  = -1;        // capa que lo dejó
};

/** @brief Resultado del pase: instancias en orden de colocación + claims finales. */
struct PropsPlaced {
    std::vector<PlacedProp> props;
    std::vector<PropClaim>  claims;
};

/**
 * @brief Coloca TODAS las capas de la tabla sobre una retícula, en orden, con claims.
 *  La capa con índice menor se coloca y reclama PRIMERO (mayor prioridad de construcción).
 *
 * @param field  Ecología + cota local (el motor implementa IPropField muestreando el campo).
 * @param g      Retícula (span, celdas, semilla, minSeparation).
 * @param table  Capas en orden: las primeras se instalan y reclaman antes.
 */
inline PropsPlaced placeProps(const PG::IPropField& field, const PG::PropGrid& g,
                              const PropLayerTable& table) {
    PropsPlaced out;
    const float cellM = g.spanM / (float)std::max(1, g.cellsPerSide);

    for (int li = 0; li < (int)table.layers.size(); ++li) {
        const PropLayer& L = table.layers[li];
        if (L.mesh.empty()) continue;                 // capa informativa: no instala

        for (int iy = 0; iy < g.cellsPerSide; ++iy) {
            for (int ix = 0; ix < g.cellsPerSide; ++ix) {
                PG::PoissonPoint pp = cellPoisson(li, ix, iy, cellM, g.minSeparation, g.seed);
                const float px = ((float)ix + pp.uv.x) * cellM;
                const float pz = ((float)iy + pp.uv.y) * cellM;

                // Ecología/pendiente locales → ¿cuánto cumple la capa? 0 = fuera de su territorio.
                const FieldSample fs = field.sampleAt(px, pz);
                const float slope   = PG::slopeAt(field, px, pz);
                // Mapa de distribución (si la capa lo declara): MANDA sobre las bandas, igual que
                // el zoneMap sobre los materiales. El campo sabe cómo traducirlo por dirección.
                const float mapD = L.densityMap.empty() ? 1.0f : field.mapDensityAt(px, pz, L.densityMap);
                const std::string zone  = field.zoneAt(px, pz);
                const std::string layer = field.layerAt(px, pz);
                const float cov   = L.coverage(fs, slope, mapD, zone, layer);
                if (cov <= 0.0f) continue;

                // Densidad local: huecos sin patrón (hash de la celda × peso de volumen).
                const float r = PG::WhiteNode::hashFloat((int)pp.seed, 200 + li, 0, pp.seed);
                if (r > L.density * cov) continue;

                // RESOLUCIÓN DE CLAIMS: un spot que cae dentro del radio de una instancia ya
                // colocada (una capa de MAYOR prioridad = índice menor) se descarta.
                const bool claimed = [&] {
                    for (const auto& c : out.claims) {
                        if (c.layer >= li) continue;             // sus propios claims no se auto-anulan
                        const float dx = px - c.pos.x, dz = pz - c.pos.y;
                        if (dx*dx + dz*dz <= c.radius * c.radius) return true;
                    }
                    return false;
                }();
                if (claimed) continue;

                PlacedProp pr;
                pr.mesh       = L.mesh;
                pr.layerIndex = li;
                pr.localPos   = glm::vec3(px, 0.0f, pz);
                pr.heightM    = field.heightAt(px, pz);
                // Sumergido: solo las capas que lo declaran (`submerged`, la roca del lecho); el
                // resto no se dibuja bajo el nivel del mar (misma regla que el scatter esférico).
                if (pr.heightM < 0.0f && !L.submerged) continue;
                pr.meshSeed   = pp.seed;
                pr.seed       = pp.seed;
                pr.scale      = L.scaleMin + (L.scaleMax - L.scaleMin) *
                                PG::WhiteNode::hashFloat((int)pp.seed, 300 + li, 0, pp.seed);
                out.props.push_back(pr);

                if (L.claimRadius > 0.0f)
                    out.claims.push_back({ glm::vec2(px, pz), L.claimRadius, li });
            }
        }
    }
    return out;
}

}} // namespace Haruka::Planet