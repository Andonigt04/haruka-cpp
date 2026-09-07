// ================================================================================================
// Test del MÓDULO DE REGLAS del DGS (sin GL): carga libharuka_rules.so con dlopen —exactamente como
// haría el host/servidor autoritativo—, resuelve dgs_game_module_v1, comprueba el abiVersion y valida
// movimiento REAL (paso legal → acepta; teleport y noclip → rechaza). El .so usa el MISMO sampler de
// terreno que el cliente, así que cliente y host deciden igual con el mismo código.
// ================================================================================================
#include "core/weather_system.h"
#include "core/scene/scene_render_policy.h"
#include "test_common.h"

#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <glm/glm.hpp>
#include <vector>
#include <random>
#include <chrono>

#include "physics/physics_engine.h"
#include "physics/world_provider.h"
#include "core/terrain/terrain_sample.h"
#include "core/planet/soi.h"
#include "core/planet/terrain_detail.h"
#include "core/planet/terrain_lod.h"
#include "net/height_field.h"
#include <cstdio>
#include <string>
#include <memory>
#include <cmath>
#include "include/dgs/game_module.h"
#include "include/dgs/packet.h"
#include "include/dgs/network.h"

// Los tests de evicción/escalado dependen de `Orchestrator` (orchestrator.h), que a su vez exige
// `httplib.h` (no vendido en el SDK). Solo se compilan donde httplib esté disponible (repo real); el
// resto (framing, truncación, fuzz) son puros de packet.h/network.h y corren siempre.
#if __has_include(<httplib.h>)
#include "include/dgs/orchestrator.h"
#define DGS_TEST_HAS_ORCHESTRATOR 1
#endif

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
    // Bajo CTest el directorio de trabajo es `build/` y acierta la primera; lanzado a mano desde la
    // raiz del repo acierta esta. Sin ella hay que COPIAR el .so al arbol de FUENTES —lo que hace la
    // CI— y eso deja un artefacto de compilacion dentro del codigo.
    if (!h) h = dlopen("./build/libharuka_rules.so", RTLD_NOW);
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
    // (3b) EL SUELO A ESCALA TIERRA. El cliente camina sobre la MALLA, que suaviza el analitico y queda
    // por debajo de el (`haruka_tests earthdiag` midio hasta 5,12 m); con el margen fijo de 5,0 m que
    // hubo, ese jugador LEGITIMO se expulsaba como tramposo. La respuesta fue la `FloorCache`: el suelo
    // es el MINIMO LOCAL del analitico, que acota la malla por debajo.
    //
    // ⚠️⚠️ ESTE BLOQUE COMPROBABA UNA CONSTANTE, NO LA REGLA — Y HAY QUE SABER POR QUE.
    // Estaba escrito como "jugador 1,5 m bajo el analitico -> ACEPTADO", y pasaba con el `-2,0 m`
    // inventado que habia en el validador. Al sustituir ese 2,0 por la suma de terminos MEDIDOS
    // (`Tolerance`, ~0,59 m a 5 m/s) el caso reventó, y al medir por que salio esto:
    //
    //     [suelo] RELIEVE del analitico en 512 puntos del planeta: +0,00 m .. +0,00 m
    //     [suelo] lo que acota el minimo local:  min 0,0000 · medio 0,0000 · MAX 0,0000 m
    //
    // `Haruka::sampleTerrainV2` **es un stub en esta rama** ("terrain sampler removed", devuelve
    // elevKm = 0). O sea que este "planeta Tierra" es una ESFERA LISA: no hay malla que suavizar, no
    // hay nada que el minimo local pueda acotar, y los 1,5 m no median suavizado ninguno — median que
    // el validador llevaba una holgura de 2 m. Un test que pasa por una constante que el propio test
    // no nombra no prueba la regla.
    //
    // Reescrito para comprobar LA REGLA, que es la misma con stub y con sampler real: se sitúa al
    // jugador RELATIVO al suelo que el validador va a calcular (el minimo local, medido aqui con el
    // mismo vecindario), y se comprueba que la frontera cae donde dice `Tolerance`. Asi el dia que
    // vuelva el sampler estas mismas lineas siguen midiendo, en vez de volverse vacias en silencio.
    //
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
        double floorM = 0.0, reliefM = -1.0;   // suelo minimo y relieve MEDIDOS abajo, no supuestos

        // ⚠️ QUE PARTE DE ESOS 1,5 m ACOTA EL MINIMO LOCAL Y QUE PARTE NO. Sin esta cifra el umbral de
        // noclip se elige a ojo — que es exactamente como se llego al `-5,0 m` que expulsaba a
        // jugadores legitimos. Se reproduce aqui el vecindario 3x3 de `FloorCache` (mismo `kCellFrac`)
        // y se imprime lo que queda POR DEBAJO del suelo minimo, que es lo unico que el margen del
        // validador tiene que cubrir.
        {
            Haruka::WorldGenParams WE = Haruka::deriveWorldParams(seedE, Re);
            WE.reliefStrength = 1.0f; WE.profile = 0;
            const glm::dvec3 d0(dirE);
            const double elev = (double)Haruka::sampleTerrainV2(glm::vec3(d0), WE, Re).elevKm * 1000.0;
            glm::dvec3 t1 = glm::normalize(glm::cross(d0, std::abs(d0.y) < 0.99 ? glm::dvec3(0,1,0)
                                                                                : glm::dvec3(1,0,0)));
            const glm::dvec3 t2 = glm::cross(d0, t1);
            double lo = 1e30;
            for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
                const glm::dvec3 nd = glm::normalize(d0 + (t1 * (double)i + t2 * (double)j) * 5.0e-6);
                lo = std::min(lo, (double)Haruka::sampleTerrainV2(glm::vec3(nd), WE, Re).elevKm * 1000.0);
            }
            std::printf("    [suelo] analitico %+.4f m · minimo local %+.4f m (acota %.4f m)\n",
                        elev, lo, elev - lo);
            floorM = lo;

            // Y CUANTO ACOTA EN GENERAL, no en un punto. Un solo sitio no dice nada (puede caer en
            // llano); lo que decide el umbral es el PEOR caso sobre el planeta.
            double wmin = 1e30, wmax = -1e30, acc = 0.0; int n = 0;
            double emin = 1e30, emax = -1e30;
            for (int t = 0; t < 512; ++t) {
                const double u = (double)t * 2.39996322972865332;      // angulo aureo → reparto uniforme
                const double z = 1.0 - 2.0 * ((double)t + 0.5) / 512.0;
                const double rr = std::sqrt(std::max(0.0, 1.0 - z * z));
                const glm::dvec3 d(rr * std::cos(u), z, rr * std::sin(u));
                const double e0 = (double)Haruka::sampleTerrainV2(glm::vec3(d), WE, Re).elevKm * 1000.0;
                glm::dvec3 a1 = glm::normalize(glm::cross(d, std::abs(d.y) < 0.99 ? glm::dvec3(0,1,0)
                                                                                 : glm::dvec3(1,0,0)));
                const glm::dvec3 a2 = glm::cross(d, a1);
                double mn = 1e30;
                for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
                    const glm::dvec3 nd = glm::normalize(d + (a1 * (double)i + a2 * (double)j) * 5.0e-6);
                    mn = std::min(mn, (double)Haruka::sampleTerrainV2(glm::vec3(nd), WE, Re).elevKm * 1000.0);
                }
                const double b = e0 - mn; wmin = std::min(wmin, b); wmax = std::max(wmax, b); acc += b; ++n;
                emin = std::min(emin, e0); emax = std::max(emax, e0);
            }
            std::printf("    [suelo] lo que acota el minimo local en 512 puntos del planeta:"
                        " min %.4f m · medio %.4f m · MAX %.4f m\n", wmin, acc / (double)n, wmax);
            std::printf("    [suelo] RELIEVE del analitico en esos 512 puntos: %+.2f m .. %+.2f m\n", emin, emax);

            // ⚠️ LA CONTRAPRUEBA QUE FALTABA: decir en voz alta si este mundo tiene relieve. Sin ella el
            // bloque entero puede volverse vacio sin que nadie se entere — que es justo lo que paso.
            reliefM = emax - emin;
            if (reliefM <= 0.0)
                std::printf("    \033[33m[suelo] ⚠️ el analitico es PLANO (sampleTerrainV2 es un stub en esta rama):\n"
                            "            la `FloorCache` no puede acotar nada y el validador juzga el noclip contra\n"
                            "            una ESFERA LISA a nivel del mar. La regla se comprueba igual; el suavizado NO.\033[0m\n");
        }

        // LA FRONTERA, relativa al suelo que el validador calcula (R + minimo local) y no a `R` a pelo:
        // asi la prueba dice lo mismo con el stub y con el sampler real. `Tolerance::physicsM(5 m/s)` =
        // 0,010 (ancla medida) + 5/60 (un sub-paso) + 0,50 (deuda declarada) = 0,593 m.
        const double physM  = 0.010 + 5.0 / 60.0 + 0.50;
        const double groundE = surfE + floorM;      // lo que `validateMove` usara como suelo

        { const glm::dvec3 ok = glm::dvec3(dirE) * (groundE - physM * 0.5);
          auto s = mkEarth(ok, ok - stepE);
          CHECK(m->validateMove(zoneE, &s, &we) == 1,
                "Tierra: jugador DENTRO del techo de divergencia fisica → ACEPTADO (no es noclip)"); }
        { const glm::dvec3 deep = glm::dvec3(dirE) * (groundE - 300.0);
          auto s = mkEarth(deep, deep - stepE);
          CHECK(m->validateMove(zoneE, &s, &we) == 0,
                "Tierra: noclip real (300 m bajo el suelo) → RECHAZADO (el minimo local no lo tapa)"); }
        // ⚠️ CONTRAPRUEBA DE LA FRONTERA. Sin ella, "acepta a -0,3 m" no distingue un umbral de 0,59 m
        // de uno de 300 m: los dos aceptarian. Esto fija que el umbral es el de `Tolerance` y no otro.
        { const glm::dvec3 just = glm::dvec3(dirE) * (groundE - physM * 3.0);
          auto s = mkEarth(just, just - stepE);
          CHECK(m->validateMove(zoneE, &s, &we) == 0,
                "Tierra: al TRIPLE del techo fisico → RECHAZADO (el umbral es `Tolerance`, no una holgura suelta)"); }
        // Y que el mundo de este bloque es el que se cree que es (o que se sepa que no lo es).
        CHECK(reliefM >= 0.0, "Tierra: el relieve del analitico queda MEDIDO, no supuesto");
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

