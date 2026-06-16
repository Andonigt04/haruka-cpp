#include "deformation_field.h"
#include <cmath>
#include <algorithm>

namespace Haruka {

long long DeformationField::cellKey(long long cx, long long cy, long long cz) const {
    const long long P1 = 73856093, P2 = 19349663, P3 = 83492791;
    return (cx * P1) ^ (cy * P2) ^ (cz * P3);
}

static inline double smoothFalloff(double t) { // 1 en t=0 → 0 en t=1
    if (t <= 0.0) return 1.0;
    if (t >= 1.0) return 0.0;
    return 1.0 - (t * t * (3.0 - 2.0 * t));
}

double DeformationField::Brush::weight(const glm::dvec3& worldPos) const {
    if (!box) {
        double d = glm::length(worldPos - center);
        if (d >= radius) return 0.0;
        return smoothFalloff(d / radius);
    }
    // Caja orientada: pasa el punto a local; la huella es X·Z, con banda de transición.
    glm::dvec3 lp = glm::transpose(rot) * (worldPos - center);
    if (std::abs(lp.y) > halfExtents.y) return 0.0;       // fuera de la banda vertical
    double band = (radius > 1e-6) ? radius : 1.0;          // ancho del borde suave
    double dx = (std::abs(lp.x) - halfExtents.x) / band;   // ≤0 dentro del núcleo
    double dz = (std::abs(lp.z) - halfExtents.z) / band;
    return smoothFalloff(std::max(0.0, std::max(dx, dz)));
}

double DeformationField::Brush::aabbRadius() const {
    return box ? (glm::length(halfExtents) + radius) : radius;
}

void DeformationField::forEachCell(const Brush& b, const std::function<void(long long)>& fn) const {
    // All grid cells the brush's AABB touches.
    double r = b.aabbRadius();
    glm::dvec3 lo = b.center - glm::dvec3(r);
    glm::dvec3 hi = b.center + glm::dvec3(r);
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
        if (b.type == Type::Flatten) continue; // needs base height → applyHeight
        double w = b.weight(worldPos);
        if (w <= 0.0) continue;
        if (b.type == Type::Subtract) sum -= b.strength * w;
        else                          sum += b.strength * w;
    }
    return sum;
}

double DeformationField::applyHeight(const glm::dvec3& worldPos, double baseHeightM) const {
    if (m_brushes.empty()) return baseHeightM;
    long long cx = (long long)std::floor(worldPos.x / kCell);
    long long cy = (long long)std::floor(worldPos.y / kCell);
    long long cz = (long long)std::floor(worldPos.z / kCell);
    auto it = m_grid.find(cellKey(cx, cy, cz));
    if (it == m_grid.end()) return baseHeightM;

    double h = baseHeightM;
    // 1) Aditivo (cavar/levantar) primero.
    for (int bi : it->second) {
        const Brush& b = m_brushes[bi];
        if (b.type == Type::Flatten) continue;
        double w = b.weight(worldPos);
        if (w <= 0.0) continue;
        if (b.type == Type::Subtract) h -= b.strength * w;
        else                          h += b.strength * w;
    }
    // 2) Nivelado: mezcla la altura hacia targetHeight (1 en el núcleo → 0 en el borde).
    for (int bi : it->second) {
        const Brush& b = m_brushes[bi];
        if (b.type != Type::Flatten) continue;
        double w = b.weight(worldPos);
        if (w <= 0.0) continue;
        h += (b.targetHeight - h) * w;
    }
    return h;
}

bool DeformationField::overlapsSphere(const glm::dvec3& center, double radius) const {
    for (const Brush& b : m_brushes) {
        double d = glm::length(center - b.center);
        if (d < radius + b.aabbRadius()) return true;
    }
    return false;
}

} // namespace Haruka
