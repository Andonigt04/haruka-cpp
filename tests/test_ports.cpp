/**
 * @file test_ports.cpp
 * @brief Puertos autorizados y RAÍLES (docs/guides/PLAN_PUERTOS.md).
 *
 * Lo que hay que demostrar, y su CONTRAPRUEBA en cada caso — un test que solo comprueba que algo
 * "no peta" no distingue una implementación buena de una que devuelve siempre lo mismo:
 *   1. El puerto lleva ORIENTACIÓN y sigue al objeto (contraprueba: si fuese un AABB, girar el prop
 *      no movería la normal del puerto).
 *   2. `portAccepts` respeta clase, kind y talla (contraprueba: talla menor NO entra).
 *   3. El raycast elige el puerto MÁS CERCANO y falla si apunta fuera (contraprueba explícita).
 *   4. El raíl BLOQUEA cuando la resistencia supera el motor y ROMPE cuando supera breakForce —
 *      que es justo lo que una animación no sabe hacer.
 *   5. Ida y vuelta por JSON sin pérdida.
 */
#include "test_common.h"
#include "game/ports/port.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace Haruka;

namespace {

Port mkPort(const char* id, glm::vec3 pos, PortClass cls, const char* kind, int size) {
    Port p; p.id = id; p.position = pos; p.cls = cls; p.kind = kind; p.size = size;
    return p;
}

} // namespace

void test_ports_transform() {
    beginTest("puertos: transform y orientacion");

    // Una toma de aire mirando a +X, a un metro del origen del prop.
    Port intake = mkPort("intake_fwd", glm::vec3(1.0f, 0.0f, 0.0f), PortClass::Mount, "chamber", 2);
    intake.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0));

    // Prop sin transformar: el puerto está donde dice.
    const glm::dmat4 I(1.0);
    const glm::dvec3 p0 = glm::dvec3(portWorld(I, intake)[3]);
    CHECK(std::abs(p0.x - 1.0) < 1e-9 && std::abs(p0.y) < 1e-9, "puerto en su posicion local");

    // El prop se mueve 100 m y gira 90° en Y: el puerto le sigue, posición Y orientación.
    glm::dmat4 X = glm::translate(glm::dmat4(1.0), glm::dvec3(100.0, 0.0, 0.0))
                 * glm::dmat4(glm::mat4_cast(glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0))));
    const glm::dvec3 p1 = glm::dvec3(portWorld(X, intake)[3]);
    // (1,0,0) girado 90° en Y → (0,0,-1); más la traslación.
    CHECK(std::abs(p1.x - 100.0) < 1e-6 && std::abs(p1.z + 1.0) < 1e-6,
          "el puerto sigue al prop (posicion)");

    // CONTRAPRUEBA de que la ORIENTACIÓN es real y no decorativa: el eje de un raíl definido en
    // local tiene que cambiar en mundo al girar el prop. Si los puertos fueran caras de un AABB
    // (lo de hoy), esto no podría distinguirse.
    Port door = mkPort("door", glm::vec3(0.0f), PortClass::Rail, "bulkhead", 1);
    door.hasRail = true;
    door.rail.axis = glm::vec3(0, 0, 1);                 // bisagra vertical en local
    const glm::dvec3 aI = railAxisWorld(I, door);
    const glm::dvec3 aX = railAxisWorld(X, door);
    CHECK(std::abs(aI.z - 1.0) < 1e-9, "eje del rail sin transformar");
    // Giro de +90° en Y: (0,0,1) → (+1,0,0). Es el mismo giro que lleva la POSICIÓN (1,0,0) a
    // (0,0,-1) unas líneas más arriba; las dos comprobaciones se sostienen la una a la otra.
    CHECK(std::abs(aX.x - 1.0) < 1e-6, "eje del rail GIRA con el prop");
    CHECK(glm::length(aI - aX) > 0.5, "contraprueba: el eje NO es invariante al giro");
}

