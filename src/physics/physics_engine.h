/**
 * @file physics_engine.h
 * @brief Rigid body physics engine with octree broad-phase and impulse-based collision resolution.
 */
#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include "octree.h"
#include "world_provider.h"   // IWorldProvider — ventana abstracta al mundo (sin GL)

namespace Haruka { namespace Physics {

/** @brief Rigid body simulation record used by the physics engine. */
struct RigidBody {
    glm::dvec3 position;
    glm::dvec3 velocity;
    glm::dvec3 acceleration;
    double mass;
    double radius;
    bool isKinematic = false;
    bool inWater = false;   // estado agua↔aire para disparar el splash SOLO al ENTRAR (no cada frame)
    std::string name;

    // --- Estado ANGULAR (opt-in). Por defecto identidad/cero y comOffset=0 → los cuerpos que no lo
    // usan (jugador, props existentes) NO rotan y el motor se comporta igual que antes. ---
    glm::dquat orientation{1, 0, 0, 0};   // rotación del cuerpo (identidad = sin girar)
    glm::dvec3 angularVel{0, 0, 0};       // velocidad angular (rad/s, en el mundo)
    glm::dvec3 comOffset{0, 0, 0};        // centro de masa DESPLAZADO del centro geométrico (LOCAL, m).
                                          // ≠0 → apoyado, la gravedad hace PALANCA y el cuerpo VUELCA
                                          // hacia donde pesa (balance). =0 → equilibrado, no vuelca.

    // --- FORMA de colisión. Sphere (por defecto): esfera de `radius`. Box: caja de medios-lados
    // `halfExtents`, que colisiona con el terreno por sus 8 ESQUINAS → se apoya en su cara y vuelca
    // sobre la ARISTA de verdad (no como una esfera). `radius` sigue siendo el radio ENVOLVENTE
    // (broad-phase y colisión cuerpo-cuerpo, que sigue siendo esférica). ---
    enum class Shape { Sphere, Box };
    Shape      shape = Shape::Sphere;
    glm::dvec3 halfExtents{0.5, 0.5, 0.5};   // solo si shape==Box (m)

    // Puntos de contacto LOCALES de la FORMA REAL del objeto (vértices del casco convexo). Si está
    // VACÍO y shape==Box, se usan las 8 esquinas de halfExtents. Rellénalo para que la colisión con el
    // terreno siga la forma REAL (no solo esfera/caja): cada vértice se prueba contra el suelo. Es el
    // primer paso de "colisión por la forma del objeto" — el modelo por-puntos de la dirección.
    std::vector<glm::dvec3> points;
};

/** @brief Un CONTACTO entre dos cuerpos. `normal` apunta de A hacia B (dirección para separarlos).
 *  `point` = punto de contacto en el mundo (brazo para el impulso ANGULAR → el choque hace girar). */
struct CollisionInfo {
    RigidBody* bodyA;
    RigidBody* bodyB;
    double penetration;
    glm::dvec3 normal;
    glm::dvec3 point{0.0};
};

/** @brief Axis-aligned static box for scene geometry collision. */
struct StaticBox {
    glm::dvec3 bmin;
    glm::dvec3 bmax;
};

/** @brief Caja ORIENTADA estática (objetos colocados: mesas, props…). center/halfExtents
 *  en mundo + rot (local→mundo, ortonormal). Caja ajustada al modelo, orientada a la
 *  superficie. */
struct StaticOBB {
    glm::dvec3 center;
    glm::dvec3 halfExtents;
    glm::dmat3 rot;
};

/**
 * @brief Main physics simulation coordinator.
 *
 * Owns rigid body storage, broad-phase acceleration structure, and collision state.
 */
class PhysicsEngine {
public:
    /** @brief Constructs an empty physics engine. */
    PhysicsEngine();
    ~PhysicsEngine();
    
    /** @brief Adds a body to the simulation. */
    void addBody(std::shared_ptr<RigidBody> body);
    /** @brief Removes body by name. */
    void removeBody(const std::string& name);
    /** @brief Returns body by name or null when missing. */
    std::shared_ptr<RigidBody> getBody(const std::string& name);