// ================================================================================================
// Golden wire-format / layout (P0 del plan §4.6). Fija el layout de las estructuras que VIAJAN por la
// red para que un cambio no rompa en silencio clientes/servidores en producción. Compile-time
// (static_assert) + runtime (CHECK), así falla tanto al compilar como al ejecutar el banco.
//
// POR QUÉ importa: añadir campos a ServerMetrics, EntityTransfer o GhostDelta CAMBIA el wire format;
// recompilar pack/unpack y regenerar libdgs_client.a (decisión D-D) es parte del mismo cambio. Este
// test hace audible cualquier alteración. NO se enlazan funciones del lib contra el .a antiguo (que
// quedó desincronizado): solo se usan tipos, sizeof y offsetof del header → es seguro.
// ================================================================================================
void test_dgs_wire_format() {
    beginTest("dgs_wire_format");

    // --- Constantes de cabecera (reemplazan los mágicos 16/4096, §4.6) ---------------------------
    static_assert(DGS::MAX_ADDR_LEN     == 16,    "MAX_ADDR_LEN cambia el layout de addr[]");
    static_assert(DGS::MAX_ENTITY_DATA  == 4096,  "MAX_ENTITY_DATA debe seguir en 4096");
    static_assert(DGS::MAX_PACKET_SIZE  == 65536, "cap de paquete sigue en 64 KiB (bug 6)");
    static_assert(DGS::DEFAULT_LEASE_MS == 30000, "lease por defecto");
    CHECK(DGS::MAX_ADDR_LEN == 16 && DGS::MAX_ENTITY_DATA == 4096,
          "constantes de layout coherentes (mágicos extraídos)");

    // --- ZoneInfo : ZoneInfoPublic: añadir `fd` heredando NO debe desplazar el layout de red ------
    // `ZoneInfo` no es standard-layout (base con miembros + miembro propio) → mejor calculamos los
    // desplazamientos sobre un objeto REAL en runtime (válido y sin -Winvalid-offsetof). La herencia
    // pública pone addr/port en MISMAS posiciones que la base; comparamos con ZoneInfoPublic, que sí es
    // standard-layout (offsetof válido).
    static_assert(offsetof(DGS::ZoneInfoPublic, port) == offsetof(DGS::ZoneInfoPublic, addr) + DGS::MAX_ADDR_LEN,
                  "port va justo al final de addr[] en ZoneInfoPublic (wire)");
    static_assert(offsetof(DGS::ZoneInfoPublic, addr) == 24,
                  "ZoneInfoPublic: 6 int32 (bounds) antes de addr[]");
    {
        DGS::ZoneInfo z{};
        DGS::ZoneInfoPublic pub{};
        const auto offAddr = (size_t)((const char*)&z.addr  - (const char*)&z);
        const auto offPort = (size_t)((const char*)&z.port  - (const char*)&z);
        const auto pubAddr = (size_t)((const char*)&pub.addr - (const char*)&pub);
        const auto pubPort = (size_t)((const char*)&pub.port - (const char*)&pub);
        CHECK(offAddr == pubAddr && offPort == pubPort && offPort == offAddr + DGS::MAX_ADDR_LEN,
              "ZoneInfo añade fd sin desplazar addr/port → el @:port que viaja es idéntico a la base");
    }

    // --- EntityTransfer (PKT_ENTITY_TRANSFER): el estado que viaja entre nodos --------------------
    static_assert(offsetof(DGS::EntityTransfer, pos) == sizeof(uint32_t),
                  "uuid (4 B) va primero; no debe moverse pos");
    static_assert(offsetof(DGS::EntityTransfer, data) + DGS::MAX_ENTITY_DATA <= sizeof(DGS::EntityTransfer),
                  "EntityTransfer.data[] (4096) cabe entero en el struct");
    CHECK(sizeof(DGS::EntityTransfer) - offsetof(DGS::EntityTransfer, data) >= DGS::MAX_ENTITY_DATA,
          "EntityTransfer conserva espacio para MAX_ENTITY_DATA (payload opaco)");

    // --- GhostDelta (PKT_GHOST_DELTA): alineado a 16 de verdad (el alignas va tras struct) --------
    static_assert(alignof(DGS::GhostDelta) == 16, "GhostDelta debe quedar alineado a 16");
    static_assert(offsetof(DGS::GhostDelta, data) + DGS::MAX_GHOST_DATA <= sizeof(DGS::GhostDelta),
                  "GhostDelta.data[] (4096) cabe entero (no se le ha robado espacio)");
    CHECK(alignof(DGS::GhostDelta) == 16 &&
          sizeof(DGS::GhostDelta) - offsetof(DGS::GhostDelta, data) >= DGS::MAX_GHOST_DATA,
          "GhostDelta alineado a 16 y con su payload opaco íntegro");

    // --- ServerMetrics: campos NUEVOS que P0 añadió. Van AL FINAL (no tocar los existentes). -------
    // ⚠️ Este layout cambió ⇒ recompilar pack/unpack y regenerar libdgs_client.a (D-D). Contiene un
    // `ZoneInfo node` → no es standard-layout → calculamos los desplazamientos en runtime sobre un
    // objeto real (sin -Winvalid-offsetof) y fijamos los invariantes de ESA decisión.
    {
        DGS::ServerMetrics m{};
        const auto base   = (const char*)&m;
        const auto perf   = (size_t)((const char*)&m.performance   - base);
        const auto start  = (size_t)((const char*)&m.startTimeS     - base);
        const auto tx     = (size_t)((const char*)&m.bytesTx        - base);
        const auto fail   = (size_t)((const char*)&m.failedTransfers- base);
        const auto active = (size_t)((const char*)&m.activeEntities - base);
        CHECK(start  > perf, "ServerMetrics: startTimeS va DESPUÉS de ramUsage/performance (§4.6 bug 5)");
        CHECK(tx     > start, "ServerMetrics: bytesTx va tras startTimeS");
        CHECK(fail   > tx,    "ServerMetrics: failedTransfers va tras los contadores de bytes");
        CHECK(active > fail,  "ServerMetrics: activeEntities es el ÚLTIMO campo añadido (heurística P3)");
    }
    static_assert(sizeof(uint64_t) == 8 && sizeof(uint32_t) == 4,
                  "contadores: 64-bit para bytes, 32-bit para transferencias/entidades");

    // --- Structs P4/§3.9 que VIAJAN por el wire: layout estable (misma técnica: solo tipos/offsetof) --
    static_assert(sizeof(DGS::ZoneLifecycle) == 8, "ZoneLifecycle: requestId(4)+ack(1) alineado a 8");
    static_assert(offsetof(DGS::ZoneLifecycle, ack) == 4, "ZoneLifecycle.ack va justo tras requestId");
    static_assert(offsetof(DGS::ZoneRegion, size) == 16, "ZoneRegion: ancla(12)+srcZone(4) antes de size");
    static_assert(offsetof(DGS::ZoneRegion, data) == 20, "ZoneRegion.data[] empieza en el byte 20");
    static_assert(offsetof(DGS::ZoneRegion, data) + DGS::MAX_REGION_BYTES == sizeof(DGS::ZoneRegion),
                  "ZoneRegion.data[] ocupa el resto del struct (payload opaco íntegro)");
    static_assert(offsetof(DGS::EntityReassign, toZone) == 24, "EntityReassign: 3x int32 + 2x uint32");
    static_assert(offsetof(DGS::ValidateAck, weight) == 6, "ValidateAck: requestId(4)+verdict(1)+weight(2)");
    CHECK(sizeof(DGS::ZoneLifecycle) == 8, "ZoneLifecycle son 8 bytes en el wire");
    CHECK(sizeof(DGS::ZoneRegion) - offsetof(DGS::ZoneRegion, data) >= DGS::MAX_REGION_BYTES,
          "ZoneRegion conserva su blob de región íntegro (§3.9)");

    // Informe de tamaños (diagnóstico al correr).
    std::printf("    sizes: EntityTransfer=%zu GhostDelta=%zu ZoneInfo=%zu ServerMetrics=%zu\n",
                sizeof(DGS::EntityTransfer), sizeof(DGS::GhostDelta),
                sizeof(DGS::ZoneInfo), sizeof(DGS::ServerMetrics));
}

