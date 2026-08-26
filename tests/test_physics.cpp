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
#include <atomic>
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

// ---------------------------------------------- TEST (Fase 2): colisión con la MALLA del terreno
// Verifica el camino nuevo de la Fase 2 de Jolt: si el IWorldProvider da GEOMETRÍA (triángulos), el
// motor colisiona contra la MALLA REAL (mesh shape), no contra una altura/esfera. Un cuerpo cae sobre
// un suelo plano dado como 2 triángulos y se ASIENTA encima. (Sin Jolt, el provider-mesh se ignora y
// esto cae al camino a mano — por eso el umbral es holgado.)
namespace {
    struct FlatMeshWorld : Haruka::Physics::IWorldProvider {
        static constexpr double R = 1.0e6;              // planeta lejano → gravedad ≈ −Y localmente
        glm::dvec3 off, center;                         // `off` = desplazamiento del mundo (escala planetaria)
        std::vector<Haruka::Physics::GravBody> grav;
        explicit FlatMeshWorld(glm::dvec3 offset = glm::dvec3(0.0))
            : off(offset), center(offset + glm::dvec3(0.0, -R, 0.0)) {
            const double G = 6.67430e-11, g = 9.81; grav.push_back({center, g * R * R / G});
        }
        bool       hasActivePlanet()    const override { return true; }
        glm::dvec3 activePlanetCenter() const override { return center; }
        double     activePlanetRadius() const override { return R; }
        const std::vector<Haruka::Physics::GravBody>& gravBodies() const override { return grav; }
        double     terrainHeightAt(const glm::dvec3&) const override { return 0.0; }
        bool terrainMesh(const glm::dvec3&, double, std::vector<glm::dvec3>& v,
                         std::vector<uint32_t>& t) const override {
            const double H = 100.0;                     // cuadrado plano en Y=off.y (winding → normal +Y)
            v = { off + glm::dvec3(-H,0,-H), off + glm::dvec3(H,0,-H), off + glm::dvec3(H,0,H), off + glm::dvec3(-H,0,H) };
            t = { 0,2,1, 0,3,2 };
            return true;
        }
    };
    // Malla cuya ALTURA cambia en caliente: simula el LOD refinando el terreno grueso→fino bajo un
    // jugador QUIETO. `surfaceY` es mutable; terrainMesh y terrainHeightAt lo leen → coherentes.
    struct RefiningMeshWorld : Haruka::Physics::IWorldProvider {
        static constexpr double R = 1.0e6;
        glm::dvec3 center{0.0, -R, 0.0};
        std::vector<Haruka::Physics::GravBody> grav;
        double surfaceY = 8.0;                          // empieza ALTO (grueso) → se baja a 0 (fino)
        RefiningMeshWorld() { const double G = 6.67430e-11, g = 9.81; grav.push_back({center, g * R * R / G}); }
        bool       hasActivePlanet()    const override { return true; }
        glm::dvec3 activePlanetCenter() const override { return center; }
        double     activePlanetRadius() const override { return R; }
        const std::vector<Haruka::Physics::GravBody>& gravBodies() const override { return grav; }
        double     terrainHeightAt(const glm::dvec3&) const override { return surfaceY; }
        bool terrainMesh(const glm::dvec3&, double, std::vector<glm::dvec3>& v,
                         std::vector<uint32_t>& t) const override {
            const double H = 100.0, Y = surfaceY;
            v = { {-H,Y,-H}, {H,Y,-H}, {H,Y,H}, {-H,Y,H} };
            t = { 0,2,1, 0,3,2 };
            return true;
        }
    };

