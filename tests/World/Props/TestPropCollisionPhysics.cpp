// ================================================================================================
// THE PROPS COLLIDE: registry -> `PropSystem::refreshColliders` -> Jolt -> a character stops.
//
// Why this exists (12-10): Andoni, "los props no tienen colision". `PropSystem.cpp` tested
// `#ifdef HARUKA_MOD_PHYSICS` without including "Haruka/Core/Modules.h", the only place that macro is
// defined, so `refreshColliders` and `meshShapesFor` compiled to NOTHING: its object file held no call
// to the physics at all. `prop_collider` checks the geometry of the colliders and passed throughout,
// because nothing ran the path from the registry into the physics. This test runs it, end to end,
// with the prop's real mesh and the player's capsule. (The build now also refuses a file that tests
// HARUKA_MOD_* without that include: `cmake/check_modules_include.cmake`.)
//
// CONTROLS, in the same test: the same walk passes where the tree was FELLED (state Destroyed: the
// stump does not block), and passes when `refreshColliders` is not called — so what stops the
// character is the prop's collider, not the ground or the walk.
// ================================================================================================
#include "Common/TestCommon.h"
#include "Haruka/Physics/PhysicsEngine.h"
#include "Haruka/World/Props/PropSystem.h"
#include "Haruka/World/Props/InstancedObject.h"

#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

namespace {

/// A far planet seen as flat ground at y = 0 (gravity ~ -Y), as in the physics tests.
struct FlatGroundWorld : Haruka::Physics::IWorldProvider {
    static constexpr double R = 1.0e6;
    glm::dvec3 center{0.0, -R, 0.0};
    std::vector<Haruka::Physics::GravBody> grav;
    FlatGroundWorld() { const double G = 6.67430e-11, g = 9.81; grav.push_back({center, g * R * R / G}); }
    bool       hasActivePlanet()    const override { return true; }
    glm::dvec3 activePlanetCenter() const override { return center; }
    double     activePlanetRadius() const override { return R; }
    const std::vector<Haruka::Physics::GravBody>& gravBodies() const override { return grav; }
    double     terrainHeightAt(const glm::dvec3&) const override { return 0.0; }
    bool terrainMesh(const glm::dvec3&, double, std::vector<glm::dvec3>& v, std::vector<uint32_t>& t) const override {
        const double H = 100.0;
        v = { {-H, 0, -H}, {H, 0, -H}, {H, 0, H}, {-H, 0, H} };
        t = { 0, 2, 1, 0, 3, 2 };
        return true;
    }
};

enum class Setup { Collide, Felled, NoRefresh };

struct Walk { double endX = 0.0, closestM = 0.0; size_t shapes = 0; };

/// The player's capsule walks +X at 3 m/s for 4 s straight at a prop standing at x = 4 m. Returns where
/// it ended, how close its axis came to the prop's (m) and the number of physics shapes the props put in.
/// A trunk is round: the capsule hits it and SLIDES AROUND it (measured: never under 0.67 m from the
/// axis, ending at z = -0.69), so "it did not go through" is the distance, not where it stopped.
Walk walkInto(const char* protoName, Setup setup) {
    Walk out;
    using namespace Haruka;
    Physics::PhysicsEngine eng;
    FlatGroundWorld w;
    eng.setWorldProvider(&w);

    World::Props::PropSystem props;
    auto& reg = props.mutableRegistry();
    World::Props::InstancedPrototype proto;
    proto.name = protoName;
    proto.meshSeed = 7u;
    const int pi = reg.addPrototype(proto);
    World::Props::InstancedObject io;
    io.prototype = pi;
    io.dir = glm::vec3(glm::normalize(glm::dvec3(4.0, 0.0, 0.0) - w.center));   // stands at x = 4 m
    io.heightM = 0.0f;
    io.scale = 1.0f;
    io.yaw = 0.0f;
    io.seed = 1u;
    io.state = (setup == Setup::Felled) ? (uint32_t)World::Props::InstancedObjectState::Destroyed
                                        : (uint32_t)World::Props::InstancedObjectState::Alive;
    reg.addInstance(io);
    if (setup != Setup::NoRefresh)
        props.refreshColliders(w.center, FlatGroundWorld::R, glm::dvec3(0.0, 1.0, 0.0), &eng);
    out.shapes = eng.getPropMeshInstances().size() + eng.getPropOBBs().size() + eng.getPropCones().size();

    auto b = std::make_shared<Physics::RigidBody>();
    b->position = glm::dvec3(0.0, 1.0, 0.0); b->radius = 0.4; b->mass = 70.0; b->name = "player";
    b->isCharacter = true; b->characterHeight = 1.9; b->characterRadius = 0.35;
    eng.addBody(b);
    for (int i = 0; i < 120; ++i) eng.advance(1.0 / 60.0);                               // settles
    out.closestM = 1e9;
    for (int i = 0; i < 240; ++i) {                                                     // walks 4 s
        b->velocity.x = 3.0; eng.advance(1.0 / 60.0);
        out.closestM = std::min(out.closestM, std::hypot(b->position.x - 4.0, b->position.z));
    }
    out.endX = b->position.x;
    return out;
}

} // namespace

