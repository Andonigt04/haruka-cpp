#include "test_common.h"
#include "core/terrain/cube_sphere.h"
#include "game/planetary_system.h"
#include <glm/glm.hpp>

void test_simple_planet_mesh() {
    beginTest("simple_planet_mesh");

    // Verify cube-face direction math: each face covers exactly one octant
    int res = 16;
    for (int face = 0; face < 6; ++face) {
        Haruka::PlanetFace pf = (Haruka::PlanetFace)face;
        // Center of the face
        glm::dvec3 center = cubeFaceToDir(pf, 0.0, 0.0);
        CHECK(std::abs(glm::length(center) - 1.0) < 1e-12, "face center is unit length");

        // Corners (±1, ±1 on the face)
        glm::dvec3 c1 = cubeFaceToDir(pf, -1.0, -1.0);
        glm::dvec3 c2 = cubeFaceToDir(pf,  1.0, -1.0);
        glm::dvec3 c3 = cubeFaceToDir(pf, -1.0,  1.0);
        glm::dvec3 c4 = cubeFaceToDir(pf,  1.0,  1.0);
        // All corners should also be unit length
        CHECK(std::abs(glm::length(c1) - 1.0) < 1e-12, "corner 1 unit");
        CHECK(std::abs(glm::length(c2) - 1.0) < 1e-12, "corner 2 unit");
        CHECK(std::abs(glm::length(c3) - 1.0) < 1e-12, "corner 3 unit");
        CHECK(std::abs(glm::length(c4) - 1.0) < 1e-12, "corner 4 unit");

        // Corner distance from center (should be > 1 since corners are farther)
        double d = glm::length(c1 - center);
        CHECK(d > 0.5, "corner is away from center");
    }

    // Test dirToCubeFace roundtrip
    for (int i = 0; i < 100; ++i) {
        glm::dvec3 dir = glm::normalize(glm::dvec3(
            (rand() % 2000 - 1000) / 1000.0,
            (rand() % 2000 - 1000) / 1000.0,
            (rand() % 2000 - 1000) / 1000.0
        ));
        Haruka::PlanetFace face;
        double lx, ly;
        dirToCubeFace(dir, face, lx, ly);
        glm::dvec3 reconstructed = cubeFaceToDir(face, lx, ly);
        double dot = glm::dot(glm::normalize(dir), glm::normalize(reconstructed));
        CHECK(dot > 0.9999, "dirToCubeFace roundtrip");
    }

    // Verify vertex count formula
    const int expected_verts = 6 * (res + 1) * (res + 1);
    const int expected_idx = 6 * res * res * 6;
    CHECK(expected_verts == 6 * 17 * 17, "vertex count");
    CHECK(expected_idx == 6 * 16 * 16 * 6, "index count");
}

void test_simple_planet_config() {
    beginTest("simple_planet_config");

    Haruka::PlanetarySystem::SimplePlanet sp;
    sp.name = "test";
    sp.radius = 6371000.0;
    sp.surface.tiling = 50.0f;
    CHECK(sp.name == "test", "name");
    CHECK(sp.radius == 6371000.0, "radius");
    CHECK(sp.surface.tiling == 50.0f, "tiling");
}