    // Malla que TARDA en estar disponible: terrainMesh falla hasta que `ready`. Simula el arranque con
    // el planeta aún no listo → la colisión cae al fallback de ESFERA. terrainHeightAt pone la esfera 30 m
    // POR ENCIMA de la malla, para distinguir "clavado en la esfera" de "subió a la malla".
    struct LateMeshWorld : Haruka::Physics::IWorldProvider {
        static constexpr double R = 1.0e6;
        glm::dvec3 center{0.0, -R, 0.0};
        std::vector<Haruka::Physics::GravBody> grav;
        mutable std::atomic<bool> ready{false};
        LateMeshWorld() { const double G = 6.67430e-11, g = 9.81; grav.push_back({center, g * R * R / G}); }
        bool       hasActivePlanet()    const override { return true; }
        glm::dvec3 activePlanetCenter() const override { return center; }
        double     activePlanetRadius() const override { return R; }
        const std::vector<Haruka::Physics::GravBody>& gravBodies() const override { return grav; }
        double     terrainHeightAt(const glm::dvec3&) const override { return 30.0; }  // esfera 30 m ARRIBA
        bool terrainMesh(const glm::dvec3&, double, std::vector<glm::dvec3>& v,
                         std::vector<uint32_t>& t) const override {
            if (!ready.load()) return false;                 // planeta aún no listo → fallback esfera
            const double H = 100.0;                          // malla plana en Y=0 (30 m BAJO la esfera)
            v = { {-H,0,-H}, {H,0,-H}, {H,0,H}, {-H,0,H} };
            t = { 0,2,1, 0,3,2 };
            return true;
        }
    };

    // Deja caer un cuerpo sobre la malla (en el mundo desplazado `offset`) y devuelve su altura sobre ella.
    static double meshSettleY(glm::dvec3 offset) {
        Haruka::Physics::PhysicsEngine eng; FlatMeshWorld w(offset); eng.setWorldProvider(&w);
        auto b = std::make_shared<Haruka::Physics::RigidBody>();
        b->position = offset + glm::dvec3(0.0, 12.0, 0.0); b->radius = 0.5; b->mass = 20.0; b->name = "probe";
        eng.addBody(b);
        for (int i = 0; i < 600; ++i) eng.advance(1.0 / 60.0);   // 10 s
        return (b->position - offset).y;                          // altura sobre la malla (Y=0 local)
    }
}
void test_physics_mesh_ground() {
    beginTest("physics_mesh_ground");
    const double y = meshSettleY(glm::dvec3(0.0));
    std::printf("    malla plana: reposo Y=%.3f m  (esperado ~0.50 = radio)\n", y);
    CHECK(y > 0.0 && y < 0.65, "el cuerpo se ASIENTA sobre la MALLA del terreno (no la atraviesa ni flota)");

    // ESCALA PLANETARIA: el mismo escenario a ~1.5e8 m (la Tierra está a 1 UA en la escena). El float de
    // Jolt tiene ~16 m de precisión ahí → sin MARCO LOCAL la física se rompe. Con él, se asienta igual.
    const double yp = meshSettleY(glm::dvec3(1.5e8, 0.0, 3.0e7));
    std::printf("    malla a escala planetaria (~1.5e8 m): reposo Y=%.3f m\n", yp);
    CHECK(yp > 0.0 && yp < 0.65, "el MARCO LOCAL hace que la física funcione a escala planetaria (float-safe)");

    // REFINAMIENTO ESTANDO QUIETO: el jugador se asienta sobre el terreno GRUESO (alto); el LOD lo
    // refina hacia abajo SIN que el jugador se mueva. La malla de Jolt se reconstruía SOLO al alejarse
    // >48 m → se quedaba a la altura vieja y el jugador "flotaba" sobre el terreno fino ya dibujado
    // (justo lo que reportó el autor). Ahora se rehace también si la altura bajo los pies cambia.
    {
        Haruka::Physics::PhysicsEngine eng; RefiningMeshWorld w; eng.setWorldProvider(&w);
        auto b = std::make_shared<Haruka::Physics::RigidBody>();
        b->position = glm::dvec3(0.0, 12.0, 0.0); b->radius = 0.5; b->mass = 20.0; b->name = "probe";
        eng.addBody(b);
        for (int i = 0; i < 300; ++i) eng.advance(1.0 / 60.0);   // 5 s: se asienta sobre Y=8 (grueso)
        const double yCoarse = b->position.y;
        w.surfaceY = 0.0;                                        // el LOD REFINA: el terreno baja a 0
        for (int i = 0; i < 300; ++i) eng.advance(1.0 / 60.0);   // 5 s más: ¿lo sigue hacia abajo?
        const double yFine = b->position.y;
        std::printf("    refinado quieto: reposo sobre grueso Y=%.2f → tras refinar Y=%.2f (esperado ~0.5, NO ~8.5)\n",
                    yCoarse, yFine);
        CHECK(yCoarse > 8.0 && yCoarse < 8.65, "primero se asienta sobre el terreno GRUESO (Y≈8.5)");
        CHECK(yFine < 0.65, "tras REFINAR bajo el jugador quieto, la colisión SIGUE al terreno (no flota)");
    }

    // MALLA QUE LLEGA TARDE: si el primer build cae a la ESFERA (planeta no listo), la colisión NO puede
    // quedarse clavada ahí — debe SUBIR a la malla cuando esté disponible. Regresión introducida al
    // hacer el refresco asíncrono: el disparador exigía `groundIsMesh`, así que la esfera nunca mejoraba
    // → el terreno aparecía decenas de m desplazado ("no aparece o 50 m abajo").
    {
        Haruka::Physics::PhysicsEngine eng; LateMeshWorld w; eng.setWorldProvider(&w);
        auto b = std::make_shared<Haruka::Physics::RigidBody>();
        b->position = glm::dvec3(0.0, 50.0, 0.0); b->radius = 0.5; b->mass = 20.0; b->name = "probe";
        eng.addBody(b);
        for (int i = 0; i < 240; ++i) eng.advance(1.0 / 60.0);   // 4 s SIN malla → descansa en la esfera (Y≈30)
        const double ySphere = b->position.y;
        w.ready.store(true);                                     // el planeta ya está: la malla aparece
        for (int i = 0; i < 600; ++i) eng.advance(1.0 / 60.0);   // 10 s: ¿sube el async de esfera a malla?
        const double yMesh = b->position.y;
        std::printf("    malla tardía: sin malla reposo Y=%.2f (esfera 30) → con malla Y=%.2f (esperado ~0.5)\n",
                    ySphere, yMesh);
        CHECK(ySphere > 29.0 && ySphere < 31.0, "sin malla: descansa en la ESFERA fallback (Y≈30)");
        CHECK(yMesh < 1.0, "al LLEGAR la malla, la colisión SUBE de esfera a malla (no se queda 30 m arriba)");
    }
}