void test_prop_collision_physics() {
    beginTest("prop_collision_physics");
    for (const char* name : { "tree", "rock" }) {
        const Walk hit = walkInto(name, Setup::Collide), felled = walkInto(name, Setup::Felled),
                   none = walkInto(name, Setup::NoRefresh);
        std::printf("    %s at x = 4 m: the capsule's axis comes within %.2f m of it, ends at x = %.2f (%zu shapes)"
                    " · felled: %.2f m (%zu) · without refreshColliders: %.2f m (%zu)\n",
                    name, hit.closestM, hit.endX, hit.shapes, felled.closestM, felled.shapes, none.closestM, none.shapes);
        const std::string n(name);
        CHECK(hit.shapes > 0, ("the living " + n + " puts its collider into the physics").c_str());
        // The capsule's radius is 0.35 m: an axis closer than that means the prop is INSIDE the player.
        CHECK(hit.closestM > 0.35, ("the player's capsule never gets into the " + n + ": it stops or slides around it").c_str());
        CHECK(felled.shapes == 0 && felled.closestM < 0.05,
              ("CONTROL: a felled " + n + " (state Destroyed) puts nothing in and is walked straight through").c_str());
        CHECK(none.shapes == 0 && none.closestM < 0.05,
              ("CONTROL: without refreshColliders the same walk goes straight through the " + n).c_str());
    }
}

// WHAT A RE-SEED COSTS NOW THAT IT RUNS: every re-seed clears and re-adds all the props' shapes within
// 96 m, and Jolt rebuilds its statics on the next step. A dense forest: 300 trees in that disc.
void test_prop_collision_cost() {
    beginTest("prop_collision_cost");
    using namespace Haruka;
    Physics::PhysicsEngine eng;
    FlatGroundWorld w;
    eng.setWorldProvider(&w);
    World::Props::PropSystem props;
    auto& reg = props.mutableRegistry();
    World::Props::InstancedPrototype proto; proto.name = "tree"; proto.meshSeed = 7u;
    const int pi = reg.addPrototype(proto);
    uint32_t rng = 12345u;
    auto rnd = [&] { rng = rng * 1664525u + 1013904223u; return (double)(rng >> 8) / 16777216.0; };
    for (int i = 0; i < 300; ++i) {
        const double a = rnd() * 6.2831853, r = 90.0 * std::sqrt(rnd());
        World::Props::InstancedObject io;
        io.prototype = pi; io.seed = (uint32_t)i + 1u; io.yaw = (float)(rnd() * 6.28);
        io.dir = glm::vec3(glm::normalize(glm::dvec3(r * std::cos(a), 0.0, r * std::sin(a)) - w.center));
        reg.addInstance(io);
    }
    auto b = std::make_shared<Physics::RigidBody>();
    b->position = glm::dvec3(0.0, 1.0, 0.0); b->radius = 0.4; b->mass = 70.0; b->name = "player";
    b->isCharacter = true; b->characterHeight = 1.9; b->characterRadius = 0.35;
    eng.addBody(b);
    for (int i = 0; i < 10; ++i) eng.advance(1.0 / 60.0);
    double worstRefresh = 0.0, worstStep = 0.0;
    for (int k = 0; k < 5; ++k) {
        const auto t0 = std::chrono::steady_clock::now();
        props.refreshColliders(w.center, FlatGroundWorld::R, glm::dvec3(0.0, 1.0, 0.0), &eng);
        const auto t1 = std::chrono::steady_clock::now();
        eng.advance(1.0 / 60.0);   // the step that rebuilds the statics
        const auto t2 = std::chrono::steady_clock::now();
        worstRefresh = std::max(worstRefresh, std::chrono::duration<double, std::milli>(t1 - t0).count());
        worstStep    = std::max(worstStep,    std::chrono::duration<double, std::milli>(t2 - t1).count());
    }
    const size_t shapes = eng.getPropMeshInstances().size();
    std::printf("    300 trees within 96 m -> %zu shapes · refreshColliders worst %.2f ms · the next physics step worst %.2f ms\n",
                shapes, worstRefresh, worstStep);
    CHECK(shapes >= 300, "every tree within 96 m puts its collider in");
}

HARUKA_REGISTER_TESTS(
    { "prop_collision_physics", test_prop_collision_physics },
    { "prop_collision_cost",    test_prop_collision_cost });
