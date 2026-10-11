// Quadtree v5: demanda, presupuesto y cobertura (qué nodos se piden y si llegan).
// (Partido de TestTerrainNode.cpp, que tenía 6 400 líneas; el registro de todos sigue allí.)
#include "World/Terrain/TerrainNodeTestCommon.h"

// ================================================================================================
// EL SEGUNDO TERRENO AL GIRAR LA CAMARA
//
// Andoni: *"cuando se gira la camara aparece un segundo terreno encima por un momento"*.
//
// El fallback por ancestro dibuja el nodo ANCESTRO ENTERO. Un ancestro cubre a sus cuatro hijos, no
// solo al que falta — asi que si una hoja no esta residente pero sus HERMANAS si, se emiten las dos
// cosas: el padre (que tapa las cuatro cuartas partes) y las hermanas finas. Dos superficies en el
// mismo sitio, separadas por lo que el detalle de un nivel cambia.
//
// Aqui se mide cuantos nodos dibujados tienen un ANCESTRO tambien dibujado, y a que distancia quedan
// las dos superficies. La contraprueba es quedarse quieto: si es el transitorio del giro, en reposo
// tiene que irse a cero.
void test_terrain_node_overlap_on_turn() {
    beginTest("terrain_node_overlap_on_turn");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / 1080.0;
    const double cone = nodeFrustumConeHalfAngle(fovY, 1920.0 / 1080.0);

    struct Out { size_t drawn = 0, covered = 0, dupes = 0; int worstUp = 0; NodeId worstChild{};
                 double errBefore = 0.0, errAfter = 0.0; size_t kept = 0;
                 size_t ancCount = 0, ancExact = 0; };

    // ⚠️ SE CALIENTA EL POOL ANTES DE GIRAR. Medir desde el arranque en frio mide OTRA COSA (la carga
    // inicial, donde solo estan las raices y por supuesto todo cae al ancestro). Andoni gira la camara
    // con el terreno ya cargado, asi que el banco tiene que estar en ese estado.
    const int kWarm = 60;

    // `reresolve` = generar lo que falta y VOLVER A RESOLVER en el mismo frame, antes de dibujar.
    // `partition` = quitar del dibujo lo que tenga un ancestro dibujado (`nodeCoveredMask`).
    auto run = [&](bool turn, bool reresolve, bool partition, Out& worst, bool trace) {
        TerrainNodePool pool(2048, 170);
        for (uint32_t f = 0; f < 6; ++f)
            pool.publish(NodeId{ (PlanetFace)f, 0, 0, 0 }, NodeRange{ -9000.0f, 9000.0f }, true);

        const glm::dvec3 dir0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
        const glm::dvec3 east = glm::normalize(glm::cross(dir0, glm::dvec3(0, 1, 0)));
        const glm::dvec3 cam  = pc + dir0 * (R + 1200.0);
        std::vector<NodeId> sel, emitted;
        std::vector<uint8_t> drop, exFlags;

        for (int step = 0; step < kWarm + 20; ++step) {
            const double ang = (turn && step >= kWarm) ? 0.06 * (double)(step - kWarm) : 0.0;
            const glm::dvec3 fwd = glm::normalize(dir0 * std::cos(ang) + east * std::sin(ang));
            pool.beginFrame();
            nodeSelectVisible(R, cam, pc, radPerPx, sel, 2048, TERRAIN_NODE_ERROR_PX, &fwd, cone,
                              5000.0, &TerrainNodePool::rangeFnAdapter, &pool);

            // Lo que el renderer emite de verdad: una instancia por entrada resuelta, sin filtrar.
            emitted.clear(); exFlags.clear();
            for (const NodeId& n : sel) {
                const TerrainNodePool::Resolved r = pool.request(n);
                if (r.slot >= 0) { emitted.push_back(r.node); exFlags.push_back(r.exact ? 1u : 0u); }
            }
            if (reresolve) {                       // generar YA lo que falta y volver a resolver
                pool.prioritisePending(cam, pc, R);
                for (const NodeId& p : pool.takePending())
                    pool.publish(p, NodeRange{ -9000.0f, 9000.0f });
                emitted.clear(); exFlags.clear();
                for (const NodeId& n : sel) {
                    const TerrainNodePool::Resolved r = pool.request(n);
                    if (r.slot >= 0) { emitted.push_back(r.node); exFlags.push_back(r.exact ? 1u : 0u); }
                }
            }
            std::unordered_map<uint64_t, int> seen;
            for (const NodeId& e : emitted) ++seen[nodeKey(e)];

            Out o; o.drawn = emitted.size();
            for (const auto& kv : seen) if (kv.second > 1) o.dupes += (size_t)(kv.second - 1);
            std::unordered_map<uint64_t, uint8_t> exOf;
            for (size_t i = 0; i < emitted.size(); ++i) exOf[nodeKey(emitted[i])] = exFlags[i];
            std::unordered_set<uint64_t> coveringAnc, coveringExact;
            for (const NodeId& e : emitted) {                 // ¿tiene un ANCESTRO tambien dibujado?
                NodeId a = e; int up = 0;
                while (a.level > 0) {
                    a.level--; a.i /= 2; a.j /= 2; ++up;
                    if (seen.count(nodeKey(a))) {
                        ++o.covered;
                        coveringAnc.insert(nodeKey(a));
                        if (exOf[nodeKey(a)]) coveringExact.insert(nodeKey(a));
                        if (up > o.worstUp) { o.worstUp = up; o.worstChild = e; }
                        break;
                    }
                }
                o.errBefore = std::max(o.errBefore, nodeScreenError(e, R, cam, pc, radPerPx));
            }
            o.ancCount = coveringAnc.size(); o.ancExact = coveringExact.size();
            // LO QUE CUESTA LA PARTICION: el peor error en pantalla de lo que QUEDA. Sin esta cifra
            // "quitar los tapados" es una afirmacion, no una medida — se pierde detalle de verdad.

            if (step >= kWarm) {
                if (step == kWarm || o.covered > worst.covered) worst = o;
                if (trace && step < kWarm + 8) {   // ¿un frame malo y ya, o sostenido?
                    const auto st = pool.stats();
                    std::printf("        giro f%-2d  emitidos %4zu  tapados %4zu  caida %d niv  "
                                "residentes %4zu/%zu  fallos %4zu  desalojos %zu\n",
                                step - kWarm, o.drawn, o.covered, o.worstUp,
                                st.resident, st.capacity, st.misses, st.evicted);
                }
            }
            if (!reresolve) {
                pool.prioritisePending(cam, pc, R);
                for (const NodeId& p : pool.takePending())
                    pool.publish(p, NodeRange{ -9000.0f, 9000.0f });
            }
        }
    };

    Out hoy, still_, reres, both;
    run(true,  false, false, hoy,   true);    // como esta hoy
    run(false, false, false, still_, false);  // contraprueba: quieto
    run(true,  true,  false, reres, false);   // generar y volver a resolver en el mismo frame
    run(true,  true,  true,  both,  false);   // + particion

    std::printf("    2048 huecos · 170 nodos/frame · %d frames de calentamiento QUIETO antes de medir\n", kWarm);
    std::printf("                                     emitidos   TAPADOS por un ancestro   copias\n");
    std::printf("      GIRANDO, como hoy             %8zu   %11zu (%4.1f%%)   %zu\n", hoy.drawn, hoy.covered,
                hoy.drawn ? 100.0 * (double)hoy.covered / (double)hoy.drawn : 0.0, hoy.dupes);
    std::printf("      QUIETO (contraprueba)         %8zu   %11zu (%4.1f%%)   %zu\n", still_.drawn, still_.covered,
                still_.drawn ? 100.0 * (double)still_.covered / (double)still_.drawn : 0.0, still_.dupes);
    std::printf("      GIRANDO, re-resolviendo       %8zu   %11zu (%4.1f%%)   %zu\n", reres.drawn, reres.covered,
                reres.drawn ? 100.0 * (double)reres.covered / (double)reres.drawn : 0.0, reres.dupes);
    // ── A QUE DISTANCIA QUEDAN LAS DOS SUPERFICIES ──────────────────────────────────────────────
    // Un nodo tapado por su ancestro se dibuja con el detalle de SU texel; el ancestro, con el suyo,
    // que es 2^up veces mas grueso. La separacion es cuanto cambia el relieve entre esas dos cotas.
    double gap = 0.0;
    if (hoy.worstUp > 0) {
        const NodeId c = hoy.worstChild;
        const double texC = nodeTexelM(c, R);
        const double texA = texC * (double)(1u << hoy.worstUp);
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += 4)
            for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += 4) {
                const glm::dvec3 d = nodeTexelDir(c, u, v);
                gap = std::max(gap, std::fabs((double)terrainDetail(d, R, (float)texC)
                                            - (double)terrainDetail(d, R, (float)texA)));
            }
        std::printf("      el peor tapado esta %d niveles bajo su ancestro (nivel %u) y las dos\n"
                    "      superficies se separan hasta %.3f m: ESO es el 'segundo terreno'\n",
                    hoy.worstUp, c.level, gap);
    }

    CHECK(hoy.drawn > 100, "se dibujan nodos de verdad (si no, el resto no mide nada)");
    CHECK(hoy.covered > 100, "CONTRAPRUEBA: SIN re-resolver la doble superficie ESTA ahi (457 de 499 medidos)");
    CHECK(still_.covered == 0, "QUIETO no pasa: es el transitorio del GIRO, no un estado del pool");
    CHECK(reres.covered == 0, "re-resolver tras generar lo quita del todo: 0 nodos con un ancestro encima");
    CHECK(reres.drawn <= still_.drawn, "y el conjunto dibujado vuelve al tamano de reposo (499 -> 252)");
    CHECK(hoy.ancExact == 0, "los que tapan son SIEMPRE fallbacks, nunca hojas legitimas del selector");
}