// Fase 2 completa: un cuerpo dinámico (1) ANDA (el motor inyecta su velocidad de locomoción en Jolt) y
// (2) NO ATRAVIESA un colisionador ESTÁTICO (un muro puesto con addPlacedOBB). Es justo la "colisión sin
// terminar" que preocupaba: si el mundo estático no estuviera en Jolt, el cuerpo cruzaría el muro.
void test_physics_static_wall() {
    beginTest("physics_static_wall");
    Haruka::Physics::PhysicsEngine eng; FlatMeshWorld w(glm::dvec3(0.0)); eng.setWorldProvider(&w);

    // Muro macizo a x=3 (frente en x=2.5), alto y ancho; caja ORIENTADA sin girar (identidad).
    eng.addPlacedOBB(glm::dvec3(3.0, 5.0, 0.0), glm::dvec3(0.5, 5.0, 5.0), glm::dmat3(1.0));

    auto b = std::make_shared<Haruka::Physics::RigidBody>();
    b->position = glm::dvec3(0.0, 0.5, 0.0); b->radius = 0.5; b->mass = 20.0; b->name = "walker";
    b->velocity = glm::dvec3(5.0, 0.0, 0.0);            // camina hacia el muro a 5 m/s
    eng.addBody(b);
    for (int i = 0; i < 600; ++i) {
        b->velocity.x    = 5.0;    // el juego lo EMPUJA cada paso (marcando el cambio a propósito)
        b->velocityDirty = true;
        eng.advance(1.0 / 60.0);
    }
    const double x = b->position.x;
    std::printf("    caminó hasta x=%.2f m (muro en 2.5; empezó en 0)\n", x);
    CHECK(x > 1.0,  "el cuerpo ANDA: el motor inyecta la velocidad de locomoción en Jolt");
    CHECK(x < 2.6,  "el cuerpo NO ATRAVIESA el muro estático (se queda del lado de acá)");
}

