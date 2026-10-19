#include "Haruka/World/Props/BlobSurface.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>

namespace Haruka::World::Props {

namespace {
/// Mínimo suave polinómico (Quílez): funde dos distancias en un radio `k`.
float smin(float a, float b, float k) {
    const float h = std::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return b + (a - b) * h - k * h * (1.0f - h);
}

uint32_t bumpHash(int x, int y, int z, uint32_t seed) {
    uint32_t v = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u ^ seed * 2654435761u;
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
    return v;
}

/// Relieve de BOLITAS: una cúpula (casquete de esfera, r = 0,7) por celda de lado 1 en un punto al
/// azar del centro de la celda; la altura de la más alta que cubre `p` (0..1). Con el punto en
/// [0,25; 0,75] basta mirar las 2×2×2 celdas del lado de `p`.
float bumps(const glm::vec3& p, uint32_t seed) {
    const glm::vec3 cell = glm::floor(p), f = p - cell;
    const int sx = f.x < 0.5f ? -1 : 1, sy = f.y < 0.5f ? -1 : 1, sz = f.z < 0.5f ? -1 : 1;
    float best = 0.0f;
    for (int i = 0; i < 8; ++i) {
        const int ox = (i & 1) * sx, oy = ((i >> 1) & 1) * sy, oz = ((i >> 2) & 1) * sz;
        const uint32_t h = bumpHash((int)cell.x + ox, (int)cell.y + oy, (int)cell.z + oz, seed);
        const glm::vec3 jit((float)(h & 1023u), (float)((h >> 10) & 1023u), (float)((h >> 20) & 1023u));
        const glm::vec3 d = glm::vec3((float)ox, (float)oy, (float)oz) + 0.25f + 0.5f * (jit / 1023.0f) - f;
        best = std::max(best, 0.49f - glm::dot(d, d));
    }
    return std::sqrt(std::max(best, 0.0f)) * 1.4286f;
}
} // namespace

float blobSurfaceSdf(const std::vector<ClumpBlob>& blobs, const BlobSurfaceParams& prm, const glm::vec3& p) {
    float d = 1e30f;
    for (const ClumpBlob& b : blobs) {
        const glm::vec3 q0 = p - b.center;
        const float c = std::cos(b.yaw), s = std::sin(b.yaw);
        const glm::vec3 q(q0.x * c + q0.z * s, q0.y, q0.x * s - q0.z * c);
        // Elipsoide entero también para los domos: la base la corta el objeto ENTERO (abajo). Si cada
        // domo cortara la suya, con tapas a alturas un poco distintas, la panza eran platos apilados.
        const float k0 = glm::length(q / b.radii), k1 = glm::length(q / (b.radii * b.radii));
        d = smin(d, k0 * (k0 - 1.0f) / std::max(k1, 1e-6f), prm.smoothK);
    }
    if (prm.bump > 0.0f && d < 2.0f * prm.bumpAmp) {
        const float t = std::clamp(p.y / std::max(prm.baseFade, 1e-6f), 0.0f, 1.0f);
        const float base = t * t * (3.0f - 2.0f * t);
        const float r = bumps(p / prm.bump, prm.seed) + 0.4f * bumps(p / (0.42f * prm.bump), prm.seed + 1u);
        d -= prm.bumpAmp * base * r;
    }
    return std::max(d, -p.y);   // la base plana
}

bool bakeBlobSurface(const std::vector<ClumpBlob>& blobs, const BlobSurfaceParams& prm,
                     std::vector<PropVertex>& verts, std::vector<uint32_t>& idx) {
    verts.clear(); idx.clear();
    if (blobs.empty()) return false;

    // La caja: los bultos + lo que saca el relieve + dos celdas (y bajo la base, para que la tapa
    // plana salga como superficie).
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const ClumpBlob& b : blobs) {
        const float rh = std::max(b.radii.x, b.radii.z);
        lo = glm::min(lo, glm::vec3(b.center.x - rh, 0.0f, b.center.z - rh));
        hi = glm::max(hi, b.center + glm::vec3(rh, b.radii.y, rh));
    }
    const float span = std::max(hi.x - lo.x, hi.z - lo.z);
    const float cs = span / (float)std::max(prm.cells, 4);
    const float pad = 1.6f * prm.bumpAmp + prm.smoothK * 0.5f + 2.0f * cs;
    lo -= glm::vec3(pad, 2.0f * cs, pad);
    hi += glm::vec3(pad);
    const int nx = (int)std::ceil((hi.x - lo.x) / cs) + 1;
    const int ny = (int)std::ceil((hi.y - lo.y) / cs) + 1;
    const int nz = (int)std::ceil((hi.z - lo.z) / cs) + 1;
    // El nodo de abajo cae MEDIA celda bajo la base: un nodo justo en y = 0 daría distancia 0 exacta.
    lo.y = -1.5f * cs;

