#include "core/terrain/reference_surface.h"
#include "core/terrain/cube_sphere.h"

#include <cmath>

namespace Haruka {

void ReferenceSurface::configure(const WorldGenParams& W, double radius, int refLod, int chunkSize) {
    const bool sameGrid = (m_refLod == refLod && m_chunkSize == chunkSize &&
                           m_R == radius && m_W.seed == W.seed);
    m_W = W; m_R = radius; m_refLod = refLod; m_chunkSize = chunkSize;
    m_step = latticeStep(refLod, chunkSize);
    if (!sameGrid) clear();
}

void ReferenceSurface::clear() {
    std::lock_guard<std::mutex> lk(m_mx);
    m_cache.clear();
}

size_t ReferenceSurface::cachedCorners() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_cache.size();
}

double ReferenceSurface::cornerM(PlanetFace f, int64_t k, int64_t l) const {
    const uint64_t key = ((uint64_t)((int)f & 0x7))
                       | ((uint64_t)((uint64_t)k & 0x1FFFFFFF) << 3)
                       | ((uint64_t)((uint64_t)l & 0x1FFFFFFF) << 32);
    {
        std::lock_guard<std::mutex> lk(m_mx);
        auto it = m_cache.find(key);
        if (it != m_cache.end()) return (double)it->second;
    }

    const double numChunks = (double)(1u << m_refLod);
    const int64_t cx = k / m_chunkSize, vx = k % m_chunkSize;
    const int64_t cy = l / m_chunkSize, vy = l % m_chunkSize;
    const double u = ((double)cx + (double)vx / (double)m_chunkSize) / numChunks;
    const double v = ((double)cy + (double)vy / (double)m_chunkSize) / numChunks;
    const glm::dvec3 d = cubeFaceToDir(f, u * 2.0 - 1.0, v * 2.0 - 1.0);

    // Simple height: 0.0 (reference sphere). Subclasses/owners override via deformation layer.
    const double h = 0.0;
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_cache.size() >= (1u << 20)) m_cache.clear();
        m_cache.emplace(key, (float)h);
    }
    return h;
}

double ReferenceSurface::elevM(const glm::dvec3& dir) const {
    if (!ready()) return 0.0;

    PlanetFace f; double lx, ly;
    dirToCubeFace(dir, f, lx, ly);

    const double fu = (lx + 1.0) / m_step, fv = (ly + 1.0) / m_step;
    const int64_t k = (int64_t)std::floor(fu), l = (int64_t)std::floor(fv);
    const double tu = fu - (double)k, tv = fv - (double)l;

    const double h00 = cornerM(f, k,     l);
    const double h10 = cornerM(f, k + 1, l);
    const double h01 = cornerM(f, k,     l + 1);
    const double h11 = cornerM(f, k + 1, l + 1);
    return glm::mix(glm::mix(h00, h10, tu), glm::mix(h01, h11, tu), tv);
}

} // namespace Haruka
