// ================================================================================================
// Test del MÓDULO DE REGLAS del DGS (sin GL): carga libharuka_rules.so con dlopen —exactamente como
// haría el host/servidor autoritativo—, resuelve dgs_game_module_v1, comprueba el abiVersion y valida
// movimiento REAL (paso legal → acepta; teleport y noclip → rechaza). El .so usa el MISMO sampler de
// terreno que el cliente, así que cliente y host deciden igual con el mismo código.
// ================================================================================================
#include "test_common.h"

#include <dlfcn.h>
#include <glm/glm.hpp>
#include <vector>

#include "include/dgs/game_module.h"

// ---------------------------------- TEST: módulo de reglas DGS cargado como .so (Fase 3, "de golpe")
// Verifica el ABI ENTERO de una vez: se carga libharuka_rules.so con dlopen (como haría el host/DGS),
// se resuelve dgs_game_module_v1, se comprueba abiVersion, y se valida movimiento REAL:
//   · un paso legal sobre la superficie → ACEPTADO (1)
//   · un teleport que rompe maxSpeed·dt → RECHAZADO (0)
//   · atravesar el suelo (noclip) → RECHAZADO (0)
// El .so es GL-free (validado con ldd) y usa el MISMO sampler de terreno que el cliente → cliente y
// host/servidor deciden igual con el mismo código. Es lo que el autor pidió: validación local.
void test_dgs_rules_module() {
    beginTest("dgs_rules_module");
    // El .so vive junto al ejecutable de tests (build/). Se prueba con varios paths por robustez.
    void* h = dlopen("./libharuka_rules.so", RTLD_NOW);
    if (!h) h = dlopen("libharuka_rules.so", RTLD_NOW);
    CHECK(h != nullptr, "carga libharuka_rules.so (dlopen — lo mismo que haría el host/DGS)");
    if (!h) { std::printf("    dlerror: %s\n", dlerror()); return; }

    auto entry = (const DGS::GameModule* (*)())dlsym(h, "dgs_game_module_v1");
    CHECK(entry != nullptr, "resuelve el símbolo dgs_game_module_v1 (dlsym)");
    if (!entry) { dlclose(h); return; }
    const DGS::GameModule* m = entry();
    CHECK(m && m->abiVersion == DGS::GAME_MODULE_ABI, "abiVersion del módulo == el del host");
    CHECK(m && m->validateMove, "el módulo aporta validateMove");
    CHECK(m && m->createZone && m->destroyZone, "el módulo aporta CICLO DE VIDA de zona (crear/destruir)");
    if (!m || !m->validateMove || !m->createZone) { dlclose(h); return; }

    // Planeta PEQUEÑO (5 km): a esa escala el pos[] float del EntityTransfer es preciso.
    const double R = 5000.0; const uint32_t seed = 1234u;
    const glm::vec3  dir    = glm::normalize(glm::vec3(0.3f, 0.9f, 0.2f));
    const double     surf   = R;                             // terrain is reference sphere (flat)

    DGS::WorldQuery w{};
    w.chunkSizeX = w.chunkSizeY = w.chunkSizeZ = 1.0f;         // pos[] ya en metros (chunk 0)
    w.planetCenter[0] = w.planetCenter[1] = w.planetCenter[2] = 0.0;
    w.planetRadius = R; w.seed = seed; w.reliefStrength = 1.0f; w.profile = 0;

    // El host crea una ZONA: el estado autoritativo cuelga de ella, no de globales del módulo. Así un
    // nodo puede servir varias y traspasarlas.
    DGS::ZoneHandle zone = m->createZone(&w);
    CHECK(zone != nullptr, "createZone devuelve una zona");
    if (!zone) { dlclose(h); return; }

    auto mkSample = [&](glm::dvec3 pos, glm::dvec3 last, float maxSpeed, float dt) {
        static DGS::EntityTransfer e;   // static: el MoveSample guarda un puntero a él
        e = DGS::EntityTransfer{};
        e.chunkX = e.chunkY = e.chunkZ = 0;
        e.pos[0] = (float)pos.x; e.pos[1] = (float)pos.y; e.pos[2] = (float)pos.z;
        DGS::MoveSample s{};
        s.now = &e; s.lastGX = (float)last.x; s.lastGY = (float)last.y; s.lastGZ = (float)last.z;
        s.maxSpeed = maxSpeed; s.dtSeconds = dt;
        return s;
    };

    const glm::dvec3 onGround = glm::dvec3(dir) * (surf + 1.0);   // 1 m sobre el suelo
    // (1) Paso legal: avanza ~4 cm en 16 ms a 5 m/s → dentro del presupuesto.
    const glm::dvec3 step = glm::normalize(glm::cross(glm::dvec3(dir), glm::dvec3(0,1,0))) * 0.04;
    { auto s = mkSample(onGround, onGround - step, 5.0f, 1.0f/60.0f);
      CHECK(m->validateMove(zone, &s, &w) == 1, "paso legal sobre la superficie → ACEPTADO"); }
    // (2) Teleport: 200 m en un frame a 5 m/s → imposible.
    { auto s = mkSample(onGround, onGround - glm::dvec3(step) * 5000.0, 5.0f, 1.0f/60.0f);
      CHECK(m->validateMove(zone, &s, &w) == 0, "teleport (rompe maxSpeed·dt) → RECHAZADO"); }
    // (3) Noclip: posición 20 m POR DEBAJO del suelo → atravesar el terreno.
    { const glm::dvec3 under = glm::dvec3(dir) * (surf - 20.0);
      auto s = mkSample(under, under - step, 5.0f, 1.0f/60.0f);
      CHECK(m->validateMove(zone, &s, &w) == 0, "atravesar el suelo (noclip) → RECHAZADO"); }
    // (3b) FALSO POSITIVO que sí ocurría, en un planeta TAMAÑO TIERRA — que es donde se midió. El
    // cliente camina sobre la MALLA, que SUAVIZA el analítico y queda por debajo de él: `haruka_tests
    // earthdiag` mide hasta 5.12 m. Con el margen fijo de 5.0 m que había, ese jugador LEGÍTIMO se
    // expulsaba como tramposo. Ahora el suelo es el MÍNIMO local del analítico, que acota la malla por
    // debajo → se acepta sin aflojar el (3).
    // ⚠️ El planeta pequeño de arriba NO sirve para esto: el suavizado escala con el radio, y a 5 km es
    // submilimétrico — la prueba pasaría sin probar nada.
    {
        const double Re = 6.371e6; const uint32_t seedE = 123158u;
        const glm::vec3  dirE  = glm::normalize(glm::vec3(0.3f, 0.9f, 0.2f));
        const double     surfE = Re;

        DGS::WorldQuery we{};
        we.chunkSizeX = we.chunkSizeY = we.chunkSizeZ = 1.0e6f;   // el grueso va en el índice de chunk…
        we.planetCenter[0] = we.planetCenter[1] = we.planetCenter[2] = 0.0;
        we.planetRadius = Re; we.seed = seedE; we.reliefStrength = 1.0f; we.profile = 0;
        DGS::ZoneHandle zoneE = m->createZone(&we);

        // …y pos[] solo guarda el resto: en float, a 6.4e6 m la resolución sería ~0.5 m y no se podría
        // distinguir un desfase de 5 m. Repartido así, el resto vive por debajo de 1e6 (≈0.06 m).
        auto mkEarth = [&](const glm::dvec3& g, const glm::dvec3& last) {
            static DGS::EntityTransfer e; e = DGS::EntityTransfer{};
            const double CS = 1.0e6;
            const double cx = std::floor(g.x / CS), cy = std::floor(g.y / CS), cz = std::floor(g.z / CS);
            e.chunkX = (int32_t)cx; e.chunkY = (int32_t)cy; e.chunkZ = (int32_t)cz;
            e.pos[0] = (float)(g.x - cx * CS); e.pos[1] = (float)(g.y - cy * CS); e.pos[2] = (float)(g.z - cz * CS);
            DGS::MoveSample s{}; s.now = &e;
            s.lastGX = (float)last.x; s.lastGY = (float)last.y; s.lastGZ = (float)last.z;
            s.maxSpeed = 5.0f; s.dtSeconds = 1.0f/60.0f;
            return s;
        };
        const glm::dvec3 stepE = glm::normalize(glm::cross(glm::dvec3(dirE), glm::dvec3(0,1,0))) * 0.04;
        { const glm::dvec3 sm = glm::dvec3(dirE) * (surfE - 1.5);
          auto s = mkEarth(sm, sm - stepE);
          CHECK(m->validateMove(zoneE, &s, &we) == 1,
                "Tierra: jugador sobre la MALLA suavizada (1.5 m bajo el analitico) → ACEPTADO (no es noclip)"); }
        { const glm::dvec3 deep = glm::dvec3(dirE) * (surfE - 300.0);
          auto s = mkEarth(deep, deep - stepE);
          CHECK(m->validateMove(zoneE, &s, &we) == 0,
                "Tierra: noclip real (300 m bajo el suelo) → RECHAZADO (el minimo local no lo tapa)"); }
        if (m->destroyZone) m->destroyZone(zoneE);
    }

    // --- validateAction (ABI v2): el blob es OPACO; el módulo por defecto valida solo invariantes ---
    CHECK(m->validateAction != nullptr, "el módulo aporta validateAction (ABI v2)");
    if (m->validateAction) {
        auto mkAction = [](uint16_t verb, float amount, uint16_t flags = 0) {
            DGS::ActionHeader h{};
            h.verb = verb; h.flags = flags; h.target = 42; h.amount = amount;
            h.at[0] = h.at[1] = h.at[2] = 0.0f;
            return h;
        };
        // (4) Acción bien formada (daño 10 a un target) → ADMITIDA.
        { auto h = mkAction(DGS::ACT_DAMAGE, 10.0f);
          CHECK(m->validateAction(zone, 1u, (const uint8_t*)&h, sizeof(h), &w) == 1, "acción DAMAGE bien formada → ADMITIDA"); }
        // (5) Verbo desconocido → RECHAZADA (blob malformado, ningún cliente honesto lo emite).
        { auto h = mkAction(999, 1.0f);
          CHECK(m->validateAction(zone, 1u, (const uint8_t*)&h, sizeof(h), &w) == 0, "verbo desconocido → RECHAZADA"); }
        // (6) Cantidad negativa / no finita → RECHAZADA.
        { auto h = mkAction(DGS::ACT_DAMAGE, -5.0f);
          CHECK(m->validateAction(zone, 1u, (const uint8_t*)&h, sizeof(h), &w) == 0, "cantidad negativa → RECHAZADA"); }
        // (7) Blob más corto que el encabezado → RECHAZADA (no hay ActionHeader).
        { auto h = mkAction(DGS::ACT_DESTROY, 1.0f);
          CHECK(m->validateAction(zone, 1u, (const uint8_t*)&h, sizeof(h) - 4, &w) == 0, "blob truncado (sin header) → RECHAZADA"); }
        // (8) Daño absurdamente alto (cheat) → RECHAZADA por la cota de cordura.
        { auto h = mkAction(DGS::ACT_DAMAGE, 1.0e9f);
          CHECK(m->validateAction(zone, 1u, (const uint8_t*)&h, sizeof(h), &w) == 0, "daño imposible (1e9) → RECHAZADA"); }
    }

    // --- ACT_PLACE (ABI v3): COLOCAR validado DE VERDAD con el núcleo de construcción -------------
    // Es el verbo que cierra "el servidor decide con el MISMO código que el cliente": no comprueba
    // que el blob esté bien formado, comprueba GEOMETRÍA — que la pieza apoye y que no solape otra ya
    // colocada. Un cliente tramposo que reenvíe la misma colocación debe ser RECHAZADO la segunda vez.
    {
        // El host manda el CATÁLOGO al cargar el mundo: sin él el módulo no sabe el tamaño de la pieza
        // y no podría comprobar solapes. Aquí, una pieza cúbica de 0.5 m que apoya en terreno y pieza.
        CHECK(m->setPieceCatalog != nullptr, "el módulo acepta el catálogo de piezas (ABI v3)");
        if (m->setPieceCatalog) {
            DGS::PieceDesc pd{};
            pd.typeId = 1; pd.supports = 1 | 2; pd.needsFlat = 0;
            pd.half[0] = pd.half[1] = pd.half[2] = 0.25f;
            m->setPieceCatalog(zone, &pd, 1);
        }

        struct PlaceBlob { DGS::ActionHeader h; DGS::PlaceAction p; };
        auto mkPlace = [&](glm::dvec3 pos, double reach) {
            PlaceBlob b{};
            b.h.verb = DGS::ACT_PLACE; b.h.flags = 0; b.h.target = 0; b.h.amount = 1.0f;
            const glm::dvec3 at = pos + glm::normalize(glm::dvec3(dir)) * reach;
            b.h.at[0] = (float)at.x; b.h.at[1] = (float)at.y; b.h.at[2] = (float)at.z;
            b.p.typeId = 1; b.p.pad = 0;
            b.p.pos[0] = pos.x; b.p.pos[1] = pos.y; b.p.pos[2] = pos.z;
            b.p.quat[0] = 0.0; b.p.quat[1] = 0.0; b.p.quat[2] = 0.0; b.p.quat[3] = 1.0;
            return b;
        };
        const glm::dvec3 onSurf = glm::dvec3(dir) * (surf + 0.5);   // apoyada en el suelo

        // (9) Primera colocación apoyada en el terreno → ACEPTADA (y pasa a ser estado autoritativo).
        { auto b = mkPlace(onSurf, 1.0);
          CHECK(m->validateAction(zone, 1u, (const uint8_t*)&b, sizeof(b), &w) == 1,
                "ACT_PLACE apoyada en el terreno → ACEPTADA"); }
        // (10) LA MISMA otra vez: ahora solapa la que el módulo ya guardó → RECHAZADA. Esto es lo que
        //      un cliente tramposo intentaría, y solo se puede parar teniendo estado autoritativo.
        { auto b = mkPlace(onSurf, 1.0);
          CHECK(m->validateAction(zone, 1u, (const uint8_t*)&b, sizeof(b), &w) == 0,
                "ACT_PLACE repetida en el mismo sitio (solapa) → RECHAZADA"); }
        // (11) Lejísimos del punto declarado de la acción → RECHAZADA (colocar a distancia arbitraria).
        { auto b = mkPlace(glm::dvec3(dir) * (surf + 400.0), 400.0);
          CHECK(m->validateAction(zone, 1u, (const uint8_t*)&b, sizeof(b), &w) == 0,
                "ACT_PLACE fuera de alcance → RECHAZADA"); }
        // (12) Payload truncado (header sin PlaceAction detrás) → RECHAZADA.
        { auto b = mkPlace(onSurf, 1.0);
          CHECK(m->validateAction(zone, 1u, (const uint8_t*)&b, sizeof(DGS::ActionHeader), &w) == 0,
                "ACT_PLACE sin payload → RECHAZADA"); }
    }

    // --- TRASPASO de una región entre nodos (reasignar trozo de escena / ampliar zona) -----------
    // Es la operación que hace posible repartir un mundo entre servidores: el nodo A serializa lo que
    // hay en una esfera, B lo incorpora, y A lo suelta. Se prueba con DOS zonas del mismo módulo.
    if (m->serializeRegion && m->mergeRegion && m->dropRegion)
    {
        const glm::dvec3 onSurf = glm::dvec3(dir) * (surf + 0.5);
        const double c[3] = { onSurf.x, onSurf.y, onSurf.z };

        // Tamaño necesario primero (out=nullptr) y luego el volcado: la zona A tiene 1 pieza ahí.
        const size_t need = m->serializeRegion(zone, c, 5.0, nullptr, 0);
        CHECK(need > 0, "serializeRegion informa del tamaño necesario (out=nullptr)");
        std::vector<uint8_t> buf(need);
        const size_t wrote = m->serializeRegion(zone, c, 5.0, buf.data(), buf.size());
        CHECK(wrote == need, "serializeRegion vuelca la región en el buffer");

        // Nodo B: zona NUEVA que recibe la región. Necesita el catálogo igual que A.
        DGS::ZoneHandle zoneB = m->createZone(&w);
        CHECK(zoneB != nullptr, "segunda zona (el nodo que recibe) creada");
        if (zoneB) {
            DGS::PieceDesc pd{};
            pd.typeId = 1; pd.supports = 1 | 2; pd.needsFlat = 0;
            pd.half[0] = pd.half[1] = pd.half[2] = 0.25f;
            m->setPieceCatalog(zoneB, &pd, 1);

            CHECK(m->mergeRegion(zoneB, buf.data(), buf.size()) == 1, "mergeRegion incorpora la región en B");
            // La prueba de que B la tiene DE VERDAD: ahora rechaza colocar encima (solapa lo recibido).
            struct PlaceBlob2 { DGS::ActionHeader h; DGS::PlaceAction p; };
            PlaceBlob2 b{};
            b.h.verb = DGS::ACT_PLACE; b.h.amount = 1.0f;
            b.h.at[0] = (float)onSurf.x; b.h.at[1] = (float)onSurf.y; b.h.at[2] = (float)onSurf.z;
            b.p.typeId = 1;
            b.p.pos[0] = onSurf.x; b.p.pos[1] = onSurf.y; b.p.pos[2] = onSurf.z;
            b.p.quat[3] = 1.0;
            CHECK(m->validateAction(zoneB, 1u, (const uint8_t*)&b, sizeof(b), &w) == 0,
                  "B conoce lo recibido: colocar encima → RECHAZADO (el traspaso fue REAL)");

            // Basura / formato ajeno → no se traga nada.
            const uint8_t junk[16] = {0};
            CHECK(m->mergeRegion(zoneB, junk, sizeof(junk)) == 0, "mergeRegion rechaza un blob ajeno");

            // A SUELTA la región (paso final de la reasignación): ya no queda nada que serializar allí.
            m->dropRegion(zone, c, 5.0);
            CHECK(m->serializeRegion(zone, c, 5.0, nullptr, 0) == sizeof(uint32_t) * 2 + sizeof(uint32_t) * 2 ||
                  m->serializeRegion(zone, c, 5.0, nullptr, 0) < need,
                  "dropRegion suelta la región en A (ya no la sirve)");
            m->destroyZone(zoneB);
        }
    }

    m->destroyZone(zone);   // liberación EXPLÍCITA (no al azar del dlclose)
    dlclose(h);
}