// ================================================================================================
// Batería SOBRE CABECERAS (sin .a / sin red): framing TCP (§4.6 bug 6), evicción de zonas (§4.6 bug 1),
// decisión de escalado (§4.3) y fuzz de robustez de `Packet`. Todo lo que toca son headers puros →
// compila y corre sin regenerar libdgs_client.a (D-D), cubriendo en el SDK local lo que §4.3 pedía
// verificar aquí y el resto queda en tests/wire_test.cpp del repo real.
// ================================================================================================
namespace {

#if defined(DGS_TEST_HAS_ORCHESTRATOR)
DGS::ServerMetrics makeMetrics(int fd, uint64_t rx, uint64_t tx, uint32_t failed,
                               float ram, float perf, uint64_t startS = 1)
{
    DGS::ServerMetrics m{};
    m.node.fd = fd;
    m.node.chunkXMin = 0; m.node.chunkXMax = 15;
    m.node.chunkYMin = 0; m.node.chunkYMax = 15;
    m.node.chunkZMin = 0; m.node.chunkZMax = 15;
    std::strncpy(m.node.addr, "10.0.0.1", DGS::MAX_ADDR_LEN - 1);
    m.node.addr[DGS::MAX_ADDR_LEN - 1] = '\0';
    m.node.port = 30300 + fd;
    m.bytesRx = rx; m.bytesTx = tx;
    m.failedTransfers = failed;
    m.ramUsage = ram; m.performance = perf;
    m.startTimeS = startS;
    m.activeEntities = 0;
    return m;
}
#endif // DGS_TEST_HAS_ORCHESTRATOR

} // namespace

