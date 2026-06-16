/**
 * @file softbody.h
 * @brief Fase B builders: turn meshes into XPBD particles + constraints.
 *
 * - makeCloth: a grid of particles with structural + shear + bending distance
 *   constraints → flags, capes, sails.
 * - makeBox:   a filled lattice of particles with edge (distance) + volume
 *   (tetra) constraints → jelly / deformable solids.
 *
 * Both append into an existing XPBDSolver so many softbodies share one solver
 * (and one collision/gravity setup). They return a SoftBodyHandle describing the
 * particle/index ranges for the renderer.
 */
#pragma once

#include "xpbd_solver.h"
#include <glm/glm.hpp>
#include <vector>

namespace Haruka::xpbd {

/** @brief Maps a built softbody to its particle range + render triangle indices. */
struct SoftBodyHandle {
    int firstParticle = 0;
    int particleCount = 0;
    std::vector<unsigned int> renderIndices; // triangles into the global particle array
    bool doubleSided = false;                // cloth: render both faces
};

/**
 * @brief Builds a cloth grid in the solver's local frame.
 * @param solver    target solver (particles/constraints appended)
 * @param origin    local-frame position of grid corner (0,0)
 * @param right,down  edge vectors spanning the grid (length = total span)
 * @param nx,ny     vertices per axis (>=2)
 * @param totalMass mass distributed across all particles
 * @param pinTopRow if true, the top row (j==0) is pinned (a hanging flag)
 * @param compliance edge stiffness (0=rigid cloth, larger=stretchy)
 */
SoftBodyHandle makeCloth(XPBDSolver& solver,
                         const glm::vec3& origin,
                         const glm::vec3& right,
                         const glm::vec3& down,
                         int nx, int ny,
                         float totalMass,
                         bool pinTopRow,
                         float compliance = 0.0f,
                         float bendCompliance = 0.02f);

/**
 * @brief Builds a filled deformable box (jelly) as a lattice of tetrahedra.
 * @param solver    target solver
 * @param origin    local-frame min corner
 * @param size      box dimensions (m)
 * @param nx,ny,nz  cells per axis (>=1)
 * @param totalMass mass of the whole body
 * @param edgeCompliance / volumeCompliance  softness knobs
 */
SoftBodyHandle makeBox(XPBDSolver& solver,
                       const glm::vec3& origin,
                       const glm::vec3& size,
                       int nx, int ny, int nz,
                       float totalMass,
                       float edgeCompliance = 0.001f,
                       float volumeCompliance = 0.0f);

} // namespace Haruka::xpbd
