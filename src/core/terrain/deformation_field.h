/**
 * @file deformation_field.h
 * @brief Player edits to the procedural terrain (craters, digging, building).
 *
 * The terrain stays procedural; this is an additive layer of "brushes" sampled
 * on top of calculateHeight():  finalHeight = procedural + field.sample(worldPos).
 *
 * A brush is a sphere of influence with a signed strength (SUBTRACT = dig/crater,
 * ADD = raise/build). Contributions sum with a smooth (smoothstep) falloff so
 * edits have clean rounded edges, not steps. Returns METRES of displacement.
 *
 * Positions are world-absolute double (planet scale ~1e8 m) → all math in double.
 * A uniform grid hash keeps sample() O(1) amortised regardless of brush count.
 * Thread-safe for concurrent readers (sample) while no writer runs; the streaming
 * generator samples from worker threads, edits are applied on the main thread
 * between frames (see PlanetarySystem).
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>

namespace Haruka {

class DeformationField {
public:
    enum class Type : uint8_t { Subtract = 0, Add = 1, Flatten = 2 };

    struct Brush {
        glm::dvec3 center{0.0};  // world-absolute
        double     radius = 1.0; // esfera: radio (m). caja: ancho de transición del borde.
        double     strength = 0.0; // metres of displacement at the centre (signed)
        Type       type = Type::Subtract;
        double     targetHeight = 0.0; // Flatten: elevación (m) hacia la que se nivela
        // Forma de CAJA orientada (footprint de un objeto). Si box=true, el área afectada
        // es la huella halfExtents (en el espacio de rot) en vez de un círculo de radius.
        bool       box = false;
        glm::dvec3 halfExtents{0.0}; // medias extensiones de la caja (m), en local
        glm::dmat3 rot{1.0};         // orientación de la caja (columnas = ejes locales)

        // Peso 0..1 del pincel en un punto (1 en el núcleo → 0 fuera). Esfera o caja.
        double weight(const glm::dvec3& worldPos) const;
        // Radio conservador del AABB (para indexado en rejilla / invalidación de chunks).
        double aabbRadius() const;
    };

    /** @brief Adds a brush and returns its index (for later removal/persistence). */
    int addBrush(const Brush& b);

    /** @brief Removes all brushes (e.g. on scene reload). */
    void clear();

    /** @brief Number of brushes (for persistence/debug). */
    int count() const { return (int)m_brushes.size(); }
    const std::vector<Brush>& brushes() const { return m_brushes; }

    /** @brief Signed displacement (metres) at a world position. 0 if no brush.
     *  Solo Add/Subtract (Flatten necesita la altura base → usar applyHeight). */
    double sample(const glm::dvec3& worldPos) const;

    /** @brief Altura final (m) tras aplicar TODAS las brushes a una altura procedural base.
     *  Add/Subtract suman/restan; Flatten mezcla hacia targetHeight. Usar en el generador. */
    double applyHeight(const glm::dvec3& worldPos, double baseHeightM) const;

    /** @brief True if any brush overlaps the sphere (center,radius) — for marking
     *  which chunks need regenerating after an edit. */
    bool overlapsSphere(const glm::dvec3& center, double radius) const;

    bool empty() const { return m_brushes.empty(); }

private:
    static constexpr double kCell = 32.0; // grid cell size (m)
    long long cellKey(long long cx, long long cy, long long cz) const;
    void forEachCell(const Brush& b, const std::function<void(long long)>& fn) const;

    std::vector<Brush> m_brushes;
    // cell → indices of brushes overlapping that cell.
    std::unordered_map<long long, std::vector<int>> m_grid;
};

} // namespace Haruka