void test_dgs_robust() {
    beginTest("dgs_robust");

    // ============================ framing TCP (bug 6) ===========================================
    {
        // (1) toFramed() = [len:4][payload], y el tamano cuadra.
        DGS::Packet p; p.pack(DGS::PKT_CHAT);
        p.writeString("hola");
        auto framed = p.toFramed();
        CHECK(framed.size() == p.getSize() + 4, "toFramed anade el prefijo de 4 bytes [len:4]");
        CHECK(!DGS::receivedWasTruncated((int)framed.size(), framed.size() + 64),
              "un datagrama mas pequeno que el buffer NUNCA se considera truncado (helper bug 6)");
        CHECK(DGS::receivedWasTruncated((int)framed.size(), framed.size() - 1),
              "un frame que LLENA el buffer se trata como truncado (bug 6)");
        CHECK(DGS::receivedWasTruncated(-1, 64), "un recv con error es truncacion (no hay datos)");

        // (2) Un recv CONCATENADO (2 paquetes en un solo read) → se entregan los 2 íntegros.
        DGS::Packet pk1; pk1.pack(DGS::PKT_CHAT); pk1.writeString("uno");
        DGS::Packet pk2; pk2.pack(DGS::PKT_METRICS); pk2.write<uint32_t>(7);
        auto f1 = pk1.toFramed(); auto f2 = pk2.toFramed();
        std::vector<uint8_t> concat(f1.begin(), f1.end());
        concat.insert(concat.end(), f2.begin(), f2.end());

        DGS::PacketFramer fr;
        fr.feed(concat.data(), concat.size());
        std::vector<uint8_t> out;
        const bool got1 = fr.next(out);
        CHECK(got1, "primer frame extraido de un read que traia DOS paquetes");
        if (got1) { DGS::Packet rp; rp.setBuffer(out.data(), out.size());
                    CHECK(rp.getType() == DGS::PKT_CHAT, "primer paquete == PKT_CHAT (payload intacto)"); }
        const bool got2 = fr.next(out);
        CHECK(got2, "segundo frame extraido del MISMO read sin datos extra perdidos");
        if (got2) { DGS::Packet rp; rp.setBuffer(out.data(), out.size());
                    CHECK(rp.getType() == DGS::PKT_METRICS, "segundo paquete == PKT_METRICS"); }
        CHECK(!fr.next(out), "no quedan frames tras consumir ambos");

        // (3) Un recv PARTIDO (medio frame en un read, el resto en el siguiente) → solo se entrega al
        //     completarse. Es el caso real de TCP fragmentado.
        DGS::PacketFramer fr2;
        const size_t cut = framed.size() / 2;
        fr2.feed(framed.data(), cut);
        CHECK(!fr2.next(out), "media cabecera+payload no se entrega como paquete (espera el resto)");
        CHECK(fr2.buffered() == cut, "el trozo se conserva entre recvs");
        fr2.feed(framed.data() + cut, framed.size() - cut);
        CHECK(fr2.next(out), "al completar el trozo, el frame se entrega entero");
        CHECK(!fr2.next(out) || fr2.buffered() == 0, "tras el frame no quedan bytes en el acumulador");

        // (4) Cabecera MENTIROSA (len 0 o > max): un frame corrupto NUNCA se entrega como bueno y la pérdida
        //     se cuenta (→ failedTransfers). Un stream sano (tras reset, como una reconexión) se
        //     parsea intacto: la corrupción no envenena de forma permanente.
        {
            DGS::PacketFramer fr4;
            auto good = p.toFramed();
            // Simula un frame cuyo prefijo de largo se corrompe: 0x0FFFFFFF (~268M) > MAX_PACKET_SIZE.
            std::vector<uint8_t> corrupt = good;
            corrupt[0] = 0xFF; corrupt[1] = 0xFF; corrupt[2] = 0xFF; corrupt[3] = 0x0F;

            // (4a) Solo el frame corrupto: no se entrega como paquete y se cuenta la pérdida.
            fr4.feed(corrupt.data(), corrupt.size());
            CHECK(!fr4.next(out) || fr4.discards() > 0,
                  "un frame con largo mentiroso nunca se entrega como si fuera bueno");
            CHECK(fr4.discards() > 0, "el largo corrupto incrementa discards (failedTransfers++)");

            // (4b) Reset (reconexión / nuevo stream) → el tráfico sano se parsea intacto.
            fr4.clear();
            fr4.feed(good.data(), good.size());
            const bool okReset = fr4.next(out);
            CHECK(okReset, "tras reset, el trafico sano se parsea entero");
            if (okReset) { DGS::Packet rp; rp.setBuffer(out.data(), out.size());
                           CHECK(rp.getType() == DGS::PKT_CHAT, "frame bueno llega intacto"); }

            // (4c) RE-SINCRONIZACIÓN EN MITAD DE STREAM (caso TCP real, sin clear()): un frame corrupto
            //      "envenenado" (prefijo > MAX) NO debe impedir que los frames buenos que vienen DETRÁS se
            //      entreguen. El acumulador descarta byte a byte hasta el primer prefijo válido, cuenta la
            //      pérdida en discards, y sigue — sin reset. Usamos payload de 0xFF para que CUALQUIER
            //      lectura desplazada del veneno dé un largo > MAX (todos descartes, sin falsos frames).
            {
                DGS::PacketFramer fr5;
                std::vector<uint8_t> poison { 0xFF, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF };
                // prefijo 0x0FFFFFFF (>MAX) + 4 bytes de "payload" a 0xFF.

                DGS::Packet g1; g1.pack(DGS::PKT_CHAT);   g1.writeString("uno");
                DGS::Packet g2; g2.pack(DGS::PKT_METRICS); g2.write<uint32_t>(9);
                auto fg1 = g1.toFramed(); auto fg2 = g2.toFramed();

                std::vector<uint8_t> stream(poison.begin(), poison.end());
                stream.insert(stream.end(), fg1.begin(), fg1.end());
                stream.insert(stream.end(), fg2.begin(), fg2.end());
                fr5.feed(stream.data(), stream.size());

                // Recorremos todo lo que entregue; LOS 2 buenos deben salir, SIN basura de por medio,
                // y la pérdida de los 8 bytes venenosos contarse entera.
                bool got1 = false, got2 = false; int frames = 0;
                while (fr5.next(out)) {
                    ++frames;
                    DGS::Packet rp; rp.setBuffer(out.data(), out.size());
                    if (rp.getType() == DGS::PKT_CHAT)    got1 = true;
                    if (rp.getType() == DGS::PKT_METRICS) got2 = true;
                }
                CHECK(fr5.discards() == 8, "se descartan EXACTOS los 8 bytes del frame corrupto al resincronizar");
                CHECK(got1 && got2, "tras la corrupcion, los frames BUENOS detras se entregan (resync sin clear())");
                CHECK(frames == 2, "solo se entregan los frames buenos, sin basura falsa del veneno");
            }
        }

        // (5) Cap de UDP (bug 6): el recibidor decide TRUNCAR cuando el datagrama llena el buffer, y no
        //     parsear bytes basura.
        DGS::Packet big; big.write<uint8_t>(0x42);
        auto bigFrame = big.toFramed();
        CHECK(DGS::receivedWasTruncated((int)bigFrame.size(), bigFrame.size() - 1),
              "un datagrama que llena el buffer se trata como truncado (se descarta, no se parsea)");
        CHECK(!DGS::receivedWasTruncated((int)bigFrame.size(), bigFrame.size() + 1),
              "un datagrama mas pequeno que el buffer NO es truncacion");
        CHECK(DGS::receivedWasTruncated(-1, 64), "un recv con error es truncacion (no hay datos)");
    }

    // ============================ evicción de zonas muertas (bug 1) ==============================
#if defined(DGS_TEST_HAS_ORCHESTRATOR)
    {
        DGS::Orchestrator o;   // ctor sin red (test)
        DGS::ServerMetrics m1 = makeMetrics(1, 100, 100, 0, 0.3f, 0.9f);
        DGS::ServerMetrics m2 = makeMetrics(2, 100, 100, 0, 0.3f, 0.9f);
        o.updateNodeTopology(1, m1);
        o.updateNodeTopology(2, m2);
        CHECK(o.activeZones.size() == 2, "dos nodos vivos se registran");

        // Dos zonas responden a sus chunks (antes de evicción).
        CHECK(o.findTargetNode(5, 5, 5) != -1, "nodo vivo sirve su rango de chunks");

        // Simular un nodo que DEJA de reportar (m1) → su lease vence.
        auto& ls = o.lastSeen;
        ls[1] = std::chrono::steady_clock::now() - std::chrono::seconds(DGS::DEFAULT_LEASE_MS / 1000 + 1);

        // (la evicción es por zona; apostamos que al menos un nodo se evicta) → ejecutar síncrono
        auto evicted = o.evictStaleZones();
        CHECK(!evicted.empty(), "zona cuyo lease vencio es evictada");
        // findTargetNode ya encuentra solo nodos vivos: si el evictado cubria ese punto…
        CHECK(o.findTargetNode(-100, -100, -100) == -1, "chunk fuera de TODAS las zonas → -1");
    }

    // ============================ decisión de escalado (dryRun, §4.3) ============================
    {
        DGS::Orchestrator o; o.dryRun = true;

        // (1) Servidor sano y ocioso → ninguna decisión. La PRIMERA métrica solo fija el baseline EWMA.
        DGS::ServerMetrics healthy = makeMetrics(1, 1000, 500, 0, 0.20f, 0.95f);
        o.evaluateServer(healthy, 1);
        CHECK(o.scaleEvents == 0, "baseline inicial no decide");
        DGS::ServerMetrics healthy2 = makeMetrics(1, 2000, 1000, 0, 0.20f, 0.95f);  // simétrico, baja carga
        o.evaluateServer(healthy2, 1);
        CHECK(o.scaleEvents == 0, "nodo sano y balanceado NO escala");

        // (2) `failedTransfers` alto (> EVAL_FAIL_THRESH=40) → escala (señal failureProne).
        DGS::Orchestrator o2; o2.dryRun = true;
        DGS::ServerMetrics f1 = makeMetrics(2, 1000, 500, 0, 0.3f, 0.9f);
        o2.evaluateServer(f1, 2);                        // baseline
        DGS::ServerMetrics f2 = makeMetrics(2, 2000, 1000, 60, 0.3f, 0.9f);  // 60 fallos
        o2.evaluateServer(f2, 2);
        CHECK(o2.scaleEvents == 1, "failedTransfers alto dispara el escalado");

        // (3) Asimetría de red Tx>>Rx (EWMA) → escala (señal netSaturated).
        DGS::Orchestrator o3; o3.dryRun = true;
        DGS::ServerMetrics n1 = makeMetrics(3, 1000, 1000, 0, 0.3f, 0.9f);
        o3.evaluateServer(n1, 3);                        // baseline 1:1
        DGS::ServerMetrics n2 = makeMetrics(3, 1001, 9000, 0, 0.3f, 0.9f);  // manda ~9x mas de lo que recibe
        o3.evaluateServer(n2, 3);
        CHECK(o3.scaleEvents == 1, "asimetria de red Tx>>Rx dispara el escalado");

        // (4) Carga combinada ram>0.8 y perf<0.36 → escala.
        DGS::Orchestrator o4; o4.dryRun = true;
        DGS::ServerMetrics c1 = makeMetrics(4, 1000, 500, 0, 0.85f, 0.20f);
        o4.evaluateServer(c1, 4);
        // baseline; la misma zona no re-arma baseline para seguir midiendo → segunda muestra en carga
        DGS::ServerMetrics c2 = makeMetrics(4, 2000, 1000, 0, 0.85f, 0.20f);
        o4.evaluateServer(c2, 4);
        CHECK(o4.scaleEvents == 1, "carga alta (ram>0.8 y perf<0.36) dispara el escalado");

        // (5) Zona demasiado pequena (width<1) NO escala aunque haya fallos — protege el split mín.
        DGS::Orchestrator o5; o5.dryRun = true;
        DGS::ServerMetrics s1 = makeMetrics(5, 1000, 500, 0, 0.3f, 0.9f);
        s1.node.chunkXMax = s1.node.chunkXMin;          // width 0
        o5.evaluateServer(s1, 5);
        DGS::ServerMetrics s2 = s1; s2.failedTransfers = 99; s2.bytesRx = 2000; s2.bytesTx = 1000;
        o5.evaluateServer(s2, 5);
        CHECK(o5.scaleEvents == 0, "zona indivisible (width<1) nunca escala aunque falle");

        // (6) Cooldown: tras una decisión, el mismo nodo no vuelve a escalar hasta EVAL_COOLDOWN_S (30s).
        DGS::Orchestrator o6; o6.dryRun = true;
        DGS::ServerMetrics d1 = makeMetrics(6, 1000, 500, 0, 0.3f, 0.9f);
        o6.evaluateServer(d1, 6);
        DGS::ServerMetrics d2 = makeMetrics(6, 2000, 1000, 60, 0.3f, 0.9f);
        o6.evaluateServer(d2, 6);
        DGS::ServerMetrics d3 = makeMetrics(6, 3000, 1500, 60, 0.3f, 0.9f);
        o6.evaluateServer(d3, 6);                       // dentro del cooldown → no suma
        CHECK(o6.scaleEvents == 1, "cooldown de 30s evita re-escalar al mismo nodo seguido");
    }
#endif // DGS_TEST_HAS_ORCHESTRATOR

    // ============================ robustez de Packet.read (fuzz ligero) ==========================
    {
        // Un payload truncado/aleatorio debe lanzar runtime_error LIMPIO (el §4.6 bug 6 evitaba que
        // ese error tumbara el hilo); nunca enmascarar ni devolver datos basura.
        std::mt19937 rng(42);
        int threwClean = 0;
        for (int i = 0; i < 200; ++i) {
            DGS::Packet zp;
            size_t n = rng() % 64;
            std::vector<uint8_t> junk(n);
            for (auto& b : junk) b = (uint8_t)(rng() & 0xFF);
            zp.setBuffer(junk.data(), junk.size());
            try {
                (void)zp.readString();   // puede lanzar (size>lo que queda) → esperado
                (void)zp.read<uint64_t>();
            } catch (const std::runtime_error&) { ++threwClean; }
            catch (...) { CHECK(false, "solo runtime_error (sin basura) — no otra excepción"); }
        }
        CHECK(threwClean <= 200, "read sobre buffers truncados nunca rompe (solo lanza limpio o devuelve)");

        // Y un over-read explícito SÍ lanza, no se sale del vector.
        DGS::Packet tiny; tiny.write<uint8_t>(1);
        bool threw = false;
        try { (void)tiny.read<uint32_t>(); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw, "leer mas de lo que hay lanza runtime_error (no UB)");

        // --- fuzz de readRaw / readString con TAMAÑOS MENTIROSOS (§4.3) ---------------------------
        // readRaw(dest, n>lo que queda) y readString (size declarado>lo que cabe) deben lanzar
        // runtime_error LIMPIO, nunca escribir en `dest` ni leer fuera del vector.
        {
            uint8_t sink[512];
            std::memset(sink, 0xAB, sizeof(sink));
            DGS::Packet small; small.write<uint16_t>(7);   // solo 2 bytes reales
            bool threwRaw = false, threwStr = false, clobbered = false;
            try { small.readRaw(sink, 512); } catch (const std::runtime_error&) { threwRaw = true; }
            // readRaw debe comprobar ANTES de memcpy → nada de `sink` se ha tocado.
            for (size_t i = 0; i < sizeof(sink); ++i) if (sink[i] != 0xAB) clobbered = true;
            CHECK(threwRaw, "readRaw pidiendo mas de lo que queda lanza runtime_error");
            CHECK(!clobbered, "readRaw no escribe en dest cuando el tamaño miente (overflow limpio)");

            // readString con prefijo de TAMANO EXAGERADO que reclama mas bytes de los que hay.
            DGS::Packet lying;
            lying.write<uint16_t>((uint16_t)0xFF00);       // reclama ~65k, pero solo hay 1 byte despues
            lying.write<uint8_t>(0x01);
            try { (void)lying.readString(); } catch (const std::runtime_error&) { threwStr = true; }
            CHECK(threwStr, "readString con tamaño mentiroso (>lo que queda) lanza runtime_error");
        }
    }
}

