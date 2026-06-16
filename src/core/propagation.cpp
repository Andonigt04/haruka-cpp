#include "core/propagation.h"

#include "core/application.h"
#include "game/planetary_system.h"
#include "physics/physics_engine.h"
#include "renderer/motor_instance.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace Haruka {

// --- Segmento ∩ OBB (slab test en el espacio local de la caja) -----------------------
static bool segmentHitsOBB(const glm::dvec3& a, const glm::dvec3& b, const StaticOBB& o) {
    glm::dmat3 inv = glm::transpose(o.rot);          // rot ortonormal → inversa = transpuesta
    glm::dvec3 d = inv * (b - a);
    glm::dvec3 m = inv * (a - o.center);
    double tmin = 0.0, tmax = 1.0;
    for (int i = 0; i < 3; ++i) {
        double he = o.halfExtents[i] + 0.05;         // pelín de margen
        if (std::abs(d[i]) < 1e-9) { if (m[i] < -he || m[i] > he) return false; }
        else {
            double t1 = (-he - m[i]) / d[i], t2 = (he - m[i]) / d[i];
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1); tmax = std::min(tmax, t2);
            if (tmin > tmax) return false;
        }
    }
    return true;
}

// Oclusión del terreno entre a y b: fracción del tramo que NO queda bajo la superficie.
static float terrainOcclusion(const glm::dvec3& a, const glm::dvec3& b) {
    Application* app = MotorInstance::getInstance().getApplication();
    auto* ps = app ? app->getPlanetarySystem() : nullptr;
    if (!app || !ps) return 1.0f;
    glm::dvec3 center; double radius; uint32_t sd; float rl;
    if (!ps->getActivePlanet(center, radius, sd, rl)) return 1.0f;
    const int N = 8; int blocked = 0;
    for (int i = 1; i < N; ++i) {
        glm::dvec3 p = glm::mix(a, b, (double)i / N);
        if (glm::length(p - center) < radius + app->getTerrainHeightAt(p) - 0.5) ++blocked;
    }
    return 1.0f - (float)blocked / (float)(N - 1);
}

static const std::vector<StaticOBB>* placedOBBs() {
    Application* app = MotorInstance::getInstance().getApplication();
    auto* phys = app ? app->getPhysicsEngine() : nullptr;
    return phys ? &phys->getPlacedOBBs() : nullptr;
}

// ¿Algún muro (OBB) corta el segmento?
static bool blockedByWall(const glm::dvec3& a, const glm::dvec3& b, const std::vector<StaticOBB>* obbs) {
    if (!obbs) return false;
    for (const auto& o : *obbs) if (segmentHitsOBB(a, b, o)) return true;
    return false;
}

// ¿El terreno corta el segmento? (algún punto del tramo queda BAJO la superficie). Bloqueo DURO
// (vs terrainOcclusion que es gradual) → se usa para enrutar el rodeo de colinas.
static bool terrainBlocks(const glm::dvec3& a, const glm::dvec3& b) {
    Application* app = MotorInstance::getInstance().getApplication();
    auto* ps = app ? app->getPlanetarySystem() : nullptr;
    if (!app || !ps) return false;
    glm::dvec3 center; double radius; uint32_t sd; float rl;
    if (!ps->getActivePlanet(center, radius, sd, rl)) return false;
    const int N = 12;
    for (int i = 1; i < N; ++i) {
        glm::dvec3 p = glm::mix(a, b, (double)i / N);
        if (glm::length(p - center) < radius + app->getTerrainHeightAt(p) - 0.3) return true;
    }
    return false;
}

// Punto a 'clearance' metros SOBRE la superficie en la vertical de p (waypoint de aire válido).
static glm::dvec3 snapAboveTerrain(const glm::dvec3& p, double clearance) {
    Application* app = MotorInstance::getInstance().getApplication();
    auto* ps = app ? app->getPlanetarySystem() : nullptr;
    if (!app || !ps) return p;
    glm::dvec3 center; double radius; uint32_t sd; float rl;
    if (!ps->getActivePlanet(center, radius, sd, rl)) return p;
    glm::dvec3 dir = p - center; double dl = glm::length(dir);
    if (dl < 1e-6) return p;
    dir /= dl;
    return center + dir * (radius + app->getTerrainHeightAt(p) + clearance);
}

// Bloqueo total (terreno O muro) — para las aristas del grafo de navegación.
static bool segmentBlocked(const glm::dvec3& a, const glm::dvec3& b, const std::vector<StaticOBB>* obbs) {
    return terrainBlocks(a, b) || blockedByWall(a, b, obbs);
}