/**
 * @brief DIBUJAR POR ANCESTRO: cuánta disparidad mete, y cuántos frames dura.
 *
 * Reportado como "disparidad al girar la cámara". Es lo ÚNICO del pase que depende de la orientación:
 * al girar entran nodos que el pool no tiene generados, y hasta que los genera se dibujan con el
 * heightmap de un ANTEPASADO. Ese mapa evalúa una escalera de octavas más gruesa, así que la
 * superficie dibujada no es la que se pisa — y vuelve a serlo sola unos frames después, que es
 * exactamente el sintoma "aparece al girar y luego se va".
 *
 * Aquí no se prueba que esté bien: se MIDE cuánto vale y cuánto dura, que es lo que decide si la
 * palanca es el pool, el presupuesto de generación o la caché.
 */
void test_terrain_node_ancestor_disparity() {
    beginTest("terrain_node_ancestor_disparity");
    const double R = 6371000.0;
    const NodeId n{ PlanetFace::FRONT, 17, (1u << 17) / 2, (1u << 17) / 2 };
    const double texM = nodeTexelM(n, R);

    std::printf("    saltos de   la superficie que se dibuja se separa de la que se pisa\n");
    std::printf("    ancestro      peor        media      (sobre 4096 texeles del nodo)\n");
    double worst1 = 0.0;
    for (uint32_t up = 1; up <= 4; ++up) {
        const double ancM = texM * (double)(1u << up);
        double w = 0.0, sum = 0.0; size_t cnt = 0;
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += 2)
            for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += 2) {
                const glm::dvec3 d = nodeTexelDir(n, u, v);
                const double dh = std::fabs((double)terrainDetail(d, R, (float)texM)
                                          - (double)terrainDetail(d, R, (float)ancM));
                w = std::max(w, dh); sum += dh; ++cnt;
            }
        std::printf("      %u        %7.3f m   %7.3f m\n", up, w, sum / (double)cnt);
        if (up == 1) worst1 = w;
    }

    // ── CUÁNTO DURA ─────────────────────────────────────────────────────────────────────────────
    // Cifras del juego: 3053 nodos seleccionados, 170 generados por frame, y `angle_independence`
    // mide 935 nodos NUEVOS al girar 90 grados.
    const double kNuevosAl90 = 935.0, kPorFrame = 170.0;
    const double frames = kNuevosAl90 / kPorFrame;
    std::printf("    al girar 90 grados entran %.0f nodos nuevos a %.0f/frame -> %.1f frames "
                "(%.0f ms a 60 fps)\n", kNuevosAl90, kPorFrame, frames, frames * 16.67);
    std::printf("    -> durante esos frames el suelo dibujado esta hasta %.2f m fuera del que se pisa\n",
                worst1);

    // Umbrales sobre lo MEDIDO, no elegidos a ojo: un salto son 3,9 cm (del orden del suelo de ~1 cm
    // que dejan render+twist juntos), y dos saltos se van a 34 cm.
    CHECK(worst1 > 0.01, "un salto de ancestro ya supera el suelo de disparidad de render+twist");
    CHECK(frames > 1.0, "y no se resuelve en un frame con el presupuesto de generacion de hoy");
}

/**
 * @brief SATURACIÓN DEL SELECTOR: cuando se acaba el presupuesto, ¿quién se queda sin dividir?
 *
 * El juego mide `sel 3053` contra un presupuesto REAL de 3072 (el pool de 4096 menos el 25 % de
 * caché). O sea que el selector va al 99,4 % y `room` se agota. Cuando eso pasa:
 *
 *     if (room && nodeShouldSplit(...)) { dividir } else { out.push_back(n); }
 *
 * el nodo se dibuja BASTO. Y quién se queda basto lo decide el ORDEN DE RECORRIDO —una pila LIFO que
 * empieza por las seis caras del cubo—, no la importancia. Eso lo hace dependiente de la orientación:
 * al girar cambia qué se recorta por el cono, cambia el orden efectivo, y le toca a otros.
 *
 * Es la explicacion candidata de "disparidad al girar la camara". Aquí se comprueba, no se afirma:
 * si el presupuesto que sobra se lo llevan nodos LEJANOS mientras uno CERCANO se queda sin dividir,
 * el reparto está mal; si los que se quedan bastos son siempre los lejanos, la causa es otra.
 */
