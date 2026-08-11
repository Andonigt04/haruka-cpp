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

    // El cuerpo NO GIRA (solo traslada). Un PERSONAJE es una esfera que no debe RODAR: si rueda, la
    // velocidad angular se convierte en avance por el contacto y sigue deslizándose aunque le pongas la
    // velocidad lineal a cero. Un objeto suelto (una piedra) sí quiere rodar → false por defecto.
    bool lockRotation = false;

    // Es un PERSONAJE: lo lleva un controlador de personaje (Jolt `CharacterVirtual`) en vez de un
    // rígido dinámico. Un rígido no es un personaje — emular a mano pisar escalones, pendientes,
    // "estar en el suelo" y la fricción sale mal pieza a pieza; el controlador lo hace nativo.
    bool isCharacter = false;
    // Lo RELLENA la física: ¿el controlador dice que pisa suelo? (fuente de verdad para el salto).
    bool onGround = false;

    // "He cambiado `velocity` a propósito, aplícala" (un empujón, una explosión, lanzar algo). Un cuerpo
    // LIBRE lo posee el motor: re-imponerle su velocidad cada frame re-aplicaba la velocidad de
    // separación del solver una y otra vez → el objeto ganaba energía y SALÍA VOLANDO. La física
    // consume esta marca y la baja.
    bool velocityDirty = false;

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
    
    /**
     * @brief ALAMBRE DE LA MALLA DE COLISIÓN: activa la captura de la geometría que se le da a Jolt.
     *
     * Existe porque la disparidad del terreno se veía a ojo y no la detectaba ninguna medida: la sonda
     * de paridad compara la función con la malla (2 cm), los pies quedan a centímetros del suelo, y el
     * clipmap dibuja con quads de 4 m — la misma retícula que colisiona. Todo coherente, y aun así se
     * ve. Dibujando encima la malla que Jolt TIENE de verdad, la diferencia deja de ser una impresión.
     *
     * Apagado por defecto: la copia del parche de 200 km son ~88 k vértices (2 MB) por reconstrucción.
     */
    void setCollisionMeshDebug(bool on);
    bool isCollisionMeshDebug() const;
    /**
     * @brief Copia la malla capturada, en coordenadas de MUNDO. `outRevision` cambia cuando se
     *        reconstruye, así que el render solo re-sube los buffers cuando hace falta.
     * @return false si no hay captura (o está apagada).
     */
    bool getCollisionMeshDebug(std::vector<glm::dvec3>& outVerts, std::vector<uint32_t>& outTris,
                               glm::dvec3& outCenter, uint64_t& outRevision) const;

    /**
     * @brief EL ANILLO CERCANO tal cual lo colisiona Jolt, para que el RENDER lo dibuje.
     *
     * No es depuración: es el camino por el que el suelo dibujado deja de ser una segunda
     * evaluación de la función y pasa a ser **los mismos bytes** que se pisan.
     *
     * Mientras el terreno cercano lo genere el teselador, la paridad no puede ser exacta: los cuatro
     * nodos de cada quad coinciden, pero un quad no es plano y hay que partirlo en dos triángulos —
     * y `clipmap.tese` declara `layout(quads, equal_spacing, ccw)`, donde **qué diagonal usa el
     * teselador NO lo fija el spec de OpenGL**. Jolt sí la fija. La diferencia en el centro del quad,
     * medida sobre el terreno real, es de 2-4 cm bajo los pies y hasta 8,9 cm en el bloque.
     *
     * Se publican VÉRTICES E ÍNDICES ya montados, no las muestras y la regla para montarlos. La
     * diferencia importa: en cuanto hay dos montadores acaban divergiendo, y esa divergencia es
     * literalmente el bug que este camino existe para cerrar. El render sube lo que recibe.
     *
     * ⚠️ Los vértices están en el PLANO TANGENTE, no sobre la esfera. El punto con el que se generó
     * la muestra es `centro + dir·(R+h)`, pero Jolt NO vuelve a la esfera: coloca el nodo en el plano.
     * El desplazamiento tangencial (`x·h/R`, 3 cm en el borde del bloque) es parte de lo que se pisa,
     * y en cuanto los dos lados lo tienen deja de ser disparidad. "Corregirlo" volvería a separarlos.
     *
     * ⚠️ Dibujar en float RELATIVO a `anchor`, nunca en coordenadas de mundo: a 6,37e6 m un float
     * tiene ~0,5 m de resolución y el suelo temblaría. `anchor` no se mueve entre reconstrucciones.
     *
     * `outRevision` cambia solo cuando el anillo se reconstruye (al saltar el anclaje del clipmap),
     * así que el render re-sube la textura únicamente entonces.
     *
     * @return false si no hay anillo (servidor, tests, o el provider no da anillos).
     */
    struct NearGroundRing {
        /// Posiciones de MUNDO de los nodos con superficie. NO son "la función evaluada otra vez":
        /// salen de la misma función que monta el alambre y que describe lo que Jolt colisiona.
        std::vector<glm::dvec3> verts;
        /// Triangulación de Jolt (`(i,j)→(i+1,j+1)`), ya montada. Se publica hecha, y no las reglas
        /// para montarla, porque dos montadores acaban divergiendo — que es el bug entero.
        std::vector<uint32_t>   tris;
        glm::dvec3 anchor{0.0};   ///< Origen ESTABLE para dibujar en float (el punto de ancla).
        double     cell   = 0.0;  ///< Lado de la celda, m.
        double     extent = 0.0;  ///< Semi-alcance, m.
        uint64_t   revision = 0;  ///< Cambia solo al reconstruirse: el render re-sube solo entonces.
    };
    bool getNearGroundRing(NearGroundRing& out) const;

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
    /** @brief Sube cada vez que cambia el mundo ESTÁTICO. Jolt reconstruye sus cuerpos estáticos solo
     *  cuando cambia — son estáticos y tocarlos es raro (colocar/romper), no cosa de cada frame. */
    uint64_t staticsVersion() const { return m_staticsVersion; }

    /** @brief Cajas de los RECURSOS del mundo (árboles/rocas) — lista aparte porque se
     *  regeneran al moverse, independiente de los objetos colocados. */
    void addPropOBB(const glm::dvec3& center, const glm::dvec3& halfExtents, const glm::dmat3& rot);
    void clearPropOBBs();
    const std::vector<StaticOBB>& getPropOBBs()    const { return propOBBs; }
    const std::vector<StaticBox>& getStaticBoxes() const { return staticBoxes; }
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
    uint64_t                                m_staticsVersion = 0;  // ver staticsVersion()
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

    // Fase 1 — wrapper de Jolt (PIMPL): Jolt NO aparece en este header (forward-decl). Solo se crea si
    // se compiló con HARUKA_HAS_JOLT; si no, `m_jolt` queda null y el motor usa el solver a mano.
    struct JoltImpl;
    std::unique_ptr<JoltImpl> m_jolt;

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