std::vector<glm::dvec3> Propagation::path(const glm::dvec3& a, const glm::dvec3& b) {
    const std::vector<StaticOBB>* obbs = placedOBBs();
    if (!segmentBlocked(a, b, obbs)) return { a, b };     // recto: ni muro ni colina en medio

    // Nodos = A, B + esquinas de muros cercanos + waypoints de rodeo de TERRENO (laterales
    // pegados al suelo y por encima) → Dijkstra elige el camino más corto sin bloqueo.
    std::vector<glm::dvec3> nodes = { a, b };
    if (obbs) {
        glm::dvec3 mid = (a + b) * 0.5; double seg = glm::length(b - a) + 1.0;
        for (const auto& o : *obbs) {
            if (glm::length(o.center - mid) > seg + glm::length(o.halfExtents)) continue;  // lejos
            for (int s = 0; s < 8; ++s) {
                glm::dvec3 sgn((s & 1) ? 1 : -1, (s & 2) ? 1 : -1, (s & 4) ? 1 : -1);
                glm::dvec3 local = o.halfExtents * sgn * 1.0;
                nodes.push_back(o.center + o.rot * (local + glm::normalize(local) * 0.6)); // esquina + margen
            }
            if (nodes.size() > 40) break;
        }
    }
    // Rodeo de terreno (colinas): base tangente; candidatos a los lados (pegados al suelo) y
    // por encima, a varias fracciones del recorrido.
    {
        glm::dvec3 dir = b - a; double len = glm::length(dir);
        // "Arriba" RADIAL respecto al CENTRO del planeta (no al origen del mundo, que está a 1 UA).
        glm::dvec3 up(0, 1, 0);
        if (Application* app = MotorInstance::getInstance().getApplication())
            if (auto* ps = app->getPlanetarySystem()) {
                glm::dvec3 c; double r; uint32_t sd; float rl;
                if (ps->getActivePlanet(c, r, sd, rl)) {
                    glm::dvec3 d = a - c; double dl = glm::length(d);
                    if (dl > 1e-6) up = d / dl;
                }
            }
        if (len > 1e-6) {
            dir /= len;
            glm::dvec3 right = glm::cross(dir, up);
            if (glm::length(right) > 1e-6) right = glm::normalize(right);
            const double fr[]  = { 0.33, 0.5, 0.66 };
            const double lat[] = { 10.0, 22.0, 40.0 };
            for (double f : fr) {
                glm::dvec3 c = glm::mix(a, b, f);
                for (double L : lat) {                                 // a los lados, pegado al suelo
                    nodes.push_back(snapAboveTerrain(c + right * L,  2.0));
                    nodes.push_back(snapAboveTerrain(c - right * L,  2.0));
                }
                nodes.push_back(c + up * 25.0);                        // por encima de la colina
                nodes.push_back(c + up * 50.0);
                if (nodes.size() > 70) break;
            }
        }
    }

    // Dijkstra A(0)→B(1) con aristas = segmentos sin bloqueo (terreno+muro). coste = distancia.
    int n = (int)nodes.size();
    std::vector<double> dist(n, std::numeric_limits<double>::infinity());
    std::vector<int> prev(n, -1);
    std::priority_queue<std::pair<double,int>, std::vector<std::pair<double,int>>, std::greater<>> pq;
    dist[0] = 0.0; pq.push({0.0, 0});
    while (!pq.empty()) {
        auto [du, u] = pq.top(); pq.pop();
        if (du > dist[u]) continue;
        if (u == 1) break;
        for (int v = 0; v < n; ++v) {
            if (v == u) continue;
            if (segmentBlocked(nodes[u], nodes[v], obbs)) continue;   // terreno + muros
            double nd = du + glm::length(nodes[v] - nodes[u]);
            if (nd < dist[v]) { dist[v] = nd; prev[v] = u; pq.push({nd, v}); }
        }
    }
    if (prev[1] < 0 && dist[1] == std::numeric_limits<double>::infinity()) return {}; // sin paso
    std::vector<glm::dvec3> out;
    for (int u = 1; u != -1; u = prev[u]) out.push_back(nodes[u]);
    std::reverse(out.begin(), out.end());
    return out;
}

float Propagation::through(const glm::dvec3& a, const glm::dvec3& b) {
    std::vector<glm::dvec3> p = path(a, b);
    if (p.empty()) return 0.1f;                            // encerrado: solo fuga amortiguada
    float terr = 1.0f; double len = 0.0;
    for (size_t i = 1; i < p.size(); ++i) {
        terr *= terrainOcclusion(p[i - 1], p[i]);
        len  += glm::length(p[i] - p[i - 1]);
    }
    double direct = glm::length(b - a);
    float detour = direct > 1e-6 ? (float)std::clamp(direct / len, 0.3, 1.0) : 1.0f; // rodeo → más bajo
    return std::max(0.1f, terr * detour);
}

} // namespace Haruka