// ================================================================================================
// EL SERVIDOR SIMULA, Y SIMULA LO MISMO QUE EL CLIENTE.
//
// ⚠️ HASTA HOY `GameModule::step` IBA A NULO: la zona no avanzaba nada y el mundo era lo que el
// cliente dijera; el servidor solo respondia si o no, y ademas juzgaba contra el sampler ANALITICO
// mientras el cliente camina sobre la MALLA. Su propia nota mide el precio de esa asimetria: la malla
// llega a quedar 5,12 m por debajo, el margen fijo era 5,0 m, y **se expulsaba a jugadores legitimos**.
//
// Con Jolt en `step` el servidor pisa la misma superficie. Esto lo comprueba en los dos sentidos que
// importan para un validador:
//
//   (1) que SIMULA — un cuerpo soltado en el aire cae y se posa, no se queda flotando;
//   (2) que simula LO MISMO — el resultado del modulo cargado por `dlopen` coincide con el del motor
//       corriendo la misma caida. Eso es lo que permite bajar el umbral de trampa de metros a
//       milimetros: no "que coincidan bit a bit", sino cuanto puede mentir un cliente sin que se note.
// ================================================================================================
void test_dgs_server_simulates() {
    beginTest("dgs_server_simulates");
    void* h = dlopen("./libharuka_rules.so", RTLD_NOW);
    // Bajo CTest el directorio de trabajo es `build/` y acierta la primera; lanzado a mano desde la
    // raiz del repo acierta esta. Sin ella hay que COPIAR el .so al arbol de FUENTES —lo que hace la
    // CI— y eso deja un artefacto de compilacion dentro del codigo.
    if (!h) h = dlopen("./build/libharuka_rules.so", RTLD_NOW);
    if (!h) h = dlopen("libharuka_rules.so", RTLD_NOW);
    CHECK(h != nullptr, "carga libharuka_rules.so");
    if (!h) return;
    auto entry = (const DGS::GameModule* (*)())dlsym(h, "dgs_game_module_v1");
    if (!entry) { CHECK(false, "dlsym"); dlclose(h); return; }
    const DGS::GameModule* m = entry();
    CHECK(m && m->step != nullptr,
          "el modulo aporta `step`: la zona SIMULA (estaba a NULO — el mundo iba por el cliente)");
    if (!m || !m->step || !m->createZone) { dlclose(h); return; }

    // Planeta pequeno, como el resto de tests del modulo: a esa escala el `pos[]` float del
    // `EntityTransfer` tiene precision de sobra y la caida es corta.
    const double R = 5000.0;
    DGS::WorldQuery w{};
    w.chunkSizeX = w.chunkSizeY = w.chunkSizeZ = 1.0f;   // pos[] ya en metros
    w.planetCenter[0] = w.planetCenter[1] = w.planetCenter[2] = 0.0;
    w.planetRadius = R; w.seed = 1234u; w.reliefStrength = 1.0f; w.profile = 0;

    DGS::ZoneHandle z = m->createZone(&w);
    CHECK(z != nullptr, "la zona se crea con su simulacion");
    if (!z) { dlclose(h); return; }

    const glm::dvec3 dir = glm::normalize(glm::dvec3(0.3, 0.9, 0.2));
    // ⚠️ LA CAIDA SE ELIGE CON LA GRAVEDAD DE ESTE PLANETA, NO A OJO. Un cuerpo de 5 km de radio con
    // densidad media terrestre da **g = 0,0077 m/s2** (`G·M/R²` con `M = rho·4/3·pi·R³`), o sea 1 300
    // veces menos que la Tierra. La primera version soltaba desde 40 m y esperaba que se posara en
    // 10 s: cayo 0,32 m — que es EXACTAMENTE lo que predice `½gt²`, o sea que la fisica estaba bien y
    // la cuenta mia mal. Desde 3 m y con 50 s, `½gt²` = 9,6 m: llega de sobra.
    const glm::dvec3 p0  = dir * (R + 3.0);

    DGS::EntityTransfer e{};
    e.uuid = 7u;
    e.chunkX = e.chunkY = e.chunkZ = 0;
    e.pos[0] = (float)p0.x; e.pos[1] = (float)p0.y; e.pos[2] = (float)p0.z;

    const double r0 = glm::length(glm::dvec3(e.pos[0], e.pos[1], e.pos[2]));
    const int    kTicks = 3000;   // 50 s
    for (int i = 0; i < kTicks; ++i) m->step(z, &e, 1.0f / 60.0f, &w);
    const glm::dvec3 pS(e.pos[0], e.pos[1], e.pos[2]);
    const double rS = glm::length(pS);

    // ── EL MISMO ESCENARIO, EN EL MOTOR ─────────────────────────────────────────────────────────
    //
    // No es un gemelo escrito a mano: es LA MISMA clase (`PhysicsEngine`) sobre el MISMO proveedor de
    // mundo que usa el modulo. Si esto y lo de arriba se separaran, el servidor no estaria corriendo
    // la fisica del cliente por mucho que compartan el binario.
    double rC = 0.0;
    {
        struct W : Haruka::Physics::IWorldProvider {
            glm::dvec3 c{0.0}; double R = 0.0;
            Haruka::WorldGenParams params{};
            std::vector<Haruka::Physics::GravBody> grav;
            bool       hasActivePlanet()    const override { return true; }
            glm::dvec3 activePlanetCenter() const override { return c; }
            double     activePlanetRadius() const override { return R; }
            const std::vector<Haruka::Physics::GravBody>& gravBodies() const override { return grav; }
            double     terrainHeightAt(const glm::dvec3& p) const override {
                const glm::dvec3 d = p - c; const double l = glm::length(d);
                if (l < 1e-6) return 0.0;
                return (double)Haruka::sampleTerrainV2(glm::vec3(d / l), params, R).elevKm * 1000.0;
            }
        } wc;
        wc.R = R; wc.params = Haruka::deriveWorldParams(1234u, R);
        const double mass = Haruka::Planet::kEarthMeanDensity * (4.0 / 3.0)
                          * 3.14159265358979323846 * R * R * R;
        wc.grav.push_back({ wc.c, mass, R, 0.0, -1 });

        Haruka::Physics::PhysicsEngine eng; eng.setWorldProvider(&wc);
        auto b = std::make_shared<Haruka::Physics::RigidBody>();
        b->position = p0; b->radius = 0.5; b->mass = 70.0; b->name = "e7";
        eng.addBody(b);
        for (int i = 0; i < kTicks; ++i) eng.advance(1.0 / 60.0);
        rC = glm::length(b->position);
    }

    const double terrain = R + (double)Haruka::sampleTerrainV2(
                                   glm::vec3(dir), Haruka::deriveWorldParams(1234u, R), R).elevKm * 1000.0;
    std::printf("    soltado a %.2f m del centro (suelo analitico en %.2f m)\n", r0, terrain);
    std::printf("    tras %d ticks: SERVIDOR %.4f m · CLIENTE %.4f m · diferencia %.6f m\n",
                kTicks, rS, rC, std::fabs(rS - rC));

    // (1) SIMULA: el cuerpo cae de verdad. Sin esto, `step` podria estar devolviendo lo que entro.
    CHECK(r0 - rS > 2.0, "el servidor SIMULA: el cuerpo cae, no se queda donde el cliente lo puso");
    // (2) Y SE POSA, no atraviesa el planeta ni sale despedido.
    CHECK(rS > terrain - 2.0 && rS < terrain + 2.0,
          "y se POSA sobre el terreno analitico, ni lo atraviesa ni rebota fuera");
    // (3) LO MISMO QUE EL CLIENTE. Es la afirmacion que sostiene todo el diseno: si el servidor
    //     simulara otra cosa, corregir seria pelear con el cliente en vez de juzgarlo.
    CHECK(std::fabs(rS - rC) < 0.05,
          "el servidor simula LO MISMO que el cliente (misma clase, mismo proveedor de mundo)");
    m->destroyZone(z);
    dlclose(h);
}

