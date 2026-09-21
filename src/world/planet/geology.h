#pragma once
#include <vector>
#include <random>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>   // glm::pi — llegaba de rebote por el cube_sphere.h borrado

namespace Haruka { namespace Planet {

// ===========================================================================
// Plate tectonics layer — mandatory
// Generates plates, boundary types, and base elevation modifiers.
// ===========================================================================

struct Plate {
    glm::dvec3 position;   // seed point on unit sphere
    glm::dvec3 velocity;   // drift direction * speed (radians per unit time)
    bool       oceanic;    // true = oceanic crust (thinner, lower)
    int        id;
};

struct GeologyConfig {
    int    numPlates   = 12;
    double oceanicFrac = 0.6;     // fraction of plates that are oceanic
    double convergenceRate = 1.0; // mountain height multiplier
    double riftingRate     = 0.3; // rift depth multiplier
    /**
     * @brief Semilla del reparto de placas. DETERMINISTA a propósito.
     *
     * Antes esto era `std::random_device`: cada arranque generaba un planeta DISTINTO con la misma
     * escena. Eso rompe tres cosas a la vez — no se puede reproducir un bug, no se puede comparar
     * un render con el anterior, y cliente y servidor generarían mundos diferentes (el motor
     * promete que el terreno es función pura de la semilla).
     */
    uint32_t seed = 1337u;
};

struct GeologyOutput {
    std::vector<Plate> plates;

    // Query functions — evaluate at any unit direction
    int    plateId(const glm::dvec3& dir) const;
    double boundaryDistance(const glm::dvec3& dir) const;   // 0 = on boundary
    double elevationModifier(const glm::dvec3& dir) const;  // km offset
};

// ===========================================================================
// Implementation
// ===========================================================================

namespace detail {

inline double greatCircleAngle(const glm::dvec3& a, const glm::dvec3& b) {
    double d = glm::dot(a, b);
    return std::acos(glm::clamp(d, -1.0, 1.0));
}

// Project velocity onto sphere tangent plane at position
inline glm::dvec3 tangentVelocity(const glm::dvec3& pos, const glm::dvec3& vel) {
    return vel - glm::dot(pos, vel) * pos;
}

} // namespace detail

inline GeologyOutput generateGeology(const GeologyConfig& cfg) {
    GeologyOutput out;
    std::mt19937_64 rng(cfg.seed);   // NO random_device: el planeta es función de la semilla
    auto rng01 = [&]() { return std::uniform_real_distribution<double>(0, 1)(rng); };

    // Generate plate seeds
    out.plates.resize(cfg.numPlates);
    for (int i = 0; i < cfg.numPlates; i++) {
        // Random point on sphere (uniform)
        double theta = rng01() * 2 * glm::pi<double>();
        double phi   = std::acos(2 * rng01() - 1);
        out.plates[i].position = {
            std::sin(phi) * std::cos(theta),
            std::cos(phi),
            std::sin(phi) * std::sin(theta)
        };
        // Random velocity (tangent to sphere)
        glm::dvec3 rawVel{ rng01() - 0.5, rng01() - 0.5, rng01() - 0.5 };
        out.plates[i].velocity = detail::tangentVelocity(out.plates[i].position, rawVel) * 0.1;
        out.plates[i].oceanic  = rng01() < cfg.oceanicFrac;
        out.plates[i].id = i;
    }

    return out;
}

// Determine which plate a direction belongs to (Voronoi on sphere)
inline int GeologyOutput::plateId(const glm::dvec3& dir) const {
    int best = -1;
    double bestAngle = 1e10;
    for (int i = 0; i < (int)plates.size(); i++) {
        double a = detail::greatCircleAngle(dir, plates[i].position);
        if (a < bestAngle) { bestAngle = a; best = i; }
    }
    return best;
}

// Compute distance to nearest plate boundary (0 = on boundary)
inline double GeologyOutput::boundaryDistance(const glm::dvec3& dir) const {
    double minDist = 1e10;
    for (int i = 0; i < (int)plates.size(); i++) {
        for (int j = i + 1; j < (int)plates.size(); j++) {
            // Midpoint between two plate seeds
            glm::dvec3 mid = glm::normalize(plates[i].position + plates[j].position);
            double angle = detail::greatCircleAngle(dir, mid);
            if (angle < minDist) minDist = angle;
        }
    }
    return minDist;
}

// Elevation modifier based on plate tectonics
inline double GeologyOutput::elevationModifier(const glm::dvec3& dir) const {
    int pid = plateId(dir);
    if (pid < 0) return 0;

    const Plate& p = plates[pid];

    // Base elevation: oceanic = -3 km, continental = +0.5 km
    double elev = p.oceanic ? -3.0 : 0.5;

    // Find nearest neighbor plate
    int nearest = -1;
    double nearestAngle = 1e10;
    for (int i = 0; i < (int)plates.size(); i++) {
        if (i == pid) continue;
        double a = detail::greatCircleAngle(dir, plates[i].position);
        if (a < nearestAngle) { nearestAngle = a; nearest = i; }
    }
    if (nearest < 0) return elev;

    // Distance to boundary
    glm::dvec3 boundaryPos = glm::normalize(p.position + plates[nearest].position);
    double distToBoundary = detail::greatCircleAngle(dir, boundaryPos);

    // Determine boundary type from relative velocity
    glm::dvec3 relVel = p.velocity - plates[nearest].velocity;
    glm::dvec3 boundaryDir = glm::normalize(boundaryPos - dir);
    double approach = glm::dot(relVel, boundaryDir);

    double mountainWidth = 0.15; // radians
    double influence = std::max(0.0, 1.0 - distToBoundary / mountainWidth);

    if (approach > 0) {
        // Convergent boundary
        if (p.oceanic && !plates[nearest].oceanic) {
            // Oceanic subducts under continental → volcanic arc
            elev += influence * 0.5; // volcanic range
        } else if (!p.oceanic && plates[nearest].oceanic) {
            // Continental over oceanic → mountain range
            elev += influence * 1.5;
        } else if (!p.oceanic && !plates[nearest].oceanic) {
            // Continental-continental collision → high mountains
            elev += influence * 3.0;
        } else {
            // Oceanic-oceanic convergence → island arc
            elev += influence * 0.8;
        }
    } else {
        // Divergent boundary
        if (p.oceanic) {
            // Mid-ocean ridge
            elev -= influence * 1.0; // less negative = ridge
        } else {
            // Continental rift
            elev -= influence * 0.5;
        }
    }

    return elev;
}

}} // namespace Haruka::Planet
