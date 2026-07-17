// ================================================================================================
// Test del MÓDULO DE REGLAS del DGS (sin GL): carga libharuka_rules.so con dlopen —exactamente como
// haría el host/servidor autoritativo—, resuelve dgs_game_module_v1, comprueba el abiVersion y valida
// movimiento REAL (paso legal → acepta; teleport y noclip → rechaza). El .so usa el MISMO sampler de
// terreno que el cliente, así que cliente y host deciden igual con el mismo código.
// ================================================================================================
#include "test_common.h"

#include <dlfcn.h>
#include <glm/glm.hpp>

#include "include/dgs/game_module.h"
#include "core/terrain/terrain_sampler_v2.h"

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
    if (!m || !m->validateMove) { dlclose(h); return; }

    // Planeta PEQUEÑO (5 km): a esa escala el pos[] float del EntityTransfer es preciso.
    const double R = 5000.0; const uint32_t seed = 1234u;
    Haruka::WorldGenParams W = Haruka::deriveWorldParams(seed, R); W.profile = 0; W.reliefStrength = 1.0f;
    const glm::vec3  dir    = glm::normalize(glm::vec3(0.3f, 0.9f, 0.2f));
    const double     elevKm = Haruka::sampleTerrainV2(dir, W, R).elevKm;
    const double     surf   = R + elevKm * 1000.0;             // radio de la superficie (m)

    DGS::WorldQuery w{};
    w.chunkSizeX = w.chunkSizeY = w.chunkSizeZ = 1.0f;         // pos[] ya en metros (chunk 0)
    w.planetCenter[0] = w.planetCenter[1] = w.planetCenter[2] = 0.0;
    w.planetRadius = R; w.seed = seed; w.reliefStrength = 1.0f; w.profile = 0;

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
      CHECK(m->validateMove(&s, &w) == 1, "paso legal sobre la superficie → ACEPTADO"); }
    // (2) Teleport: 200 m en un frame a 5 m/s → imposible.
    { auto s = mkSample(onGround, onGround - glm::dvec3(step) * 5000.0, 5.0f, 1.0f/60.0f);
      CHECK(m->validateMove(&s, &w) == 0, "teleport (rompe maxSpeed·dt) → RECHAZADO"); }
    // (3) Noclip: posición 20 m POR DEBAJO del suelo → atravesar el terreno.
    { const glm::dvec3 under = glm::dvec3(dir) * (surf - 20.0);
      auto s = mkSample(under, under - step, 5.0f, 1.0f/60.0f);
      CHECK(m->validateMove(&s, &w) == 0, "atravesar el suelo (noclip) → RECHAZADO"); }

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
          CHECK(m->validateAction(1u, (const uint8_t*)&h, sizeof(h), &w) == 1, "acción DAMAGE bien formada → ADMITIDA"); }
        // (5) Verbo desconocido → RECHAZADA (blob malformado, ningún cliente honesto lo emite).
        { auto h = mkAction(999, 1.0f);
          CHECK(m->validateAction(1u, (const uint8_t*)&h, sizeof(h), &w) == 0, "verbo desconocido → RECHAZADA"); }
        // (6) Cantidad negativa / no finita → RECHAZADA.
        { auto h = mkAction(DGS::ACT_DAMAGE, -5.0f);
          CHECK(m->validateAction(1u, (const uint8_t*)&h, sizeof(h), &w) == 0, "cantidad negativa → RECHAZADA"); }
        // (7) Blob más corto que el encabezado → RECHAZADA (no hay ActionHeader).
        { auto h = mkAction(DGS::ACT_DESTROY, 1.0f);
          CHECK(m->validateAction(1u, (const uint8_t*)&h, sizeof(h) - 4, &w) == 0, "blob truncado (sin header) → RECHAZADA"); }
        // (8) Daño absurdamente alto (cheat) → RECHAZADA por la cota de cordura.
        { auto h = mkAction(DGS::ACT_DAMAGE, 1.0e9f);
          CHECK(m->validateAction(1u, (const uint8_t*)&h, sizeof(h), &w) == 0, "daño imposible (1e9) → RECHAZADA"); }
    }

    dlclose(h);
}