// ---------------------------------------- TEST: el servidor pisa EL MISMO SUELO que el cliente ---
// ⚠️ EL AGUJERO QUE CIERRA. `dgs_rules_module` decia probar el falso positivo del suavizado de malla
// en un planeta tamaño Tierra, y su mundo NO TENIA RELIEVE: `Haruka::sampleTerrainV2` es un stub en
// esta rama (`terrain sampler removed`, devuelve 0). Medido, 512 direcciones: relieve +0,00..+0,00 m,
// y la `FloorCache` acotando 0,0000 m. O sea que el validador decidia "estas atravesando el suelo"
// comparando contra una ESFERA LISA a nivel del mar, mientras el cliente camina sobre la rejilla
// horneada del `TerrestrialPlanet`. El test pasaba por una constante de 2 m que no nombraba.
//
// Aqui se le entrega al modulo el MISMO campo horneado (`haruka_rules_set_height_field`) y se
// comprueba lo unico que importa: que el suelo del servidor ES el del cliente, y que con el deja de
// equivocarse en los dos sentidos.
//
// ⚠️ CON CONTRAPRUEBA EN LOS DOS SENTIDOS, porque sin ella esto no distinguiria un validador que
// mira el campo de uno que acepta todo:
//   · un jugador de pie en un VALLE (bajo el radio de referencia) → ACEPTADO con campo, y se
//     comprueba que SIN campo el mismo jugador se expulsa. Ese es el falso positivo, medido.
//   · un jugador dentro de una MONTAÑA (sobre el radio, pero bajo el terreno) → RECHAZADO con campo,
//     y se comprueba que SIN campo pasa por bueno. Ese es el noclip que hoy se cuela.
void test_dgs_ground_matches_engine() {
    beginTest("dgs_ground_matches_engine");
    void* h = dlopen("./libharuka_rules.so", RTLD_NOW);
    // Bajo CTest el directorio de trabajo es `build/` y acierta la primera; lanzado a mano desde la
    // raiz del repo acierta esta. Sin ella hay que COPIAR el .so al arbol de FUENTES —lo que hace la
    // CI— y eso deja un artefacto de compilacion dentro del codigo.
    if (!h) h = dlopen("./build/libharuka_rules.so", RTLD_NOW);
    if (!h) h = dlopen("libharuka_rules.so", RTLD_NOW);
    CHECK(h != nullptr, "carga libharuka_rules.so");
    if (!h) return;
    auto entry = (const DGS::GameModule* (*)())dlsym(h, "dgs_game_module_v1");
    auto setField = (int (*)(int, int, const float*, double))dlsym(h, "haruka_rules_set_height_field");
    auto loadField = (int (*)(const char*))dlsym(h, "haruka_rules_load_height_field");
    CHECK(entry && setField && loadField,
          "el modulo exporta el canal del suelo (simbolos ADICIONALES, sin tocar el ABI del DGS)");
    if (!entry || !setField || !loadField) { dlclose(h); return; }
    const DGS::GameModule* m = entry();
    if (!m || !m->createZone || !m->validateMove) { dlclose(h); return; }

    // Un campo de altura SINTETICO pero con la forma del real: una equirect 2:1 con un valle y una
    // montaña de cotas conocidas. No hace falta hornear un planeta entero — lo que se prueba es que
    // el modulo muestrea ESTE dato con la MISMA cuenta que `TerrestrialPlanet::sampleHeight`.
    const double Re = 6.371e6;
    const int    W = 256, H = 128;
    std::vector<float> field((size_t)W * H, 0.0f);
    // Direcciones de prueba y las cotas que se les impone.
    const glm::dvec3 dirValle = glm::normalize(glm::dvec3( 0.3,  0.9, 0.2));
    const glm::dvec3 dirMonte = glm::normalize(glm::dvec3(-0.5,  0.1, 0.8));
    auto stamp = [&](const glm::dvec3& d, float meters) {
        // Una MESETA de 5x5 texeles, no un texel suelto: la bilineal de `sampleHeightField` mezcla
        // los cuatro vecinos, asi que un pico de un texel no da la cota que se cree que da.
        const glm::vec2 uv = Haruka::Planet::equirectUV(glm::vec3(d));
        const int cx = (int)std::floor(uv.x * W), cy = (int)std::floor(uv.y * H);
        for (int j = -2; j <= 2; ++j) for (int i = -2; i <= 2; ++i) {
            int x = ((cx + i) % W + W) % W, y = glm::clamp(cy + j, 0, H - 1);
            field[(size_t)y * W + x] = meters;
        }
    };
    stamp(dirValle, -400.0f);    // un valle 400 m BAJO el nivel del mar
    stamp(dirMonte, 2500.0f);    // una montaña de 2,5 km

    // La referencia: el MISMO muestreo que hace el motor, calculado aqui a mano con las mismas
    // funciones puras. Si el modulo se separa de esto, el suelo del servidor no es el del cliente.
    auto engineHeight = [&](const glm::dvec3& d) {
        const glm::vec2 uv = Haruka::Planet::equirectUV(glm::vec3(d));
        const float baseH = Haruka::Planet::sampleHeightField(uv, W, H, field.data());
        const float baseR = (float)Re + baseH;
        float det = Haruka::Planet::terrainDetail(glm::vec3(d), baseR, Haruka::Planet::terrainTriM(0.0))
                  * Haruka::Planet::seaLevelAttenuation(baseH);
        if (baseH > 0.0f) det = glm::max(det, -baseH);
        return (double)baseH + (double)det;
    };
    const double hValle = engineHeight(dirValle), hMonte = engineHeight(dirMonte);
    std::printf("    [suelo] valle %+.2f m · monte %+.2f m (cotas del campo horneado)\n", hValle, hMonte);
    CHECK(hValle < -300.0 && hMonte > 2000.0,
          "el campo de prueba TIENE relieve (si no, lo de abajo no probaria nada)");

    DGS::WorldQuery w{};
    w.chunkSizeX = w.chunkSizeY = w.chunkSizeZ = 1.0e6f;
    w.planetCenter[0] = w.planetCenter[1] = w.planetCenter[2] = 0.0;
    w.planetRadius = Re; w.seed = 4242u; w.reliefStrength = 1.0f; w.profile = 0;

    // ⚠️ EL `EntityTransfer` LO PONE EL LLAMANTE, no un `static` de la lambda. Con un static
    // compartido, dos `mk` seguidas dejan la PRIMERA muestra apuntando al dato de la SEGUNDA — y eso
    // ya me costo un falso fallo aqui: `okValle` validaba la posicion de la montaña.
    auto mk = [&](DGS::EntityTransfer& e, const glm::dvec3& g, const glm::dvec3& last) {
        e = DGS::EntityTransfer{};
        const double CS = 1.0e6;
        const double cx = std::floor(g.x/CS), cy = std::floor(g.y/CS), cz = std::floor(g.z/CS);
        e.chunkX = (int32_t)cx; e.chunkY = (int32_t)cy; e.chunkZ = (int32_t)cz;
        e.pos[0] = (float)(g.x - cx*CS); e.pos[1] = (float)(g.y - cy*CS); e.pos[2] = (float)(g.z - cz*CS);
        DGS::MoveSample s{}; s.now = &e;
        s.lastGX = (float)last.x; s.lastGY = (float)last.y; s.lastGZ = (float)last.z;
        s.maxSpeed = 5.0f; s.dtSeconds = 1.0f/60.0f;
        return s;
    };
    const glm::dvec3 paso = glm::normalize(glm::cross(dirValle, glm::dvec3(0,1,0))) * 0.04;
    // De pie en el fondo del valle (1 m sobre su suelo), y DENTRO de la montaña (500 m bajo su cima
    // pero MUY por encima del radio de referencia — que es lo que el validador miraba antes).
    const glm::dvec3 pieValle  = dirValle * (Re + hValle + 1.0);
    const glm::dvec3 dentroMon = dirMonte * (Re + hMonte - 500.0);

    // ── SIN CAMPO: lo que hace hoy. Es la contraprueba, no un adorno. ───────────────────────────
    setField(0, 0, nullptr, 0.0);           // desinstala
    {
        DGS::ZoneHandle z = m->createZone(&w);
        DGS::EntityTransfer ev{}, em{};
        auto sv = mk(ev, pieValle, pieValle - paso);
        auto sm = mk(em, dentroMon, dentroMon - paso);
        const int okValle = m->validateMove(z, &sv, &w);
        const int okMonte = m->validateMove(z, &sm, &w);
        std::printf("    [sin campo] valle → %s · dentro de la montaña → %s\n",
                    okValle ? "ACEPTA" : "EXPULSA", okMonte ? "ACEPTA" : "EXPULSA");
        CHECK(okValle == 0, "SIN campo: al jugador del valle se le EXPULSA (el falso positivo, medido)");
        CHECK(okMonte == 1, "SIN campo: el noclip dentro de la montaña PASA (el agujero, medido)");
        if (m->destroyZone) m->destroyZone(z);
    }

    // ── CON CAMPO: el suelo de verdad. ──────────────────────────────────────────────────────────
    CHECK(setField(W, H, field.data(), Re) == 1, "el host entrega el campo horneado al modulo");
    {
        DGS::ZoneHandle z = m->createZone(&w);
        DGS::EntityTransfer ev{}, em{};
        auto sv = mk(ev, pieValle, pieValle - paso);
        auto sm = mk(em, dentroMon, dentroMon - paso);
        const int okValle = m->validateMove(z, &sv, &w);
        const int okMonte = m->validateMove(z, &sm, &w);
        std::printf("    [con campo] valle → %s · dentro de la montaña → %s\n",
                    okValle ? "ACEPTA" : "EXPULSA", okMonte ? "ACEPTA" : "EXPULSA");
        CHECK(okValle == 1, "CON campo: el jugador del valle se ACEPTA (400 m bajo el nivel del mar)");
        CHECK(okMonte == 0, "CON campo: el noclip dentro de la montaña se RECHAZA (2 km sobre el mar)");

        // ⚠️ Y EL TERMINO DEL TRANSPORTE, medido. `MoveSample::lastG*` son FLOAT en el ABI del DGS, y
        // a radio terrestre un float tiene 0,5 m de resolucion: un jugador QUIETO parece dar un salto.
        // Se barren direcciones para dar con el peor caso, porque depende de la magnitud de cada eje.
        double peor = 0.0;
        for (int t = 0; t < 256; ++t) {
            const double u = (double)t * 2.39996322972865332;
            const double zz = 1.0 - 2.0 * ((double)t + 0.5) / 256.0;
            const double rr = std::sqrt(std::max(0.0, 1.0 - zz*zz));
            const glm::dvec3 d(rr*std::cos(u), zz, rr*std::sin(u));
            const glm::dvec3 g = d * (Re + 1000.0);
            const double CS = 1.0e6;
            const double cx = std::floor(g.x/CS), cy = std::floor(g.y/CS), cz = std::floor(g.z/CS);
            const double rx = (double)(float)(g.x - cx*CS) + cx*CS - (double)(float)g.x;
            const double ry = (double)(float)(g.y - cy*CS) + cy*CS - (double)(float)g.y;
            const double rz = (double)(float)(g.z - cz*CS) + cz*CS - (double)(float)g.z;
            peor = std::max(peor, std::sqrt(rx*rx + ry*ry + rz*rz));
        }
        const double presupuesto = 5.0/60.0 + (0.010 + 5.0/60.0 + 0.50);
        std::printf("    [transporte] un jugador QUIETO parece saltar hasta %.4f m por la cuantizacion\n"
                    "    [transporte] del float de `lastG*` · presupuesto a 5 m/s y 1/60 s: %.4f m (%.0f %%)\n",
                    peor, presupuesto, 100.0 * peor / presupuesto);
        CHECK(peor > 0.01, "la cuantizacion del ABI es MEDIBLE (si diera 0, el termino sobraria)");
        CHECK(peor < presupuesto, "y cabe en el presupuesto: no expulsa por si sola a un jugador quieto");

        if (m->destroyZone) m->destroyZone(z);
    }

    // ── El fichero: mismo campo, ida y vuelta por disco. Es lo que consume un servidor headless. ──
    {
        Haruka::Net::HeightField hf;
        hf.w = W; hf.h = H; hf.baseRadiusM = (float)Re; hf.data = field;
        const std::string path = "./test_ground.hfield";
        CHECK(Haruka::Net::saveHeightField(path, hf), "el campo se persiste en crudo (float, no PNG)");
        Haruka::Net::HeightField back;
        CHECK(Haruka::Net::loadHeightField(path, back) && back.valid(), "y se vuelve a leer");
        CHECK(back.w == W && back.h == H && back.data == field,
              "ida y vuelta por disco SIN perdida (el PNG de 16 bits cuantiza a ~1 m; esto no)");
        // Y que el modulo lo acepta por ese camino.
        CHECK(loadField(path.c_str()) == 1, "el modulo carga el campo desde el fichero del bake");
        // Y que ese campo cargado DA LO MISMO que el que se paso en memoria.
        CHECK(std::fabs(back.heightAt(dirMonte) - hMonte) < 1e-6,
              "el muestreo del modulo es el GEMELO EXACTO del motor (< 1 µm)");
        std::remove(path.c_str());
    }

    // Se deja desinstalado: los demas tests del banco esperan el comportamiento sin campo.
    setField(0, 0, nullptr, 0.0);
    dlclose(h);
}