// OBJETOS con física real (lo que sueltas: cae, se asienta) + RETIRADA limpia del cuerpo.
// El bug que cubre: `removeBody` borraba el cuerpo de la lista pero NO de Jolt → quedaba un
// colisionador FANTASMA invisible en ese sitio (y una clave `RigidBody*` colgando en el mapa). Se
// detecta soltando otro objeto en el mismo punto: si el fantasma sigue ahí, se posa MÁS ARRIBA.
void test_physics_body_removal() {
    beginTest("physics_body_removal");
    Haruka::Physics::PhysicsEngine eng; FlatMeshWorld w(glm::dvec3(0.0)); eng.setWorldProvider(&w);

    auto drop = [&](const char* name) {
        auto b = std::make_shared<Haruka::Physics::RigidBody>();
        b->name        = name;
        b->position    = glm::dvec3(0.0, 4.0, 0.0);
        b->radius      = 0.3; b->mass = 5.0;
        b->shape       = Haruka::Physics::RigidBody::Shape::Box;
        b->halfExtents = glm::dvec3(0.25);
        eng.addBody(b);
        for (int i = 0; i < 240; ++i) eng.advance(1.0 / 60.0);   // 4 s: cae y se asienta
        return b;
    };

    auto first = drop("caja1");
    const double y1 = first->position.y;
    CHECK(y1 > 0.0 && y1 < 0.45, "el objeto CAE y se asienta sobre el suelo (no lo atraviesa)");

    // Y SE QUEDA quieto: el motor no debe devolverle energía. Cuando se le re-imponía su velocidad cada
    // paso, la velocidad de separación del solver se re-aplicaba y el objeto trepaba/salía volando
    // "como si no pesara". Cuatro segundos más en reposo lo destapan.
    for (int i = 0; i < 240; ++i) eng.advance(1.0 / 60.0);
    std::printf("    tras 4 s más en reposo: Y=%.3f  |v|=%.3f m/s\n",
                first->position.y, glm::length(first->velocity));
    CHECK(std::abs(first->position.y - y1) < 0.05 && glm::length(first->velocity) < 0.5,
          "asentado SE QUEDA quieto (no gana energía: no sale volando)");

    eng.removeBody("caja1");                 // debe llevarse también su cuerpo de Jolt
    auto second = drop("caja2");
    const double y2 = second->position.y;
    std::printf("    1ª caja reposó en Y=%.3f · tras retirarla, la 2ª en Y=%.3f\n", y1, y2);
    CHECK(std::abs(y2 - y1) < 0.1, "retirar el cuerpo lo quita de Jolt (sin colisionador FANTASMA)");
}

// CONTROLADOR DE PERSONAJE (Jolt CharacterVirtual). Un personaje no es un rígido: cae y se ASIENTA, sabe
// que PISA SUELO (fuente de verdad del salto), anda, y no atraviesa un muro. Emular esto sobre una esfera
// rígida fue el origen de que rodara, se enterrara, no saltara y deslizara.
void test_physics_character() {
    beginTest("physics_character");
    Haruka::Physics::PhysicsEngine eng; FlatMeshWorld w(glm::dvec3(0.0)); eng.setWorldProvider(&w);
    eng.addPlacedOBB(glm::dvec3(3.0, 5.0, 0.0), glm::dvec3(0.5, 5.0, 5.0), glm::dmat3(1.0));

    auto b = std::make_shared<Haruka::Physics::RigidBody>();
    b->position    = glm::dvec3(0.0, 2.0, 0.0);     // suelto desde 2 m → debe aterrizar
    b->radius      = 0.5; b->mass = 70.0; b->name = "player";
    b->isCharacter = true;
    eng.addBody(b);

    for (int i = 0; i < 180; ++i) eng.advance(1.0 / 60.0);   // 3 s: cae y se asienta
    std::printf("    aterrizó a Y=%.3f m (esperado ~0.50 = radio) · pisaSuelo=%s\n",
                b->position.y, b->onGround ? "sí" : "no");
    CHECK(b->onGround, "el controlador reporta PISA SUELO al aterrizar (habilita el salto)");
    CHECK(b->position.y > 0.0 && b->position.y < 0.65, "se ASIENTA sobre el suelo (ni lo atraviesa ni flota)");

    for (int i = 0; i < 300; ++i) { b->velocity.x = 4.0; eng.advance(1.0 / 60.0); }   // camina al muro
    std::printf("    caminó hasta x=%.2f m (muro en 2.5)\n", b->position.x);
    CHECK(b->position.x > 1.0, "el personaje ANDA");
    CHECK(b->position.x < 2.6, "el personaje NO ATRAVIESA el muro");
}

