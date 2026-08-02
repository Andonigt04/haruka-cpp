// ================================================================================================
// Tests del SISTEMA DE CONSTRUCCIÓN (F1): validación de colocación free-form. Verifica lo que el DGS
// usará para validar (mismo código que el cliente): una pieza NO penetra el terreno, NO solapa otra, y
// debe estar ANCLADA (apoyada en el suelo o tocando una pieza). Geometría pura, sin GL. Ver
// docs/guides/PLAN_CONSTRUCCION.md.
// ================================================================================================
#include "test_common.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

#include "game/construction/construction.h"
#include "physics/world_provider.h"

#include <thread>
#include <atomic>
#include <random>

namespace {
    // Mundo de juguete: planeta en el origen, radio 1000 m, terreno CONSTANTE a 50 m sobre la esfera.
    // La superficie está a radio 1050 m; el "arriba" del polo norte (0,1050,0) es +Y (localmente plano).
    struct FlatWorld : Haruka::Physics::IWorldProvider {
        glm::dvec3 center{0.0};
        double     radius = 1000.0, terrain = 50.0;
        double     slope = 0.0;   // pendiente del terreno en +X (0 = llano; para probar TooSteep)
        std::vector<Haruka::Physics::GravBody> grav;   // sin gravedad (F1 no la usa)
        bool       hasActivePlanet()    const override { return true; }
        glm::dvec3 activePlanetCenter() const override { return center; }
        double     activePlanetRadius() const override { return radius; }
        const std::vector<Haruka::Physics::GravBody>& gravBodies() const override { return grav; }
        double     terrainHeightAt(const glm::dvec3& wp) const override { return terrain + slope * wp.x; }
    };

    using namespace Haruka::Construction;
    const glm::dquat kNoRot{1.0, 0.0, 0.0, 0.0};

    // Estado con un catálogo de UNA caja de 1×1×1 m (semiejes 0.5).
    ConstructionState makeState() {
        ConstructionState s;
        PieceType box; box.id = 1; box.halfExtents = glm::dvec3(0.5);
        s.setCatalog({ box });
        return s;
    }
    // Posición del CENTRO de una caja apoyada en la superficie del polo norte (fondo justo en el terreno).
    glm::dvec3 groundedCenter(double surfaceR) { return glm::dvec3(0.0, surfaceR + 0.5, 0.0); }
}