    /** @brief Registers a static AABB for scene geometry collision. */
    void addStaticBox(const glm::dvec3& center, const glm::dvec3& halfExtents);
    /** @brief Removes all static boxes (e.g. on scene reload). */
    void clearStaticBoxes();

    /** @brief Registra/limpia cajas orientadas de objetos colocados (mesas, estaciones). */
    void addPlacedOBB(const glm::dvec3& center, const glm::dvec3& halfExtents, const glm::dmat3& rot);
    // Obstáculos colocados (paredes/estructuras) — para navegación/propagación (sonido, conjuros).
    const std::vector<StaticOBB>& getPlacedOBBs() const { return placedOBBs; }
    void clearPlacedOBBs();

    /** @brief Cajas de los RECURSOS del mundo (árboles/rocas) — lista aparte porque se
     *  regeneran al moverse, independiente de los objetos colocados. */
    void addPropOBB(const glm::dvec3& center, const glm::dvec3& halfExtents, const glm::dmat3& rot);
    void clearPropOBBs();
    /** @brief Empuja una esfera fuera de los OBB colocados (te subes encima o te frena).
     *  Devuelve el centro corregido; pone grounded=true si el empuje fue a favor de 'up'. */
    glm::dvec3 resolveSphere(const glm::dvec3& center, double radius,
                             const glm::dvec3& up, bool& grounded) const;

    /** @brief Avanza la simulación UN paso del tamaño dado. Normalmente NO se llama directo: usar
     *  `advance()`, que trocea en pasos fijos. Directo solo para tests deterministas. */
    void update(double deltaTime);

    /** @brief Avanza la simulación con TIMESTEP FIJO (acumulador). Llamar 1×/frame con el dt de
     *  reloj. Determinista → cliente y servidor coinciden. Ver kFixedDt. */
    void advance(double frameDt);

    /** @brief Paso fijo de la simulación (s). 1/60. La misma constante la usa el DGS. */
    static constexpr double kFixedDt  = 1.0 / 60.0;
    static constexpr double kMaxAccum = 0.25;   // tope anti-espiral (no recuperar >0.25 s de golpe)
    /** @brief Sets constant gravity acceleration. */
    /** @brief Callback de SPLASH: se dispara cuando un cuerpo dinámico CRUZA la superficie del mar
     *  hacia dentro con velocidad de entrada apreciable. (pos superficie, velocidad de impacto,
     *  radio). El juego lo engancha para emitir partículas PBF → acopla rígidos↔fluido. */
    void setWaterEntryCallback(std::function<void(const glm::dvec3&, const glm::dvec3&, double)> cb) {
        m_onWaterEntry = std::move(cb);
    }

    void setGravity(glm::dvec3 g) { gravity = g; }
    /** @brief Returns current gravity acceleration. */
    glm::dvec3 getGravity() const { return gravity; }

    /** @brief Sets the ambient WIND velocity (m/s) used for aerodynamic drag. The
     *  atmosphere (WorldSystem) provides it; the engine applies it per active body
     *  in integrateForces (O(bodies), inherentemente localizado — sin coste global). */
    void setWind(const glm::dvec3& windVel) { m_wind = windVel; }
    
    /** @brief Returns collision events from last update. */
    const std::vector<CollisionInfo>& getCollisions() const { return collisions; }
    /** @brief Replaces broad-phase octree root. */
    void initOctree(glm::dvec3 center, double size) {
        octree = std::make_unique<Octree>(center, size);
    }
    
    /** @brief Inyecta la ventana al mundo (gravedad + mar + terreno). El motor no conoce
     *  WorldSystem/PlanetarySystem; el llamador pasa un adaptador (ver world_provider.h). */
    void setWorldProvider(IWorldProvider* provider) { m_world = provider; }
    