// QUIETO EN EL SUELO NO SE ACUMULA LA CAÍDA.
//
// `CharacterVirtual` no es un rígido: el contacto con el suelo NO le devuelve la velocidad cancelada.
// El motor integraba la gravedad en TODOS los pasos, así que la componente radial de un personaje
// PARADO crecía sin tope. Medido en el juego con la sonda `CharContacts`, quieto sobre terreno llano:
//
//     onGround=1 · v radial -11.4 → -21.4 → -31.7 → ... → -83.4 m/s   (≈ -10 m/s por segundo)
//
// A 83 m/s el personaje pide meterse 1,3 m dentro del suelo cada frame y la colisión lo expulsa: se
// atasca, se desliza y "va pesado", y el sentido de la expulsión depende de qué triángulos toque —
// de ahí que pareciera que "pisa un montículo" estando quieto. La forma del suelo no tenía la culpa.
//
// ⚠️ CONTRAPRUEBAS (si no, este test lo aprobaría un motor SIN gravedad, que es peor que el bug):
//   (B) EN EL AIRE la gravedad sigue viva y la velocidad SÍ crece;
//   (C) apoyado, la velocidad TANGENCIAL que se le impone sobrevive — no se anula todo el vector.
void test_physics_character_resting_velocity() {
    beginTest("physics_character_resting_velocity");
    using namespace Haruka::Physics;

    auto spawn = [](PhysicsEngine& eng, double y) {
        auto b = std::make_shared<RigidBody>();
        b->position    = glm::dvec3(0.0, y, 0.0);
        b->radius      = 0.5; b->mass = 70.0; b->name = "player";
        b->isCharacter = true;
        eng.addBody(b);
        return b;
    };

    // (A) APOYADO: 6 s quieto. Con el bug esto llegaba a ~-59 m/s; el listón es 1 m/s.
    PhysicsEngine eng; FlatMeshWorld w(glm::dvec3(0.0)); eng.setWorldProvider(&w);
    auto b = spawn(eng, 2.0);
    for (int i = 0; i < 120; ++i) eng.advance(1.0 / 60.0);          // 2 s: cae y se asienta
    CHECK(b->onGround, "(A) el personaje está APOYADO antes de medir (si no, mide caída libre)");

    double vMax = 0.0;
    for (int i = 0; i < 360; ++i) {                                  // 6 s SIN tocar la velocidad
        eng.advance(1.0 / 60.0);
        vMax = std::max(vMax, glm::length(b->velocity));
    }
    const double vFree = 9.81 * 6.0;                                 // lo que daría el bug: caída libre
    std::printf("    (A) quieto 6 s: |v| max %.3f m/s · Y=%.3f · el bug daba ~%.1f m/s\n",
                vMax, b->position.y, vFree);
    CHECK(vMax < 1.0, "(A) QUIETO en el suelo la velocidad NO se acumula (no se clava contra el terreno)");
    CHECK(vMax < vFree * 0.1, "(A) y está un orden de magnitud por debajo de la caída libre del bug");

    // (B) CONTRAPRUEBA: en el AIRE la gravedad sigue integrándose. Sin esto, "vMax<1" lo cumpliría
    //     también un motor al que le hubiéramos apagado la gravedad entera.
    PhysicsEngine air; FlatMeshWorld wAir(glm::dvec3(0.0)); air.setWorldProvider(&wAir);
    auto f = spawn(air, 400.0);                                      // muy alto: no llega al suelo
    for (int i = 0; i < 60; ++i) air.advance(1.0 / 60.0);            // 1 s de caída
    std::printf("    (B) contraprueba en el aire tras 1 s: vY=%+.3f m/s (esperado ≈ -9.8) · Y=%.1f\n",
                f->velocity.y, f->position.y);
    CHECK(!f->onGround, "(B) el cuerpo de contraprueba SIGUE en el aire (si no, no prueba nada)");
    CHECK(f->velocity.y < -9.0, "(B) EN EL AIRE la gravedad sigue viva (no se ha apagado, sólo no se acumula apoyado)");

    // (C) CONTRAPRUEBA: apoyado, lo que se le manda en tangencial NO se borra.
    for (int i = 0; i < 60; ++i) { b->velocity.x = 4.0; eng.advance(1.0 / 60.0); }
    std::printf("    (C) contraprueba andando: vX=%+.3f m/s · x=%.2f m\n", b->velocity.x, b->position.x);
    CHECK(b->position.x > 1.0, "(C) apoyado, la velocidad TANGENCIAL sobrevive (anda; no se anula el vector entero)");
}