void test_terrain_node_budget_starvation() {
    beginTest("terrain_node_budget_starvation");
    const double R = 6371000.0;
    const glm::dvec3 center(0.0);
    const double radPerPx = 9.4e-4, errPx = 1.0;
    const glm::dvec3 dir0 = glm::normalize(glm::dvec3(1.0, 0.35, 0.22));
    const glm::dvec3 cam  = center + dir0 * (R + 2.0);
    auto distOf = [&](const NodeId& n) {
        return glm::length(center + nodeTexelDir(n, TERRAIN_NODE_CELLS/2, TERRAIN_NODE_CELLS/2) * R
                           - cam);
    };

    // ⚠️ EL HORIZONTE LLEVA LA COTA DEL NODO (18-09: el recorte tiraba las cordilleras mas alla del
    // horizonte liso). Sin `rangeFn` el selector asume 5 km en TODOS los nodos (horizonte a 252 km):
    // la demanda sube de ~2 900 a 7 856. Este mundo es una esfera lisa con relieve procedural de
    // ±915 m como mucho, y eso es lo que se le dice: es lo que el pool sabe en el juego una vez
    // generados los nodos. La politica LIFO de referencia recibe la MISMA cota.
    static const auto reliefRange = [](const NodeId&, void*) { NodeRange r; r.minM = -915.0f; r.maxM = 915.0f; return r; };
    const double kReliefM = 915.0;
    std::vector<NodeId> full;
    nodeSelectVisible(R, cam, center, radPerPx, full, 1000000, errPx, nullptr, 0.0, 5000.0, reliefRange, nullptr);
    std::printf("    demanda sin tope: %zu nodos (horizonte con relieve de %.0f m) · presupuesto del juego (pool 4096): 3072\n",
                full.size(), kReliefM);
    std::printf("    ⚠️ aqui NO satura. El juego mide 3053 porque el selector recibe las ELEVACIONES\n");
    std::printf("       del pool y divide mas; queda a 19 del tope, o sea plausible pero SIN medir.\n");

    // ── LO QUE SI SE PUEDE DECIDIR AQUI: LA POLITICA DE REPARTO ─────────────────────────────────
    //
    // ⚠️ LA METRICA BUENA ES EL PEOR ERROR EN PANTALLA, NO LA DISTANCIA. El primer intento comparaba
    // "el nodo basto mas cercano" contra "el fino mas lejano" y marcaba inversion siempre, tambien
    // con el reparto ya arreglado — porque mezcla niveles: el error es `texel/(dist·radPerPx)`, y un
    // nodo grueso a 700 m puede tener menos error que uno fino a 1400 m o mas, segun su nivel. Medir
    // la distancia en vez del error es medir otra cosa.
    //
    // Lo que el reparto tiene que conseguir es MINIMIZAR EL PEOR ERROR con el presupuesto dado. Se
    // compara contra la politica que habia (pila LIFO sembrada con las seis caras), reimplementada
    // aqui: sin la referencia, "0,8 px" no dice si el cambio sirvio de algo.
    auto lifoSelect = [&](size_t budget, std::vector<NodeId>& out) {
        std::vector<NodeId> stack;
        out.clear();
        for (int f = 0; f < 6; ++f) stack.push_back(NodeId{ (PlanetFace)f, 0, 0, 0 });
        while (!stack.empty()) {
            const NodeId n = stack.back(); stack.pop_back();
            if (nodeBelowHorizon(n, R, cam, center, kReliefM)) continue;
            const bool room = (out.size() + stack.size() + 4) <= budget;
            if (room && nodeShouldSplit(n, R, cam, center, radPerPx, errPx, kReliefM)) {
                NodeId kids[4]; nodeChildren(n, kids);
                for (const NodeId& k : kids) stack.push_back(k);
            } else out.push_back(n);
        }
    };
    auto lifoWorstErr = [&](size_t budget) {
        std::vector<NodeId> out; lifoSelect(budget, out);
        double w = 0.0;
        for (const NodeId& n : out) w = std::max(w, nodeScreenError(n, R, cam, center, radPerPx, kReliefM));
        return w;
    };

    std::printf("\n    presupuesto   PEOR error en pantalla (px)     ganancia\n");
    std::printf("                   LIFO (antes)   por error (ahora)\n");
    bool better = false;
    for (size_t budget : { (size_t)2500, (size_t)2000, (size_t)1500, (size_t)1000 }) {
        std::vector<NodeId> tight;
        nodeSelectVisible(R, cam, center, radPerPx, tight, budget, errPx, nullptr, 0.0, 5000.0, reliefRange, nullptr);
        double wNew = 0.0;
        for (const NodeId& n : tight) wNew = std::max(wNew, nodeScreenError(n, R, cam, center, radPerPx, kReliefM));
        const double wOld = lifoWorstErr(budget);
        if (wNew < wOld * 0.95) better = true;
        std::printf("    %9zu   %12.1f   %17.1f     %s\n", budget, wOld, wNew,
                    (wNew < wOld) ? "mejor" : (wNew > wOld ? "PEOR" : "igual"));
    }
    std::printf("    (el presupuesto se gasta primero en el nodo con MAS error; lo que queda en la\n"
                "     cola al agotarse es por construccion lo de menos, y es lo que sale basto)\n");

    // ── LO QUE CUESTA EL CAMBIO ─────────────────────────────────────────────────────────────────
    //
    // El monticulo calcula el error al METER el nodo, asi que lo paga tambien para los que luego
    // recorta el horizonte —la pila lo calculaba al sacarlo, despues de recortar—. Puede salir mas
    // caro; si sale MUCHO mas caro, el arreglo no compensa y hay que ordenar de otra forma.
    {
        // ⚠️ LOS MILISEGUNDOS DEPENDEN DEL ARBOL DE BUILD. `haruka-cpp/build` es **Debug** y da ~5,6
        // ms; el arbol de Survival es **RelWithDebInfo** y da ~1,0 ms en caliente. La cifra absoluta
        // solo vale si se dice de cual sale. La RAZON sobrevive a las dos (x1,25-1,28 medido en
        // ambas), porque las dos politicas hacen el mismo tipo de trabajo.
        //
        // En -O2 eso son **+0,25 ms de frame** (1,5 % a 60 fps) cuando NO satura, que es el precio
        // de llevar el error en el candidato. Cuando satura se paga ademas la segunda pasada.
        //
        // Y la comparacion tiene que ser JUSTA: la primera version cronometraba `lifoWorstErr`, que
        // ademas de seleccionar barria los ~2900 nodos calculando errores. Eso le cargaba a la
        // politica vieja un trabajo que la nueva no hace, y salia un x0,92 que no significaba nada.
        const int kIter = 20;
        std::vector<NodeId> tmp, tmp2;
        const auto t0 = std::chrono::steady_clock::now();
        // A 8192 (por encima de la demanda de 6 215): saturado se paga la segunda pasada y la
        // comparacion dejaria de medir el coste de ORDENAR.
        for (int i = 0; i < kIter; ++i) nodeSelectVisible(R, cam, center, radPerPx, tmp, 8192, errPx, nullptr, 0.0, 5000.0, reliefRange, nullptr);
        const auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < kIter; ++i) lifoSelect(8192, tmp2);
        const auto t2 = std::chrono::steady_clock::now();
        const double heapMs = std::chrono::duration<double, std::milli>(t1 - t0).count() / kIter;
        const double lifoMs = std::chrono::duration<double, std::milli>(t2 - t1).count() / kIter;
        std::printf("\n    coste del selector (absoluto = ESTE build; la razon vale en los dos)\n"
                    "      LIFO %.2f ms -> "
                    "por error %.2f ms  (x%.2f)\n", lifoMs, heapMs, heapMs / std::max(lifoMs, 1e-9));
        CHECK(heapMs < lifoMs * 2.0, "ordenar por error no llega a duplicar el coste del selector "
                                     "(si lo duplicara no compensaria el arreglo)");
    }

    // ── ¿PUEDE PARPADEAR LA PROPIA POLITICA? ────────────────────────────────────────────────────
    //
    // El selector usa la pila si no satura y el monticulo si satura. Si la saturacion oscila entre
    // frames, la politica oscila con ella — y si las dos dieran conjuntos DISTINTOS, el terreno
    // saltaria entre dos cortes cada frame. Eso seria otro temblor, introducido por el arreglo.
    //
    // La premisa que lo salva es que sin presion las dos dan EXACTAMENTE lo mismo. No se asume:
    // se comprueba nodo a nodo, porque de ella depende que no haya que anadir mas histeresis.
    {
        std::vector<NodeId> byHeap, byLifo;
        // Por encima de la demanda (6 215 con el horizonte por cota): ahi no hay presion.
        nodeSelectVisible(R, cam, center, radPerPx, byHeap, 8192, errPx, nullptr, 0.0, 5000.0, reliefRange, nullptr);
        lifoSelect(8192, byLifo);
        std::sort(byHeap.begin(), byHeap.end(), [](const NodeId& a, const NodeId& b) {
            return std::tie(a.face, a.level, a.i, a.j) < std::tie(b.face, b.level, b.i, b.j); });
        std::sort(byLifo.begin(), byLifo.end(), [](const NodeId& a, const NodeId& b) {
            return std::tie(a.face, a.level, a.i, a.j) < std::tie(b.face, b.level, b.i, b.j); });
        const bool same = (byHeap.size() == byLifo.size())
                       && std::equal(byHeap.begin(), byHeap.end(), byLifo.begin());
        std::printf("\n    sin saturar, las dos politicas dan el MISMO conjunto: %s (%zu vs %zu nodos)\n",
                    same ? "si" : "NO", byHeap.size(), byLifo.size());
        CHECK(same, "sin presion pila y monticulo coinciden nodo a nodo, asi que alternar entre "
                    "ellas no puede hacer saltar el terreno");
    }

    // ⚠️ HALLAZGO (18-09): con el horizonte POR COTA la demanda a 2 m es de 6 215 nodos (relieve de
    // 915 m: horizonte a 107 km) contra los 3 072 del juego: SATURA. Con el horizonte liso (cota 0)
    // eran ~2 900 y no saturaba. En el juego el pool da la cota REAL de cada nodo (el mar a 0 no
    // amplia nada), asi que la demanda real esta entre las dos y hay que leerla del log (`sel N`).
    {
        std::vector<NodeId> flat;
        static const auto zeroRange = [](const NodeId&, void*) { NodeRange r; r.minM = 0.0f; r.maxM = 0.0f; return r; };
        nodeSelectVisible(R, cam, center, radPerPx, flat, 1000000, errPx, nullptr, 0.0, 5000.0, zeroRange, nullptr);
        std::printf("    demanda con el horizonte LISO (cota 0): %zu · con relieve de 915 m: %zu · presupuesto 3072\n", flat.size(), full.size());
        CHECK(flat.size() < 3072, "con el horizonte liso este punto de vista no satura (lo de antes)");
        CHECK(full.size() > flat.size(), "el horizonte por cota admite MAS nodos (los que asoman mas alla del liso)");
    }
    CHECK(better, "repartir por error en pantalla baja el PEOR error frente a la pila LIFO que habia "
                  "(si no bajara, el cambio no valdria para nada y habria que revertirlo)");
}

/**
 * @brief EL TERRENO TIEMBLA AL ANDAR: ¿de qué, y cuánto?
 *
 * Reportado despues de arreglar el reparto del presupuesto. "Temblar" es inestabilidad TEMPORAL: la
 * superficie de un mismo punto del suelo cambia entre un frame y el siguiente. Los tres mecanismos
 * que pueden hacerlo, y aqui se separan porque cada uno se arregla en un sitio distinto:
 *
 *   · **STRIDE** — `nodeStrideIndex` sale de la distancia y NO tiene histeresis. Un nodo parado justo
 *     en un umbral cambia de stride cada frame, y con el cambia la cuerda con la que se dibuja.
 *     Es NUEVO: con el stride global esto no podia pasar.
 *   · **NIVEL** — `nodeShouldSplit` tampoco tiene histeresis, pero ese salto ya lo cierra el
 *     geomorph por distancia (`nodeParentMorph`), que es continuo.
 *   · **ANCESTRO** — el pool no llega a generar y se dibuja el mapa del padre. Medido aparte.
 *
 * Se camina a 5 m/s (8,3 cm por frame a 60 fps) y se mira UN punto fijo del suelo.
 */