void test_ports_fit() {
    beginTest("puertos: encaje por clase, kind y talla");

    Port bed = mkPort("bed_main", glm::vec3(0.0f), PortClass::Mount, "reinforced_bed", 3);

    CHECK(portAccepts(bed, "reinforced_bed", 3), "talla igual entra");
    CHECK(portAccepts(bed, "reinforced_bed", 1), "pieza mas pequena entra");
    // CONTRAPRUEBAS: sin ellas, un `return true` pasaría el test.
    CHECK(!portAccepts(bed, "reinforced_bed", 4), "contraprueba: pieza MAS GRANDE no entra");
    CHECK(!portAccepts(bed, "floor", 1),          "contraprueba: otro kind no entra");

    Port panel = mkPort("gauge", glm::vec3(0.0f), PortClass::Interact, "reinforced_bed", 3);
    CHECK(!portAccepts(panel, "reinforced_bed", 1),
          "contraprueba: en un puerto de INTERACCION no se atornilla");
}

void test_ports_raycast() {
    beginTest("puertos: raycast elige el mas cercano");

    PortSet set; set.assetId = "crawler_hull";
    set.ports.push_back(mkPort("near", glm::vec3(0.0f, 0.0f, -2.0f), PortClass::Interact, "bulkhead", 1));
    set.ports.push_back(mkPort("far",  glm::vec3(0.0f, 0.0f, -8.0f), PortClass::Interact, "bulkhead", 1));
    for (auto& p : set.ports) p.radius = 0.3f;

    const glm::dmat4 I(1.0);
    const Port* hit = nullptr; double t = 0.0;
    bool ok = raycastPorts(I, set, glm::dvec3(0.0), glm::dvec3(0, 0, -1), 100.0, &hit, &t);
    CHECK(ok && hit && hit->id == "near", "acierta el puerto mas cercano");
    CHECK(std::abs(t - 1.7) < 1e-6, "t = distancia a la superficie de enganche");

    // CONTRAPRUEBAS: apuntar al lado no debe acertar, y un alcance corto tampoco.
    hit = nullptr;
    CHECK(!raycastPorts(I, set, glm::dvec3(0.0), glm::dvec3(0, 1, 0), 100.0, &hit, &t),
          "contraprueba: apuntando a otro lado no hay puerto");
    CHECK(!raycastPorts(I, set, glm::dvec3(0.0), glm::dvec3(0, 0, -1), 1.0, &hit, &t),
          "contraprueba: fuera de alcance no hay puerto");
}

void test_rail_mechanism() {
    beginTest("rail: bloquea y rompe (lo que una animacion no hace)");

    // Rampa de carga motorizada: 0→80°, motor de 12 kN·m, cede a 40 kN·m.
    Rail ramp;
    ramp.dof = RailDof::Hinge; ramp.drive = RailDrive::Motor;
    ramp.limits = glm::vec2(0.0f, 80.0f);
    ramp.motorForce = 12000.0f; ramp.breakForce = 40000.0f;

    // (a) Sin resistencia baja entera. 80° a 40°/s = 2 s.
    RailState st; st.target = 80.0f;
    for (int i = 0; i < 100; ++i) railStep(ramp, st, 0.05f, 40.0f, 0.0f);
    CHECK(std::abs(st.value - 80.0f) < 1e-3f, "sin carga llega al limite");
    CHECK(!st.blocked && !st.broken, "sin carga ni bloqueo ni rotura");

    // (b) El límite se respeta: pedir 200° no da 200°.
    st.target = 200.0f;
    railStep(ramp, st, 0.05f, 40.0f, 0.0f);
    CHECK(st.value <= 80.0f + 1e-4f, "contraprueba: no pasa del limite pedido de mas");

    // (c) Con un acorazado encima el motor NO puede: se queda A MEDIAS. Este es el caso que una
    // animación resuelve mal — levantaría el peso igual que una pluma.
    RailState st2; st2.target = 80.0f;
    for (int i = 0; i < 20; ++i) railStep(ramp, st2, 0.05f, 40.0f, 25000.0f);
    CHECK(st2.blocked, "resistencia > motor -> BLOQUEADO");
    CHECK(st2.value < 1e-4f, "bloqueado significa que NO avanza");

    // (d) Y si la carga supera `breakForce`, la unión cede: el mecanismo queda roto para siempre.
    RailState st3; st3.target = 80.0f;
    railStep(ramp, st3, 0.05f, 40.0f, 55000.0f);
    CHECK(st3.broken, "resistencia > breakForce -> ROTO");
    st3.target = 0.0f;
    railStep(ramp, st3, 0.05f, 40.0f, 0.0f);
    CHECK(st3.broken && std::abs(st3.value) < 1e-4f, "roto no vuelve a obedecer");

    // (e) Una suspensión (drive=Spring) no tiene tope de motor: no se "bloquea" por carga.
    Rail susp; susp.dof = RailDof::Slider; susp.drive = RailDrive::Spring;
    susp.limits = glm::vec2(-0.25f, 0.0f); susp.compliance = 4e-4f; susp.breakForce = 180000.0f;
    RailState ss; ss.target = -0.25f;
    railStep(susp, ss, 0.05f, 1.0f, 90000.0f);
    CHECK(!ss.blocked && !ss.broken, "la suspension no se bloquea por carga alta");
    CHECK(railClamp(susp, -5.0f) <= -0.25f + 1e-6f, "clamp del recorrido de la suspension");
}

