#pragma once
// Lo COMPARTIDO por los tests del mar (TestOcean*.cpp, TestWaterFill.cpp): los helpers que usa mas de un archivo.
#include "Common/TestCommon.h"
#include "Haruka/World/Terrain/TerrainDetail.h"   // equirectUV / sampleHeightField / terrainDetail
#include "Haruka/World/Terrain/TerrainLOD.h"      // terrainTriM
#include <cmath>
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include "Haruka/World/Water/OceanWave.h"
#include "Haruka/World/Water/WaterFill.h"
#include "Haruka/Physics/PhysicsEngine.h"


using Haruka::World::Water::oceanWaveHeight;

using Haruka::World::Water::oceanWaveVelocity;

using Haruka::World::Water::oceanShoalAmp;


namespace {

// Un punto de mar abierto sobre un planeta de radio terrestre, y su marco local.
constexpr double kR = 6371000.0;
glm::vec3 upAt(double lonRad, double latRad) {
    return glm::vec3(std::cos(latRad) * std::cos(lonRad),
                     std::sin(latRad),
                     std::cos(latRad) * std::sin(lonRad));
}

}


// --------------------------------- TEST 4: el acople REAL — flotar y ser arrastrado por la corriente
namespace {
    // Mundo de juguete con MAR: plano, con una corriente horizontal constante. La corriente constante
    // (en vez de la ola de verdad) es deliberada: aísla lo que se quiere medir —que el arrastre
    // transporta— de la oscilación, que promediaría el desplazamiento a casi cero en pocos ciclos.
    struct FakeOcean : Haruka::Physics::IWorldProvider {
        glm::dvec3 center{0, 0, 0};
        double     radius  = 1000.0;
        double     terrain = -50.0;      // fondo 50 m BAJO el nivel del mar → hay columna de agua
        double     waterH  = 0.0;        // superficie del agua, sobre la esfera de referencia
        glm::dvec3 current{0, 0, 0};     // velocidad del agua (m/s)
        float      foam = 0.0f;          // 0 = agua limpia · 1 = agua blanca (rompiente)
        std::vector<Haruka::Physics::GravBody> grav;
        FakeOcean() {
            const double G = 6.67430e-11, g = 9.81;
            grav.push_back({ center, g * radius * radius / G });
        }
        bool       hasActivePlanet()    const override { return true; }
        glm::dvec3 activePlanetCenter() const override { return center; }
        double     activePlanetRadius() const override { return radius; }
        const std::vector<Haruka::Physics::GravBody>& gravBodies() const override { return grav; }
        double     terrainHeightAt(const glm::dvec3&) const override { return terrain; }
        double     waterSurfaceAt(const glm::dvec3&) const override { return waterH; }
        glm::dvec3 waterVelocityAt(const glm::dvec3&) const override { return current; }
        float      waterFoamAt(const glm::dvec3&) const override { return foam; }
    };

    // Suelta un cuerpo sobre el agua y simula. Devuelve el radio final y la deriva TANGENCIAL.
    void floatSim(const glm::dvec3& current, double seconds,
                  double& outDist, double& outDrift, double& outTangSpeed,
                  float foam = 0.0f) {
        Haruka::Physics::PhysicsEngine eng;
        FakeOcean w; w.current = current; w.foam = foam;
        eng.setWorldProvider(&w);
        auto b = std::make_shared<Haruka::Physics::RigidBody>();
        b->position = glm::dvec3(w.radius + 2.0, 0, 0);   // 2 m sobre el agua, en +X
        b->radius = 0.5; b->mass = 70.0; b->name = "boya";
        eng.addBody(b);
        for (double s = 0.0; s < seconds; s += 1.0 / 60.0) eng.advance(1.0 / 60.0);
        outDist  = glm::length(b->position - w.center);
        // Deriva tangencial: cuánto se ha movido en el plano perpendicular al radial inicial.
        const glm::dvec3 radial0(1, 0, 0);
        const glm::dvec3 rel = b->position - w.center;
        outDrift = glm::length(rel - radial0 * glm::dot(rel, radial0));
        const glm::dvec3 v = b->velocity;
        outTangSpeed = glm::length(v - radial0 * glm::dot(v, radial0));
    }
}

// --- Tests definidos en los otros archivos (los registra el principal) ---
void test_ocean_shoaling();
void test_ocean_break_limit();
void test_ocean_break_fold();
void test_ocean_whitecaps();
void test_ocean_foam();
void test_ocean_finite_depth();
void test_ocean_refraction_and_break();
void test_ocean_swash();
void test_ocean_tide();
void test_ocean_stokes_transport();
void test_ocean_buoyancy_drift();
void test_ocean_whitewater_drag();
void test_ocean_no_water_no_float();
void test_water_fill_global();
void test_ocean_fetch();
void test_water_bake_resolution_limit();
void test_water_fill_window();
void test_water_window_policy();