void test_construction_placement() {
    beginTest("construction_placement");
    FlatWorld w;
    const double surfR = w.radius + w.terrain;   // 1050 m
    ConstructionState s = makeState();

    // (1) Caja APOYADA en el suelo → OK (anclada al terreno, sin penetrar ni solapar).
    const Placement rGround = s.validate(1, groundedCenter(surfR), kNoRot, w);
    CHECK(rGround == Placement::Ok, "una caja apoyada en el terreno es colocable (OK)");

    // (2) Caja FLOTANDO 10 m, sin nada debajo → NotAnchored (no se sostiene).
    const Placement rFloat = s.validate(1, glm::dvec3(0.0, surfR + 10.0, 0.0), kNoRot, w);
    CHECK(rFloat == Placement::NotAnchored, "una caja flotando sin apoyo se RECHAZA (NotAnchored)");

    // (3) Caja HUNDIDA en el terreno → OK: NO se rechaza por penetrar (el suelo se moldea con la
    //     herramienta de terreno, no rechazando la pieza); enterrada sigue anclada al suelo.
    const Placement rDeep = s.validate(1, glm::dvec3(0.0, surfR - 1.0, 0.0), kNoRot, w);
    CHECK(rDeep == Placement::Ok, "una caja hundida en el terreno se PERMITE (sin rechazo por penetración)");

    // Coloca una caja de referencia apoyada en el suelo.
    const uint64_t id0 = s.add(1, groundedCenter(surfR), kNoRot, w);
    CHECK(id0 != 0 && s.size() == 1, "add() coloca la pieza y devuelve un id válido");

    // (4) Caja que SOLAPA la de referencia (centros a 0.3 m < 1.0 m de solape) → OverlapsPiece.
    const Placement rOver = s.validate(1, groundedCenter(surfR) + glm::dvec3(0.3, 0.0, 0.0), kNoRot, w);
    CHECK(rOver == Placement::OverlapsPiece, "una caja que solapa otra pieza se RECHAZA (OverlapsPiece)");

    // (5) Caja APILADA encima de la de referencia (flotando respecto al suelo, pero TOCÁNDOLA) → OK
    //     (anclada a la pieza). Centro 1.05 m arriba → hueco de 0.05 m ≤ kAnchorGap, sin solape real.
    const glm::dvec3 stackCenter = groundedCenter(surfR) + glm::dvec3(0.0, 1.05, 0.0);
    const Placement rStack = s.validate(1, stackCenter, kNoRot, w);
    CHECK(rStack == Placement::Ok, "una caja apilada sobre otra (tocándola, sin apoyo en suelo) es colocable (OK)");

    std::printf("    F1 colocación: apoyada=OK · flotando=NotAnchored · hundida=OK · solape=Overlaps · apilada=OK\n");

    // ---- F2: grafo de anclaje (fijaciones = aristas; soporte = alcanzar el suelo por el grafo) ----
    // Apila la caja (5) sobre la de referencia: se crea 1 fijación (id0—id1) y la de arriba se sostiene
    // POR el grafo (ella no toca el suelo, pero id0 sí).
    const uint64_t id1 = s.add(1, stackCenter, kNoRot, w);
    CHECK(s.fastenerCount() == 1, "apilar teje 1 fijación (arista) entre las dos cajas");
    CHECK(s.grounded(id0) && !s.grounded(id1), "solo la de abajo toca el suelo (grounded)");
    CHECK(s.isSupported(id1), "la caja de arriba se SOSTIENE por el grafo (llega al suelo vía id0)");
    CHECK(s.structureComponent(id1).size() == 2, "ambas cajas forman UNA estructura (componente conexa)");

    // Una caja SUELTA a un lado (1.5 m, sin tocar la pila; cerca del polo → +Y sigue siendo el "arriba"
    // local), apoyada en el suelo: estructura propia, sin fijaciones, soportada por el suelo.
    const uint64_t id2 = s.add(1, glm::dvec3(1.5, surfR + 0.5, 0.0), kNoRot, w);
    CHECK(s.fasteners(id2).empty() && s.isSupported(id2), "una caja suelta en el suelo no tiene fijaciones pero se sostiene");
    CHECK(s.structureComponent(id2).size() == 1, "la caja aparte es su propia estructura");

    std::printf("    F2 grafo: fijaciones=1 · apilada soportada vía suelo · estructura {id0,id1} conexa · suelta aparte\n");

    // ---- F2b: reglas de colocación "sobre qué / dónde" (tipo de soporte + suelo llano) ----
    ConstructionState s2;
    PieceType found; found.id = 10; found.halfExtents = glm::dvec3(0.5);
    found.supports = kSupportTerrain; found.needsFlat = true;   // cimentación: SOLO suelo, y LLANO
    PieceType furn;  furn.id = 11;  furn.halfExtents = glm::dvec3(0.5);
    furn.supports = kSupportPiece;                              // mueble: SOLO sobre otra pieza
    s2.setCatalog({ found, furn });

    // (a) Cimentación sobre suelo LLANO → Ok.
    CHECK(s2.validate(10, groundedCenter(surfR), kNoRot, w) == Placement::Ok,
          "cimentación (solo suelo, llano) sobre terreno llano = Ok");
    const uint64_t f0 = s2.add(10, groundedCenter(surfR), kNoRot, w);   // cimentación de referencia
    CHECK(f0 != 0, "cimentación colocada");

    // (b) Mueble (solo-sobre-pieza) apoyado SOLO en el suelo, lejos de piezas → WrongSupport.
    CHECK(s2.validate(11, glm::dvec3(1.5, surfR + 0.5, 0.0), kNoRot, w) == Placement::WrongSupport,
          "mueble (solo sobre pieza) apoyado en el suelo = WrongSupport");
    // (c) Mismo mueble APILADO sobre la cimentación (toca pieza) → Ok.
    CHECK(s2.validate(11, groundedCenter(surfR) + glm::dvec3(0.0, 1.05, 0.0), kNoRot, w) == Placement::Ok,
          "mueble (solo sobre pieza) apilado sobre otra pieza = Ok");

    // (d) Cimentación sobre terreno INCLINADO (desnivel > kFlatTol bajo la huella) → TooSteep. Estado
    //     LIMPIO (sin la f0 de arriba, que ocuparía el sitio y daría OverlapsPiece antes que TooSteep).
    ConstructionState s3; s3.setCatalog({ found });
    FlatWorld ws = w; ws.slope = 1.0;   // 1 m de desnivel por m en +X → huella de 1 m ⇒ spread ~1 m
    CHECK(s3.validate(10, groundedCenter(surfR), kNoRot, ws) == Placement::TooSteep,
          "cimentación (needsFlat) sobre terreno inclinado = TooSteep");

    std::printf("    F2b reglas: cimentación llana=Ok · solo-pieza en suelo=WrongSupport · apilada=Ok · inclinado=TooSteep\n");

    // ---- F3: rotura estructural (quitar pieza → colapso en cascada de lo no soportado) ----
    // Pila a—b—c (cada una toca la de abajo; solo `a` toca el suelo).
    ConstructionState s4 = makeState();
    const uint64_t a = s4.add(1, groundedCenter(surfR), kNoRot, w);
    const uint64_t b = s4.add(1, groundedCenter(surfR) + glm::dvec3(0.0, 1.05, 0.0), kNoRot, w);
    const uint64_t c = s4.add(1, groundedCenter(surfR) + glm::dvec3(0.0, 2.10, 0.0), kNoRot, w);
    CHECK(s4.fastenerCount() == 2 && s4.isSupported(c), "pila a-b-c: 2 fijaciones y la cima soportada");
    const std::vector<uint64_t> fell = s4.breakPiece(a);
    CHECK(fell.size() == 2 && s4.size() == 0, "romper la BASE hace caer toda la pila (cascada b,c)");
    (void)b; (void)c;

    // Zona PROTEGIDA (irrompible + ancla del mundo).
    ConstructionState s5 = makeState();
    const uint64_t g0 = s5.add(1, groundedCenter(surfR), kNoRot, w);
    s5.add(1, groundedCenter(surfR) + glm::dvec3(0.0, 1.05, 0.0), kNoRot, w);
    s5.setProtected(g0, true);
    CHECK(s5.breakPiece(g0).empty() && s5.size() == 2, "una pieza PROTEGIDA no se puede romper (irrompible)");

    // Protegida FLOTANTE como ancla: sostiene a la que cuelga de ella aunque ninguna toque el suelo.
    ConstructionState s6 = makeState();
    const glm::dvec3 pPos(0.0, surfR + 10.0, 0.0);
    const uint64_t p = s6.add(1, pPos, kNoRot, w);
    s6.setProtected(p, true);
    const uint64_t q = s6.add(1, pPos + glm::dvec3(0.0, 1.05, 0.0), kNoRot, w);
    CHECK(!s6.grounded(p) && !s6.grounded(q), "ambas flotan (no tocan el suelo)");
    CHECK(s6.isSupported(q), "q se sostiene por la pieza PROTEGIDA (ancla), sin tocar el suelo");
    CHECK(s6.breakPiece(q).empty() && s6.size() == 1, "romper q no arrastra a la protegida p");

    std::printf("    F3 rotura: base rota → pila cae (cascada) · protegida irrompible · protegida = ancla del mundo\n");

    // ---- Prefabs: colocación por LOTE (un suelo de 2 cajas contiguas colocado de una) ----
    ConstructionState s7 = makeState();
    // Dos cajas en fila con micro-hueco (0.02): no solapan, pero rozan (< kAnchorGap) → se fijan.
    std::vector<ConstructionState::Placed> floor = {
        { 1, groundedCenter(surfR),                              kNoRot },
        { 1, groundedCenter(surfR) + glm::dvec3(1.02, 0.0, 0.0), kNoRot },
    };
    CHECK(s7.validateBatch(floor, w) == Placement::Ok, "prefab (2 cajas contiguas en el suelo) validado = Ok");
    const std::vector<uint64_t> made = s7.addBatch(floor, w);
    CHECK(made.size() == 2 && s7.size() == 2, "el lote coloca las 2 piezas de una");
    CHECK(s7.fastenerCount() == 1 && s7.structureComponent(made[0]).size() == 2,
          "las 2 piezas del prefab quedan fijadas en UNA estructura");
    CHECK(s7.isSupported(made[0]) && s7.isSupported(made[1]), "ambas soportadas (apoyan en el suelo)");

    // Lote que SOLAPA una pieza existente → OverlapsPiece.
    std::vector<ConstructionState::Placed> bad = { { 1, groundedCenter(surfR), kNoRot } };
    CHECK(s7.validateBatch(bad, w) == Placement::OverlapsPiece, "prefab que pisa una pieza existente = OverlapsPiece");
    // Lote FLOTANDO (nada debajo, sin existentes cerca) → NotAnchored.
    ConstructionState s8 = makeState();
    std::vector<ConstructionState::Placed> air = {
        { 1, glm::dvec3(0.0, surfR + 20.0, 0.0),                 kNoRot },
        { 1, glm::dvec3(1.02, surfR + 20.0, 0.0),                kNoRot },
    };
    CHECK(s8.validateBatch(air, w) == Placement::NotAnchored, "prefab flotando sin apoyo = NotAnchored");

    std::printf("    Prefab lote: suelo 2 piezas = 1 estructura soportada · solapa existente=Overlaps · flotando=NotAnchored\n");

    // ---- Fixtures: la REGLA de dónde van puerta/ventanas en un muro (para los .glb puerta/ventana/marco) ----
    {
        // Muro FRONTAL de planta baja (6×2.6 m): 1 puerta centrada en la base + ventanas en el ritmo (2 m).
        std::vector<Opening> fr = wallOpenings(6.0, 2.6, true, 1.1, 2.1, 0.9, 1.2, 0.9, 2.0);
        int doors = 0, wins = 0; bool doorCentered = true, winsAtSill = true, noOverlap = true;
        for (const Opening& o : fr) {
            if (o.kind == FixtureKind::Door) {
                doors++;
                if (std::abs(o.center.x) > 1e-9 || std::abs(o.center.y - 1.05) > 1e-6) doorCentered = false;
            } else {
                wins++;
                if (std::abs(o.center.y - 1.5) > 1e-6) winsAtSill = false;       // sill 0.9 + winH/2 0.6
                if (std::abs(o.center.x) < 1.1 * 0.5 + 0.9 * 0.5) noOverlap = false; // pisaría la puerta
            }
        }
        CHECK(doors == 1 && doorCentered, "muro frontal de planta baja: 1 PUERTA centrada y a la base");
        CHECK(wins >= 1 && winsAtSill && noOverlap, "VENTANAS en el ritmo, al alféizar, SIN pisar la puerta");

        // Muro LATERAL (5 m, no frontal): sin puerta, solo ventanas.
        std::vector<Opening> sd = wallOpenings(5.0, 2.6, false, 1.1, 2.1, 0.9, 1.2, 0.9, 2.0);
        int sdDoors = 0; for (const Opening& o : sd) if (o.kind == FixtureKind::Door) sdDoors++;
        CHECK(sdDoors == 0 && !sd.empty(), "muro lateral: SIN puerta, con ventanas");

        // Muro BAJO (2.0 m): la ventana no cabe bajo el techo → sin ventanas.
        std::vector<Opening> low = wallOpenings(6.0, 2.0, false, 1.1, 2.1, 0.9, 1.2, 1.4, 2.0);
        CHECK(low.empty(), "muro demasiado bajo: la ventana no cabe (sin huecos)");

        // fixtureFits: una puerta 1.0×2.0 CABE en un hueco 1.1×2.1; una de 1.5 de ancho NO.
        Opening door; door.kind = FixtureKind::Door; door.halfExtents = glm::dvec2(1.1 * 0.5, 2.1 * 0.5);
        CHECK(fixtureFits(door, glm::dvec2(0.5, 1.0)),  "puerta 1.0×2.0 cabe en hueco 1.1×2.1");
        CHECK(!fixtureFits(door, glm::dvec2(0.75, 1.0)), "puerta de 1.5 de ancho NO cabe en hueco de 1.1");

        std::printf("    Fixtures: frontal=1 puerta+ventanas · lateral=solo ventanas · muro bajo=sin ventanas · fit ok\n");
    }

    // ---- LAYOUT: la ESTRUCTURA del edificio en datos (sin render) que luego se teje con piezas ----
    {
        // 2 plantas de 6×4, altura 3, puerta 1.1×2.1, ventanas 0.9×1.2 (alféizar 0.9, ritmo 2).
        BuildingLayout L = buildingLayout(2, 6.0, 4.0, 0.25, 3.0, 1.1, 2.1, 0.9, 1.2, 0.9, 2.0);
        CHECK(L.floors == 2 && (int)L.walls.size() == 8, "layout: 2 plantas → 4 muros/planta = 8 muros");

        // Determinismo: misma entrada → misma salida.
        BuildingLayout L2 = buildingLayout(2, 6.0, 4.0, 0.25, 3.0, 1.1, 2.1, 0.9, 1.2, 0.9, 2.0);
        bool same = L2.walls.size() == L.walls.size();
        for (size_t i = 0; same && i < L.walls.size(); ++i)
            same = L.walls[i].a == L2.walls[i].a && L.walls[i].b == L2.walls[i].b &&
                   L.walls[i].openings.size() == L2.walls[i].openings.size();
        CHECK(same, "layout DETERMINISTA: misma entrada → mismo layout");

        // SOLO el frontal de la planta baja lleva puerta; las plantas altas no.
        int frontGround = 0, doors = 0, groundDoors = 0, upperDoors = 0;
        for (const WallRun& w : L.walls) {
            if (w.isFrontGround) frontGround++;
            for (const Opening& o : w.openings)
                if (o.kind == FixtureKind::Door) {
                    doors++;
                    if (w.baseY < 1e-9) groundDoors++; else upperDoors++;
                }
        }
        CHECK(frontGround == 1, "layout: exactamente 1 muro = frontal de planta baja");
        CHECK(doors == 1 && groundDoors == 1 && upperDoors == 0, "layout: puerta SOLO en el frontal de la baja");

        // Las plantas están apiladas: baseY = 0 y 3 (floorH).
        bool stacked = false;
        for (const WallRun& w : L.walls) if (std::abs(w.baseY - 3.0) < 1e-9) stacked = true;
        CHECK(stacked, "layout: la 2ª planta se apila a baseY = floorH");

        std::printf("    Layout: 8 muros · 1 puerta (frontal baja) · determinista · plantas apiladas\n");

        // ---- COLISIÓN desde el LAYOUT: trozos macizos del muro (no una caja por pieza) ----
        // ¿Está el punto (x,y) del plano del muro dentro de algún trozo macizo?
        auto solid = [](const std::vector<WallSolid>& v, double x, double y) {
            for (const WallSolid& s : v)
                if (std::abs(x - s.center.x) <= s.halfExtents.x && std::abs(y - s.center.y) <= s.halfExtents.y)
                    return true;
            return false;
        };
        const WallRun* front = nullptr; const WallRun* side = nullptr;
        for (const WallRun& w : L.walls) {
            if (w.isFrontGround) front = &w;
            else if (!side && w.baseY < 1e-9) side = &w;
        }
        CHECK(front && side, "layout: hay muro frontal de planta baja y muro lateral");
        if (front && side) {
            const double fw = glm::length(front->b - front->a);
            std::vector<WallSolid> fs = wallSolids(fw, front->height, front->openings);
            // Se puede CRUZAR la puerta (centrada, a la altura del pecho) pero no el muro a los lados.
            CHECK(!solid(fs, 0.0, 1.0), "colisión: el hueco de la PUERTA está libre");
            CHECK(solid(fs, -fw * 0.5 + 0.3, 1.0) && solid(fs, fw * 0.5 - 0.3, 1.0),
                  "colisión: el muro a AMBOS lados de la puerta es macizo");
            CHECK(solid(fs, 0.0, front->height - 0.1), "colisión: el DINTEL sobre la puerta es macizo");

            // Muro lateral (solo ventanas): macizo entero, incluso a la altura de la ventana.
            const double sw = glm::length(side->b - side->a);
            std::vector<WallSolid> ss = wallSolids(sw, side->height, side->openings);
            CHECK(ss.size() == 1 && solid(ss, 0.0, 1.5),
                  "colisión: muro con solo ventanas = UNA caja maciza (no se cuela por la ventana)");
            std::printf("    Colisión: frontal=%zu cajas (puerta libre + dintel) · lateral=%zu caja\n",
                        fs.size(), ss.size());
        }
    }
}
