/**
 * @file hybrid_water.h
 * @brief Fase E coupler: unifies the three water domains into one coherent fluid.
 *
 *   ocean (Gerstner surface)  +  shallow-water heightfield (rivers/lakes)
 *                             +  PBF particles (splash/cascade)
 *
 * Responsibilities:
 *  1. waterLevelAlongUp(): authoritative water surface = max(ocean, heightfield).
 *  2. cascade → lake: PBF particles that land inside the heightfield patch and
 *     fall below its water surface are absorbed — their volume is poured into the
 *     heightfield cell and the particle is removed (mass-conserving).
 *  3. river → ocean: the heightfield's coastal cells are pinned to sea level so
 *     rivers meet the sea instead of piling up at the patch edge.
 *
 * The coupler holds non-owning pointers; the game owns the systems. All work is
 * done in the shared up-relative frame of the heightfield patch.
 */
#pragma once

#include <glm/glm.hpp>

namespace Haruka::fluid {

class ShallowWaterSim;
class PBFSolver;

class HybridWater {
public:
    ShallowWaterSim* heightfield = nullptr; // rivers/lakes patch
    PBFSolver*       particles   = nullptr; // splash/cascade fluid

    // Sea level expressed in the heightfield's up-relative frame (metres).
    // Set < -1e8 to disable the ocean coupling (no sea boundary).
    float seaLevelAlongUp = -1e9f;

    /** @brief Runs one coupling tick after the individual sims have stepped. */
    void update(float dt);

    /**
     * @brief Authoritative water surface height (up-relative) at a world pos,
     *        combining ocean sea level and the heightfield. Returns the higher of
     *        the two where the patch applies; sea level elsewhere.
     */
    float waterLevelAlongUp(const glm::dvec3& worldPos) const;

private:
    void absorbParticles();
};

} // namespace Haruka::fluid
