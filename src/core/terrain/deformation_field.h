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
        double     radius = 1.0; // metres
        double     strength = 0.0; // metres of displacement at the centre (signed)
        Type       type = Type::Subtract;
    };

    /** @brief Adds a brush and returns its index (for later removal/persistence). */
    int addBrush(const Brush& b);

    /** @brief Removes all brushes (e.g. on scene reload). */
    void clear();

    /** @brief Number of brushes (for persistence/debug). */
    int count() const { return (int)m_brushes.size(); }
    const std::vector<Brush>& brushes() const { return m_brushes; }

    /** @brief Signed displacement (metres) at a world position. 0 if no brush. */
    double sample(const glm::dvec3& worldPos) const;

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