    /**
     * @brief Calculates gravitational acceleration at a world position.
     * 
     * Sums gravitational contributions from all celestial bodies in the world.
     * @param worldPos Position in world space
     * @param outGravityDir Output direction of gravity (normalized)
     * @return Gravitational acceleration magnitude (m/s²)
     */
    double calculateGravityAtPosition(const glm::dvec3& worldPos, glm::dvec3& outGravityDir);
    
    /**
     * @brief Calculates gravity contribution from a single celestial body.
     * @param body Celestial body to calculate gravity from
     * @param worldPos Test position in world space
     * @return Gravitational acceleration vector (m/s²)
     */
    glm::dvec3 calculateGravityContribution(const GravBody& body, const glm::dvec3& worldPos);
    
    /**
     * @brief Checks if a position collides with terrain.
     * @param worldPos Position to check for collision
     * @param outCollisionPoint Optional: world position of collision point
     * @param outCollisionNormal Optional: surface normal at collision point
     * @return true if collision detected, false otherwise
     */
    bool checkTerrainCollision(
        const glm::dvec3& worldPos,
        glm::dvec3* outCollisionPoint = nullptr,
        glm::dvec3* outCollisionNormal = nullptr);
    
    /**
     * @brief Applies gravity force to an object.
     * @param worldPos Current position of the object
     * @param deltaTime Time step
     * @param inOutVelocity Velocity to update with gravity acceleration
     */
    void applyGravity(const glm::dvec3& worldPos, double deltaTime, glm::dvec3& inOutVelocity);
    
    /**
     * @brief Sets the gravitational constant.
     * @param G Gravitational constant to use
     */
    void setGravitationalConstant(double G) { gravitationalConstant = G; }
    
    double getGravitationalConstant() const { return gravitationalConstant; }
    

private:
    std::vector<std::shared_ptr<RigidBody>> bodies;
    std::vector<StaticBox>                  staticBoxes;
    std::vector<StaticOBB>                  placedOBBs;   // objetos colocados por el jugador
    std::vector<StaticOBB>                  propOBBs;     // recursos del mundo (árboles/rocas)
    std::vector<CollisionInfo> collisions;
    glm::dvec3 gravity{0.0, -9.81, 0.0};

    // Arrastre aerodinámico: viento ambiente (m/s) + coeficientes SUAVES (la
    // resistencia del aire amortigua hacia 0; el viento empuja sutilmente). Valores
    // pequeños para no zarandear al jugador; afecta sobre todo a objetos sueltos.
    std::function<void(const glm::dvec3&, const glm::dvec3&, double)> m_onWaterEntry; // splash al entrar al agua
    glm::dvec3 m_wind{0.0};
    double     m_airDamp  = 0.10;  // amortiguación del aire (1/s) hacia velocidad 0
    double     m_windCoef = 0.010; // acoplamiento cuadrático con la vel. relativa al viento

    std::unique_ptr<Octree> octree;

    // Ventana al mundo (gravedad + mar + terreno). Inyectada; el motor no conoce el resto del engine.
    IWorldProvider* m_world = nullptr;
    double gravitationalConstant = 6.67430e-11;
    double m_accum = 0.0;   // acumulador del timestep fijo (ver advance)

    /** @brief Integrates external forces for all bodies. */
    void integrateForces(double dt);
    /** @brief Detects collisions and fills collision list. */
    void detectCollisions();
    /** @brief Resolves collision responses for detected contacts. */
    void resolveCollisions();
    /** @brief Resolves sphere vs static AABB contacts. */
    void resolveStaticCollisions();
    /** @brief Runs broad-phase AABB traversal/culling. */
    void broadPhaseAABB();
};

}} // namespace Haruka::Physics

// Back-compat aliases during the namespace migration.
using Haruka::Physics::RigidBody;
using Haruka::Physics::CollisionInfo;
using Haruka::Physics::StaticBox;
using Haruka::Physics::StaticOBB;
using Haruka::Physics::PhysicsEngine;
namespace Haruka {
    using Physics::RigidBody; using Physics::CollisionInfo; using Physics::StaticBox;
    using Physics::StaticOBB; using Physics::PhysicsEngine;
}