// ================================================================================================
// test_net_entity_visible — an entity the world feed spawns must be DRAWN, not merely present.
//
// ⚠️ THIS FAILURE MODE HAS HAPPENED TWICE AND LOOKS LIKE A WORKING GAME BOTH TIMES. `classifySceneObject`
// dispatches on the `objectType` ENUM; the string `type` is a label. The first time, networked players
// were left at `UNKNOWN`: added to the scene, moved every frame, culled, counted — and never drawn.
// The second door is still open in the same wall: `ObjectType::MODEL` goes to the model renderer,
// which does `getOrLoadModelCached(obj->modelPath)` and gives up on null, so every entity the feed
// spawns that is NOT a player or an NPC arrives with an empty path and is invisible in exactly the
// same silent way.
//
// A capsule is not a nice model. It is a VISIBLE one. The counter-proof is the second half: an object
// that DOES have a model must still go to the model renderer, or this "fix" would just have made
// every model in the game a capsule.
// ================================================================================================
void test_net_entity_visible() {
    beginTest("net_entity_visible");

    // A player from the world feed: no mesh, no model, no primitive property.
    Haruka::SceneObject player;
    player.name       = "entity_8100";
    player.type       = "Character";
    player.objectType = Haruka::ObjectType::CHARACTER;
    Haruka::RenderCommand c = Haruka::classifySceneObject(player);
    CHECK(c.kind == Haruka::RenderKind::Primitive, "a networked player is drawn");
    CHECK(c.primitive == Haruka::PrimitiveType::CAPSULE, "and it is drawn as a capsule");

    // Anything else the feed spawns (an item, a vehicle) with nothing to load.
    Haruka::SceneObject thing;
    thing.name       = "entity_8101";
    thing.objectType = Haruka::ObjectType::MODEL;   // modelPath deliberately empty
    Haruka::RenderCommand t = Haruka::classifySceneObject(thing);
    CHECK(t.kind == Haruka::RenderKind::Primitive,
          "a MODEL with nothing to load is drawn anyway (it used to be silently invisible)");
    CHECK(t.primitive == Haruka::PrimitiveType::CAPSULE, "as a capsule");

    // COUNTER-PROOF: a real model must NOT be replaced by a capsule.
    Haruka::SceneObject real;
    real.name       = "crate";
    real.objectType = Haruka::ObjectType::MODEL;
    real.modelPath  = "assets/models/crate.obj";
    Haruka::RenderCommand r = Haruka::classifySceneObject(real);
    CHECK(r.kind == Haruka::RenderKind::Model,
          "an object that HAS a model still goes to the model renderer");
    CHECK(r.primitive != Haruka::PrimitiveType::CAPSULE,
          "the fallback did not swallow every model in the game");
}