void test_terrain_node_walk_shimmer() {
    beginTest("terrain_node_walk_shimmer");
    const double R = 6371000.0;
    const glm::dvec3 center(0.0);
    const double radPerPx = 9.4e-4, errPx = 1.0, vertPx = 4.0;
    const double fineCell = TERRAIN_RING_FINE_CELL;
    const uint32_t maxIdx = 6;

    const NodeId target{ PlanetFace::FRONT, 17, (1u << 17) / 2, (1u << 17) / 2 };
    const glm::dvec3 dirT = nodeTexelDir(target, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2);
    const glm::dvec3 walk = glm::normalize(glm::cross(dirT, glm::dvec3(0, 0, 1)));
    const double texM = nodeTexelM(target, R);

    // ⚠️ EL PUNTO DE SONDA NO PUEDE SER EL CENTRO DEL NODO. El primer intento medía el téxel 64, que
    // es múltiplo de 1, 2, 4, 8, 16, 32 y 64 — o sea que cae en la retícula de TODOS los strides y su
    // altura no cambia nunca. Daba 0,0000 m de salto y parecía que el stride era inocente. Hay que
    // sondear puntos IMPARES, que son los que ningún stride grueso dibuja.
    auto drawnH = [&](uint32_t stride, uint32_t c) {
        const uint32_t b2 = (c / stride) * stride;
        const uint32_t nx = std::min(b2 + stride, (uint32_t)TERRAIN_NODE_CELLS);
        if (nx == b2) return (double)terrainDetail(nodeTexelDir(target, c, c), R, (float)texM);
        const double t = (double)(c - b2) / (double)(nx - b2);
        const double hA = (double)terrainDetail(nodeTexelDir(target, b2, c), R, (float)texM);
        const double hB = (double)terrainDetail(nodeTexelDir(target, nx, c), R, (float)texM);
        return hA + (hB - hA) * t;
    };
    const uint32_t kProbes[] = { 61u, 63u, 65u, 67u, 75u, 83u };   // impares: fuera de toda reticula

    // ── DONDE ESTAN DE VERDAD LOS UMBRALES: barriendo, no con una formula ───────────────────────
    auto strideAt = [&](double alongM) {
        const glm::dvec3 cam = center + glm::normalize(dirT + walk * (alongM / R)) * (R + 2.0);
        return nodeStrideIndex(target, R, cam, center, errPx, vertPx, fineCell, maxIdx);
    };
    std::vector<double> bounds;
    {
        uint32_t last = strideAt(0.0);
        for (int i = 1; i <= 40000; ++i) {              // 0..400 m a 1 cm
            const double d = i * 0.01;
            const uint32_t sk = strideAt(d);
            if (sk != last) { bounds.push_back(d); last = sk; }
        }
    }
    std::printf("    umbrales de stride medidos (barrido a 1 cm): ");
    for (double d : bounds) std::printf("%.2f m  ", d);
    std::printf("\n");

    // ── 1. ANDANDO: cuanto salta la superficie al cruzar un umbral ──────────────────────────────
    const double kStepM = 5.0 / 60.0;                   // 5 m/s a 60 fps
    uint32_t prevSk = 999; double prevH[6] = {0,0,0,0,0,0};
    size_t flips = 0; double worstJump = 0.0, flipAtM = 0.0;
    for (int i = 0; i <= 4800; ++i) {
        const double along = i * kStepM;
        const uint32_t sk = strideAt(along);
        double h[6];
        for (int q = 0; q < 6; ++q) h[q] = drawnH(1u << sk, kProbes[q]);
        if (prevSk != 999 && sk != prevSk) {
            ++flips;
            for (int q = 0; q < 6; ++q) {
                const double jump = std::fabs(h[q] - prevH[q]);
                if (jump > worstJump) { worstJump = jump; flipAtM = along; }
            }
        }
        prevSk = sk;
        for (int q = 0; q < 6; ++q) prevH[q] = h[q];
    }
    std::printf("    andando 400 m a 5 m/s (%.3f m/frame), sobre 6 puntos IMPARES del nodo:\n", kStepM);
    std::printf("      cambios de stride: %zu · salto peor de la superficie: %.4f m (a %.1f m)\n",
                flips, worstJump, flipAtM);

    // ── 2. PARADO EN UN UMBRAL: ¿parpadea? ──────────────────────────────────────────────────────
    //
    // Un salto aislado al cruzar es un pop. Ida y vuelta en frames consecutivos es un PARPADEO, y
    // eso es lo que se lee como "tiembla". Se prueba en CADA umbral medido, con el micro-temblor
    // que tiene cualquier controlador de personaje.
    size_t worstToggles = 0, worstNoHyst = 0; double worstAt = 0.0;
    for (double d : bounds) {
        size_t toggles = 0, noHyst = 0;
        uint32_t last = kNoPrevStride, lastRaw = 999;
        for (int i = 0; i < 120; ++i) {
            const double dd = d + ((i % 2) ? +0.01 : -0.01);
            const glm::dvec3 cam = center + glm::normalize(dirT + walk * (dd / R)) * (R + 2.0);
            const double want = nodeStrideWant(target, R, cam, center, errPx, vertPx, fineCell);
            // CON historia, que es como corre el motor.
            const uint32_t sk = nodeStrideQuantise(want, maxIdx, last);
            if (last != kNoPrevStride && sk != last) ++toggles;
            last = sk;
            // SIN historia: la contraprueba. Si esta tampoco parpadeara, el test no probaria nada.
            const uint32_t raw = nodeStrideQuantise(want, maxIdx);
            if (lastRaw != 999 && raw != lastRaw) ++noHyst;
            lastRaw = raw;
        }
        if (toggles > worstToggles) { worstToggles = toggles; worstAt = d; }
        worstNoHyst = std::max(worstNoHyst, noHyst);
    }
    std::printf("      PARADO en un umbral con 1 cm de temblor, en 120 frames:\n");
    std::printf("        SIN histeresis: %zu cambios   <- el temblor reportado\n", worstNoHyst);
    std::printf("        CON histeresis: %zu cambios (peor umbral, a %.2f m)\n",
                worstToggles, worstAt);
    CHECK(worstNoHyst > 100, "CONTRAPRUEBA: sin histeresis SI parpadea casi cada frame — si esto "
                             "fallara, el temblor no seria del stride y la histeresis sobraria");

    CHECK(!bounds.empty(), "el barrido ENCUENTRA umbrales (si no, no se esta midiendo nada)");
    CHECK(worstToggles == 0, "parado en un umbral, un temblor de 1 cm NO hace parpadear el stride");
    CHECK(worstJump < 0.05, "y cruzarlo andando no mueve la superficie mas de 5 cm");
}

/**
 * @brief EL TEMBLOR NO ES EL STRIDE: ¿es la CUANTIZACIÓN DE `uCenter`?
 *
 * `terrain_node.vert` compone la posición así:
 *
 *     vFragPos = uCenter.xyz + dirF * (uMisc.x + h)
 *
 * y `uCenter` lo llena la CPU con `glm::vec3(planetCenter - camPos)`. La resta va en double, bien —
 * pero el resultado se guarda en **float**, y su magnitud es el radio del planeta: 6,37e6. Un float
 * ahí tiene un ulp de **0,5 m**.
 *
 * Eso NO es el muro de precisión estático que ya está documentado (ese distorsiona el terreno pero de
 * forma FIJA, y no se ve). Esto es distinto: `uCenter` cambia CADA FRAME al andar, así que se mueve a
 * saltos de medio metro mientras la cámara se mueve suave. Todo el terreno salta de golpe.
 *
 * A 5 m/s cruzas un escalón de 0,5 m diez veces por segundo. Eso se lee como temblor, no como salto.
 */
void test_terrain_node_ucenter_jitter() {
    beginTest("terrain_node_ucenter_jitter");
    const double R = 6371000.0;
    const glm::dvec3 center(0.0);
    const NodeId n{ PlanetFace::FRONT, 17, (1u << 17) / 2, (1u << 17) / 2 };
    const glm::dvec3 dirT = nodeTexelDir(n, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2);
    const glm::dvec3 walk = glm::normalize(glm::cross(dirT, glm::dvec3(0, 0, 1)));
    const double h = (double)terrainDetail(dirT, R, (float)nodeTexelM(n, R));

    std::printf("    ulp de un float en 6.37e6: %.4f m\n",
                (double)std::nextafterf((float)R, 2.0f * (float)R) - R);

    // ⚠️ LA DIRECCION DE LA CAMINATA IMPORTA, Y EL PRIMER INTENTO ELIGIO LA MEJOR. Con la camara
    // sobre el centro de la cara FRONT, `center - cam` es casi (-6.37e6, 0, 0): la componente GORDA
    // (ulp 0,5 m) apenas cambia al andar de lado, y lo que cambia son las pequenas, cuyo ulp es de
    // micras. Salia 0,0000 m — cierto, y sin valor: es el caso mas favorable de los seis.
    //
    // Se barren direcciones generales, donde las tres componentes son grandes a la vez y andar SI
    // mueve una de ulp 0,5 m.
    const double kStepM = 5.0 / 60.0;              // 5 m/s a 60 fps
    double worstJump = 0.0, worstMean = 0.0, worstFix = 0.0; int worstCase = -1;
    const glm::dvec3 kDirs[] = {
        glm::normalize(glm::dvec3(1.0, 0.0, 0.0)),      // el caso facil de antes
        glm::normalize(glm::dvec3(1.0, 1.0, 1.0)),      // las tres componentes grandes
        glm::normalize(glm::dvec3(0.7, 0.5, 0.51)),
        glm::normalize(glm::dvec3(0.9, 0.31, 0.30)),
    };
    for (int c = 0; c < 4; ++c) {
        const glm::dvec3 d0 = kDirs[c];
        const double h0 = (double)terrainDetail(d0, R, 0.6f);
        // Andar A LO LARGO de la componente mas grande, que es el peor caso.
        // ⚠️ El eje se elige por la componente MENOR, no la mayor: con la mayor, si `d0` esta
        // alineado con un eje la proyeccion sale nula y `normalize` devuelve NaN — la primera version
        // imprimia "0.0000 m" para (1,0,0) y era un NaN disfrazado, no un caso bueno.
        glm::dvec3 axis(0.0); int small = 0;
        for (int k = 1; k < 3; ++k) if (std::fabs(d0[k]) < std::fabs(d0[small])) small = k;
        axis[small] = 1.0;
        const glm::dvec3 w = glm::normalize(axis - d0 * glm::dot(axis, d0));
        double wj = 0.0, sum = 0.0; size_t cnt = 0, cntFix = 0;
        double wjFix = 0.0, sumFix = 0.0;
        glm::dvec3 prevErr(0.0), prevErrFix(0.0); bool have = false;
        for (int i = 0; i <= 600; ++i) {
            const glm::dvec3 cam = center + glm::normalize(d0 + w * ((i * kStepM) / R)) * (R + 2.0);
            const glm::vec3 uCenter = glm::vec3(center - cam);
            const glm::vec3 posF = uCenter + glm::vec3(d0) * (float)(R + h0);
            const glm::dvec3 posD = d0 * (R + h0) - cam;
            const glm::dvec3 err = glm::dvec3(posF) - posD;
            // EL ARREGLO, tal cual lo hace el motor: el trozo gordo cuantizado a 64 m (exacto en
            // float ahi) y el fino sumado DESPUES de la cancelacion.
            const double kQ = 64.0;
            const glm::dvec3 rel = center - cam;
            const glm::dvec3 hiD(std::round(rel.x/kQ)*kQ, std::round(rel.y/kQ)*kQ,
                                 std::round(rel.z/kQ)*kQ);
            const glm::vec3 posFix = (glm::vec3(hiD) + glm::vec3(d0) * (float)(R + h0))
                                   + glm::vec3(rel - hiD);
            const glm::dvec3 errFix = glm::dvec3(posFix) - posD;
            if (have) { const double dd = glm::length(err - prevErr); wj = std::max(wj, dd);
                        sum += dd; ++cnt;
                        const double df = glm::length(errFix - prevErrFix);
                        wjFix = std::max(wjFix, df); sumFix += df; ++cntFix; }
            prevErr = err; prevErrFix = errFix; have = true;
        }
        std::printf("      dir (%.2f,%.2f,%.2f):  ANTES peor %.4f m medio %.4f m  ->  "
                    "CON EL ARREGLO peor %.6f m\n",
                    d0.x, d0.y, d0.z, wj, sum / (double)cnt, wjFix);
        if (wj > worstJump) { worstJump = wj; worstMean = sum / (double)cnt; worstCase = c; }
        worstFix = std::max(worstFix, wjFix);
        (void)cntFix; (void)sumFix;
    }
    std::printf("    el paso real de la camara es %.4f m/frame · PEOR caso: dir #%d\n",
                kStepM, worstCase);
    (void)worstMean;

    // Sin veredicto prefijado: si sale grande es una causa de temblor; si sale cero, `uCenter` queda
    // DESCARTADO y hay que buscar en otro sitio. Lo que no vale es no medirlo.
    std::printf("    -> partido en dos, el salto entre frames baja de %.4f m a %.6f m (%.0fx)\n",
                worstJump, worstFix, worstJump / std::max(worstFix, 1e-12));
    CHECK(worstJump > 0.2, "CONTRAPRUEBA: con `uCenter` en un solo float el terreno SI salta medio "
                           "metro entre frames — si esto no fallara, el arreglo no haria falta");
    CHECK(worstFix < 0.001, "partido en grueso (multiplo de 64 m, exacto) + fino, el salto entre "
                            "frames desaparece");
}

