#include "softbody.h"

namespace Haruka::xpbd {

static void addEdge(XPBDSolver& s, int a, int b, float compliance) {
    DistanceConstraint c;
    c.a = a; c.b = b;
    c.restLength = glm::length(s.particles.position[a] - s.particles.position[b]);
    c.compliance = compliance;
    s.distanceConstraints.push_back(c);
}

SoftBodyHandle makeCloth(XPBDSolver& solver,
                         const glm::vec3& origin,
                         const glm::vec3& right,
                         const glm::vec3& down,
                         int nx, int ny,
                         float totalMass,
                         bool pinTopRow,
                         float compliance,
                         float bendCompliance) {
    nx = (nx < 2) ? 2 : nx;
    ny = (ny < 2) ? 2 : ny;

    SoftBodyHandle h;
    h.firstParticle = solver.particles.count();
    h.particleCount = nx * ny;
    h.doubleSided   = true;

    const float perParticleMass = totalMass / float(nx * ny);
    const glm::vec3 du = right / float(nx - 1);
    const glm::vec3 dv = down  / float(ny - 1);

    auto gid = [&](int i, int j) { return h.firstParticle + j * nx + i; };

    // Particles (row j=0 is the "top" that can be pinned).
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            glm::vec3 p = origin + du * float(i) + dv * float(j);
            float mass = (pinTopRow && j == 0) ? 0.0f : perParticleMass;
            solver.particles.add(p, mass);
        }
    }

    // Structural (horizontal + vertical) + shear (diagonals).
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            if (i + 1 < nx) addEdge(solver, gid(i, j), gid(i + 1, j), compliance);
            if (j + 1 < ny) addEdge(solver, gid(i, j), gid(i, j + 1), compliance);
            if (i + 1 < nx && j + 1 < ny) {
                addEdge(solver, gid(i, j),     gid(i + 1, j + 1), compliance);
                addEdge(solver, gid(i + 1, j), gid(i,     j + 1), compliance);
            }
        }
    }
    // Bending (skip-one neighbours) keeps cloth from folding flat.
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            if (i + 2 < nx) addEdge(solver, gid(i, j), gid(i + 2, j), bendCompliance);
            if (j + 2 < ny) addEdge(solver, gid(i, j), gid(i, j + 2), bendCompliance);
        }

    // Render triangles (two per quad).
    for (int j = 0; j + 1 < ny; ++j)
        for (int i = 0; i + 1 < nx; ++i) {
            unsigned int a = gid(i, j),     b = gid(i + 1, j);
            unsigned int c = gid(i, j + 1), d = gid(i + 1, j + 1);
            h.renderIndices.insert(h.renderIndices.end(), {a, c, b, b, c, d});
        }

    return h;
}

SoftBodyHandle makeBox(XPBDSolver& solver,
                       const glm::vec3& origin,
                       const glm::vec3& size,
                       int nx, int ny, int nz,
                       float totalMass,
                       float edgeCompliance,
                       float volumeCompliance) {
    nx = (nx < 1) ? 1 : nx;
    ny = (ny < 1) ? 1 : ny;
    nz = (nz < 1) ? 1 : nz;
    const int vx = nx + 1, vy = ny + 1, vz = nz + 1;

    SoftBodyHandle h;
    h.firstParticle = solver.particles.count();
    h.particleCount = vx * vy * vz;

    const float perParticleMass = totalMass / float(h.particleCount);
    const glm::vec3 step = size / glm::vec3(float(nx), float(ny), float(nz));

    auto gid = [&](int i, int j, int k) {
        return h.firstParticle + (k * vy + j) * vx + i;
    };

    for (int k = 0; k < vz; ++k)
        for (int j = 0; j < vy; ++j)
            for (int i = 0; i < vx; ++i)
                solver.particles.add(origin + step * glm::vec3(i, j, k), perParticleMass);

    // Edges of each cell (12 per cell, dedup via i/j/k forward neighbours).
    for (int k = 0; k < vz; ++k)
        for (int j = 0; j < vy; ++j)
            for (int i = 0; i < vx; ++i) {
                if (i + 1 < vx) addEdge(solver, gid(i,j,k), gid(i+1,j,k), edgeCompliance);
                if (j + 1 < vy) addEdge(solver, gid(i,j,k), gid(i,j+1,k), edgeCompliance);
                if (k + 1 < vz) addEdge(solver, gid(i,j,k), gid(i,j,k+1), edgeCompliance);
            }

    // Volume constraints: 5 tets per cell fill the lattice (incompressibility).
    auto addTet = [&](int a, int b, int c, int d) {
        VolumeConstraint vc;
        vc.a = a; vc.b = b; vc.c = c; vc.d = d;
        vc.restVolume = VolumeConstraint::sixVolume(
            solver.particles.position[a], solver.particles.position[b],
            solver.particles.position[c], solver.particles.position[d]);
        vc.compliance = volumeCompliance;
        solver.volumeConstraints.push_back(vc);
    };
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                int v000 = gid(i,j,k),     v100 = gid(i+1,j,k);
                int v010 = gid(i,j+1,k),   v110 = gid(i+1,j+1,k);
                int v001 = gid(i,j,k+1),   v101 = gid(i+1,j,k+1);
                int v011 = gid(i,j+1,k+1), v111 = gid(i+1,j+1,k+1);
                addTet(v000, v100, v010, v001);
                addTet(v100, v110, v010, v111);
                addTet(v100, v010, v001, v111);
                addTet(v100, v101, v001, v111);
                addTet(v010, v001, v011, v111);
            }

    // Render: outer surface quads (6 faces) as triangles.
    auto face = [&](int a, int b, int c, int d) {
        h.renderIndices.insert(h.renderIndices.end(),
            {(unsigned)a,(unsigned)b,(unsigned)c,(unsigned)a,(unsigned)c,(unsigned)d});
    };
    for (int j = 0; j < ny; ++j) for (int i = 0; i < nx; ++i) {
        face(gid(i,j,0), gid(i,j+1,0), gid(i+1,j+1,0), gid(i+1,j,0));        // -z
        face(gid(i,j,vz-1), gid(i+1,j,vz-1), gid(i+1,j+1,vz-1), gid(i,j+1,vz-1)); // +z
    }
    for (int k = 0; k < nz; ++k) for (int i = 0; i < nx; ++i) {
        face(gid(i,0,k), gid(i+1,0,k), gid(i+1,0,k+1), gid(i,0,k+1));        // -y
        face(gid(i,vy-1,k), gid(i,vy-1,k+1), gid(i+1,vy-1,k+1), gid(i+1,vy-1,k)); // +y
    }
    for (int k = 0; k < nz; ++k) for (int j = 0; j < ny; ++j) {
        face(gid(0,j,k), gid(0,j,k+1), gid(0,j+1,k+1), gid(0,j+1,k));        // -x
        face(gid(vx-1,j,k), gid(vx-1,j+1,k), gid(vx-1,j+1,k+1), gid(vx-1,j,k+1)); // +x
    }

    return h;
}

} // namespace Haruka::xpbd
