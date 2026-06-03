#include "deformation_field.h"
#include <cmath>
#include <algorithm>

namespace Haruka {

long long DeformationField::cellKey(long long cx, long long cy, long long cz) const {
    const long long P1 = 73856093, P2 = 19349663, P3 = 83492791;
    return (cx * P1) ^ (cy * P2) ^ (cz * P3);
}

void DeformationField::forEachCell(const Brush& b, const std::function<void(long long)>& fn) const {
    // All grid cells the brush's AABB touches.
    glm::dvec3 lo = b.center - glm::dvec3(b.radius);
    glm::dvec3 hi = b.center + glm::dvec3(b.radius);
    long long x0 = (long long)std::floor(lo.x / kCell), x1 = (long long)std::floor(hi.x / kCell);
    long long y0 = (long long)std::floor(lo.y / kCell), y1 = (long long)std::floor(hi.y / kCell);
    long long z0 = (long long)std::floor(lo.z / kCell), z1 = (long long)std::floor(hi.z / kCell);
    for (long long z = z0; z <= z1; ++z)
        for (long long y = y0; y <= y1; ++y)
            for (long long x = x0; x <= x1; ++x)
                fn(cellKey(x, y, z));
}

int DeformationField::addBrush(const Brush& b) {
    int idx = (int)m_brushes.size();
    m_brushes.push_back(b);
    forEachCell(b, [&](long long key){ m_grid[key].push_back(idx); });
    return idx;
}

void DeformationField::clear() {
    m_brushes.clear();
    m_grid.clear();
}

double DeformationField::sample(const glm::dvec3& worldPos) const {
    if (m_brushes.empty()) return 0.0;
    long long cx = (long long)std::floor(worldPos.x / kCell);
    long long cy = (long long)std::floor(worldPos.y / kCell);
    long long cz = (long long)std::floor(worldPos.z / kCell);
    auto it = m_grid.find(cellKey(cx, cy, cz));
    if (it == m_grid.end()) return 0.0;

    double sum = 0.0;
    for (int bi : it->second) {
        const Brush& b = m_brushes[bi];
        double d = glm::length(worldPos - b.center);
        if (d >= b.radius) continue;
        // Smoothstep falloff: 1 at centre → 0 at radius (rounded edges).
        double t = d / b.radius;          // 0..1
        double w = 1.0 - (t * t * (3.0 - 2.0 * t)); // smootherstep-ish weight
        switch (b.type) {
            case Type::Subtract: sum -= b.strength * w; break;
            case Type::Add:      sum += b.strength * w; break;
            case Type::Flatten:  /* handled by generator (needs base height) */ break;
        }
    }
    return sum;
}

bool DeformationField::overlapsSphere(const glm::dvec3& center, double radius) const {
    for (const Brush& b : m_brushes) {
        double d = glm::length(center - b.center);
        if (d < radius + b.radius) return true;
    }
    return false;
}

} // namespace Haruka
