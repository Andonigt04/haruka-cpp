#include "hybrid_water.h"
#include "shallow_water.h"
#include "pbf_solver.h"
#include <glm/glm.hpp>
#include <algorithm>

namespace Haruka::fluid {

void HybridWater::update(float /*dt*/) {
    // 1. River → ocean: pin coastal heightfield cells to sea level.
    if (heightfield && seaLevelAlongUp > -1e8f)
        heightfield->applySeaLevel(seaLevelAlongUp);

    // 2. Cascade → lake: absorb PBF particles that fell into the lake/heightfield.
    absorbParticles();
}

void HybridWater::absorbParticles() {
    if (!heightfield || !particles) return;

    const glm::dvec3 patchUp = heightfield->up();
    const glm::dvec3 anchor  = heightfield->anchor();
    const float pvol = particles->particleVolume();
    // Absorption band: a particle within ~one smoothing radius of the surface
    // (lake water OR the terrain floor) has "merged" and becomes heightfield
    // water. This lets a puddle start on dry ground (settled splash) and a
    // cascade merge into an existing lake — both mass-conserving.
    const float band = particles->h;

    // Iterate backwards so swap-remove (removeParticle) stays valid.
    for (int i = particles->count() - 1; i >= 0; --i) {
        glm::dvec3 wp = particles->worldPos(i);
        if (!heightfield->containsWorld(wp)) continue;

        float particleH = (float)glm::dot(wp - anchor, patchUp);
        // Surface to merge into = max(local water surface, terrain floor).
        float waterSurf = waterLevelAlongUp(wp);
        float floorH    = heightfield->surfaceAlongUpAtWorld(wp); // terrain+water; floor when dry
        float mergeH    = std::max(waterSurf, floorH);

        if (particleH <= mergeH + band) {
            heightfield->addVolumeAtWorld(wp, pvol);
            particles->removeParticle(i);
        }
    }
}

float HybridWater::waterLevelAlongUp(const glm::dvec3& worldPos) const {
    float lvl = seaLevelAlongUp; // ocean baseline (very negative if disabled)
    if (heightfield && heightfield->containsWorld(worldPos)) {
        float hfSurf = heightfield->surfaceAlongUpAtWorld(worldPos);
        if (hfSurf > -1e8f) lvl = std::max(lvl, hfSurf);
    }
    return lvl;
}

} // namespace Haruka::fluid