/**
 * @brief PINCHOS: ¿deja el selector vecinos con más de UN nivel de diferencia?
 *
 * El cosido y el geomorph cierran exactamente **un** nivel: el nodo fino adopta la altura de su
 * PADRE en la arista, que es lo que el vecino calcula porque el vecino ESTÁ al nivel del padre. Con
 * dos o más niveles de diferencia el padre ya no es el vecino, el morph apunta al sitio equivocado y
 * la zancada del cosido se dispara (`sNb << lv`, que con lv grande se sale del nodo entero). El
 * resultado es una arista tirada de punta a punta: un pincho.
 *
 * Es la condicion **2:1** clasica de los quadtree con LOD, y aqui nunca se impuso — funcionaba de
 * rebote porque la pila LIFO refina en profundidad y deja vecinos parecidos. Repartir por error
 * refina donde mas falta hace, GLOBALMENTE, y eso puede juntar un nivel 17 con uno muy grueso.
 *
 * Si esto encuentra saltos > 1, los pinchos son esto y hay que equilibrar el arbol.
 */
void test_terrain_node_level_balance() {
    beginTest("terrain_node_level_balance");
    const double R = 6371000.0;
    const glm::dvec3 center(0.0);
    const double radPerPx = 9.4e-4, errPx = 1.0;
    const glm::dvec3 dir0 = glm::normalize(glm::dvec3(1.0, 0.35, 0.22));
    const glm::dvec3 cam  = center + dir0 * (R + 2.0);

    auto worstDiff = [&](size_t budget, size_t& outPairs, size_t& outBad) {
        std::vector<NodeId> sel;
        nodeSelectVisible(R, cam, center, radPerPx, sel, budget, errPx);
        std::unordered_map<uint64_t, uint32_t> lv;
        for (const NodeId& n : sel) lv[nodeKey(n)] = n.level;
        int worst = 0; outPairs = 0; outBad = 0;
        for (const NodeId& n : sel) {
            int c[4]; NodeId nb[4];
            nodeNeighbourLevels(n, lv, c, nb);
            for (int e = 0; e < 4; ++e) {
                if (nb[e] == n) continue;              // sin vecino dibujado por esa arista
                ++outPairs;
                if (c[e] > worst) worst = c[e];
                if (c[e] > 1) ++outBad;
            }
        }
        return worst;
    };

    std::printf("    presupuesto   parejas   saltos de >1 nivel   peor salto\n");
    int worstAll = 0;
    for (size_t budget : { (size_t)3072, (size_t)2000, (size_t)1000 }) {
        size_t pairs = 0, bad = 0;
        const int w = worstDiff(budget, pairs, bad);
        worstAll = std::max(worstAll, w);
        std::printf("    %9zu   %7zu   %18zu   %10d\n", budget, pairs, bad, w);
    }
    std::printf("    (el cosido y el morph cierran UN nivel; con mas, la zancada `sNb << lv` se sale\n"
                "     del nodo y la arista se dibuja de punta a punta -> pincho)\n");

    CHECK(worstAll <= 1, "el arbol SELECCIONADO cumple 2:1 y el cosido puede cerrar sus costuras");

    // ── PERO LO QUE SE DIBUJA NO ES LO QUE SE SELECCIONA ─────────────────────────────────────────
    //
    // Un nodo sin hueco en el pool se dibuja con el mapa de un ANCESTRO (el log del juego lo cuenta
    // como "por ancestro"). El conjunto DIBUJADO puede entonces tener saltos de varios niveles
    // aunque el seleccionado sea 2:1 — y `nodeNeighbourLevels` se alimenta del dibujado.
    {
        std::vector<NodeId> sel;
        nodeSelectVisible(R, cam, center, radPerPx, sel, 3072, errPx);
        // Simula la caida a ancestro: 1 de cada 128 nodos retrocede 3 niveles, que es lo que pasa
        // cuando el pool no llega a generarlos (al girar entran 935 de golpe a 170 por frame).
        std::vector<NodeId> drawn; drawn.reserve(sel.size());
        size_t fell = 0;
        for (size_t i = 0; i < sel.size(); ++i) {
            NodeId n = sel[i];
            if (i % 128 == 0 && n.level >= 3) {
                n.level -= 3; n.i /= 8; n.j /= 8; ++fell;
            }
            drawn.push_back(n);
        }
        std::unordered_map<uint64_t, uint32_t> lv;
        for (const NodeId& n : drawn) lv[nodeKey(n)] = n.level;

        auto audit = [&](const std::vector<NodeId>& set, size_t& bad) {
            std::unordered_map<uint64_t, uint32_t> ix;
            for (const NodeId& n : set) ix[nodeKey(n)] = n.level;
            int worst = 0; bad = 0;
            for (const NodeId& n : set) {
                int c[4]; NodeId nb[4];
                nodeNeighbourLevels(n, ix, c, nb);
                for (int e = 0; e < 4; ++e) {
                    if (nb[e] == n) continue;
                    worst = std::max(worst, c[e]);
                    if (c[e] > 1) ++bad;
                }
            }
            return worst;
        };
        size_t badBefore = 0;
        const int worstBefore = audit(drawn, badBefore);
        std::printf("\n    con %zu nodos caidos a ancestro (lo que pasa al girar):\n", fell);
        std::printf("      SIN equilibrar: peor salto %d niveles · %zu parejas con mas de uno\n",
                    worstBefore, badBefore);

        // El equilibrado, con la MISMA regla que usa el renderer (`nodeBalanceDrop`).
        for (int pass = 0; pass < 12; ++pass) {
            std::unordered_map<uint64_t, uint32_t> ix;
            for (const NodeId& n : drawn) ix[nodeKey(n)] = n.level;
            size_t changed = 0;
            for (NodeId& n : drawn) {
                const int drop = nodeBalanceDrop(n, ix);
                if (drop <= 0) continue;
                for (int k = 0; k < drop && n.level > 0; ++k) { n.level--; n.i /= 2; n.j /= 2; }
                ++changed;
            }
            if (!changed) break;
            // ⚠️ Y HAY QUE QUITAR LOS DESCENDIENTES, NO SOLO LOS DUPLICADOS. Bajar un nodo de nivel
            // 17 a 13 lo convierte en un ancestro que CUBRE 256 nodos finos que siguen en el
            // conjunto: se dibujarian dos superficies solapadas, que es peor que el escalon. El
            // conjunto dibujado tiene que ser una ANTICADENA — ningun nodo ancestro de otro.
            std::unordered_map<uint64_t, uint32_t> have;
            for (const NodeId& n : drawn) have[nodeKey(n)] = n.level;
            std::vector<NodeId> keep; keep.reserve(drawn.size());
            std::unordered_map<uint64_t, char> emitted;
            for (const NodeId& n : drawn) {
                bool covered = false;
                NodeId a = n;
                while (a.level > 0) {
                    a.level--; a.i /= 2; a.j /= 2;
                    if (have.find(nodeKey(a)) != have.end()) { covered = true; break; }
                }
                if (covered) continue;
                const uint64_t k = nodeKey(n);
                if (emitted.find(k) != emitted.end()) continue;
                emitted[k] = 1; keep.push_back(n);
            }
            drawn.swap(keep);
        }
        size_t badAfter = 0;
        const int worstAfter = audit(drawn, badAfter);
        std::printf("      EQUILIBRADO:    peor salto %d niveles · %zu parejas con mas de uno · "
                    "%zu nodos (eran %zu)\n", worstAfter, badAfter, drawn.size(), sel.size());
        std::printf("      -> a 2 niveles el escalon contra el vecino es 0,339 m; a 4, 1,372 m\n");

        CHECK(badBefore > 0, "CONTRAPRUEBA: la caida a ancestro SI rompe el 2:1 del conjunto "
                             "dibujado (si no lo rompiera, el equilibrado sobraria)");
        CHECK(badAfter == 0, "equilibrar bajando SI cierra el 2:1...");
        // ...pero a un precio que lo descarta como arreglo, y por eso se mide aqui: sin este numero
        // el equilibrado parece la solucion obvia y no lo es.
        CHECK(drawn.size() < sel.size() / 4, "...y se lleva por delante mas del 75 % de los nodos: 23 "
                                             "caidas arrastran al 90 % del terreno a bastarse. NO es "
                                             "el arreglo; el arreglo es que el pool garantice el padre");
    }
}