void test_ports_json_roundtrip() {
    beginTest("puertos: ida y vuelta por JSON");

    PortSet set; set.assetId = "crawler_hull";
    Port seat = mkPort("seat_pilot", glm::vec3(0.5f, 1.2f, -3.0f), PortClass::Interact, "floor", 1);
    seat.partIndex = 2;
    Port door = mkPort("door_stbd", glm::vec3(2.0f, 0.0f, 0.0f), PortClass::Mount, "bulkhead", 2);
    door.hasRail = true;
    door.rail.dof = RailDof::Hinge; door.rail.drive = RailDrive::Motor;
    door.rail.axis = glm::vec3(0, 1, 0); door.rail.limits = glm::vec2(0.0f, 95.0f);
    door.rail.motorForce = 2500.0f; door.rail.breakForce = 30000.0f;
    set.ports = { seat, door };

    const std::string path =
        (std::filesystem::temp_directory_path() / "haruka_ports_test.json").string();
    CHECK(savePortSetJson(path, set), "guarda el JSON");

    PortSet back;
    CHECK(loadPortSetJson(path, back), "vuelve a leer el JSON");
    CHECK(back.assetId == "crawler_hull", "asset conservado");
    CHECK(back.ports.size() == 2, "numero de puertos conservado");

    const Port* s2 = back.find("seat_pilot");
    CHECK(s2 && s2->cls == PortClass::Interact && s2->partIndex == 2,
          "clase y partIndex conservados");
    CHECK(s2 && std::abs(s2->position.y - 1.2f) < 1e-6f, "posicion conservada");

    const Port* d2 = back.find("door_stbd");
    CHECK(d2 && d2->hasRail, "el rail sobrevive");
    // Un puerto con raíl ES de clase Rail aunque se guardara como Mount: una sola verdad.
    CHECK(d2 && d2->cls == PortClass::Rail, "con rail, la clase pasa a Rail");
    CHECK(d2 && d2->rail.drive == RailDrive::Motor &&
          std::abs(d2->rail.limits.y - 95.0f) < 1e-6f, "parametros del rail conservados");
    CHECK(d2 && std::abs(d2->rail.breakForce - 30000.0f) < 1e-3f, "breakForce conservado");

    // CONTRAPRUEBA: un fichero que no es un PortSet no debe "cargar bien".
    PortSet bad;
    CHECK(!loadPortSetJson((std::filesystem::temp_directory_path() / "no_existe_12345.json").string(), bad),
          "contraprueba: fichero inexistente no carga");

    std::error_code ec; std::filesystem::remove(path, ec);

    // Registro: el find falla para lo no registrado (contraprueba del singleton).
    PortRegistry::get().add(set);
    CHECK(PortRegistry::get().find("crawler_hull") != nullptr, "registro devuelve lo registrado");
    CHECK(PortRegistry::get().find("no_registrado") == nullptr,
          "contraprueba: el registro NO inventa assets");
}
