// ================================================================================================
// Tests de FÍSICA (sin GL): caída radial + determinismo del timestep fijo (Fase 1). Un cuerpo cae
// hacia el planeta por gravedad RADIAL y se asienta sobre el terreno vía IWorldProvider; con paso fijo
// el resultado es idéntico independientemente de cómo se trocee el frame → base del acuerdo cliente↔
// servidor. Usa un FakeWorld (IWorldProvider de juguete) para no depender del motor de render.
// ================================================================================================
#include "test_common.h"

#include <cmath>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

#include "physics/physics_engine.h"

// ---------------------------------------------- TEST: física — caída radial + determinismo (F1)
// Verifica lo que la Fase 1 promete y NADIE más cubre: (1) un cuerpo dinámico cae hacia el planeta
// (gravedad RADIAL, no plana) y se ASIENTA justo sobre el terreno (ground-snap vía IWorldProvider),
// y (2) con TIMESTEP FIJO el resultado es DETERMINISTA (idéntico con distinto trocear de frames) —
// requisito para que cliente y servidor coincidan. Corre SIN GL: la física ya no depende del render.
namespace {
    struct FakeWorld : Haruka::Physics::IWorldProvider {
        glm::dvec3 center{0,0,0};
        double     radius = 1000.0;         // planeta pequeño (m)
        double     terrain = 50.0;          // 50 m de terreno constante sobre la esfera
        std::vector<Haruka::Physics::GravBody> grav;
        FakeWorld() {
            // Masa que da ~9.81 m/s² en la superficie: g = G·M/r² → M = g·r²/G.
            const double G = 6.67430e-11, g = 9.81;
            grav.push_back({ center, g * radius * radius / G });
        }
        bool       hasActivePlanet()    const override { return true; }
        glm::dvec3 activePlanetCenter() const override { return center; }
        double     activePlanetRadius() const override { return radius; }
        const std::vector<Haruka::Physics::GravBody>& gravBodies() const override { return grav; }
        double     terrainHeightAt(const glm::dvec3&) const override { return terrain; }
    };
    // Simula 'seconds' de caída y devuelve la distancia final al centro. `steps` = cuántos trozos
    // de frame (para probar que el timestep fijo hace el resultado independiente del troceado).
    static double dropSim(int steps, double seconds, double startDist) {
        Haruka::Physics::PhysicsEngine eng; FakeWorld w; eng.setWorldProvider(&w);
        auto b = std::make_shared<Haruka::Physics::RigidBody>();
        b->position = glm::dvec3(startDist, 0, 0);   // arranca en +X, sobre el "polo"
        b->radius = 0.5; b->mass = 70.0; b->name = "probe";
        eng.addBody(b);
        const double frameDt = seconds / steps;
        for (int i = 0; i < steps; ++i) eng.advance(frameDt);
        return glm::length(b->position - w.center);
    }
}
void test_physics_radial_fall() {
    beginTest("physics_radial_fall");
    Haruka::Physics::PhysicsEngine e0; FakeWorld w0;
    const double floor = w0.radius + w0.terrain + 0.5;   // radio + terreno + r del cuerpo

    // (1) Cae desde 200 m sobre el suelo y se asienta EN el suelo (no lo atraviesa, no rebota lejos).
    //     40 s: la gravedad radial arriba es menor → hay que dar tiempo a caer y asentarse.
    const double land = dropSim(2400, 40.0, floor + 200.0);
    std::printf("    caida: suelo=%.2f m  ·  reposo=%.2f m  (delta=%.3f m)\n", floor, land, land - floor);
    CHECK(std::abs(land - floor) < 0.05, "el cuerpo se ASIENTA sobre el terreno (ground-snap radial)");

    // (2) DETERMINISMO del timestep fijo: mismo tiempo simulado, distinto nº de frames → mismo
    //     resultado (el acumulador trocea en pasos fijos idénticos).
    const double a = dropSim(100, 3.0, floor + 200.0);   // frames gordos
    const double b = dropSim(900, 3.0, floor + 200.0);   // frames finos
    std::printf("    determinismo: 100 frames=%.4f m  ·  900 frames=%.4f m  (diff=%.2e)\n", a, b, std::abs(a - b));
    CHECK(std::abs(a - b) < 1e-6, "timestep fijo → resultado independiente del troceado de frames");
}
