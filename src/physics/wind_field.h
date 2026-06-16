/**
 * @file wind_field.h
 * @brief Localized wind as a set of ZONES, not a global force.
 *
 * Each WindZone is a sphere in world space with a direction + strength. A point
 * is only pushed by the zones it falls inside; outside every zone the wind is
 * zero. Multiple overlapping zones sum. This is the data model the future storm
 * system feeds: the server/gameplay declares "storm here: pos, radius, dir,
 * force, duration" and the client just adds a zone — the simulation reads the
 * field via WindField::sample() and needs no other changes.
 *
 * Shared by the XPBD softbody solver (cloth) and any other system that wants
 * wind (PBF spray, particles, foliage…), via a sample callback.
 */
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cmath>

namespace Haruka {

struct WindZone {
    glm::dvec3 center{0.0};        ///< world-space centre
    double     radius = 10.0;      ///< influence radius (m)
    glm::vec3  direction{1,0,0};   ///< unit push direction
    float      strength = 5.0f;    ///< steady acceleration (m/s²) at the centre
    float      gust = 0.0f;        ///< gust amplitude (×strength), time-varying
    float      ttl = -1.0f;        ///< seconds remaining; <0 = permanent (storm duration)
};

class WindField {
public:
    std::vector<WindZone> zones;
    float time = 0.0f;

    /** @brief Advances gust phase and expires timed zones. */
    void advance(float dt) {
        time += dt;
        for (size_t i = 0; i < zones.size();) {
            if (zones[i].ttl >= 0.0f) {
                zones[i].ttl -= dt;
                if (zones[i].ttl <= 0.0f) { zones[i] = zones.back(); zones.pop_back(); continue; }
            }
            ++i;
        }
    }

    /** @brief Adds a zone, returns its index. */
    int add(const WindZone& z) { zones.push_back(z); return (int)zones.size() - 1; }
    void clear() { zones.clear(); }

    /**
     * @brief Wind acceleration (m/s²) at a world position = sum of the zones it
     * lies inside, each with smooth edge falloff and a travelling gust ripple.
     * Returns zero outside every zone.
     */
    glm::vec3 sample(const glm::dvec3& worldPos) const {
        glm::vec3 acc(0.0f);
        for (const auto& z : zones) {
            glm::dvec3 d = worldPos - z.center;
            double dist = glm::length(d);
            if (dist >= z.radius) continue;
            // Smooth falloff: 1 at centre → 0 at edge (smoothstep).
            float t = float(dist / z.radius);
            float fall = 1.0f - t * t * (3.0f - 2.0f * t);
            float s = z.strength;
            if (z.gust != 0.0f) {
                float ph = std::sin(time * 3.1f + float(worldPos.x) * 0.7f + float(worldPos.y) * 1.3f)
                         + std::sin(time * 1.7f + float(worldPos.z) * 0.9f);
                s += z.strength * z.gust * 0.5f * ph;
            }
            acc += z.direction * (s * fall);
        }
        return acc;
    }
};

} // namespace Haruka