/**
 * @brief DESDE ÓRBITA: ¿hay píxeles de planeta sin nodo que los cubra?
 *
 * ── POR QUÉ NO BASTA CON `terrain_node_frustum_corners` ─────────────────────────────────────────
 *
 * Aquel test pregunta lo contrario de lo que hace falta: coge los nodos SELECCIONADOS y comprueba
 * que caen dentro del cono. Eso descarta que se dibuje de más, no que falte. Y llega hasta 50 km,
 * que no es órbita.
 *
 * Aquí se va al revés y desde donde se reporta el fallo: se lanzan rayos por el cuadro —esquinas
 * incluidas—, se corta con el planeta, y de cada impacto se pregunta si ALGÚN nodo seleccionado lo
 * cubre. Un impacto sin nodo es un agujero en pantalla, que es el sintoma.
 */
void test_terrain_node_orbit_coverage() {
    beginTest("terrain_node_orbit_coverage");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double aspect = 1920.0 / 1080.0;
    const double radPerPx = fovY / 1080.0;
    const double cone = nodeFrustumConeHalfAngle(fovY, aspect);
    const double t = std::tan(fovY * 0.5);

    // ¿Cubre `n` la direccion `d`? Se compara en coordenadas de CARA, que es donde el nodo es un
    // rectangulo exacto.
    auto covers = [&](const NodeId& n, const glm::dvec3& d) {
        PlanetFace f; double lx, ly;
        dirToCubeFaceClosed(d, f, lx, ly);
        if (f != n.face) return false;
        double x0, y0, x1, y1;
        nodeTexelFaceCoord(n, 0, 0, x0, y0);
        nodeTexelFaceCoord(n, TERRAIN_NODE_CELLS, TERRAIN_NODE_CELLS, x1, y1);
        const double e = 1e-12;
        return lx >= x0 - e && lx <= x1 + e && ly >= y0 - e && ly <= y1 + e;
    };

    // ⚠️ SE PRUEBAN LOS DOS PRESUPUESTOS. Con uno infinito se comprueba el CONO; con el del juego
    // (pool 4096 -> 3072) se comprueba lo que de verdad se dibuja. Si el cono sale limpio y el del
    // juego no, el recorte de esquinas no es de recorte: es de PRESUPUESTO.
    std::printf("    presup.  altitud    rayos al planeta   SIN nodo   nodos elegidos   peor angulo\n");
    size_t worstHoles = 0, worstHolesBudget = 0;
    for (size_t budget : { (size_t)200000, (size_t)3072 })
    for (double altKm : { 140.0, 500.0, 1000.0, 2000.0, 20000.0 }) {
        const glm::dvec3 up = glm::normalize(glm::dvec3(1.0, 0.3, 0.2));
        const glm::dvec3 cam = pc + up * (R + altKm * 1000.0);
        const glm::dvec3 fwd = glm::normalize(pc - cam);          // mirando al planeta (nadir)
        // ⚠️ LA BASE NO PUEDE SALIR DE `up`. Mirando al nadir `fwd == -up`, asi que `cross(fwd, up)`
        // es CERO y `normalize` devuelve NaN: los rayos salen NaN, las comparaciones con NaN son
        // todas falsas y el test contaba 2401 agujeros de 2401 — "no se ve el planeta" cuando lo que
        // no existia era el rayo. Se elige el eje mundial MENOS alineado con `fwd`.
        glm::dvec3 seed(0.0);
        { int small = 0;
          for (int k = 1; k < 3; ++k) if (std::fabs(fwd[k]) < std::fabs(fwd[small])) small = k;
          seed[small] = 1.0; }
        const glm::dvec3 rgt = glm::normalize(glm::cross(fwd, seed));
        const glm::dvec3 upv = glm::cross(rgt, fwd);

        std::vector<NodeId> sel;
        nodeSelectVisible(R, cam, pc, radPerPx, sel, budget, TERRAIN_NODE_ERROR_PX,
                          &fwd, cone, 5000.0);

        size_t hits = 0, holes = 0; double worstAng = 0.0;
        for (int iv = -24; iv <= 24; ++iv)
            for (int iu = -24; iu <= 24; ++iu) {
                const double sy = (double)iv / 24.0, sx = (double)iu / 24.0;
                const glm::dvec3 ray = glm::normalize(fwd + rgt * (sx * t * aspect) + upv * (sy * t));
                // Corte con la esfera de radio R.
                const glm::dvec3 oc = cam - pc;
                const double b = glm::dot(oc, ray), c = glm::dot(oc, oc) - R * R;
                const double disc = b * b - c;
                if (disc < 0.0) continue;                          // ese pixel es cielo
                const double tHit = -b - std::sqrt(disc);
                if (tHit < 0.0) continue;
                ++hits;
                const glm::dvec3 d = glm::normalize(oc + ray * tHit);
                bool ok = false;
                for (const NodeId& n : sel) if (covers(n, d)) { ok = true; break; }
                if (!ok) {
                    ++holes;
                    worstAng = std::max(worstAng,
                                        std::acos(glm::clamp(glm::dot(ray, fwd), -1.0, 1.0)) * 180.0 / 3.14159265358979);
                }
            }
        std::printf("    %7zu  %5.0f km   %16zu   %8zu   %14zu   %8.2f\n",
                    budget, altKm, hits, holes, sel.size(), worstAng);
        if (budget >= 200000) worstHoles = std::max(worstHoles, holes);
        else                  worstHolesBudget = std::max(worstHolesBudget, holes);
    }
    std::printf("    (el cono usado es de %.2f grados de semiangulo)\n", cone * 180.0 / 3.14159265358979);

    CHECK(worstHoles == 0, "con presupuesto de sobra, el CONO no deja ningun pixel del planeta sin "
                           "nodo: el recorte de frustum queda descartado como causa");
    std::printf("    -> con el presupuesto REAL del juego quedan %zu pixeles sin cubrir\n",
                worstHolesBudget);
    CHECK(worstHolesBudget == 0, "y con el presupuesto del juego tampoco (si esto falla, las esquinas "
                                 "que faltan son de PRESUPUESTO, no de recorte)");

    // ── LOS CONTADORES DE DESCARTE, QUE MENTIAN ─────────────────────────────────────────────────
    //
    // El log del juego decia `horizonte 0` en TODOS los frames. No era un hallazgo: el renderer
    // sacaba el desglose llamando dos veces mas al selector con los MISMOS argumentos y restando
    // tamaños, asi que la resta del horizonte era 0 por construccion y el cono se llevaba la culpa
    // de todo. Ahora los cuenta el selector por dentro. Aqui se comprueba que dan algo real.
    {
        const glm::dvec3 up = glm::normalize(glm::dvec3(1.0, 0.3, 0.2));
        const glm::dvec3 cam = pc + up * (R + 140000.0);
        const glm::dvec3 fwd = -up;                        // al nadir
        std::vector<NodeId> sel;
        size_t ch = 0, cf = 0;
        nodeSelectVisible(R, cam, pc, radPerPx, sel, 3072, TERRAIN_NODE_ERROR_PX,
                          &fwd, cone, 5000.0, nullptr, nullptr, &ch, &cf);
        std::printf("    a 140 km, mirando al nadir: %zu nodos · descartes horizonte %zu / cono %zu\n",
                    sel.size(), ch, cf);
        CHECK(ch > 0, "el contador de HORIZONTE cuenta de verdad (era siempre 0 por un bug del "
                      "diagnostico, no porque el horizonte no descartara nada)");
        CHECK(cf > 0, "y el del cono tambien");
    }
}