    auto node = [&](int i, int j, int k) { return lo + glm::vec3((float)i, (float)j, (float)k) * cs; };
    std::vector<float> f((size_t)nx * ny * nz);
    auto F = [&](int i, int j, int k) -> float& { return f[((size_t)k * ny + j) * nx + i]; };
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) F(i, j, k) = blobSurfaceSdf(blobs, prm, node(i, j, k));

    // SURFACE NETS: un vértice por celda cortada (la media de los cortes de sus aristas) y un quad por
    // arista cortada (las 4 celdas que la rodean).
    const int cx = nx - 1, cy = ny - 1, cz = nz - 1;
    std::vector<int32_t> cellV((size_t)cx * cy * cz, -1);
    auto CV = [&](int i, int j, int k) -> int32_t& { return cellV[((size_t)k * cy + j) * cx + i]; };
    static const int kCorner[8][3] = { {0,0,0},{1,0,0},{0,1,0},{1,1,0},{0,0,1},{1,0,1},{0,1,1},{1,1,1} };
    static const int kEdge[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
    const uint32_t white = glm::packUnorm4x8(glm::vec4(1.0f, 1.0f, 1.0f, 0.25f));
    const float eps = 0.5f * cs;
    for (int k = 0; k < cz; ++k)
        for (int j = 0; j < cy; ++j)
            for (int i = 0; i < cx; ++i) {
                float v[8]; int inside = 0;
                for (int c = 0; c < 8; ++c) {
                    v[c] = F(i + kCorner[c][0], j + kCorner[c][1], k + kCorner[c][2]);
                    inside += v[c] < 0.0f;
                }
                if (inside == 0 || inside == 8) continue;
                glm::vec3 acc(0.0f); int n = 0;
                for (const auto& e : kEdge) {
                    const float a = v[e[0]], b = v[e[1]];
                    if ((a < 0.0f) == (b < 0.0f)) continue;
                    const float t = a / (a - b);
                    const glm::vec3 pa(kCorner[e[0]][0], kCorner[e[0]][1], kCorner[e[0]][2]);
                    const glm::vec3 pb(kCorner[e[1]][0], kCorner[e[1]][1], kCorner[e[1]][2]);
                    acc += pa + (pb - pa) * t; ++n;
                }
                const glm::vec3 p = node(i, j, k) + (acc / (float)n) * cs;
                const glm::vec3 g(blobSurfaceSdf(blobs, prm, p + glm::vec3(eps, 0, 0)) - blobSurfaceSdf(blobs, prm, p - glm::vec3(eps, 0, 0)),
                                  blobSurfaceSdf(blobs, prm, p + glm::vec3(0, eps, 0)) - blobSurfaceSdf(blobs, prm, p - glm::vec3(0, eps, 0)),
                                  blobSurfaceSdf(blobs, prm, p + glm::vec3(0, 0, eps)) - blobSurfaceSdf(blobs, prm, p - glm::vec3(0, 0, eps)));
                PropVertex pv;
                pv.pos = glm::vec3(p.x, std::max(p.y, 0.0f), p.z);   // la base, exacta en y = 0
                const glm::vec3 nrm = (glm::dot(g, g) > 1e-20f) ? glm::normalize(g) : glm::vec3(0, 1, 0);
                pv.normal = glm::packSnorm3x10_1x2(glm::vec4(nrm, 0.0f));
                pv.color = white;
                pv.uv = glm::vec2(0.0f);
                pv.partId = 0.0f;
                CV(i, j, k) = (int32_t)verts.size();
                verts.push_back(pv);
            }

    // El sentido de cada triángulo: el de la normal de la superficie (el gradiente de la distancia en
    // sus tres vértices), cada uno POR SEPARADO. ⚠️ Decidirlo por quad dejaba el segundo triángulo de
    // un quad no plano al revés (0,5 %, medido en `blob_surface_winding`), y decidirlo por el eje de la
    // arista falla donde surface nets pliega un triángulo en un rasgo fino (1,3 %).
    auto nrmOf = [&](int32_t vi) { return glm::vec3(glm::unpackSnorm3x10_1x2(verts[(size_t)vi].normal)); };
    auto tri = [&](int32_t a, int32_t b, int32_t c) {
        const glm::vec3 pa = verts[(size_t)a].pos, pb = verts[(size_t)b].pos, pc = verts[(size_t)c].pos;
        const glm::vec3 out = nrmOf(a) + nrmOf(b) + nrmOf(c);
        if (glm::dot(glm::cross(pb - pa, pc - pa), out) >= 0.0f) idx.insert(idx.end(), { (uint32_t)a, (uint32_t)b, (uint32_t)c });
        else                                                     idx.insert(idx.end(), { (uint32_t)a, (uint32_t)c, (uint32_t)b });
    };
    auto quad = [&](int32_t a, int32_t b, int32_t c, int32_t d) {
        if (a < 0 || b < 0 || c < 0 || d < 0) return;
        tri(a, b, c); tri(a, c, d);
    };
    for (int k = 1; k < nz - 1; ++k)
        for (int j = 1; j < ny - 1; ++j)
            for (int i = 1; i < nx - 1; ++i) {
                const bool s0 = F(i, j, k) < 0.0f;
                if (s0 != (F(i + 1, j, k) < 0.0f))   // arista en x
                    quad(CV(i, j - 1, k - 1), CV(i, j, k - 1), CV(i, j, k), CV(i, j - 1, k));
                if (s0 != (F(i, j + 1, k) < 0.0f))   // arista en y
                    quad(CV(i - 1, j, k - 1), CV(i, j, k - 1), CV(i, j, k), CV(i - 1, j, k));
                if (s0 != (F(i, j, k + 1) < 0.0f))   // arista en z
                    quad(CV(i - 1, j - 1, k), CV(i, j - 1, k), CV(i, j, k), CV(i - 1, j, k));
            }
    return !idx.empty();
}

} // namespace Haruka::World::Props