// ================================================================================================
// test_weather_replicated — two clients must be standing in the SAME storm.
//
// Nothing about the weather travels on the wire, and nothing needs to: a front's axis, start point,
// speed and size all come out of `hashf(seed, i, k)`, and its position at any moment is
// `rotateAround(start, axis, omega * time)`. There is no RNG and no wall clock anywhere in that path
// — the header says so in as many words: the time "NO es un steady_clock: tiene que ser el mismo
// numero en el cliente y en el servidor".
//
// So the weather is replicated by REPRODUCTION, not transmission: both machines compute the same
// storm because they compute the same function of the same inputs. That is cheaper and exact, and it
// holds only while every input is identical — which is precisely what this pins. It became true the
// day the simulation clock stopped being a local accumulator and started coming from the cluster
// (see `Client::hasWorldTime` and `PlanetarySystem::setWorldClock`).
//
// THE COUNTER-PROOF IS THE THIRD BLOCK. "Two systems agree" would also pass on a weather system that
// returned a constant, which would be a broken world that happens to be identical everywhere. So the
// same pair is asked again at a different time and must DISAGREE: the storm has to move.
// ================================================================================================
void test_weather_replicated() {
    beginTest("weather_replicated");

    // A handful of directions spread over the sphere, not one: a single sample could agree by luck.
    const glm::dvec3 dirs[] = {
        glm::normalize(glm::dvec3( 1,  0.2,  0.3)),
        glm::normalize(glm::dvec3(-0.4, 0.9, 0.1)),
        glm::normalize(glm::dvec3( 0.1,-0.8, 0.6)),
        glm::normalize(glm::dvec3(-0.7,-0.2,-0.7)),
        glm::normalize(glm::dvec3( 0.3, 0.3, 0.9)),
    };
    const uint32_t seed = 4242;
    const double   tA   = 9137.5;   // "player A's world clock"
    const double   tB   = tA;       // player B, who logged in much later, gets the SAME number

    Haruka::WeatherSystem a, b;
    a.configure(seed); b.configure(seed);
    a.setTime(tA);     b.setTime(tB);

    int differing = 0;
    float worstCover = 0.0f;
    for (const auto& d : dirs) {
        const Haruka::WeatherSample sa = a.sampleAt(d, 15.0f, 0.5f);
        const Haruka::WeatherSample sb = b.sampleAt(d, 15.0f, 0.5f);
        // BIT for bit: same inputs through the same pure code must give the same floats. Anything
        // less would mean state hiding somewhere, and "close enough" would hide it.
        if (sa.cloudCover != sb.cloudCover || sa.precip != sb.precip ||
            sa.cloudBaseM != sb.cloudBaseM || sa.cloudTopM != sb.cloudTopM ||
            sa.humidity  != sb.humidity   || sa.tempC     != sb.tempC ||
            sa.wind      != sb.wind       || sa.type      != sb.type) ++differing;
        worstCover = std::max(worstCover, sa.cloudCover);
    }
    std::printf("    mismo seed y mismo t=%.1f: %d de %d muestras difieren (cobertura max %.3f)\n",
                tA, differing, (int)(sizeof(dirs)/sizeof(dirs[0])), worstCover);
    CHECK(differing == 0, "two clients on the same world clock compute the SAME weather");

    // Reconfiguring must not move anything either: `configure` is idempotent, and a client that
    // reloads its planet must not end up in a different storm from the one next to it.
    b.configure(seed);
    b.setTime(tB);
    int afterReconfigure = 0;
    for (const auto& d : dirs)
        if (a.sampleAt(d, 15.0f, 0.5f).cloudCover != b.sampleAt(d, 15.0f, 0.5f).cloudCover)
            ++afterReconfigure;
    CHECK(afterReconfigure == 0, "and reconfiguring with the same seed does not move the storm");

    // ── COUNTER-PROOF: it has to MOVE. Agreement alone would also pass on a constant sky.
    b.setTime(tA + 600.0);   // ten minutes of world later
    int moved = 0;
    for (const auto& d : dirs)
        if (a.sampleAt(d, 15.0f, 0.5f).cloudCover != b.sampleAt(d, 15.0f, 0.5f).cloudCover) ++moved;
    std::printf("    CONTRAPRUEBA t+600 s: %d de %d muestras cambian\n",
                moved, (int)(sizeof(dirs)/sizeof(dirs[0])));
    CHECK(moved > 0, "and at a DIFFERENT time it differs — the weather is not a constant");

    // And a different world (another seed) must be a different world, or the seed does nothing.
    Haruka::WeatherSystem other;
    other.configure(seed + 1);
    other.setTime(tA);
    int otherWorld = 0;
    for (const auto& d : dirs)
        if (a.sampleAt(d, 15.0f, 0.5f).cloudCover != other.sampleAt(d, 15.0f, 0.5f).cloudCover)
            ++otherWorld;
    CHECK(otherWorld > 0, "and a different seed is a different world");
}