/**
 * @brief EL RANGO DEL NODO: sin él, todo se mide al nivel del mar.
 *
 * `generatePending` publicaba `NodeRange{}` para todos los nodos, así que `rangeOf` devolvía
 * inválido SIEMPRE. El log del juego lo dijo con `SIN RANGO 3070` de 3070, y de ahí viven tres
 * cosas: el criterio de subdivisión, el stride por nodo y la envolvente del frustum.
 *
 * ⚠️ Lo grave no es el rango: es que **un arreglo dado por bueno llevaba semanas sin funcionar**. El
 * criterio de subdivisión se cambió para medir al TERRENO y no al mar, se midió en test (donde el
 * rango se pasa a mano) y se dio por cerrado — pero en el juego el rango venía vacío, así que seguía
 * midiendo al mar. Un test que inyecta el dato bueno no prueba que el motor lo tenga.
 */
void test_terrain_node_range_published() {
    beginTest("terrain_node_range_published");
    const double R = 6371000.0;
    const glm::dvec3 center(0.0);
    // Un "planeta" de mentira con una meseta de 1200 m: basta para que el rango importe.
    struct Ctx { double R; } ctx{ R };
    auto heightFn = [](const glm::dvec3& dir, void* c) -> float {
        (void)c;
        return 1200.0f + 30.0f * (float)std::sin(dir.y * 4000.0);
    };

    const NodeId n{ PlanetFace::FRONT, 17, (1u << 17) / 2, (1u << 17) / 2 };
    const NodeRange sinFn = nodeEstimateRange(n, R, nullptr, nullptr);
    const NodeRange conFn = nodeEstimateRange(n, R, heightFn, &ctx);
    std::printf("    sin muestreador: valido=%d  (es lo que publicaba el motor)\n", (int)sinFn.valid());
    std::printf("    con muestreador: valido=%d  min %.1f  max %.1f  bound %.1f m\n",
                (int)conFn.valid(), conFn.minM, conFn.maxM, conFn.boundM());
    CHECK(!sinFn.valid(), "sin muestreador el rango sale invalido — el estado que tenia el juego");
    CHECK(conFn.valid() && conFn.maxM > 1200.0f, "con muestreador captura la cota real del terreno");

    // ── LO QUE CAMBIA AGUAS ABAJO ───────────────────────────────────────────────────────────────
    const glm::dvec3 d = nodeTexelDir(n, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2);
    const glm::dvec3 cam = center + d * (R + 1200.0 + 2.0);     // 2 m sobre la meseta
    const double radPerPx = 9.4e-4;

    const double errSea  = nodeScreenError(n, R, cam, center, radPerPx, 0.0);
    const double errReal = nodeScreenError(n, R, cam, center, radPerPx, (double)conFn.maxM);
    std::printf("    error en pantalla del nodo bajo los pies:\n");
    std::printf("      midiendo al NIVEL DEL MAR : %8.3f px\n", errSea);
    std::printf("      midiendo al TERRENO       : %8.3f px  (x%.0f)\n", errReal, errReal / errSea);

    const uint32_t skSea  = nodeStrideIndex(n, R, cam, center, 1.0, 4.0,
                                            TERRAIN_RING_FINE_CELL, 6, 0.0);
    const uint32_t skReal = nodeStrideIndex(n, R, cam, center, 1.0, 4.0,
                                            TERRAIN_RING_FINE_CELL, 6,
                                            (double)conFn.maxM);
    std::printf("      stride: al mar %u  ·  al terreno %u\n", 1u << skSea, 1u << skReal);

    CHECK(errReal > errSea * 100.0, "medir al mar SUBESTIMA el error del nodo bajo los pies en dos "
                                    "ordenes de magnitud: por eso subdividia de menos en las laderas");
    CHECK(skReal == 0 && skSea > 0, "y deja el stride basto donde la colision pide el fino — el "
                                    "`stride 4..4` del log");
}

/**
 * @brief LA DEMANDA CON RANGO VÁLIDO: ¿cuántos nodos pide ahora el selector?
 *
 * Publicar el rango de verdad (antes salía vacío) hace que el criterio mida la distancia al TERRENO
 * y no al mar. Eso es correcto — y multiplica la demanda, porque el error de un nodo bajo los pies
 * pasa de 0,53 px a 634 px. Si la demanda se dispara muy por encima del presupuesto, el selector
 * queda permanentemente hambriento y eso produce los DOS sintomas reportados a la vez:
 *
 *   · **PINCHOS** — vecinos con más de un nivel de diferencia; el geomorph solo cierra uno.
 *   · **PARPADEO** — quién se queda sin dividir cambia con el menor movimiento de cámara.
 *
 * Aquí se mide cuánto pide de verdad, con una cota de terreno realista.
 */
void test_terrain_node_demand_with_range() {
    beginTest("terrain_node_demand_with_range");
    const double R = 6371000.0;
    const glm::dvec3 center(0.0);
    const double radPerPx = 9.4e-4, errPx = 1.0;
    const glm::dvec3 d0 = glm::normalize(glm::dvec3(1.0, 0.35, 0.22));

    // Cota realista: el bake del juego va de -11 km a +9 km y el spawn esta a ~1026 m.
    struct RangeCtx { double R; };
    RangeCtx rc{ R };
    auto rangeFn = [](const NodeId& n, void* user) -> NodeRange {
        auto* c = static_cast<RangeCtx*>(user);
        NodeRange r;
        const uint32_t step = TERRAIN_NODE_CELLS / 4;
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += step)
            for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += step) {
                const glm::dvec3 dd = nodeTexelDir(n, u, v);
                const float h = 1026.0f + (float)terrainDetail(dd, c->R, 4.0f);
                r.minM = std::min(r.minM, h); r.maxM = std::max(r.maxM, h);
            }
        const float pad = 0.25f * (r.maxM - r.minM);
        r.minM -= pad; r.maxM += pad;
        return r;
    };

    std::printf("    altura de camara   demanda SIN rango   demanda CON rango   presupuesto\n");
    size_t worstDemand = 0;
    for (double alt : { 2.0, 200.0, 20000.0 }) {
        const glm::dvec3 cam = center + d0 * (R + 1026.0 + alt);
        std::vector<NodeId> a, b;
        nodeSelectVisible(R, cam, center, radPerPx, a, 400000, errPx);
        nodeSelectVisible(R, cam, center, radPerPx, b, 400000, errPx, nullptr, 0.0, 5000.0,
                          rangeFn, &rc);
        std::printf("    %14.0f m   %17zu   %17zu   %11d\n", alt, a.size(), b.size(), 3072);
        worstDemand = std::max(worstDemand, b.size());
    }
    std::printf("    -> el presupuesto con pool 4096 es 3072. Hambre = demanda/presupuesto.\n");
    std::printf("       Hambriento => nodos sin dividir junto a nodos finos (PINCHOS) y reparto\n");
    std::printf("       que cambia con la camara (PARPADEO).\n");

    std::printf("    hambre peor con errorPx=1: x%.1f\n", (double)worstDemand / 3072.0);

    // ── LA PALANCA: `errorPx` NO ES LA DENSIDAD DE TRIANGULOS ───────────────────────────────────
    //
    // `errorPx` decide a que nivel se subdivide, o sea cuantos TEXELES DE HEIGHTMAP hay por pixel.
    // `vertexPx` (4) decide cuantos pixeles hay entre VERTICES dibujados. Con errorPx=1 el heightmap
    // tiene un texel por pixel mientras se dibuja un vertice cada 4: **16 veces mas texeles que
    // vertices**. Subir errorPx baja los nodos sin bajar los triangulos, porque el stride se ajusta
    // solo (`screenWant = vertexPx/errorPx`) y mantiene la misma densidad EN PANTALLA.
    std::printf("\n    errorPx   demanda   hambre   texeles/pixel   stride lejano   triangulos\n");
    const glm::dvec3 cam2 = center + d0 * (R + 1026.0 + 2.0);
    for (double ep : { 1.0, 2.0, 3.0, 4.0 }) {
        std::vector<NodeId> v;
        nodeSelectVisible(R, cam2, center, radPerPx, v, 400000, ep, nullptr, 0.0, 5000.0,
                          rangeFn, &rc);
        const double screenWant = 4.0 / ep;
        uint32_t sk = 0; while (sk + 1 <= 6 && (double)(1u << (sk + 1)) <= screenWant) ++sk;
        const uint32_t stride = 1u << sk;
        // Triangulos = nodos * (celdas/stride)^2 * 2
        const double cells = (double)TERRAIN_NODE_CELLS / (double)stride;
        std::printf("    %7.0f   %7zu   %5.1fx   %13.0f   %13u   %8.1f M\n",
                    ep, v.size(), (double)v.size() / 3072.0, 1.0 / ep, stride,
                    (double)v.size() * cells * cells * 2.0 / 1e6);
    }
    std::printf("    -> subir errorPx baja los NODOS (y la VRAM) sin cambiar la densidad en pantalla:\n"
                "       el stride compensa. Lo que baja es el heightmap sobrante.\n");

    CHECK(worstDemand > 3072, "con errorPx=1 y el rango publicado, la demanda SUPERA el presupuesto: "
                              "el selector va hambriento y de ahi salen los pinchos y el parpadeo");

    // ── LO QUE NO SE PUEDE PERDER AL SUBIR errorPx ──────────────────────────────────────────────
    //
    // Subir el umbral recorta el campo medio; si recortara tambien el campo CERCANO, se llevaria por
    // delante la paridad con la colision, que es lo que costo media sesion. Se comprueba con el valor
    // POR DEFECTO —no con uno pasado a mano—, que es lo que corre el motor.
    {
        std::vector<NodeId> v;
        nodeSelectVisible(R, cam2, center, radPerPx, v, 400000, TERRAIN_NODE_ERROR_PX,
                          nullptr, 0.0, 5000.0, rangeFn, &rc);
        uint32_t deepest = 0; double nearestDeep = 1e300;
        for (const NodeId& n : v) {
            deepest = std::max(deepest, n.level);
            if (n.level == nodeMaxLevelFor(R)) {
                const glm::dvec3 p = center + nodeTexelDir(n, TERRAIN_NODE_CELLS/2,
                                                           TERRAIN_NODE_CELLS/2) * (R + 1026.0);
                nearestDeep = std::min(nearestDeep, glm::length(p - cam2));
            }
        }
        // Y el stride del nodo bajo los pies, con el errorPx por defecto.
        const uint32_t Lmax = nodeMaxLevelFor(R);
        const NodeId foot{ PlanetFace::FRONT, Lmax, (1u << Lmax) / 2, (1u << Lmax) / 2 };
        const glm::dvec3 dF = nodeTexelDir(foot, TERRAIN_NODE_CELLS/2, TERRAIN_NODE_CELLS/2);
        const glm::dvec3 camF = center + dF * (R + 1026.0 + 2.0);
        const uint32_t sk = nodeStrideIndex(foot, R, camF, center, TERRAIN_NODE_ERROR_PX, 4.0,
                                            TERRAIN_RING_FINE_CELL, 6, 1026.0);
        // ── LA DEMANDA CON EL CONO, que es la que decide el TAMANO DEL POOL ─────────────────────────
    //
    // Todo lo de arriba mide 360 grados. El motor recorta con el cono del frustum, asi que el pool
    // solo tiene que aguantar lo que cabe en pantalla. Este es el numero del que sale el valor por
    // defecto de `capacity`, y hasta ahora ese valor (1024 -> presupuesto 768) era heredado de
    // cuando el criterio media al nivel del mar y pedia mucho menos.
    {
        const double fovY = 60.0 * 3.14159265358979 / 180.0;
        const double cone = nodeFrustumConeHalfAngle(fovY, 1920.0 / 1080.0);
        std::printf("\n    con el cono del frustum (lo que de verdad se pide):\n");
        std::printf("      altura   demanda   pool necesario (demanda/0,75)\n");
        size_t worstCone = 0;
        for (double alt : { 2.0, 200.0, 2000.0, 20000.0 }) {
            const glm::dvec3 c2 = center + d0 * (R + 1026.0 + alt);
            const glm::dvec3 fwd = glm::normalize(glm::cross(d0, glm::dvec3(0,0,1)));  // horizonte
            std::vector<NodeId> v;
            nodeSelectVisible(R, c2, center, radPerPx, v, 400000, TERRAIN_NODE_ERROR_PX,
                              &fwd, cone, 5000.0, rangeFn, &rc);
            std::printf("      %5.0f m   %7zu   %zu\n", alt, v.size(),
                        (size_t)((double)v.size() / 0.75));
            worstCone = std::max(worstCone, v.size());
        }
        std::printf("      -> pool necesario: %zu huecos = %.0f MB de VRAM a 130 KB/nodo\n",
                    (size_t)((double)worstCone / 0.75),
                    (double)worstCone / 0.75 * 130.0 / 1024.0);
    }

    std::printf("\n    con el errorPx POR DEFECTO (%.0f): nivel mas fino %u · demanda %zu · "
                    "stride bajo los pies %u\n", TERRAIN_NODE_ERROR_PX, deepest, v.size(), 1u << sk);
        CHECK(deepest == nodeMaxLevelFor(R), "cerca se sigue llegando al nivel MAXIMO: subir el "
                                                 "umbral recorta el campo medio, no el que se pisa");
        CHECK(sk == 0, "y el stride bajo los pies sigue siendo 1, que es lo que iguala render y "
                       "colision (si esto falla, subir errorPx se ha llevado la paridad)");
        CHECK(v.size() < 3072, "y la demanda cabe en el presupuesto: sin hambre no hay pinchos ni "
                               "parpadeo por reparto");
    }
}

