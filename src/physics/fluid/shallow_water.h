/**
 * @file shallow_water.h
 * @brief Regional shallow-water simulation (Fase D): rivers, lakes, flooding.
 *
 * Uses the "virtual pipes" model (Mei et al., "Fast Hydraulic Erosion Simulation
 * and Visualization on GPU", 2007): a 2D grid of water columns connected to their
 * 4 neighbours by pipes. Flux is driven by the difference in TOTAL height
 * (terrain + water); flux is clamped so a cell never drains more water than it
 * holds, which makes the scheme unconditionally mass-conserving.
 *
 * The grid is a flat tangent patch anchored near the player. At regional scale
 * (tens–hundreds of metres) planet curvature is negligible, so a flat grid over
 * the real sampled terrain height is accurate and cheap. Designed CPU-first with
 * flat arrays (SoA) so it can move to a compute shader later unchanged.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <functional>

namespace Haruka::fluid {

class ShallowWaterSim {
public:
    /**
     * @brief Builds the grid and samples terrain.
     * @param anchor   world-space center of the patch
     * @param tangent  unit vector along the grid +X axis (in the tangent plane)
     * @param bitangent unit vector along the grid +Z axis
     * @param up       surface normal (terrain sampled along this axis)
     * @param spanM    patch side length in metres
     * @param n        cells per side (grid is n×n)
     * @param terrainHeight  callback: world pos → terrain elevation along `up`
     *                       (metres). Same source the renderer/collision use.
     */
    void init(const glm::dvec3& anchor,
              const glm::dvec3& tangent,
              const glm::dvec3& bitangent,
              const glm::dvec3& up,
              double spanM, int n,
              std::function<double(const glm::dvec3&)> terrainHeight);

    /** @brief Advances the simulation by dt seconds (sub-stepped internally for CFL). */
    void step(float dt);

    /** @brief Llena las hondonadas del parche hasta su punto de derrame (priority-flood).
     *
     *  La llama `init` tras muestrear el terreno. Sin esto la simulación arranca SECA y no hay lagos
     *  ni ríos: las únicas fuentes eran la lluvia y el rellenado costero, así que el sistema era un
     *  acumulador de lluvia. Ver la nota larga de la implementación (incluida su limitación: el
     *  resultado depende del borde del parche). */
    void seedLakes();

    /** @brief Adds water depth (m) at a grid cell (sources: springs, rain). */
    void addWater(int i, int j, float depth);

    /** @brief Adds uniform rain (m of depth per call) across the whole patch. */
    void addRain(float depth);

    /** @brief Adds a circular spring at world position (radius in m, rate m/s). */
    void addSpringWorld(const glm::dvec3& worldPos, float radiusM, float ratePerSec, float dt);

    /**
     * @brief Pours a volume (m³) of water into the cell nearest a world position.
     * Used by the hybrid coupler to absorb PBF particles into the heightfield
     * (cascade → lake), conserving mass. Returns false if the point is off-grid.
     */
    bool addVolumeAtWorld(const glm::dvec3& worldPos, float volumeM3);

    /** @brief True if the world position projects inside this patch. */
    bool containsWorld(const glm::dvec3& worldPos) const;

    /**
     * @brief Water surface height (terrain + water, up-relative metres) at the
     * grid cell nearest a world position. Returns -1e9 if off-grid.
     */
    float surfaceAlongUpAtWorld(const glm::dvec3& worldPos) const;

    /**
     * @brief Clamps coastal water to the sea level so rivers meet the ocean
     * without piling up. seaLevelAlongUp is the ocean surface height measured in
     * the same up-relative frame as the terrain (metres). Cells whose terrain is
     * at/below sea level have their water surface pinned to it.
     */
    void applySeaLevel(float seaLevelAlongUp);

    int   size() const { return m_n; }
    float cellSize() const { return (float)m_dx; }
    double totalWater() const; // sum of depth*cellArea (m³), for mass checks
    /** @brief true si la malla está lista para render/step: grid > 1 y arrays CPU dimensionados. */
    bool valid() const { return m_n > 1 && m_terrain.size() >= size_t(m_n) * (size_t)m_n
                                        && m_water.size()  >= size_t(m_n) * (size_t)m_n; }

    // --- Accessors for rendering ---
    float terrainAt(int i, int j) const { return m_terrain[idx(i,j)]; }
    float waterAt(int i, int j)   const { return m_water[idx(i,j)]; }
    glm::dvec3 worldPosAt(int i, int j) const;
    bool  hasWaterAt(int i, int j, float eps = 0.02f) const { return m_water[idx(i,j)] > eps; }

    const glm::dvec3& anchor() const { return m_anchor; }
    const glm::dvec3& up()     const { return m_up; }
    /** @brief Ejes del parche en el plano tangente. Los necesita el render del AGUA para llevar una
     *  dirección del planeta a la celda (i,j) — ver `TerrestrialPlanet::setInlandWater`. */
    const glm::dvec3& tangent()   const { return m_tan; }
    const glm::dvec3& bitangent() const { return m_bit; }
    // F5.3: último nivel del mar aplicado (up-relativo). El render salta las celdas cuya
    // SUPERFICIE (terreno+agua) está a/bajo este nivel → esas las dibuja el océano (sin doble lámina).
    float seaLevelAlongUp() const { return m_seaLevelAlongUp; }
    float surfaceAt(int i, int j) const { return m_terrain[idx(i,j)] + m_water[idx(i,j)]; }

    /** @brief One overflow point: a wet cell spilling over a steep terrain drop. */
    struct Overflow {
        glm::dvec3 worldPos;   // world position of the spilling cell surface
        glm::dvec3 flowDir;    // unit world direction of the spill (downhill)
        float      rate;       // spill volume rate proxy (m³/s)
        float      drop;       // terrain height drop over the edge (m)
    };
    /**
     * @brief Finds cells whose water spills over a steep edge (waterfall sources).
     * A cell qualifies when it holds water AND a 4-neighbour has a terrain drop
     * greater than dropThresh metres. Used by the hybrid coupler to spawn PBF
     * cascade particles (lake overflow → waterfall). Cheap: one grid pass.
     */
    void collectOverflowEdges(float dropThresh, std::vector<Overflow>& out) const;

private:
    int idx(int i, int j) const { return j * m_n + i; }
    void substep(float dt);

    int    m_n = 0;
    double m_dx = 1.0;
    double m_span = 0.0;
    glm::dvec3 m_anchor{0.0}, m_tan{1,0,0}, m_bit{0,0,1}, m_up{0,1,0};

    float  m_seaLevelAlongUp = -1e9f; // F5.3: nivel del mar aplicado (-1e9 = sin océano acoplado)
    std::vector<float> m_terrain;  // elevation (m) along up at each cell
    std::vector<float> m_water;    // water depth (m)
    // Outgoing flux per cell to L,R,B,T neighbours (m³/s-ish, pipe model units).
    std::vector<float> m_fL, m_fR, m_fB, m_fT;
};

} // namespace Haruka::fluid