// ---------------------------------------------- TEST: RAÍLES contra Jolt (PLAN_PUERTOS.md §4)
// Una puerta/rampa NO es una animación: es una restricción. Se demuestra justo lo que una animación
// no sabe hacer, y con CONTRAPRUEBA en cada caso:
//   (1) el motor la MUEVE al objetivo pedido;
//   (2) el LÍMITE acota lo que se pide de más — y hay que ver que LLEGA al límite, no que se quedó
//       corta por otra razón (si no, "37° <= 90°" pasaría el test sin probar nada);
//   (3) sin motor (Manual) el mismo objetivo NO la mueve sola — si se moviera, seguiría siendo un clip;
//   (4) con algo estorbando, el motor se queda A MEDIAS y lo REPORTA (`blocked`);
//   (5) con una carga por encima de `breakForce` la unión CEDE y ya no obedece.
//
// ⚠️ Los cuerpos arrancan MUY por encima del suelo salvo en (4): en caída libre los dos caen igual,
// así que no hay par relativo y el mecanismo se mide limpio. Pegados al suelo, la hoja choca — que es
// exactamente el caso (4) y hay que provocarlo a propósito, no sufrirlo en todos los demás.
void test_rail_mechanism_jolt() {
    beginTest("physics_rail_jolt");
    using namespace Haruka::Physics;

    // ⚠️ La ORIENTACIÓN de la bisagra decide si el mecanismo puede chocar con el suelo. Aquí "abajo"
    // es -X (gravedad radial, cuerpos sobre el eje +X). Con el eje de giro RADIAL (1,0,0) la hoja se
    // mueve en el plano tangente = en HORIZONTAL, y nunca toca el suelo por mucho que gire. Para que
    // choque, el eje tiene que ser TANGENCIAL. Es el mismo motivo por el que una puerta y una rampa
    // no son la misma pieza aunque compartan restricción.
    auto build = [](PhysicsEngine& eng, FakeWorld& w, PhysicsEngine::RailSpec::Drive drive,
                    double motorForce, double breakForce, double altitude,
                    const glm::dvec3& axis, const glm::dvec3& leafOffset, int& railId) {
        eng.setWorldProvider(&w);
        const double r0 = w.radius + w.terrain + altitude;
        auto hull = std::make_shared<RigidBody>();
        hull->position = glm::dvec3(r0, 0, 0);
        hull->radius = 1.0; hull->mass = 5000.0; hull->name = "hull";
        hull->lockRotation = true;
        auto leaf = std::make_shared<RigidBody>();
        leaf->position = hull->position + leafOffset;
        leaf->radius = 0.5; leaf->mass = 60.0; leaf->name = "leaf";
        eng.addBody(hull); eng.addBody(leaf);

        PhysicsEngine::RailSpec spec;
        spec.dof    = PhysicsEngine::RailSpec::Dof::Hinge;
        spec.anchor = hull->position;
        spec.axis   = axis;
        spec.limits = glm::dvec2(-90.0, 90.0);
        spec.drive  = drive;
        spec.motorForce = motorForce;
        spec.breakForce = breakForce;
        railId = eng.addRail("hull", "leaf", spec);
    };
    const double kHigh = 400.0;   // bien alto: 5 s de caída libre son ~123 m, no llega al suelo

    // (1) y (2) MOTOR: llega a lo pedido, y el límite acota lo que se pide de más.
    {
        PhysicsEngine eng; FakeWorld w; int id = -1;
        build(eng, w, PhysicsEngine::RailSpec::Drive::Motor, 5.0e5, 0.0, kHigh,
              glm::dvec3(1,0,0), glm::dvec3(0,0,1), id);
        CHECK(id >= 0, "el rail se crea sobre Jolt");
        eng.railSetTarget(id, 45.0);
        for (int i = 0; i < 300; ++i) eng.advance(1.0 / 60.0);
        PhysicsEngine::RailInfo inf;
        CHECK(eng.railGet(id, inf), "se puede leer el estado del rail");
        std::printf("    motor: pedido 45.0 deg -> %.2f deg  (fuerza %.0f N)\n", inf.value, inf.force);
        CHECK(std::abs(inf.value - 45.0) < 2.0, "el motor lleva la hoja al objetivo");
        CHECK(!inf.broken && !inf.blocked, "sin estorbo ni rotura ni bloqueo");

        eng.railSetTarget(id, 500.0);
        for (int i = 0; i < 300; ++i) eng.advance(1.0 / 60.0);
        eng.railGet(id, inf);
        std::printf("    limite: pedido 500 deg -> %.2f deg\n", inf.value);
        CHECK(inf.value <= 90.5, "el limite acota lo que se pide de mas");
        // Sin esto, el caso anterior lo cumpliría un rail que no se moviese en absoluto.
        CHECK(inf.value > 85.0, "contraprueba: y LLEGA al limite, no se queda corta");
    }

    // (3) MANUAL: mismo objetivo, sin motor → no se mueve sola. Si se moviera, sería una animación.
    {
        PhysicsEngine eng; FakeWorld w; int id = -1;
        build(eng, w, PhysicsEngine::RailSpec::Drive::Manual, 0.0, 0.0, kHigh,
              glm::dvec3(1,0,0), glm::dvec3(0,0,1), id);
        eng.railSetTarget(id, 45.0);
        for (int i = 0; i < 300; ++i) eng.advance(1.0 / 60.0);
        PhysicsEngine::RailInfo inf; eng.railGet(id, inf);
        std::printf("    manual: pedido 45.0 deg -> %.2f deg (sin motor)\n", inf.value);
        CHECK(std::abs(inf.value) < 5.0, "contraprueba: sin motor, pedir no mueve");
    }

    // (4) BLOQUEO: una rampa con demasiado peso para su motor. El casco se apoya en el suelo y la
    //     hoja sale en HORIZONTAL, así que la gravedad hace un par de ~589 N·m sobre la bisagra
    //     (60 kg x 9.81 x 1 m). Con un motor de 1 N·m no puede levantarla: se queda a medias y lo
    //     REPORTA. Es la razón 4 del plan — con animación, una rampa levanta un acorazado igual que
    //     una pluma. Se provoca con el PAR, no con un choque: así es determinista.
    {
        PhysicsEngine eng; FakeWorld w; int id = -1;
        build(eng, w, PhysicsEngine::RailSpec::Drive::Motor, 1.0, 0.0, 1.0,
              glm::dvec3(0,0,1), glm::dvec3(0,1,0), id);
        CHECK(id >= 0, "el rail de la rampa se crea");
        for (int i = 0; i < 120; ++i) eng.advance(1.0 / 60.0);   // que el casco se asiente
        eng.railSetTarget(id, -90.0);                            // LEVANTAR la rampa
        for (int i = 0; i < 300; ++i) eng.advance(1.0 / 60.0);
        PhysicsEngine::RailInfo inf; eng.railGet(id, inf);
        std::printf("    bloqueo: motor 1 N-m vs carga ~589 N-m -> %.2f deg  blocked=%d\n",
                    inf.value, (int)inf.blocked);
        CHECK(inf.blocked, "el motor no puede con la carga: se queda a medias y lo REPORTA");
        CHECK(inf.value > -45.0, "y de hecho NO la levanta");
    }

    // (5) ROTURA: con un breakForce ridiculo, la propia carga basta para que la union ceda.
    {
        PhysicsEngine eng; FakeWorld w; int id = -1;
        build(eng, w, PhysicsEngine::RailSpec::Drive::Manual, 0.0, 1.0e-3, 0.0,
              glm::dvec3(1,0,0), glm::dvec3(0,0,1), id);
        for (int i = 0; i < 120; ++i) eng.advance(1.0 / 60.0);
        PhysicsEngine::RailInfo inf; eng.railGet(id, inf);
        std::printf("    rotura: breakForce=1e-3 N -> broken=%d\n", (int)inf.broken);
        CHECK(inf.broken, "la union CEDE por encima de breakForce");
        eng.railSetTarget(id, 0.0);
        eng.railGet(id, inf);
        CHECK(inf.broken, "roto sigue roto: no vuelve a obedecer");
    }
}