/**
 * @brief PARPADEO POR RANGO HEREDADO: el pool frío y el pool caliente eligen árboles distintos.
 *
 * `rangeOf` devuelve el rango del ANCESTRO cuando el nodo no está residente. Para acotar (la
 * envolvente del frustum) es correcto: el área del hijo está dentro de la del padre. Para el
 * CRITERIO DE SUBDIVISIÓN no lo es, y en la dirección mala:
 *
 *   · El `maxM` del ancestro es el pico de un área enorme. Un nodo en un valle hereda la altura de
 *     la montaña de al lado, así que `nodeScreenError` lo coloca a esa altura, más cerca de la
 *     cámara de lo que está, y decide subdividir.
 *   · Cuando el pool lo genera y pasa a tener su rango PROPIO —mucho más ajustado— el error cae y
 *     el nodo se deshace.
 *
 * Generar cambia la decisión que causó la generación. Eso es un bucle, y se ve como parpadeo; y
 * mientras dura, unos nodos van finos y sus vecinos no, que es lo que abre los pinchos.
 *
 * Aquí se compara el árbol que sale con el pool FRÍO (todo heredado del nivel 4) contra el que sale
 * con el pool CALIENTE (cada nodo con su rango). Si son muy distintos, el bucle es real.
 */
void test_terrain_node_inherited_range_flicker() {
    beginTest("terrain_node_inherited_range_flicker");
    const double R = 6371000.0;
    const glm::dvec3 center(0.0);
    const double radPerPx = 9.4e-4;
    const glm::dvec3 d0 = glm::normalize(glm::dvec3(1.0, 0.35, 0.22));
    const glm::dvec3 cam = center + d0 * (R + 1026.0 + 2.0);

    struct Ctx { int inheritFrom; };            // 0 = rango propio; >0 = heredado de ese nivel
    auto rangeAt = [](const NodeId& n, void* user) -> NodeRange {
        auto* c = static_cast<Ctx*>(user);
        NodeId q = n;
        while ((int)q.level > c->inheritFrom && q.level > 0) { q.level--; q.i /= 2; q.j /= 2; }
        NodeRange r;
        const uint32_t step = TERRAIN_NODE_CELLS / 4;
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += step)
            for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += step) {
                const float h = 1026.0f
                              + (float)terrainDetail(nodeTexelDir(q, u, v),
                                                                     6371000.0, 4.0f);
                r.minM = std::min(r.minM, h); r.maxM = std::max(r.maxM, h);
            }
        const float pad = 0.25f * (r.maxM - r.minM);
        r.minM -= pad; r.maxM += pad;
        return r;
    };

    std::printf("    rango heredado desde   nodos   nivel max   maxM del nodo bajo los pies\n");
    std::vector<NodeId> warm;
    size_t coldCount = 0;
    for (int from : { 99, 12, 8, 4 }) {         // 99 = cada nodo con SU rango (pool caliente)
        Ctx c{ from };
        std::vector<NodeId> v;
        nodeSelectVisible(R, cam, center, radPerPx, v, 400000, TERRAIN_NODE_ERROR_PX,
                          nullptr, 0.0, 5000.0, rangeAt, &c);
        uint32_t mx = 0; for (const NodeId& n : v) mx = std::max(mx, n.level);
        const NodeId foot{ PlanetFace::FRONT, 17, (1u << 17) / 2, (1u << 17) / 2 };
        const NodeRange fr = rangeAt(foot, &c);
        std::printf("    %20s   %5zu   %9u   %20.1f m\n",
                    from == 99 ? "su propio nivel" : (from == 12 ? "nivel 12"
                                 : (from == 8 ? "nivel 8" : "nivel 4")),
                    v.size(), mx, fr.maxM);
        if (from == 99) warm = v;
        else coldCount = v.size();
    }

    // ¿Cuanto se parecen los dos arboles? Es la magnitud del salto entre un frame y el siguiente.
    {
        Ctx c{ 4 };
        std::vector<NodeId> cold;
        nodeSelectVisible(R, cam, center, radPerPx, cold, 400000, TERRAIN_NODE_ERROR_PX,
                          nullptr, 0.0, 5000.0, rangeAt, &c);
        std::unordered_map<uint64_t, char> inWarm;
        for (const NodeId& n : warm) inWarm[nodeKey(n)] = 1;
        size_t shared = 0;
        for (const NodeId& n : cold) if (inWarm.count(nodeKey(n))) ++shared;
        const double pct = 100.0 * (double)shared / (double)std::max<size_t>(cold.size(), 1);
        std::printf("    nodos EN COMUN entre el arbol frio y el caliente: %zu de %zu (%.1f %%)\n",
                    shared, cold.size(), pct);
        std::printf("    -> lo que NO comparten se dibuja distinto en frames consecutivos\n");
        CHECK(coldCount > 0, "el arbol frio se calcula");
        CHECK(pct > 90.0, "el rango heredado NO cambia sustancialmente el arbol elegido (si baja de "
                          "90 %%, generar un nodo cambia la decision que lo genero: eso es el bucle "
                          "que se ve como parpadeo)");
    }
}
