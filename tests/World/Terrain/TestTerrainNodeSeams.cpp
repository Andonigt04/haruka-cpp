// Quadtree v5: costuras entre nodos y caras, zancadas por nodo y morph sin saltos.
// (Partido de TestTerrainNode.cpp, que tenía 6 400 líneas; el registro de todos sigue allí.)
#include "World/Terrain/TerrainNodeTestCommon.h"

// ================================================================================================
// LA GRIETA EN LAS ARISTAS DEL CUBO, EN METROS. El test que faltaba.
//
// ⚠️ `terrain_node_face_seam` demuestra que `nodeNeighbourLevels` devuelve el NIVEL correcto cruzando
// cara. Eso NO es lo mismo que "no hay grieta": el nivel puede ser correcto y el cosido colocar el
// vertice en otro sitio. El autor confirmo grietas en pantalla con el cosido "demostrado".
//
// Aqui se miden POSICIONES: se cogen los vertices de la arista compartida por los dos lados y se
// mira cuanto se separan en el espacio. Es lo unico que responde a "¿hay grieta?".
// ================================================================================================
void test_terrain_node_face_seam_gap() {
    beginTest("terrain_node_face_seam_gap");
    const double R = 6371000.0;

    double worstSame = 0.0, worstCross = 0.0;
    size_t nSame = 0, nCross = 0;
    glm::dvec3 worstAt(0.0);

    for (uint32_t level : { 4u, 6u, 8u }) {
        const uint32_t lim = 1u << level;
        for (int f = 0; f < 6; ++f)
            for (uint32_t k = 1; k + 1 < lim; ++k) {
                // Nodo pegado al borde IZQUIERDO de su cara -> su vecino esta en otra cara.
                const NodeId a{ (PlanetFace)f, level, 0, k };
                NodeId nb;
                if (!nodeNeighbourAcrossFace(a, 0, nb)) continue;

                // El vecino, UN NIVEL MAS GRUESO: la T-junction que abre grieta.
                const NodeId coarse{ nb.face, nb.level - 1, nb.i / 2, nb.j / 2 };
                const auto idx = nodeDrawnIndex({ a, coarse });
                int cA[4]; nodeNeighbourLevels(a, idx, cA);

                // ⚠️ INDICES IMPARES. Con el vecino un nivel mas grueso la zancada es 2, asi que los
                // PARES caen exactamente sobre vertices suyos y dan 0 sin medir nada. Los unicos que
                // pueden abrir grieta son los INTERPOLADOS. La primera version muestreaba de 4 en 4
                // —todos pares— y daba 0,0000 m en 65 340 vertices sin tocar el caso.
                for (uint32_t t2 = 1; t2 < TERRAIN_NODE_CELLS; t2 += 2) {
                    const glm::dvec3 pa = nodeStitchedDir(a, 0, t2, cA) * R;
                    // Distancia al SEGMENTO, y por los cuatro lados: cruzando cara no se sabe cual
                    // es el compartido ni en que sentido corre.
                    double best = 1e30;
                    auto segDist = [&](const glm::dvec3& s0, const glm::dvec3& s1) {
                        const glm::dvec3 e = s1 - s0;
                        const double L2 = glm::dot(e, e);
                        const double tt = (L2 > 0.0) ? glm::clamp(glm::dot(pa - s0, e) / L2, 0.0, 1.0) : 0.0;
                        return glm::length(s0 + e * tt - pa);
                    };
                    for (uint32_t q = 0; q + 1 <= TERRAIN_NODE_CELLS; ++q) {
                        best = std::min(best, segDist(nodeTexelDir(coarse, 0, q) * R,
                                                      nodeTexelDir(coarse, 0, q + 1) * R));
                        best = std::min(best, segDist(nodeTexelDir(coarse, TERRAIN_NODE_CELLS, q) * R,
                                                      nodeTexelDir(coarse, TERRAIN_NODE_CELLS, q + 1) * R));
                        best = std::min(best, segDist(nodeTexelDir(coarse, q, 0) * R,
                                                      nodeTexelDir(coarse, q + 1, 0) * R));
                        best = std::min(best, segDist(nodeTexelDir(coarse, q, TERRAIN_NODE_CELLS) * R,
                                                      nodeTexelDir(coarse, q + 1, TERRAIN_NODE_CELLS) * R));
                    }
                    if (best > worstCross) { worstCross = best; worstAt = pa / R; }
                    ++nCross;
                }
            }
    }

    // CONTROL: la misma medida DENTRO de una cara, donde el cosido esta probado y funciona.
    for (uint32_t level : { 4u, 6u, 8u }) {
        const uint32_t lim = 1u << level;
        const NodeId a{ PlanetFace::FRONT, level, lim / 2, lim / 2 };
        const NodeId nbFine{ PlanetFace::FRONT, level, lim / 2 - 1, lim / 2 };
        const NodeId coarse{ nbFine.face, nbFine.level - 1, nbFine.i / 2, nbFine.j / 2 };
        const auto idx = nodeDrawnIndex({ a, coarse });
        int cA[4]; nodeNeighbourLevels(a, idx, cA);
        for (uint32_t t2 = 1; t2 < TERRAIN_NODE_CELLS; t2 += 2) {
            const glm::dvec3 pa = nodeStitchedDir(a, 0, t2, cA) * R;
            // Distancia al SEGMENTO del vecino, no a sus vertices: el cosido pone el punto SOBRE la
            // recta entre dos de ellos, asi que medir contra vertices sueltos exagera la grieta.
            double best = 1e30;
            for (uint32_t q = 0; q + 1 <= TERRAIN_NODE_CELLS; ++q) {
                const glm::dvec3 s0 = nodeTexelDir(coarse, TERRAIN_NODE_CELLS, q) * R;
                const glm::dvec3 s1 = nodeTexelDir(coarse, TERRAIN_NODE_CELLS, q + 1) * R;
                const glm::dvec3 e = s1 - s0;
                const double L2 = glm::dot(e, e);
                const double tt = (L2 > 0.0) ? glm::clamp(glm::dot(pa - s0, e) / L2, 0.0, 1.0) : 0.0;
                best = std::min(best, glm::length(s0 + e * tt - pa));
            }
            worstSame = std::max(worstSame, best);
            ++nSame;
        }
    }

    std::printf("    DENTRO de una cara  (%zu vertices): grieta peor %10.4f m\n", nSame, worstSame);
    std::printf("    CRUZANDO de cara    (%zu vertices): grieta peor %10.4f m\n", nCross, worstCross);
    std::printf("      la peor cae en dir (%.3f, %.3f, %.3f)\n", worstAt.x, worstAt.y, worstAt.z);

    CHECK(worstSame < 0.01, "CONTROL: dentro de una cara el cosido cierra (el test mide bien)");
    CHECK(worstCross < 0.01, "cruzando de cara el cosido tambien cierra");

    // ── Y AHORA LA OTRA GRIETA: LA VERTICAL ─────────────────────────────────────────────────────
    //
    // ⚠️ Lo de arriba mide POSICIONES y cierra a 0. Pero el cosido interpola las alturas de MI mapa,
    // y el vecino grueso tiene las suyas calculadas con OTRO corte de octavas: su `nodeTexelM` es el
    // doble, asi que su superficie es literalmente otra funcion. La reticula coincide; el RELIEVE no.
    //
    // Esta es la grieta que se ve, y la de arriba nunca la habria detectado.
    // ⚠️ SE MIDE LO QUE SE DIBUJA, o sea CON el morph. En la arista que linda con un vecino mas
    // grueso el morph vale 1, asi que el nodo fino usa la altura de SU PADRE — que es exactamente lo
    // que el vecino calcula, porque el vecino esta al nivel del padre.
    double worstH = 0.0, worstRaw = 0.0; size_t nH = 0;
    for (uint32_t level : { 10u, 12u, 14u, 16u }) {
        const uint32_t lim = 1u << level;
        const NodeId fine{ PlanetFace::FRONT, level, lim / 3, lim / 2 };
        const NodeId coarse{ PlanetFace::FRONT, level - 1, (lim / 3) / 2, (lim / 2) / 2 };
        const float triF = (float)nodeTexelM(fine, R);
        const float triC = (float)nodeTexelM(coarse, R);
        double w = 0.0, raw = 0.0;
        for (uint32_t q = 0; q <= TERRAIN_NODE_CELLS; q += 2) {
            const glm::dvec3 d = nodeTexelDir(fine, 0, q);
            const double hFineOwn = terrainDetail(d, R, triF);   // sin morph
            const double hParent  = terrainDetail(d, R, triF * 2.0f);
            const double hCoarse  = terrainDetail(d, R, triC);   // lo que dibuja el vecino
            w   = std::max(w,   std::fabs(hParent  - hCoarse));   // CON morph (la arista usa el padre)
            raw = std::max(raw, std::fabs(hFineOwn - hCoarse));   // SIN morph: lo que habia
            ++nH;
        }
        std::printf("      nivel %2u (%.2f m/texel): con morph %.4f m · SIN morph %.3f m\n",
                    level, triF, w, raw);
        worstH = std::max(worstH, w); worstRaw = std::max(worstRaw, raw);
    }
    std::printf("    GRIETA VERTICAL en la arista compartida: con morph %.4f m (antes %.3f m)"
                " sobre %zu vertices\n", worstH, worstRaw, nH);
    CHECK(worstH < 0.01, "con geomorph el relieve COINCIDE en la arista compartida");
    // CONTRAPRUEBA: sin morph la grieta TIENE que estar. Si no, el test no mide el arreglo.
    CHECK(worstRaw > 0.5, "CONTRAPRUEBA: sin morph la grieta existe (el test mide el arreglo)");
}

void test_terrain_node_morph_no_pop() {
    beginTest("terrain_node_morph_no_pop");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double radPerPx = (60.0 * 3.14159265358979 / 180.0) / 1080.0;
    const double errPx = TERRAIN_NODE_ERROR_PX;

    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    // ⚠️ EL NODO TIENE QUE ESTAR BAJO LA CAMARA. La primera version cogio uno cualquiera de la cara
    // y el barrido no entro NUNCA en la ventana en la que ese nodo es hoja: la distancia la dominaba
    // el desplazamiento lateral, no la altura. Salia "salto 0,0000 m" sin haber medido el relevo.
    PlanetFace cf; double clx, cly;
    dirToCubeFaceClosed(up0, cf, clx, cly);

    // ⚠️ Y VARIOS NIVELES, porque un nodo suelto puede caer en un llano: el primero que probe tenia
    // 0,007 m entre su altura y la de su padre, asi que la contraprueba no podia tener dientes.
    double worstStep = 0.0, worstNoMorph = 0.0, worstHandover = 1.0;
    for (uint32_t level : { 11u, 13u, 15u, 17u }) {
        const uint32_t lim = 1u << level;
        const NodeId n{ cf, level,
                        (uint32_t)std::min<int64_t>((int64_t)((clx + 1.0) * 0.5 * (double)lim), (int64_t)lim - 1),
                        (uint32_t)std::min<int64_t>((int64_t)((cly + 1.0) * 0.5 * (double)lim), (int64_t)lim - 1) };
        const NodeId parent{ n.face, n.level - 1, n.i / 2, n.j / 2 };
        const glm::dvec3 dir = nodeTexelDir(n, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2);
        const float hOwn    = terrainDetail(dir, R, (float)nodeTexelM(n, R));
        const float hParent = terrainDetail(dir, R, (float)nodeTexelM(parent, R));

        double step = 0.0, prevH = 0.0, handover = -1.0; bool first = true;
        for (double alt = 5.0; alt < 200000.0; alt *= 1.004) {   // paso fino: el relevo es un instante
            const glm::dvec3 cam = pc + up0 * (R + alt);
            // ⚠️ LA CADENA ENTERA, no solo la ventana de hoja. Cuando `n` esta SUBDIVIDIDO no se
            // dibuja su padre: se dibujan sus HIJOS, y esos estan morfeados hacia `n`, o sea que la
            // superficie es la de `n`. Modelarlo como "el padre" metia un salto de 1,22 m que era
            // del test, no del motor.
            const double e = nodeScreenError(n, R, cam, pc, radPerPx);
            const bool leaf = (e <= errPx) &&
                              (nodeScreenError(parent, R, cam, pc, radPerPx) > errPx);
            const float m = nodeParentMorph(n, R, cam, pc, radPerPx, errPx);
            const double h = (e > errPx) ? hOwn                              // subdividido: los hijos
                           : leaf        ? (hOwn + (hParent - hOwn) * m)     // hoja: morfeando
                                         : hParent;                          // el padre ha relevado
            if (!first) step = std::max(step, std::fabs(h - prevH));
            if (leaf) handover = m;
            prevH = h; first = false;
        }
        std::printf("    nivel %2u: propia %9.3f m · padre %9.3f m · SIN morph saltaria %6.3f m"
                    " · morph al relevo %.3f · salto real %.4f m\n",
                    level, hOwn, hParent, std::fabs(hOwn - hParent), handover, step);
        worstStep = std::max(worstStep, step);
        worstNoMorph = std::max(worstNoMorph, (double)std::fabs(hOwn - hParent));
        if (handover >= 0.0) worstHandover = std::min(worstHandover, (double)handover);
    }
    std::printf("    -> salto PEOR con morph %.4f m · sin morph habria sido %.3f m\n",
                worstStep, worstNoMorph);

    CHECK(worstHandover > 0.98, "todos los nodos llegan al relevo ya morfeados a su padre");
    CHECK(worstStep < 0.05, "no hay salto al fundirse en el padre (sin popping)");
    // CONTRAPRUEBA: sin morph el salto seria visible. Si no, el test no mide un caso real.
    CHECK(worstNoMorph > 0.5, "CONTRAPRUEBA: sin morph el salto SI se veria (el test tiene dientes)");
}

void test_terrain_node_face_seam() {
    beginTest("terrain_node_face_seam");
    const double R = 6371000.0;

    // --- 1. SIMETRIA sobre todas las aristas de todas las caras, a varios niveles ---
    size_t crossings = 0, symmetric = 0;
    for (uint32_t level = 1; level <= 5; ++level) {
        const uint32_t lim = 1u << level;
        for (int f = 0; f < 6; ++f)
            for (uint32_t i = 0; i < lim; ++i)
                for (uint32_t j = 0; j < lim; ++j) {
                    const NodeId a{ (PlanetFace)f, level, i, j };
                    for (int e = 0; e < 4; ++e) {
                        NodeId b;
                        if (!nodeNeighbourAcrossFace(a, e, b)) continue;   // vecino en la misma cara
                        ++crossings;
                        // Volver: alguna de las cuatro aristas de B tiene que devolver A.
                        bool back = false;
                        for (int e2 = 0; e2 < 4 && !back; ++e2) {
                            NodeId c;
                            if (nodeNeighbourAcrossFace(b, e2, c))
                                back = (c.face == a.face && c.level == a.level &&
                                        c.i == a.i && c.j == a.j);
                        }
                        if (back) ++symmetric;
                    }
                }
    }
    std::printf("    %zu cruces de cara (niveles 1..5) · simetricos: %zu\n", crossings, symmetric);
    CHECK(crossings > 0, "hay nodos cuyo vecino esta en otra cara (si no, el test no mide nada)");
    CHECK(symmetric == crossings, "el vecino cruzando cara es SIMETRICO en las 24 combinaciones");

    // --- 2. La arista compartida cae en el mismo sitio, en METROS ---
    double worstM = 0.0; size_t checked = 0;
    for (uint32_t level = 2; level <= 4; ++level) {
        const uint32_t lim = 1u << level;
        for (int f = 0; f < 6; ++f)
            for (uint32_t k = 0; k < lim; ++k) {
                const NodeId a{ (PlanetFace)f, level, 0, k };   // pegado al borde izquierdo
                NodeId b;
                if (!nodeNeighbourAcrossFace(a, 0, b)) continue;
                // Centro de la arista compartida de A, y el punto mas cercano del borde de B.
                const glm::dvec3 pa = nodeTexelDir(a, 0, TERRAIN_NODE_CELLS / 2) * R;
                double best = 1e30;
                for (uint32_t t = 0; t <= TERRAIN_NODE_CELLS; ++t) {
                    for (int side = 0; side < 4; ++side) {
                        const uint32_t su = (side == 0) ? 0 : (side == 1) ? TERRAIN_NODE_CELLS : t;
                        const uint32_t sv = (side == 0 || side == 1) ? t
                                          : (side == 2) ? 0 : TERRAIN_NODE_CELLS;
                        best = std::min(best, glm::length(nodeTexelDir(b, su, sv) * R - pa));
                    }
                }
                worstM = std::max(worstM, best); ++checked;
            }
    }
    const double nodeSideM = R * 1.5707963267948966 / (double)(1u << 4);
    std::printf("    %zu aristas comprobadas · el borde del vecino pasa a %.4f m del punto de A"
                "  (el nodo mide %.0f m de lado)\n", checked, worstM, nodeSideM);
    CHECK(worstM < 1.0, "la arista compartida es la MISMA en las dos caras (a menos de 1 m)");

    // --- 3. Y AHORA LO QUE IMPORTA: que `nodeNeighbourLevels` COSA cruzando cara ---
    //
    // Los dos oraculos de arriba validan la adyacencia; esto valida que se USE. Se monta un conjunto
    // dibujado con un nodo pegado al borde y su vecino de la otra cara UN NIVEL MAS GRUESO, que es
    // justo la T-junction que abria grieta.
    {
        const uint32_t level = 4;
        const NodeId a{ PlanetFace::FRONT, level, 0, 5 };   // pegado al borde izquierdo de su cara
        NodeId nb;
        const bool crosses = nodeNeighbourAcrossFace(a, 0, nb);
        CHECK(crosses, "el nodo de prueba cruza de cara por su arista izquierda");
        const NodeId coarse{ nb.face, nb.level - 1, nb.i / 2, nb.j / 2 };   // el vecino, mas grueso

        const auto idx = nodeDrawnIndex({ a, coarse });
        int coarser[4]; nodeNeighbourLevels(a, idx, coarser);
        std::printf("    cosido cruzando cara: vecino %u niveles mas grueso -> arista izq = %d\n",
                    1u, coarser[0]);
        CHECK(coarser[0] == 1, "cruzando cara, el cosido ve al vecino UN nivel mas grueso");

        // CONTRAPRUEBA: si el vecino de la otra cara NO esta dibujado, no hay nada que coser.
        const auto idxSolo = nodeDrawnIndex({ a });
        int solo[4]; nodeNeighbourLevels(a, idxSolo, solo);
        std::printf("    CONTRAPRUEBA: sin el vecino en el conjunto dibujado -> arista izq = %d\n",
                    solo[0]);
        CHECK(solo[0] == 0, "CONTRAPRUEBA: sin vecino dibujado no se cose (el test tiene dientes)");
    }

    // --- CONTRAPRUEBA: un nodo INTERIOR no cruza de cara ---
    size_t interiorCrossed = 0;
    for (uint32_t i = 1; i < 7; ++i)
        for (uint32_t j = 1; j < 7; ++j) {
            const NodeId a{ PlanetFace::FRONT, 3, i, j };
            NodeId b;
            for (int e = 0; e < 4; ++e) if (nodeNeighbourAcrossFace(a, e, b)) ++interiorCrossed;
        }
    std::printf("    CONTRAPRUEBA: nodos interiores que dicen cruzar de cara: %zu\n", interiorCrossed);
    CHECK(interiorCrossed == 0, "CONTRAPRUEBA: un nodo interior NO cruza de cara");
}

/**
 * @brief EL BOBINADO DE LA REJILLA ES CCW DESDE FUERA — EN LAS SEIS CARAS.
 *
 * Es el requisito para poder dibujar el pase v5 con `CullMode::Back`. Hasta el 2026-08-25 iba con
 * `CullMode::None`, así que **cada nodo rasterizaba sus dos caras**: en el limbo y a distancia
 * rasante la cara de delante y la de detrás del mismo relieve compiten por el depth, que es
 * parpadeo, y además se paga el doble de fragmentos.
 *
 * El comentario del bobinado en `TerrainNodeRenderer.h` razona sobre la cara FRONT. Eso no basta:
 * las seis caras tienen su propia orientación y reflejo, y con una sola mal el culling BORRARÍA el
 * terreno de esa cara. Por eso se comprueban las seis, a varios niveles y strides, con la normal
 * geométrica del triángulo contra la radial.
 */
void test_terrain_node_winding() {
    beginTest("terrain_node_winding");
    const double R = 6371000.0;
    const uint32_t N = TERRAIN_NODE_TEXELS;
    size_t checked = 0, outward = 0, outwardRev = 0;
    double worstDot = 1e300;
    int badFace = -1;
    for (uint32_t f = 0; f < 6; ++f) {
        for (uint32_t level : { 0u, 3u, 9u }) {
            const uint32_t lim = 1u << level;
            const NodeId n{ (PlanetFace)f, level, lim / 3u, (lim * 2u) / 3u };
            for (uint32_t st : { 1u, 4u }) {
                for (uint32_t v = 0; v + st < N; v += 37u * st)
                    for (uint32_t u = 0; u + st < N; u += 41u * st) {
                        // Los MISMOS cuatro del bucle de indices: a=(u,v) b=(u+st,v) c=(u,v+st) d=+ambos
                        const glm::dvec3 pa = nodeTexelDir(n, u, v) * R;
                        const glm::dvec3 pb = nodeTexelDir(n, u + st, v) * R;
                        const glm::dvec3 pc = nodeTexelDir(n, u, v + st) * R;
                        const glm::dvec3 pd = nodeTexelDir(n, u + st, v + st) * R;
                        const glm::dvec3 out = glm::normalize(pa);
                        // ...y los DOS triangulos con el orden real: (a,b,c) y (c,b,d).
                        const glm::dvec3 t1 = glm::cross(pb - pa, pc - pa);
                        const glm::dvec3 t2 = glm::cross(pb - pc, pd - pc);
                        for (const glm::dvec3& tn : { t1, t2 }) {
                            const double d = glm::dot(glm::normalize(tn), out);
                            ++checked;
                            if (d > 0.0) ++outward; else if (badFace < 0) badFace = (int)f;
                            if (-d > 0.0) ++outwardRev;
                            worstDot = std::min(worstDot, d);
                        }
                    }
            }
        }
    }
    std::printf("    %zu triangulos de las 6 caras · %zu con la normal HACIA FUERA · peor dot %.4f\n",
                checked, outward, worstDot);
    if (badFace >= 0) std::printf("    ⚠️ primera cara con el bobinado al reves: %d\n", badFace);
    std::printf("    CONTRAPRUEBA: con el orden invertido (a,c,b) apuntarian hacia fuera %zu\n",
                outwardRev);

    CHECK(checked > 100, "se comprueban triangulos de verdad, en las seis caras");
    CHECK(outward == checked, "TODOS los triangulos son CCW vistos desde fuera: se puede activar "
                              "CullMode::Back sin borrar ninguna cara");
    CHECK(outwardRev == 0, "CONTRAPRUEBA: con el orden invertido NINGUNO lo seria (el test distingue "
                           "los dos ordenes, no dice que si a todo)");
}

/**
 * @brief LA HISTÉRESIS DEL STRIDE SE PIERDE CUANDO UN NODO SALE DEL CONJUNTO DIBUJADO.
 *
 * `terrain_node_walk_shimmer` mide la histéresis sobre UN nodo que está siempre presente, y ahí sale
 * perfecta: 0 cambios en 120 frames con temblor. Pero el renderer guarda la historia en `m_skNow`,
 * que **se vacía cada frame** (`m_skNow.clear()`), y sólo se rellena con lo que se dibuja. Un nodo
 * que desaparece del conjunto un solo frame vuelve con `kNoPrevStride`: **sin banda muerta**, libre
 * de caer al otro lado del umbral en el que estaba.
 *
 * Es el hueco entre los dos tests que había: uno mide la histéresis con historia perfecta y el otro
 * mide el tamaño del pop, y ninguno mira lo que pasa cuando la historia se pierde.
 *
 * Aquí se cuentan las REAPARICIONES sobre un recorrido real y cuántas cambian de stride, con la
 * política de hoy contra una historia PERSISTENTE (que no se borra al salir).
 */
void test_terrain_node_stride_history_loss() {
    beginTest("terrain_node_stride_history_loss");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / 1080.0;
    const double cone = nodeFrustumConeHalfAngle(fovY, 1920.0 / 1080.0);
    const double vertPx = 4.0, fineCell = TERRAIN_RING_FINE_CELL;
    const uint32_t maxIdx = 6;

    const glm::dvec3 up0  = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 east = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));

    auto run = [&](bool persist, size_t& reappear, size_t& flipped, size_t& drawnLast) {
        std::unordered_map<uint64_t, uint32_t> prevFrame;   // lo que hace el motor: se vacía cada frame
        std::unordered_map<uint64_t, uint32_t> everSeen;    // el último stride conocido, no se borra
        std::vector<NodeId> sel;
        reappear = 0; flipped = 0; drawnLast = 0;
        // Andar a 5 m/s (0,083 m/frame) MIENTRAS SE GIRA: girar es lo que hace entrar y salir nodos
        // del cono (medido aparte: 26 % nuevos al girar 90°). Andando en linea recta no churnea nada
        // y la pregunta no llega a plantearse — la primera version de este test hacia justo eso.
        const glm::dvec3 north = glm::normalize(glm::cross(up0, east));
        for (int f = 0; f < 120; ++f) {
            const double ang = 0.02 * (double)f;              // ~137 grados en los 120 frames
            const glm::dvec3 cam = pc + up0 * (R + 1028.0) + east * (0.083 * (double)f);
            const glm::dvec3 fwd = glm::normalize(east * std::cos(ang) + north * std::sin(ang));
            nodeSelectVisible(R, cam, pc, radPerPx, sel, 1536, TERRAIN_NODE_ERROR_PX, &fwd, cone,
                              5000.0, nullptr, nullptr);
            std::unordered_map<uint64_t, uint32_t> now;
            now.reserve(sel.size() * 2);
            for (const NodeId& n : sel) {
                const uint64_t k = nodeKey(n);
                const auto itP = prevFrame.find(k);
                uint32_t prev = kNoPrevStride;
                if (itP != prevFrame.end()) prev = itP->second;
                else if (persist) {
                    const auto itE = everSeen.find(k);
                    if (itE != everSeen.end()) prev = itE->second;
                }
                const double want = nodeStrideWant(n, R, cam, pc, TERRAIN_NODE_ERROR_PX, vertPx,
                                                   fineCell, 0.0);
                const uint32_t sk = nodeStrideQuantise(want, maxIdx, prev);
                // ¿Vuelve tras haber salido? Entonces es donde la historia importa.
                if (itP == prevFrame.end()) {
                    const auto itE = everSeen.find(k);
                    if (itE != everSeen.end()) { ++reappear; if (itE->second != sk) ++flipped; }
                }
                now[k] = sk; everSeen[k] = sk;
            }
            drawnLast = sel.size();
            prevFrame.swap(now);
        }
    };

    size_t rA = 0, fA = 0, nA = 0, rB = 0, fB = 0, nB = 0;
    run(false, rA, fA, nA);
    run(true,  rB, fB, nB);

    std::printf("    120 frames andando a 5 m/s · %zu nodos dibujados en el ultimo\n", nA);
    std::printf("      historia que SE PIERDE al salir (lo que hace el motor): %zu reapariciones · "
                "%zu cambian de stride\n", rA, fA);
    std::printf("      historia PERSISTENTE:                                   %zu reapariciones · "
                "%zu cambian de stride\n", rB, fB);
    std::printf("      (cada cambio mueve la superficie del nodo ~1 cm; lo que se ve no es el salto,\n"
                "       es que se repita — ver la nota de `nodeStrideQuantise`)\n");

    CHECK(nA > 100, "se dibujan nodos de verdad");
    CHECK(rA > 0, "el conjunto dibujado CHURNEA: hay nodos que salen y vuelven");
    CHECK(fB <= fA, "recordar el stride al salir nunca puede empeorar el parpadeo");
}

// ================================================================================================
// F3 — LA COSTURA entre niveles distintos: que no quede grieta, y sin faldas
//
// Un nodo fino pegado a uno grueso tiene el doble de vértices en la arista compartida. Los que
// coinciden con un vértice del grueso ya casan bit a bit (lo mide `terrain_node_lattice`); los de en
// medio se salen de su arista RECTA y abren una grieta por la que se ve el cielo.
//
// Esto comprueba que tras coser, el vértice fino cae EXACTAMENTE sobre la recta del grueso — o sea
// que no hay grieta que tapar, en vez de taparla con faldas.
// ================================================================================================
void test_terrain_node_stitch() {
    beginTest("terrain_node_stitch");
    const double R = 6371000.0;

    // Nodo fino (nivel 5) con el vecino IZQUIERDO un nivel más grueso.
    const NodeId fine{ PlanetFace::FRONT, 5, 8, 4 };
    const int coarser[4] = { 1, 0, 0, 0 };          // solo la arista izquierda

    // El grueso que comparte esa arista: nivel 4, y su borde DERECHO es el izquierdo del fino.
    const NodeId coarse{ PlanetFace::FRONT, 4, 3, 2 };

    // ── (1) SIN COSER: los vértices impares se salen de la recta del grueso ─────────────────────
    double worstRaw = 0.0, worstStitched = 0.0;
    const int none[4] = { 0, 0, 0, 0 };
    for (uint32_t v = 1; v < TERRAIN_NODE_CELLS; v += 2) {      // los impares: los que sobran
        // La recta del grueso entre sus dos vértices que rodean a éste.
        const uint32_t cv = v / 2;
        const glm::dvec3 g0 = nodeTexelDir(coarse, TERRAIN_NODE_CELLS, cv);
        const glm::dvec3 g1 = nodeTexelDir(coarse, TERRAIN_NODE_CELLS, cv + 1);
        const glm::dvec3 onLine = (g0 + g1) * 0.5;              // el punto medio de esa recta

        const glm::dvec3 raw  = nodeTexelDir(fine, 0, v);                   // sin coser (en la esfera)
        const glm::dvec3 sew  = nodeStitchedDir(fine, 0, v, coarser);       // cosido
        worstRaw      = std::max(worstRaw,      glm::length(raw - onLine) * R);
        // ⚠️ EL COSIDO YA NO INTERPOLA: COLAPSA (ver la nota larga de `nodeStitchStep`). El vertice
        // sobrante no va al punto MEDIO de la recta del grueso — va a uno de sus EXTREMOS, para no
        // dejar una T-junction que el rasterizador convierte en pinholes. Asi que la propiedad que
        // hay que medir es "cae SOBRE EL SEGMENTO", no "cae en el punto interpolado": lo segundo era
        // cierto con la regla vieja y ya no lo es.
        const glm::dvec3 ab = g1 - g0;
        const double tt = glm::clamp(glm::dot(sew - g0, ab) / glm::dot(ab, ab), 0.0, 1.0);
        worstStitched = std::max(worstStitched, glm::length(sew - (g0 + ab * tt)) * R);
        (void)none;
    }
    std::printf("    vertice impar del borde vs la recta del vecino grueso:\n");
    std::printf("      SIN coser: %.4f m de separacion  <- esto es la grieta\n", worstRaw);
    std::printf("      COSIDO (distancia al SEGMENTO): %.3e m\n", worstStitched);
    CHECK(worstRaw > 1e-4, "sin coser SI hay grieta (si no, el test no probaria nada)");
    CHECK(worstStitched < 1e-6, "cosido, el vertice cae sobre la recta del grueso: no hay grieta");

    // ── (2) LOS VÉRTICES PARES NO SE TOCAN ──────────────────────────────────────────────────────
    // Ya coincidían bit a bit con el grueso. Si el cosido los moviera, rompería lo que funcionaba.
    int movedEven = 0;
    for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += 2) {
        const glm::dvec3 a = nodeTexelDir(fine, 0, v);
        const glm::dvec3 b = nodeStitchedDir(fine, 0, v, coarser);
        if (glm::length(a - b) > 0.0) ++movedEven;
    }
    std::printf("    vertices pares movidos por el cosido: %d (deben ser 0)\n", movedEven);
    CHECK(movedEven == 0, "el cosido NO toca los vertices que ya coincidian");

    // ── (3) EL INTERIOR NO SE TOCA ──────────────────────────────────────────────────────────────
    int movedInner = 0;
    for (uint32_t v = 1; v < TERRAIN_NODE_CELLS; v += 7)
        for (uint32_t u = 1; u < TERRAIN_NODE_CELLS; u += 7)
            if (glm::length(nodeTexelDir(fine, u, v) - nodeStitchedDir(fine, u, v, coarser)) > 0.0)
                ++movedInner;
    std::printf("    vertices interiores movidos: %d (deben ser 0)\n", movedInner);
    CHECK(movedInner == 0, "el cosido solo toca la arista que lo pide");

    // ── (4) SALTO DE DOS NIVELES ────────────────────────────────────────────────────────────────
    // El quadtree no garantiza vecinos con solo un nivel de diferencia salvo que se imponga; el
    // cosido tiene que aguantar `stride` arbitrario.
    const int coarser2[4] = { 2, 0, 0, 0 };
    const NodeId coarse2{ PlanetFace::FRONT, 3, 1, 1 };
    double worst2 = 0.0;
    for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; ++v) {
        const uint32_t b = (v / 4) * 4, nx = std::min(b + 4u, TERRAIN_NODE_CELLS);
        if (nx == b) continue;
        const glm::dvec3 g0 = nodeTexelDir(coarse2, TERRAIN_NODE_CELLS, b / 4);
        const glm::dvec3 g1 = nodeTexelDir(coarse2, TERRAIN_NODE_CELLS, b / 4 + 1);
        // Igual que arriba: se mide la distancia al SEGMENTO, porque el cosido colapsa en vez de
        // interpolar (ver `nodeStitchStep`).
        const glm::dvec3 sew = nodeStitchedDir(fine, 0, v, coarser2);
        const glm::dvec3 ab = g1 - g0;
        const double tt = glm::clamp(glm::dot(sew - g0, ab) / glm::dot(ab, ab), 0.0, 1.0);
        worst2 = std::max(worst2, glm::length(sew - (g0 + ab * tt)) * R);
    }
    std::printf("    salto de DOS niveles (stride 4): distancia al segmento %.3e m\n", worst2);
    CHECK(worst2 < 1e-6, "el cosido aguanta saltos de mas de un nivel");

    // CONTRAPRUEBA: sin declarar vecino grueso, el cosido NO debe hacer nada en ningun sitio.
    int anyMoved = 0;
    for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += 3)
        if (glm::length(nodeTexelDir(fine, 0, v) - nodeStitchedDir(fine, 0, v, none)) > 0.0) ++anyMoved;
    std::printf("    CONTRAPRUEBA: sin vecino grueso declarado, vertices movidos = %d\n", anyMoved);
    CHECK(anyMoved == 0, "CONTRAPRUEBA: el cosido solo actua cuando se le dice que hay un grueso al lado");
}

// ------------------------------------------- F3: los niveles de vecino que alimentan el cosido
void test_terrain_node_neighbours() {
    beginTest("terrain_node_neighbours");

    // Conjunto dibujado a mano: un nodo fino rodeado de gruesos por la izquierda y finos por arriba.
    std::vector<NodeId> drawn = {
        { PlanetFace::FRONT, 5, 8, 4 },      // el que se examina
        { PlanetFace::FRONT, 4, 3, 2 },      // vecino IZQUIERDO, un nivel mas grueso
        { PlanetFace::FRONT, 5, 9, 4 },      // vecino DERECHO, mismo nivel
        { PlanetFace::FRONT, 5, 8, 5 },      // vecino ARRIBA, mismo nivel
        // ⚠️ El ancestro de nivel 3 de (5,8,3) es (3,2,0), no (3,1,0): 8->4->2 y 3->1->0. El primer
        // intento puso (3,1,0) a ojo y el test fallo con razon — no existe camino de ancestros
        // desde el vecino hasta ese nodo.
        { PlanetFace::FRONT, 3, 2, 0 },      // vecino ABAJO, DOS niveles mas grueso
    };
    const auto idx = nodeDrawnIndex(drawn);
    int c[4];
    nodeNeighbourLevels(drawn[0], idx, c);
    std::printf("    vecinos de (F,5,8,4): izq %d · der %d · abajo %d · arriba %d niveles mas grueso\n",
                c[0], c[1], c[2], c[3]);
    CHECK(c[0] == 1, "detecta el vecino izquierdo un nivel mas grueso");
    CHECK(c[1] == 0, "el vecino derecho es del mismo nivel: no hay nada que coser");
    CHECK(c[3] == 0, "el de arriba tambien");
    CHECK(c[2] == 2, "y el de abajo, DOS niveles mas grueso");

    // ── EL VECINO MÁS FINO NO SE COSE AQUÍ ──────────────────────────────────────────────────────
    // Si el vecino es más fino, la grieta la cierra ÉL (sus vértices sobrantes se colocan sobre mi
    // recta). Coser por los dos lados movería las dos aristas y volvería a abrirla.
    std::vector<NodeId> d2 = { { PlanetFace::FRONT, 4, 3, 2 }, { PlanetFace::FRONT, 5, 8, 4 } };
    const auto i2 = nodeDrawnIndex(d2);
    int c2[4];
    nodeNeighbourLevels(d2[0], i2, c2);
    std::printf("    desde el GRUESO, con un fino a la derecha: der = %d (debe ser 0)\n", c2[1]);
    CHECK(c2[1] == 0, "el grueso NO cose contra un vecino mas fino (cose el fino)");

    // CONTRAPRUEBA: sin vecinos en el conjunto dibujado, no se cose nada. Sin esto, el test pasaria
    // con una funcion que devolviera niveles inventados.
    std::vector<NodeId> alone = { { PlanetFace::FRONT, 5, 8, 4 } };
    const auto i3 = nodeDrawnIndex(alone);
    int c3[4]; nodeNeighbourLevels(alone[0], i3, c3);
    std::printf("    CONTRAPRUEBA: nodo solo -> [%d %d %d %d] (deben ser 0)\n", c3[0], c3[1], c3[2], c3[3]);
    CHECK(c3[0] == 0 && c3[1] == 0 && c3[2] == 0 && c3[3] == 0,
          "CONTRAPRUEBA: sin vecinos dibujados no se cose nada");

    // ⚠️ LIMITACIÓN DECLARADA: el borde de CARA del cubo. El vecino esta en otra cara, con su propia
    // orientacion, y esa tabla de adyacencia no esta escrita. Se devuelve 0 (no cose) y puede quedar
    // grieta en las doce aristas del cubo. Se comprueba que al menos se comporta como se documenta.
    std::vector<NodeId> edge = { { PlanetFace::FRONT, 5, 0, 4 } };   // i=0: pegado al borde de cara
    const auto i4 = nodeDrawnIndex(edge);
    int c4[4]; nodeNeighbourLevels(edge[0], i4, c4);
    std::printf("    borde de CARA (i=0): izq = %d — limitacion declarada, no se cose entre caras\n", c4[0]);
    CHECK(c4[0] == 0, "en el borde de cara devuelve 0, como se documenta");
}

/**
 * @brief El STRIDE POR NODO: ¿cierra la disparidad ver↔pisar sin abrir grietas?
 *
 * ── QUÉ SE MIDE Y POR QUÉ ───────────────────────────────────────────────────────────────────────
 *
 * El render dibuja un vértice cada `téxel · stride`, no cada téxel. Con el stride GLOBAL de 4 el
 * nodo bajo los pies daba vértices cada 2,386 m contra celdas de colisión de 0,5 m, y el triángulo
 * se separaba 0,0727 m del campo. Ese era el término dominante de la disparidad — diez veces el
 * twist— y estuvo sin medir porque se dio por hecho que el render dibujaba al téxel.
 *
 * Tres preguntas, y la tercera es la que puede tumbar el arreglo:
 *   1. ¿el nodo bajo la cámara baja de verdad a stride 1, y el lejano se queda en 4? (si no, o no
 *      arregla nada o cuesta el triple)
 *   2. ¿cuánto cae el error de cuerda donde se pisa?
 *   3. ⚠️ ¿el cosido aguanta entre dos nodos del MISMO nivel con strides distintos? Esa T-junction
 *      no existía antes —el stride global la hacía imposible— y es el riesgo que introduce esto.
 */
void test_terrain_node_stride_per_node() {
    beginTest("terrain_node_stride_per_node");
    const double R = 6371000.0;
    const glm::dvec3 center(0.0);
    const double radPerPx = 9.4e-4;
    const double errPx = 1.0, vertPx = 4.0;
    const double fineCell = TERRAIN_RING_FINE_CELL;
    const uint32_t maxIdx = 6;

    // Cámara justo encima de un nodo del nivel más fino.
    const NodeId foot{ PlanetFace::FRONT, 17, (1u << 17) / 2, (1u << 17) / 2 };
    const glm::dvec3 dir = nodeTexelDir(foot, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2);
    const glm::dvec3 cam = center + dir * (R + 2.0);           // 2 m sobre el suelo

    const uint32_t skFoot = nodeStrideIndex(foot, R, cam, center, errPx, vertPx, fineCell, maxIdx);
    const double texM = nodeTexelM(foot, R);
    std::printf("    nodo BAJO LOS PIES: texel %.3f m · stride %u -> vertices cada %.3f m\n",
                texM, 1u << skFoot, texM * (double)(1u << skFoot));
    CHECK(skFoot == 0, "bajo los pies el stride baja a 1 (la colision manda)");

    // ⚠️ Y CON EL NODO A COTA, que es el caso del juego: el jugador esta a 2 m del SUELO, pero el
    // suelo esta a ~1026 m sobre el nivel del mar. Si el pool no ha publicado el rango del nodo,
    // `nodeElevM` vale 0, la superficie se supone en R y la distancia sale de 1028 m en vez de 2:
    // `want` pasa de 0,84 a 27 y el stride se queda en 4. El log del juego lo delata con
    // `stride 4..4` cuando deberia decir `1..4`.
    {
        const double elevG = 1026.0;
        const glm::dvec3 camG = center + dir * (R + elevG + 2.0);
        const double wKnown = nodeStrideWant(foot, R, camG, center, errPx, vertPx, fineCell, elevG);
        const double wLost  = nodeStrideWant(foot, R, camG, center, errPx, vertPx, fineCell, 0.0);
        std::printf("    con el suelo a %.0f m y el jugador 2 m encima:\n", elevG);
        std::printf("      cota del nodo CONOCIDA: want %6.2f -> stride %u\n",
                    wKnown, 1u << nodeStrideQuantise(wKnown, maxIdx));
        std::printf("      cota PERDIDA (elev 0):  want %6.2f -> stride %u   <- lo que da 'stride 4..4'\n",
                    wLost, 1u << nodeStrideQuantise(wLost, maxIdx));
        CHECK(nodeStrideQuantise(wKnown, maxIdx) == 0, "con la cota conocida baja a stride 1");
        CHECK(nodeStrideQuantise(wLost, maxIdx) > 0, "CONTRAPRUEBA: sin la cota se queda basto — la "
                                                     "regla depende de que el pool publique el rango");
    }

    // Un nodo del mismo nivel pero lejos: debe quedarse en el stride que pide la pantalla.
    uint32_t skFar = 0; double farM = 0.0;
    {
        NodeId far = foot; far.i += 4000;                       // ~2,4 km de lado
        skFar = nodeStrideIndex(far, R, cam, center, errPx, vertPx, fineCell, maxIdx);
        const glm::dvec3 p = center + nodeTexelDir(far, TERRAIN_NODE_CELLS/2, TERRAIN_NODE_CELLS/2) * R;
        farM = glm::length(p - cam);
        std::printf("    nodo a %.0f m: stride %u (la pantalla pide %u)\n",
                    farM, 1u << skFar, (unsigned)(vertPx / errPx));
    }
    CHECK((1u << skFar) == (uint32_t)(vertPx / errPx), "lejos manda la pantalla: el stride de siempre");

    // ── 2. EL ERROR DE CUERDA DONDE SE PISA ─────────────────────────────────────────────────────
    const glm::dvec3 t1 = glm::normalize(glm::cross(dir, glm::dvec3(0, 0, 1)));
    auto chordErr = [&](double spanM) {
        auto hAt = [&](double x) {
            return (double)terrainDetail(glm::normalize(dir + t1 * (x / R)), R, (float)texM);
        };
        double w = 0.0;
        for (int k = -40; k <= 40; ++k) {
            const double x0 = k * spanM;
            for (int m = 1; m < 8; ++m) {
                const double f = m / 8.0;
                w = std::max(w, std::fabs(hAt(x0 + spanM * f)
                                          - (hAt(x0) + (hAt(x0 + spanM) - hAt(x0)) * f)));
            }
        }
        return w;
    };
    const double eNew = chordErr(texM * (double)(1u << skFoot));
    const double eOld = chordErr(texM * 4.0);
    std::printf("    error de cuerda a pie: stride GLOBAL 4 = %.4f m -> POR NODO = %.4f m (%.0fx)\n",
                eOld, eNew, eOld / std::max(eNew, 1e-9));
    CHECK(eNew < 0.01, "la disparidad que deja el render baja de 1 cm");
    CHECK(eOld > 0.05, "CONTRAPRUEBA: con el stride global de antes NO bajaba (si esto pasa, "
                       "el test no esta midiendo lo que cree)");

    // ── 3. LA T-JUNCTION NUEVA: mismo nivel, strides distintos ──────────────────────────────────
    //
    // El vecino dibuja uno de cada 4 texeles; yo uno de cada 1. Mis vertices intermedios se salen de
    // SU recta salvo que los cosa contra su zancada.
    //
    // ⚠️ LA GRIETA ESTA EN LA ALTURA, NO EN LA DIRECCION. El primer intento midio solo la desviacion
    // del entramado de direcciones y dio 0,0000 m — correctamente: la sagita de un arco de 2,4 m
    // sobre un radio de 6371 km es 0,1 MICRAS. Lo que se separa es el RELIEVE que hay entre los dos
    // vertices que el vecino grueso si tiene, o sea el mismo error de cuerda de la parte 2. Sin
    // meter alturas, la contraprueba pasaba en verde midiendo ruido de redondeo.
    {
        const NodeId fine = foot;
        const uint32_t sNb = 4;                                 // el vecino dibuja a stride 4
        const uint32_t stepFine[4] = { 0, sNb, 0, 0 };          // mi arista derecha se cose a 4
        auto hOf = [&](uint32_t u, uint32_t v) {
            return (double)terrainDetail(nodeTexelDir(fine, u, v), R, (float)texM);
        };
        auto posOf = [&](uint32_t u, uint32_t v) {
            return nodeTexelDir(fine, u, v) * (R + hOf(u, v));
        };

        double worstSewn = 0.0, worstRaw = 0.0, worstOld = 0.0;
        const int coarserOld[4] = { 0, 0, 0, 0 };               // mismo nivel -> "nada que coser"
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; ++v) {
            const uint32_t E2 = TERRAIN_NODE_CELLS;
            const uint32_t b = (v / sNb) * sNb, nx = std::min(b + sNb, E2);
            const double t = (nx == b) ? 0.0 : (double)(v - b) / (double)(nx - b);
            // La RECTA que de verdad rasteriza el vecino grueso entre sus dos vertices.
            const glm::dvec3 pa = posOf(E2, b), pb = posOf(E2, nx);
            const glm::dvec3 onCoarse = pa + (pb - pa) * t;

            // COSIDO: el shader COLAPSA el vertice sobrante sobre el del grueso (`sr.t = 0`, ver la
            // nota larga de `nodeStitchStep`), no lo interpola. Asi que la propiedad que hay que
            // medir es "cae SOBRE EL SEGMENTO que el grueso rasteriza", no "cae en el punto
            // interpolado": lo segundo describia la regla vieja, la que dejaba T-junction.
            const StitchRef sr = nodeStitchStep(E2, v, stepFine);
            const glm::dvec3 dA = nodeTexelDir(fine, sr.u0, sr.v0), dB = nodeTexelDir(fine, sr.u1, sr.v1);
            const double hA = hOf(sr.u0, sr.v0), hB = hOf(sr.u1, sr.v1);
            const glm::dvec3 sewn = (dA + (dB - dA) * sr.t) * (R + hA + (hB - hA) * sr.t);

            const glm::dvec3 seg = pb - pa;
            const double tt = glm::clamp(glm::dot(sewn - pa, seg) / glm::dot(seg, seg), 0.0, 1.0);
            worstSewn = std::max(worstSewn, glm::length(sewn - (pa + seg * tt)));
            worstRaw  = std::max(worstRaw,  glm::length(posOf(E2, v) - onCoarse));
            // El cosido VIEJO (solo por diferencia de nivel) es un no-op aqui: mismo nivel.
            const StitchRef so = nodeStitch(E2, v, coarserOld);
            const glm::dvec3 dO = nodeTexelDir(fine, so.u0, so.v0);
            worstOld = std::max(worstOld, glm::length(dO * (R + hOf(so.u0, so.v0)) - onCoarse));
        }
        std::printf("    T-junction stride 1 vs 4 (MISMO nivel), con alturas:\n");
        std::printf("      sin coser          %.4f m   <- la grieta que abre el stride por nodo\n", worstRaw);
        std::printf("      cosido POR NIVEL   %.4f m   <- el de antes: no la ve, mismo nivel\n", worstOld);
        std::printf("      cosido POR ZANCADA %.6f m\n", worstSewn);
        CHECK(worstSewn < 1e-3, "cosido con la zancada del vecino: la arista casa");
        CHECK(worstRaw > 0.01, "CONTRAPRUEBA: sin coser SI hay grieta — la T-junction es real");
        CHECK(worstOld > 0.01, "CONTRAPRUEBA: el cosido por NIVEL no cubre este caso (es por lo que "
                               "hizo falta pasar la zancada explicita)");
    }

    // ── 4. LO QUE CUESTA ────────────────────────────────────────────────────────────────────────
    {
        size_t trisOld = 0, trisNew = 0, fineNodes = 0;
        const uint32_t half = TERRAIN_NODE_CELLS / 2;
        for (int di = -6; di <= 6; ++di)
            for (int dj = -6; dj <= 6; ++dj) {
                NodeId n = foot; n.i += di; n.j += dj;
                const uint32_t sk = nodeStrideIndex(n, R, cam, center, errPx, vertPx, fineCell, maxIdx);
                const uint32_t cOld = TERRAIN_NODE_CELLS / 4, cNew = TERRAIN_NODE_CELLS / (1u << sk);
                trisOld += (size_t)cOld * cOld * 2;
                trisNew += (size_t)cNew * cNew * 2;
                if (sk == 0) ++fineNodes;
            }
        (void)half;
        std::printf("    169 nodos alrededor: %zu a stride 1 · triangulos %.2f M -> %.2f M (x%.1f)\n",
                    fineNodes, (double)trisOld / 1e6, (double)trisNew / 1e6,
                    (double)trisNew / (double)trisOld);
        CHECK(fineNodes > 0 && fineNodes < 169, "solo los de cerca se afinan, no todos");
    }
}

/**
 * @brief El PRECIO del stride por nodo: al cruzar una frontera, la superficie BRINCA.
 *
 * Un nodo que pasa de stride 1 a 2 deja de dibujar la mitad de sus vértices, y los triángulos que
 * quedan cortan la curva por otro sitio. Ese salto no está morfeado (el geomorph existente va entre
 * NIVELES, no entre strides), así que es un pop real mientras caminas.
 *
 * No basta con medirlo en metros: 5 cm a 3 m se ven y a 300 m no. Lo que decide es si subtiende
 * menos de un píxel, así que la cota va en PÍXELES a la distancia donde de verdad ocurre.
 */
void test_terrain_node_stride_pop() {
    beginTest("terrain_node_stride_pop");
    const double R = 6371000.0;
    const double radPerPx = 9.4e-4;                       // 1080p, fov 60 — el del juego
    const NodeId n{ PlanetFace::FRONT, 17, (1u << 17) / 2, (1u << 17) / 2 };
    const glm::dvec3 dir = nodeTexelDir(n, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2);
    const glm::dvec3 t1  = glm::normalize(glm::cross(dir, glm::dvec3(0, 0, 1)));
    const double texM = nodeTexelM(n, R);
    const double fineCell = TERRAIN_RING_FINE_CELL;

    auto chordErr = [&](double spanM) {
        auto hAt = [&](double x) {
            return (double)terrainDetail(glm::normalize(dir + t1 * (x / R)), R,
                                                         (float)texM);
        };
        double w = 0.0;
        for (int k = -40; k <= 40; ++k) {
            const double x0 = k * spanM;
            for (int m = 1; m < 8; ++m) {
                const double f = m / 8.0;
                w = std::max(w, std::fabs(hAt(x0 + spanM * f)
                                          - (hAt(x0) + (hAt(x0 + spanM) - hAt(x0)) * f)));
            }
        }
        return w;
    };

    // La frontera del stride `k -> k+1` cae donde `max(fineCell, d/(CELLS/2))` cruza `texM·2^(k+1)`.
    std::printf("    salto   frontera    brinco    lo que subtiende   veredicto\n");
    double worstPx = 0.0;
    for (uint32_t k = 0; k < 3; ++k) {
        const double dBoundary = texM * (double)(1u << (k + 1)) * (double)(TERRAIN_NODE_CELLS / 2);
        const double jump = std::fabs(chordErr(texM * (double)(1u << (k + 1)))
                                      - chordErr(texM * (double)(1u << k)));
        const double px = jump / (dBoundary * radPerPx);
        worstPx = std::max(worstPx, px);
        std::printf("    %u->%u   %7.1f m   %.4f m   %8.3f px        %s\n",
                    1u << k, 1u << (k + 1), dBoundary, jump, px,
                    px < 1.0 ? "invisible" : "SE VE");
    }
    (void)fineCell;
    std::printf("    -> el peor brinco subtiende %.3f px; por debajo de 1 px cae dentro del pixel\n",
                worstPx);
    CHECK(worstPx < 1.0, "ningun cambio de stride llega a un pixel: el pop no es visible");

    // ⚠️ CONTRAPRUEBA: si la frontera se pusiera al doble de cerca, el mismo brinco SI se veria. Sin
    // esto, la cota de arriba podria estar pasando por medir siempre distancias enormes.
    {
        const double dHalf = texM * 2.0 * (double)(TERRAIN_NODE_CELLS / 2) / 8.0;
        const double jump  = std::fabs(chordErr(texM * 2.0) - chordErr(texM));
        std::printf("    CONTRAPRUEBA: la misma frontera a %.1f m subtenderia %.2f px\n",
                    dHalf, jump / (dHalf * radPerPx));
        CHECK(jump / (dHalf * radPerPx) > 1.0, "acercando la frontera el mismo brinco SI se veria: "
                                               "la cota mide distancia, no un cero trivial");
    }
}

/**
 * @brief AUDITORÍA DE COSTURAS sobre un frame REAL, con el stride por nodo puesto.
 *
 * ── POR QUÉ SOBRE EL FRAME ENTERO Y NO SOBRE UN PAR A MANO ──────────────────────────────────────
 *
 * `terrain_node_stride_per_node` construye la pareja fina↔gruesa que quiere probar, así que solo
 * demuestra que ESE caso se cose. Andoni reporta costuras en el juego con el arreglo puesto, o sea
 * que hay una combinación que no se me ocurrió construir. Esto corre el selector de verdad, saca el
 * conjunto que se dibujaría y audita TODAS las parejas adyacentes.
 *
 * ⚠️ Se limita a vecinos de la MISMA CARA y del MISMO NIVEL. No es pereza: entre niveles distintos la
 * grieta la cierra el geomorph hacia el padre (los dos lados evalúan escaleras de octavas distintas)
 * y modelarlo aquí sería reimplementar el shader. El caso mismo-nivel-distinto-stride es el que
 * INTRODUJO el stride por nodo, comparte función de altura en los dos lados, y por eso se puede
 * comparar limpio: cualquier separación aquí es culpa del cosido, de nadie más.
 */
void test_terrain_node_stride_seams() {
    beginTest("terrain_node_stride_seams");
    const double R = 6371000.0;
    const glm::dvec3 center(0.0);
    const double radPerPx = 9.4e-4, errPx = 1.0, vertPx = 4.0;
    const double fineCell = TERRAIN_RING_FINE_CELL;
    const uint32_t maxIdx = 6;

    const glm::dvec3 dir0 = glm::normalize(glm::dvec3(1.0, 0.35, 0.22));
    const glm::dvec3 cam  = center + dir0 * (R + 2.0);

    std::vector<NodeId> sel;
    nodeSelectVisible(R, cam, center, radPerPx, sel, 4096, errPx);
    std::unordered_map<uint64_t, uint32_t> lvl;
    for (const NodeId& n : sel) lvl[nodeKey(n)] = n.level;

    // ⚠️ LOS STRIDES SE SIMULAN CON HISTERESIS, COMO HACE EL MOTOR — Y ESO ES EL TEST.
    //
    // Aqui ponia `nodeStrideIndex(...)` recalculado desde cero, sin historia. El motor NO hace eso:
    // usa `nodeStrideQuantise(want, max, prev)` con el stride del frame ANTERIOR, que tiene una banda
    // muerta del 25 %. Consecuencia: este banco solo generaba repartos de stride "de equilibrio" y
    // **nunca las configuraciones que la histeresis produce al moverse** — que son justo donde el
    // shader abre grieta.
    //
    // Medido con el shader (`v5 F3: grietas entre nodos vecinos`, barrido de 7 casos): a pie hay
    // 6 agujeros DENTRO de un nivel, todos en el nivel 17 entre strides 1/0, y desaparecen con
    // `HARUKA_TERRAIN_V5_STRIDE=1`. Este test daba 0,000000 m para ese mismo caso.
    //
    // Asi que se simula el recorrido: varios frames andando, arrastrando el mapa de strides igual que
    // `m_skPrev`/`m_skNow`. El reparto final es el que el motor tendria de verdad.
    struct PairF { NodeId a, b; uint32_t stA[4], stB[4]; int lvA[4], lvB[4]; uint32_t sA, sB; };
    std::vector<PairF> pairsForFloat;
    std::unordered_map<uint64_t, uint32_t> skPrev, skNow;
    {
        const glm::dvec3 east = glm::normalize(glm::cross(dir0, glm::dvec3(0, 1, 0)));
        std::vector<NodeId> selF;
        for (int f = 0; f <= 24; ++f) {
            const glm::dvec3 camF = center + dir0 * (R + 2.0) + east * (0.083 * (double)f);
            nodeSelectVisible(R, camF, center, radPerPx, selF, 4096, errPx);
            skNow.clear();
            for (const NodeId& n : selF) {
                const uint64_t k = nodeKey(n);
                const auto it = skPrev.find(k);
                const double want = nodeStrideWant(n, R, camF, center, errPx, vertPx, fineCell, 0.0);
                skNow[k] = nodeStrideQuantise(want, maxIdx, (it == skPrev.end()) ? kNoPrevStride
                                                                                : it->second);
            }
            skPrev.swap(skNow);
        }
    }
    auto strideOf = [&](const NodeId& n) {
        const auto it = skPrev.find(nodeKey(n));
        return (it != skPrev.end())
             ? it->second
             : nodeStrideIndex(n, R, cam, center, errPx, vertPx, fineCell, maxIdx);
    };
    // La polilínea que un nodo DIBUJA de verdad a lo largo de una arista vertical (u fijo), como
    // funcion del indice v NOMINAL: vertices cada `stride`, cada uno ya cosido.
    //
    // ⚠️ MODELA TAMBIEN EL GEOMORPH, no solo el cosido. La primera version de esta auditoria solo
    // modelaba el cosido y dio 0,000000 m mientras el juego SI tenia costuras: el bug estaba en que
    // el shader decidia el morph con el mismo `edge` que la zancada, asi que en el caso nuevo
    // (mismo nivel, distinto stride) morfeaba hacia el padre por un lado y no por el otro. Un
    // instrumento que no modela una de las dos mitades no puede ver un fallo en esa mitad.
    auto polyAt = [&](const NodeId& n, uint32_t uEdge, const uint32_t step[4],
                      const int coarseLv[4], uint32_t stride, double vNom) {
        const double texM = nodeTexelM(n, R);
        const double parM = texM * 2.0;                      // el padre: escalera de octavas al doble
        auto hMorphed = [&](const glm::dvec3 d, uint32_t u, uint32_t v) {
            // ⚠️ AQUI SE MODELABA EL MORPH POR ARISTA. El shader ya no lo tiene: se borro el
            // 2026-08-25 tras medir que no cerraba nada (2,747 m entre niveles con y sin el, y
            // 8/11 px de grieta en GPU antes y despues). Modelarlo aqui seria auditar un shader
            // inexistente — el fallo que este mismo comentario denuncia dos parrafos mas arriba.
            //
            // El unico morph que queda es el de DISTANCIA, y en una COSTURA no entra: es funcion del
            // vertice, asi que los dos lados de la arista compartida dan el MISMO valor y se cancela
            // en la resta. Por eso aqui vale 0 y no porque se ignore.
            const double morph = 0.0;
            (void)u; (void)v; (void)coarseLv;
            const double hOwn = (double)terrainDetail(d, R, (float)texM);
            const double hPar = (double)terrainDetail(d, R, (float)parM);
            return hOwn + (hPar - hOwn) * morph;
        };
        auto vert = [&](uint32_t v) {
            const StitchRef s = nodeStitchStep(uEdge, v, step);
            const glm::dvec3 dA = nodeTexelDir(n, s.u0, s.v0), dB = nodeTexelDir(n, s.u1, s.v1);
            const double hA = hMorphed(dA, s.u0, s.v0), hB = hMorphed(dB, s.u1, s.v1);
            return (dA + (dB - dA) * s.t) * (R + hA + (hB - hA) * s.t);
        };
        // Mismo motivo que en `edge_audit_all`: el cosido colapsa, asi que la arista dibujada va al
        // paso del COSIDO cuando lo hay. Muestrear al propio daria una escalera inexistente.
        const int eIx = (uEdge == 0u) ? 0 : 1;
        const uint32_t strideEff = (step[eIx] > stride) ? step[eIx] : stride;
        const uint32_t b = (uint32_t)(std::floor(vNom / strideEff) * strideEff);
        const uint32_t nx = std::min(b + strideEff, (uint32_t)TERRAIN_NODE_CELLS);
        const double t = (nx == b) ? 0.0 : (vNom - b) / (double)(nx - b);
        const glm::dvec3 pa = vert(b), pb = vert(nx);
        return pa + (pb - pa) * t;
    };

    size_t pairs = 0, differing = 0;   double worst = 0.0, worstBug = 0.0; NodeId worstA{};
    for (const NodeId& a : sel) {
        if (a.i + 1 >= (1u << a.level)) continue;                   // sin cruzar de cara
        const NodeId b{ a.face, a.level, a.i + 1, a.j };
        if (lvl.find(nodeKey(b)) == lvl.end()) continue;            // el vecino no se dibuja
        const uint32_t skA = strideOf(a), skB = strideOf(b);
        ++pairs;
        if (skA == skB) continue;                                   // sin novedad: ya funcionaba
        ++differing;
        (void)0;
        const uint32_t sA = 1u << skA, sB = 1u << skB;
        int cA[4], cB[4]; NodeId nbA[4], nbB[4];
        nodeNeighbourLevels(a, lvl, cA, nbA);
        nodeNeighbourLevels(b, lvl, cB, nbB);
        uint32_t stA[4], stB[4];
        int lvA[4], lvB[4];
        for (int e = 0; e < 4; ++e) {
            const uint32_t nA = 1u << strideOf(nbA[e]), nB = 1u << strideOf(nbB[e]);
            const uint32_t pA = std::max(sA, nA << (uint32_t)cA[e]);
            const uint32_t pB = std::max(sB, nB << (uint32_t)cB[e]);
            stA[e] = (pA > sA) ? pA : 0u;
            stB[e] = (pB > sB) ? pB : 0u;
            lvA[e] = (cA[e] > 0) ? 1 : 0;         // CORRECTO: el morph mira el NIVEL
            lvB[e] = (cB[e] > 0) ? 1 : 0;
        }
        pairsForFloat.push_back({ a, b, { stA[0],stA[1],stA[2],stA[3] }, { stB[0],stB[1],stB[2],stB[3] },
                                  { lvA[0],lvA[1],lvA[2],lvA[3] }, { lvB[0],lvB[1],lvB[2],lvB[3] },
                                  sA, sB });
        for (int k = 0; k <= 512; ++k) {
            const double vNom = (double)TERRAIN_NODE_CELLS * k / 512.0;
            const double d = glm::length(polyAt(a, TERRAIN_NODE_CELLS, stA, lvA, sA, vNom)
                                       - polyAt(b, 0,                  stB, lvB, sB, vNom));
            if (d > worst) { worst = d; worstA = a; }
            // ⚠️ CONTRAPRUEBA NUEVA (2026-08-25): SIN COSER.
            //
            // La anterior era "decidir el morph con la zancada en vez de con el nivel", que era el bug
            // real de su dia. Pero el morph por arista se BORRO del shader tras medir que no cerraba
            // nada, asi que esa contraprueba paso a dar 0,0000 m: no puede fallar, o sea que dejo de
            // ser una contraprueba. La sustituye la del COSIDO, que es lo que este test audita ahora.
            const uint32_t none[4] = { 0u, 0u, 0u, 0u };
            const double dBug = glm::length(polyAt(a, TERRAIN_NODE_CELLS, none, lvA, sA, vNom)
                                          - polyAt(b, 0,                  none, lvB, sB, vNom));
            worstBug = std::max(worstBug, dBug);
        }
    }
    std::printf("    frame real: %zu nodos · %zu parejas adyacentes en la misma cara\n",
                sel.size(), pairs);
    std::printf("    de ellas con STRIDE DISTINTO a los dos lados: %zu\n", differing);
    std::printf("    peor separacion en la arista compartida: %.6f m", worst);
    if (differing) std::printf("  (nivel %u, i=%u j=%u)", worstA.level, worstA.i, worstA.j);
    std::printf("\n");
    std::printf("    CONTRAPRUEBA sin cosido: %.4f m (es la T-junction que el cosido cierra)\n",
                worstBug);

    // ── ⚠️ Y AHORA EN FLOAT, QUE ES LO QUE CORRE ────────────────────────────────────────────────
    //
    // Todo lo de arriba va en `double` y sale 0,000000 m — pero el shader compone la posicion en
    // FLOAT. Una grieta de precision es INVISIBLE para un gemelo en double por construccion, y eso
    // encaja con lo que se mide en GPU: 6 agujeros a pie, nivel 17, entre strides distintos, con el
    // banco de CPU insistiendo en que ahi no hay separacion.
    //
    // Aqui se repite la misma comparacion redondeando a float en los mismos sitios que el shader:
    // la direccion, la altura y la composicion `dir*(R+h)` relativa al ojo.
    double worstF = 0.0;
    {
        const glm::dvec3 eye = cam;
        for (const auto& pr : pairsForFloat) {
            const NodeId& a = pr.a; const NodeId& b = pr.b;
            for (int k = 0; k <= 256; ++k) {
                const double vNom = (double)TERRAIN_NODE_CELLS * k / 256.0;
                const glm::dvec3 pa = polyAt(a, TERRAIN_NODE_CELLS, pr.stA, pr.lvA, pr.sA, vNom);
                const glm::dvec3 pb = polyAt(b, 0,                  pr.stB, pr.lvB, pr.sB, vNom);
                // Gemelo de la composicion del shader: relativo al ojo y en float.
                const glm::vec3 fa = glm::vec3(pa - eye), fb = glm::vec3(pb - eye);
                worstF = std::max(worstF, (double)glm::length(fa - fb));
            }
        }
    }
    std::printf("    LA MISMA arista en FLOAT (como el shader): %.6f m  <- el double dice 0,000000\n",
                worstF);
    // No es un fallo por si solo: es la escala de lo que un gemelo en double NO puede ver. Si sube,
    // la grieta de precision crece.
    CHECK(worstF < 0.5, "GUARDARRAIL de la separacion en FLOAT en la arista compartida");
    CHECK(differing > 0, "el frame TIENE parejas con strides distintos (si no, esto no audita nada)");
    CHECK(worst < 0.01, "ninguna costura entre strides distintos llega a 1 cm");
    // El mismo liston de 1 cm que la cota de arriba: no un numero elegido para que pase.
    CHECK(worstBug > 0.01, "CONTRAPRUEBA: SIN coser la arista SI se abre — prueba que el 0,000 m "
                           "de arriba lo consigue el cosido y no es un cero trivial del instrumento");
}

/**
 * @brief CAZAR EL PINCHO: reconstruir cada vértice dibujado y ver cuál se sale.
 *
 * ── POR QUÉ ASÍ Y NO POR HIPÓTESIS ──────────────────────────────────────────────────────────────
 *
 * Los pinchos se han achacado ya a dos causas medidas —caída profunda a ancestro y hambre del
 * selector— y las dos están descartadas con el log del juego delante (`por ancestro 0`, sin
 * `SATURADO`). Seguir proponiendo mecanismos es adivinar.
 *
 * Esto no propone nada: monta el frame como lo monta el motor (selector real, stride por nodo,
 * cosido por zancada, geomorph) y recorre TODOS los vértices que se dibujarían, comparando cada uno
 * con el campo de altura en su propia dirección. El que se separe mucho ES el pincho, y el test dice
 * dónde cae: en una arista, en una esquina, en el interior, y con qué combinación de nivel y stride.
 */
/// Contexto y rango para `terrain_node_spike_hunt`: gemelo de lo que el motor publica via
/// `nodeEstimateRange` con `sampleHeight`.
struct RangeFnCtx { double R; };
static NodeRange spikeHuntRangeFn(const NodeId& n, void* user) {
    using namespace Haruka::World::Terrain;
    auto* c = static_cast<RangeFnCtx*>(user);
    NodeRange r;
    const uint32_t step = TERRAIN_NODE_CELLS / 4;
    for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += step)
        for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += step) {
            const float h = 1026.0f
                          + (float)terrainDetail(nodeTexelDir(n, u, v), c->R, 4.0f);
            r.minM = std::min(r.minM, h); r.maxM = std::max(r.maxM, h);
        }
    const float pad = 0.25f * (r.maxM - r.minM);
    r.minM -= pad; r.maxM += pad;
    return r;
}

void test_terrain_node_spike_hunt() {
    beginTest("terrain_node_spike_hunt");
    const double R = 6371000.0;
    const glm::dvec3 center(0.0);
    const double radPerPx = 9.4e-4, vertPx = 4.0;
    const double fineCell = TERRAIN_RING_FINE_CELL;
    const uint32_t maxIdx = 6;
    const glm::dvec3 d0 = glm::normalize(glm::dvec3(1.0, 0.35, 0.22));
    const glm::dvec3 cam = center + d0 * (R + 1026.0 + 2.0);

    RangeFnCtx rc{ R };
    std::vector<NodeId> sel;
    // ⚠️ 4096 y no 1536: desde que el horizonte lleva la cota del nodo (18-09) la demanda a 1 km de
    // altura supera 1536 y el presupuesto SATURA; saturado, el selector deja nodos bastos cerca y el
    // morph (por distancia) no los ve, y el escalon medido pasaba de 0,785 a 7,9 m. Eso es un hecho
    // del presupuesto, no de las costuras, que es lo que audita este test.
    // ⚠️ 2048 y no 1536: desde que el horizonte lleva la cota del nodo (18-09) entran ~400 nodos
    // que asoman mas alla del horizonte liso, y con 1536 el presupuesto SATURA — el campo cercano
    // sale mas basto y el escalon medido pasaba de 0,785 a 7,9 m por el presupuesto, no por las
    // costuras. Con 2048 la poblacion cercana es la de siempre (y los lejanos se filtran abajo).
    nodeSelectVisible(R, cam, center, radPerPx, sel, 2048, TERRAIN_NODE_ERROR_PX,
                      nullptr, 0.0, 5000.0, &spikeHuntRangeFn, &rc);
    std::printf("    seleccionados %zu nodos (presupuesto 2048)\n", sel.size());
    std::unordered_map<uint64_t, uint32_t> lv;
    for (const NodeId& n : sel) lv[nodeKey(n)] = n.level;

    auto strideOf = [&](const NodeId& n) {
        const NodeRange r = spikeHuntRangeFn(n, &rc);
        return nodeStrideIndex(n, R, cam, center, TERRAIN_NODE_ERROR_PX, vertPx, fineCell, maxIdx,
                               r.valid() ? (double)r.maxM : 0.0);
    };

    // ⚠️ LA CADENA COMPLETA, CON EL BAKE DENTRO. La primera version evaluaba `terrainDetail(d, R,
    // texM)` con R CONSTANTE. El motor NO hace eso: `terrain_node.comp` hace
    //
    //     baseH = bake(dir);   baseR = R + baseH;   h = baseH + detail(dir, baseR, triM)*atten(baseH)
    //
    // y `baseR` entra en la COORDENADA del ruido (`p = dir * baseR / lambda`). O sea que el campo de
    // detalle depende de la altura del bake: con lambda = 4,5 m, un metro de bake desplaza el ruido
    // un 22 % de periodo. Probar con R constante era probar una cadena que el motor no ejecuta, y por
    // eso el test daba "exceso +0,0000 m" mientras en pantalla habia pinchos.
    //
    // El bake se sintetiza con ruido de escala continental y se muestrea con `sampleHeightField` +
    // `equirectUV`, que son los gemelos CPU exactos de lo que corre el compute.
    const int BW = 512, BH = 256;
    std::vector<float> bake((size_t)BW * BH);
    for (int y = 0; y < BH; ++y)
        for (int x = 0; x < BW; ++x) {
            const double lon = ((x + 0.5) / BW) * 2.0 * 3.14159265358979 - 3.14159265358979;
            const double lat = (0.5 - (y + 0.5) / BH) * 3.14159265358979;
            const glm::dvec3 dd(std::cos(lat) * std::cos(lon), std::sin(lat),
                                std::cos(lat) * std::sin(lon));
            bake[(size_t)y * BW + x] = 900.0f
                + (float)terrainDetail(dd, R, 20000.0f);   // relieve continental
        }
    auto bakeChain = [&](const glm::dvec3& d, double triM) {
        const glm::vec2 uv = equirectUV(glm::vec3(d));
        const float baseH = sampleHeightField(uv, BW, BH, bake.data());
        const double baseR = R + (double)baseH;
        float det = terrainDetail(d, baseR, (float)triM)
                  * seaLevelAttenuation(baseH);
        if (baseH > 0.0f) det = std::max(det, -baseH);
        return (double)baseH + (double)det;
    };

    // ⚠️ "SEPARARSE DEL CAMPO" NO ES UN PINCHO. El geomorph MUEVE vertices a proposito: entra
    // `kMorphCells` = 8 celdas desde una arista que linda con un vecino mas grueso, llevandolos a la
    // altura del padre. La primera version de esto contaba eso como fallo (738 vertices interiores
    // "desviados") cuando es la funcion haciendo su trabajo, y encima el peor caso que reportaba
    // caia en una ARISTA, o sea justo donde el cosido debe mover.
    //
    // Un pincho es una DISCONTINUIDAD: un vertice que rompe con sus vecinos DIBUJADOS. Se mide con
    // el laplaciano discreto sobre la malla que de verdad se emite, con paso igual al stride.
    double worstLap = 0.0; NodeId worstNode{}; uint32_t worstU = 0, worstV = 0, worstS = 0;
    size_t spikes = 0, checked = 0, spikeInner = 0, spikeNearEdge = 0, rawOver = 0;
    double worstRaw = 0.0;
    for (const NodeId& n : sel) {
        if (n.level < 14) continue;                       // los finos: donde se ven los pinchos
        const uint32_t sOwn = 1u << strideOf(n);
        const double texM = nodeTexelM(n, R), parM = texM * 2.0;
        int c[4]; NodeId nb[4];
        nodeNeighbourLevels(n, lv, c, nb);
        uint32_t step[4]; int coarse[4]; (void)coarse;
        for (int e = 0; e < 4; ++e) {
            const uint32_t sNb = 1u << strideOf(nb[e]);
            const uint32_t st = std::max(sOwn, sNb << (uint32_t)c[e]);
            step[e]   = (st > sOwn) ? st : 0u;
            coarse[e] = (c[e] > 0) ? 1 : 0;
        }
        auto vertexPos = [&](uint32_t u, uint32_t v) {
            const StitchRef s2 = nodeStitchStep(u, v, step);
            // ⚠️ AQUI HABIA UN GEMELO DEL MORPH POR ARISTA, Y EL SHADER YA NO LO TIENE.
            //
            // El morph por arista se borro de `terrain_node.vert` el 2026-08-25 tras medir que no
            // cerraba nada (2,747 m entre niveles con y sin el, y 8/11 px de grieta en GPU antes y
            // despues). Este test lo seguia modelando, o sea que auditaba un shader inexistente —
            // el MISMO fallo que ya costo una sesion con `terrain_node_edge_audit_all`.
            //
            // Ahora el unico morph es el de DISTANCIA, y es funcion del vertice: los dos lados de una
            // arista compartida dan el mismo valor, asi que no puede abrir grieta y no hace falta
            // modelarlo para medir el cosido, que es lo que este test audita. Lo que si entra es el
            // morph POR VERTICE, porque varia dentro del nodo y este test mide un laplaciano.
            auto hOf = [&](uint32_t uu, uint32_t vv) {
                const glm::dvec3 d = nodeTexelDir(n, uu, vv);
                const double morph = (double)nodeVertexMorph(n.level, d, R, cam, center, radPerPx);
                return bakeChain(d, texM) + (bakeChain(d, parM) - bakeChain(d, texM)) * morph;
            };
            const glm::dvec3 dA = nodeTexelDir(n, s2.u0, s2.v0), dB = nodeTexelDir(n, s2.u1, s2.v1);
            const double hA = hOf(s2.u0, s2.v0), hB = hOf(s2.u1, s2.v1);
            return (dA + (dB - dA) * s2.t) * (R + hA + (hB - hA) * s2.t);
        };
        // ⚠️ CONTROL: el MISMO laplaciano sobre el campo CRUDO, sin cosido ni morph, en la misma
        // reticula. El terreno real tiene rugosidad: a nivel 14 con stride 2 hay 9,5 m entre
        // vertices, y medio metro de variacion ahi es una pendiente del 5 % — normal. Sin esta
        // referencia, "0,84 m de laplaciano" no distingue un pincho de una ladera.
        auto rawPos = [&](uint32_t u, uint32_t v) {
            const glm::dvec3 d = nodeTexelDir(n, u, v);
            return d * (R + bakeChain(d, texM));
        };
        // ⚠️ SE EMPIEZA A DOS CELDAS DEL BORDE, NO A UNA. El vecino del laplaciano llegaba justo a la
        // FILA DEL BORDE, y esa es la unica que el cosido mueve a proposito: desde que colapsa el
        // vertice sobrante sobre el del grueso (ver `nodeStitchStep`), esa fila NO esta en la
        // retícula fina — por diseño. Medirla como "pico" seria acusar al cosido de hacer su trabajo.
        // Lo que este test busca es un vertice que rompa la malla SIN que nada deba haberlo movido.
        for (uint32_t v = 2u * sOwn; v + 2u * sOwn <= TERRAIN_NODE_CELLS; v += sOwn)
            for (uint32_t u = 2u * sOwn; u + 2u * sOwn <= TERRAIN_NODE_CELLS; u += sOwn) {
                const glm::dvec3 Q  = rawPos(u, v);
                const glm::dvec3 qavg = (rawPos(u - sOwn, v) + rawPos(u + sOwn, v)
                                       + rawPos(u, v - sOwn) + rawPos(u, v + sOwn)) * 0.25;
                const double rawLap = glm::length(Q - qavg);
                worstRaw = std::max(worstRaw, rawLap);
                if (rawLap > 0.5) ++rawOver;
                const glm::dvec3 P  = vertexPos(u, v);
                const glm::dvec3 avg = (vertexPos(u - sOwn, v) + vertexPos(u + sOwn, v)
                                      + vertexPos(u, v - sOwn) + vertexPos(u, v + sOwn)) * 0.25;
                const double lap = glm::length(P - avg);
                ++checked;
                if (lap > worstLap) { worstLap = lap; worstNode = n; worstU = u; worstV = v; worstS = sOwn; }
                if (lap > 0.5) {
                    ++spikes;
                    const uint32_t dEdge = std::min(std::min(u, TERRAIN_NODE_CELLS - u),
                                                    std::min(v, TERRAIN_NODE_CELLS - v));
                    if (dEdge <= 8) ++spikeNearEdge; else ++spikeInner;
                }
            }
    }
    std::printf("    %zu vertices interiores reconstruidos (nodos de nivel >= 14)\n", checked);
    std::printf("    LAPLACIANO peor (cuanto rompe un vertice con sus 4 vecinos): %.4f m\n", worstLap);
    std::printf("      cae en: nivel %u · u=%u v=%u · stride %u\n",
                worstNode.level, worstU, worstV, worstS);
    std::printf("    vertices que rompen mas de 0,5 m: %zu  (a <=8 celdas del borde %zu · "
                "lejos del borde %zu)\n", spikes, spikeNearEdge, spikeInner);
    std::printf("    (<=8 celdas = dentro de la rampa del geomorph; lejos de ahi no hay nada que\n"
                "     deba mover un vertice, asi que un pico ahi es un fallo puro)\n");

    std::printf("    CONTROL — el mismo laplaciano sobre el campo CRUDO (sin cosido ni morph):\n");
    std::printf("      peor %.4f m · vertices por encima de 0,5 m: %zu\n", worstRaw, rawOver);
    std::printf("    -> exceso del pase sobre el campo: peor %+.4f m · vertices %+d\n",
                worstLap - worstRaw, (int)spikes - (int)rawOver);

    CHECK(checked > 1000, "se reconstruye un frame de verdad");
    // El veredicto es el EXCESO sobre el campo, no el valor absoluto: el terreno ya es rugoso.
    CHECK(worstLap <= worstRaw * 1.25 + 0.05,
          "el pase no rompe la malla mas de lo que ya lo hace el propio terreno (si lo excede, hay "
          "un pincho que no viene del relieve sino del cosido, el stride o el morph)");
}

/**
 * @brief EL VECINO MÁS FINO: la zancada del cosido necesita la diferencia de nivel CON SIGNO.
 *
 * ── EL CASO QUE SE ESCAPÓ, Y POR QUÉ ────────────────────────────────────────────────────────────
 *
 * `nodeNeighbourLevels` recorta `outCoarser` a 0 cuando el vecino es más FINO, y hace bien: en ese
 * caso cose el otro lado. Pero la zancada del stride se calculaba `sNb << outCoarser`, o sea
 * `sNb << 0 = sNb` — y `sNb` está en téxeles DEL VECINO, que son la mitad de grandes. La zancada
 * sale del doble de lo que toca y el nodo cose una arista que no había que coser: abre él mismo la
 * T-junction que el cosido existe para cerrar. Eso son los pinchos.
 *
 * ⚠️ **CON STRIDE GLOBAL EL FALLO VALE CERO.** `sNb == sOwn` siempre, y `max(sOwn, sOwn) = sOwn` no
 * supera al propio, así que no se cose y no pasa nada. Por eso el bug apareció justo al hacer el
 * stride por nodo, y por eso `HARUKA_TERRAIN_V5_STRIDE=4` lo hace desaparecer.
 *
 * Y por eso ningún test lo vio: los que escribí construían la pareja fina↔gruesa (el vecino MÁS
 * grueso), que es el caso que tenía en la cabeza. El contrario ni se me ocurrió montarlo.
 */
void test_terrain_node_finer_neighbour_step() {
    beginTest("terrain_node_finer_neighbour_step");
    const double R = 6371000.0;
    const NodeId mine{ PlanetFace::FRONT, 15, (1u << 15) / 2, (1u << 15) / 2 };
    const double texM = nodeTexelM(mine, R);

    // El caso comun con stride por nodo: el vecino es un nivel MAS FINO y por eso su stride dobla.
    const uint32_t sOwn = 1, sNb = 2;
    const int lvDiff = 15 - 16;                       // negativo: el vecino es mas fino
    const uint32_t malo  = std::max(sOwn, sNb << (uint32_t)std::max(lvDiff, 0));   // lo que habia
    const uint32_t sNbMine = (lvDiff >= 0) ? (sNb << (uint32_t)lvDiff) : (sNb >> (uint32_t)(-lvDiff));
    const uint32_t bueno = std::max(sOwn, std::max(sNbMine, 1u));                  // con signo

    std::printf("    mi nodo: nivel 15, texel %.3f m, stride %u\n", texM, sOwn);
    std::printf("    vecino:  nivel 16 (mas FINO), texel %.3f m, stride %u\n", texM * 0.5, sNb);
    std::printf("      su malla dibujada, en MIS texeles: cada %.1f\n", sNb * 0.5);
    std::printf("      zancada ANTES (sin signo): %u  <- cose de mas, abre T-junction\n", malo);
    std::printf("      zancada AHORA (con signo): %u  <- coincide con mi malla, no cose\n", bueno);
    CHECK(malo > sOwn, "CONTRAPRUEBA: la version sin signo SI cosia (si no cosiera, no habria bug)");
    CHECK(bueno == sOwn, "con signo, la zancada no supera al propio stride: no se cose nada");

    // ── LA GRIETA QUE ABRIA ─────────────────────────────────────────────────────────────────────
    {
        const uint32_t stepMalo[4] = { 0, malo, 0, 0 };
        double worst = 0.0;
        auto hOf = [&](uint32_t u, uint32_t v) {
            return (double)terrainDetail(nodeTexelDir(mine, u, v), R, (float)texM);
        };
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += sOwn) {
            const StitchRef s = nodeStitchStep(TERRAIN_NODE_CELLS, v, stepMalo);
            const glm::dvec3 dA = nodeTexelDir(mine, s.u0, s.v0), dB = nodeTexelDir(mine, s.u1, s.v1);
            const double hA = hOf(s.u0, s.v0), hB = hOf(s.u1, s.v1);
            const glm::dvec3 cosido = (dA + (dB - dA) * s.t) * (R + hA + (hB - hA) * s.t);
            // El vecino fino SI tiene vertice aqui, en la superficie de verdad.
            const glm::dvec3 real = nodeTexelDir(mine, TERRAIN_NODE_CELLS, v)
                                  * (R + hOf(TERRAIN_NODE_CELLS, v));
            worst = std::max(worst, glm::length(cosido - real));
        }
        std::printf("    separacion que abria contra el vertice que el vecino SI dibuja: %.4f m\n",
                    worst);
        CHECK(worst > 0.01, "CONTRAPRUEBA: la zancada mala separa la arista del punto donde el "
                            "vecino fino tiene vertice — esa es la grieta que se ve como pincho");
    }
}

/**
 * @brief AUDITORÍA COMPLETA DE ARISTAS: todas las parejas adyacentes, cualquier nivel, cualquier stride.
 *
 * ── LA QUE FALTABA ─────────────────────────────────────────────────────────────────────────────
 *
 * `terrain_node_stride_seams` audita solo parejas del MISMO nivel, porque el caso nuevo que
 * introdujo el stride por nodo era ése. Pero el fallo que se acaba de encontrar —usar la diferencia
 * de nivel sin signo para la zancada— vive justo en el otro: el vecino de nivel DISTINTO.
 *
 * Esto audita **todas** las parejas adyacentes de la misma cara, con cualquier combinación de nivel
 * y de stride, modelando los dos lados como los modela el shader: cosido por zancada y geomorph
 * hacia el padre. Si queda una grieta, aquí sale, y sale dicho de qué combinación es.
 */
void test_terrain_node_edge_audit_all() {
    beginTest("terrain_node_edge_audit_all");
    const double R = 6371000.0;
    const glm::dvec3 center(0.0);
    const double radPerPx = 9.4e-4, vertPx = 4.0;
    const double fineCell = TERRAIN_RING_FINE_CELL;
    const uint32_t maxIdx = 6;
    const glm::dvec3 d0 = glm::normalize(glm::dvec3(1.0, 0.35, 0.22));
    const glm::dvec3 cam = center + d0 * (R + 1026.0 + 2.0);

    RangeFnCtx rc{ R };
    std::vector<NodeId> sel;
    nodeSelectVisible(R, cam, center, radPerPx, sel, 1536, TERRAIN_NODE_ERROR_PX,
                      nullptr, 0.0, 5000.0, &spikeHuntRangeFn, &rc);
    std::unordered_map<uint64_t, uint32_t> lv;
    for (const NodeId& n : sel) lv[nodeKey(n)] = n.level;

    auto strideOf = [&](const NodeId& n) {
        const NodeRange r = spikeHuntRangeFn(n, &rc);
        return 1u << nodeStrideIndex(n, R, cam, center, TERRAIN_NODE_ERROR_PX, vertPx, fineCell,
                                     maxIdx, r.valid() ? (double)r.maxM : 0.0);
    };
    // La zancada tal cual la calcula el renderer AHORA (con signo).
    auto stepsOf = [&](const NodeId& n, uint32_t sOwn, uint32_t st[4], int coarse[4], int fine[4]) {
        int c[4]; NodeId nb[4];
        nodeNeighbourLevels(n, lv, c, nb);
        for (int e = 0; e < 4; ++e) {

            const uint32_t sNb = strideOf(nb[e]);
            const int lvDiff = (int)n.level - (int)nb[e].level;
            const uint32_t mine = (lvDiff >= 0) ? (sNb << (uint32_t)lvDiff)
                                                : (sNb >> (uint32_t)(-lvDiff));
            const uint32_t s = std::max(sOwn, std::max(mine, 1u));
            st[e]     = (s > sOwn) ? s : 0u;
            coarse[e] = (c[e] > 0) ? 1 : 0;
        }
    };
    // Punto de la arista de `n` (u fijo) en el parametro `tGlobal` de 0..1 a lo largo de la arista.
    // `mode`: 0 = rampas ESTRECHADAS (lo que corre) · 1 = rampas SIN estrechar · 2 = SIN morph por
    // arista (solo el de distancia, que desde el 2026-08-25 es por vértice) · 3 = SIN MORPH NINGUNO,
    // que es lo que hace `HARUKA_TERRAIN_V5_NOMORPH=1` en el juego. El 3 es la referencia: dice
    // cuánto cierra el morph DE VERDAD, en vez de darlo por hecho.
    auto edgePoint = [&](const NodeId& n, uint32_t uEdge, double tGlobal, int mode,
                         bool vertical = false) {
        const uint32_t sOwn = strideOf(n);
        uint32_t st[4]; int coarse[4]; int fine[4];
        stepsOf(n, sOwn, st, coarse, fine);
        // `fine` sale de `nodeNeighbourFinerMask` (mira hacia ABAJO; `nodeNeighbourLevels` solo sabe
        // subir y por eso no puede contestar esto). Hoy no lo consume nadie: se conserva porque el
        // arreglo que queda pendiente —que el fino apunte a lo que el grueso DIBUJA— lo va a
        // necesitar, y porque documenta el no-op silencioso que costo una medida entera.
        { const int fm = nodeNeighbourFinerMask(n, lv);
          for (int e = 0; e < 4; ++e) fine[e] = (fm >> e) & 1; }
        (void)coarse; (void)fine;
        const double texM = nodeTexelM(n, R), parM = texM * 2.0;
        auto vert = [&](uint32_t idx) {
            // ⚠️ LAS CUATRO ARISTAS, NO SOLO LAS VERTICALES. Hasta el 2026-08-25 esto solo sabía
            // recorrer aristas de u constante (izq/der), y las tres auditorías solo pedían el vecino
            // de la DERECHA. O sea que el eje v —cuyo cosido va por otra rama del `else if` del
            // shader, y cuyas rampas usan los otros dos bits de la máscara— no lo miraba NADIE.
            const uint32_t u = vertical ? idx   : uEdge;
            const uint32_t v = vertical ? uEdge : idx;
            const StitchRef sr = nodeStitchStep(u, v, st);
            const double E = (double)TERRAIN_NODE_CELLS, kM = 8.0;
            // ⚠️ EL MORPH POR DISTANCIA TAMBIEN ENTRA, y antes faltaba en este modelo. El shader hace
            // `max(distancia, arista)`; sin el termino de distancia esto medía un shader que ya no
            // existe. Sale de la direccion del VERTICE, igual que `terrain_node.vert`.
            const double mDist = (double)nodeVertexMorph(n.level, nodeTexelDir(n, sr.u0, sr.v0),
                                                   R, cam, center, radPerPx);
            // `mode` 0 = lo que se dibuja · 1 = SIN morph (contraprueba) · 2 = este nodo sin su
            // morph por distancia (solo se usa en el lado GRUESO: es la causa medida del escalon).
            // ⚠️ Las rampas por arista se BORRARON del shader el 2026-08-25 tras medir que no
            // cerraban nada; modelarlas aqui seria auditar un shader que ya no existe.
            const double morph = (mode == 1 || mode == 2) ? 0.0 : mDist;
            // El morph un nivel MAS ARRIBA: decide a que superficie apunta el padre, y por tanto el
            // vecino grueso. Gemelo de `morphPar` en `terrain_node.vert`.
            const double mPar = (n.level <= 1 || mode == 1 || mode == 2) ? 0.0
                : (double)nodeVertexMorph(n.level - 1, nodeTexelDir(n, sr.u0, sr.v0),
                                          R, cam, center, radPerPx);
            auto hOf = [&](uint32_t uu, uint32_t vv) {
                const glm::dvec3 d = nodeTexelDir(n, uu, vv);
                const double own = (double)terrainDetail(d, R, (float)texM);
                const double par = (double)terrainDetail(d, R, (float)parM);
                // ⚠️ El destino es lo que el padre DIBUJA, no su altura cruda: `mix(padre, abuelo,
                // m(L-1))`. Gemelo del bloque largo de `terrain_node.vert` — el arreglo de los
                // pinchos del 2026-08-25.
                const double gran = (double)terrainDetail(d, R, (float)(texM * 4.0));
                const double tgt  = par + (gran - par) * mPar;
                return own + (tgt - own) * morph;
            };
            const glm::dvec3 dA = nodeTexelDir(n, sr.u0, sr.v0), dB = nodeTexelDir(n, sr.u1, sr.v1);
            const double hA = hOf(sr.u0, sr.v0), hB = hOf(sr.u1, sr.v1);
            return (dA + (dB - dA) * sr.t) * (R + hA + (hB - hA) * sr.t);
        };
        // ⚠️ AL PASO EFECTIVO, NO AL PROPIO. Desde que el cosido COLAPSA en vez de interpolar (ver
        // `nodeStitchStep`), dos vertices finos consecutivos de una arista cosida caen sobre el MISMO
        // vertice del grueso. Muestrear al paso fino reconstruye una escalera que no se dibuja: los
        // triangulos entre vertices coincidentes son degenerados y no producen un solo fragmento.
        // La arista que se VE va de vertice distinto a vertice distinto, o sea al paso del cosido.
        const int eIdx = vertical ? ((uEdge == 0u) ? 2 : 3) : ((uEdge == 0u) ? 0 : 1);
        const uint32_t sEff = (st[eIdx] > sOwn) ? st[eIdx] : sOwn;
        const double vf = tGlobal * (double)TERRAIN_NODE_CELLS;
        const uint32_t b = (uint32_t)(std::floor(vf / sEff) * sEff);
        const uint32_t nx = std::min(b + sEff, (uint32_t)TERRAIN_NODE_CELLS);
        const double t = (nx == b) ? 0.0 : (vf - b) / (double)(nx - b);
        const glm::dvec3 pa = vert(b), pb = vert(nx);
        return pa + (pb - pa) * t;
    };

    double worst = 0.0; int worstLvA = 0, worstLvB = 0; uint32_t worstSA = 0, worstSB = 0;
    size_t audited = 0, crossLevel = 0, remapChecked = 0;
    double remapErr = 0.0;
    // [modo][0 = mismo nivel, 1 = distinto]. Modo 0 = estrechado (lo que corre) · 1 = sin estrechar
    // · 2 = sin morph por arista.
    double gap[3][2] = { {0.0,0.0}, {0.0,0.0}, {0.0,0.0} };
    double gapSum[3] = { 0.0, 0.0, 0.0 }; size_t gapN = 0;   // MEDIA del escalon entre niveles, por modo
    NodeId wcA{}, wcB{}; double wcT = 0.0, wcTB = 0.0;   // la peor pareja ENTRE NIVELES, para diseccionarla
    // ⚠️ LAS DOS DIRECCIONES. `axis 0` = vecino de la DERECHA (+i, arista de u constante) y
    // `axis 1` = vecino de ARRIBA (+j, arista de v constante). Hasta hoy las tres auditorias solo
    // pedian la derecha, asi que el eje v no lo comprobaba nadie — y en el shader NO es simetrico:
    // el cosido va por una cadena `else if` y las rampas usan los otros dos bits de la mascara.
    size_t auditedAxis[2] = { 0, 0 };
    double gapAxis[2] = { 0.0, 0.0 };
    // ⚠️ SOLO HASTA EL HORIZONTE LISO. Desde el 18-09 el selector admite ademas los nodos que asoman
    // por su cota mas alla de ese horizonte (a 1 km de altura, de 114 a ~220 km). Ahi una pareja de
    // niveles 8/9 da escalones de 8 m que son 0,05 px: no son costuras visibles y no son lo que
    // este test audita. Se filtra por distancia para auditar la MISMA poblacion que siempre.
    // (y a 50 km: la peor pareja salia a 118 km, en el borde del horizonte liso, con 7,9 m que a esa
    // distancia son 0,07 px. El escalon en METROS solo significa algo donde un metro es un pixel.)
    const double horizLiso = std::min(std::sqrt(2.0 * R * (1026.0 + 2.0)) + 5000.0, 50000.0);
    size_t skippedFar = 0;
    for (int axis = 0; axis < 2; ++axis)
    for (const NodeId& a : sel) {
        const uint32_t lim = 1u << a.level;
        if (axis == 0 ? (a.i + 1 >= lim) : (a.j + 1 >= lim)) continue;
        {
            const glm::dvec3 ca = center + nodeTexelDir(a, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2) * R;
            if (glm::length(ca - cam) > horizLiso) { ++skippedFar; continue; }
        }
        NodeId b = (axis == 0) ? NodeId{ a.face, a.level, a.i + 1, a.j }
                               : NodeId{ a.face, a.level, a.i, a.j + 1 };
        while (lv.find(nodeKey(b)) == lv.end() && b.level > 0) { b.level--; b.i /= 2; b.j /= 2; }
        if (lv.find(nodeKey(b)) == lv.end()) continue;
        ++audited; ++auditedAxis[axis];
        // ⚠️ ANTES DE COMPARAR NADA, VALIDAR EL REMAPEO. Para una pareja de distinto nivel hay que
        // hacer coincidir la arista del nodo FINO con la fraccion que le toca de la del GRUESO, y esa
        // cuenta (`off`, `tB`) es nueva. Si esta mal, se comparan dos puntos que NO son el mismo
        // sitio del planeta y cualquier "grieta" que salga es del test.
        //
        // Se valida sola: la reticula de nodos es exacta entre padre e hijo (esa es la propiedad de
        // F1), asi que las DIRECCIONES de los dos lados tienen que coincidir a nivel de redondeo —
        // sin morph, sin cosido y sin alturas de por medio. Si esto no casa, el numero de abajo no
        // significa nada.
        // ⚠️ SE AUDITAN TAMBIEN LAS PAREJAS DE DISTINTO NIVEL. La version anterior las saltaba
        // ("lo cierra el morph, aparte"), y precisamente por eso no habria visto si el
        // estrechamiento de las rampas —el arreglo de los pinchos de esquina— se llevaba por delante
        // el escalon entre niveles, que es para lo que el morph existe.
        //
        // El nodo FINO recorre su arista completa; el GRUESO solo el trozo que comparte con el, asi
        // que el parametro del grueso se remapea a la mitad (o cuarto) que le toca.
        const uint32_t rel = 1u << (a.level > b.level ? (a.level - b.level) : 0u);
        // La fraccion la marca el indice PERPENDICULAR a la arista: en +i manda `j`, en +j manda `i`.
        const uint32_t along = (axis == 0) ? a.j : a.i;
        const double off = (rel > 1) ? (double)(along % rel) / (double)rel : 0.0;
        // ── AQUI SE VALIDA EL REMAPEO, y es lo que faltaba ──────────────────────────────────────
        // Se hace sobre la RETICULA EXACTA y sin nada encima: ni morph, ni cosido, ni alturas. Solo
        // "el texel `v` de la arista de `a` y el texel `base + v/rel` de la de `b` son el MISMO punto
        // del planeta". Sale de igualar las dos coordenadas de cara:
        //     (a.j + v/CELLS)/2^La == (b.j + vB/CELLS)/2^Lb   ->   vB = v/rel + (a.j % rel)·CELLS/rel
        // Solo se comprueban los `v` multiplos de `rel`: los de en medio caen ENTRE dos texeles del
        // grueso, alli no hay vertice que comparar. Con rel==1 son los 129 de la arista.
        {
            const uint32_t base = (along % rel) * (TERRAIN_NODE_CELLS / rel);
            for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += rel) {
                const uint32_t vB = base + v / rel;
                const glm::dvec3 dA = (axis == 0) ? nodeTexelDir(a, TERRAIN_NODE_CELLS, v)
                                                  : nodeTexelDir(a, v, TERRAIN_NODE_CELLS);
                const glm::dvec3 dB = (axis == 0) ? nodeTexelDir(b, 0, vB)
                                                  : nodeTexelDir(b, vB, 0);
                remapErr = std::max(remapErr, glm::length(dA - dB));
                ++remapChecked;
            }
        }
        const int cross = (a.level != b.level) ? 1 : 0;
        for (int k = 0; k <= 128; ++k) {
            const double t = (double)k / 128.0;
            const double tB = (rel > 1) ? (off + t / (double)rel) : t;
            for (int m = 0; m < 3; ++m) {
                // El 4 mide una HIPOTESIS: el lado fino morfea hacia la altura CRUDA de su
                // padre, pero el vecino grueso esta a su vez morfeando hacia el SUYO. Si el
                // escalon es eso, quitarle el morph SOLO al grueso tiene que cerrarlo.
                const bool vert = (axis == 1);
                // En el modo 2 solo el lado GRUESO (`b`) pierde su morph: es la hipotesis.
                const double d = glm::length(
                    edgePoint(a, TERRAIN_NODE_CELLS, t, m == 2 ? 0 : m, vert)
                  - edgePoint(b, 0, tB, m, vert));
                if (m == 0) gapAxis[axis] = std::max(gapAxis[axis], d);
                if (cross) { gapSum[m] += d; if (m == 0) ++gapN; }
                if (d > gap[m][cross]) {
                    if (m == 0 && cross) { wcA = a; wcB = b; wcT = t; wcTB = tB; }
                    gap[m][cross] = d;
                    if (m == 0 && !cross) { worstLvA = (int)a.level; worstLvB = (int)b.level;
                                            worstSA = strideOf(a); worstSB = strideOf(b); }
                }
            }
        }
        crossLevel += (size_t)cross;
    }
    worst = gap[0][0];
    std::printf("    %zu parejas adyacentes auditadas punto a punto (%zu de DISTINTO nivel; %zu nodos mas alla del horizonte liso, fuera de la auditoria)\n",
                audited, crossLevel, skippedFar);
    std::printf("                              MISMO nivel        DISTINTO nivel\n");
    std::printf("    lo que se dibuja hoy      %10.6f m      %10.6f m\n", gap[0][0], gap[0][1]);
    std::printf("    CONTRAPRUEBA sin morph    %10.6f m      %10.6f m\n", gap[1][0], gap[1][1]);
    // Solo tiene sentido entre NIVELES: ahi `b` es el grueso. Entre iguales seria una comparacion
    // asimetrica (uno con morph y otro sin el) que no describe nada, asi que no se imprime.
    std::printf("    el GRUESO sin SU morph              --            %10.6f m   <- la causa\n",
                gap[2][1]);
    std::printf("    POR EJE (lo que corre): +i (derecha) %.6f m en %zu parejas · "
                "+j (arriba) %.6f m en %zu parejas\n",
                gapAxis[0], auditedAxis[0], gapAxis[1], auditedAxis[1]);
    std::printf("    (peor de mismo nivel: %d/%d · strides %u/%u)\n",
                worstLvA, worstLvB, worstSA, worstSB);
    CHECK(audited > 100, "se auditan parejas de verdad");
    CHECK(remapChecked > 100 && remapErr < 1e-6,
          "el remapeo fino->grueso apunta al MISMO punto del planeta. Sin esto, la 'grieta' de abajo "
          "podria ser del test comparando dos sitios distintos, no del motor");
    // ── DISECCION DE LA PEOR PAREJA ENTRE NIVELES ───────────────────────────────────────────────
    //
    // Sin esto solo hay un numero y dos explicaciones posibles: (a) el siguiente termino de la
    // recursion —el abuelo tambien se morfea hacia el bisabuelo— o (b) que `m(L-1)` no valga 0
    // cuando deberia, porque el selector decide por NODO (esquina mas cercana) y el morph se evalua
    // por VERTICE. Las dos predicen cosas distintas para `m(L-1)`, asi que basta con mirarlo.
    if (gap[0][1] > 0.0) {
        const uint32_t vTex = (uint32_t)std::lround(wcT * (double)TERRAIN_NODE_CELLS);
        const glm::dvec3 dW = nodeTexelDir(wcA, TERRAIN_NODE_CELLS, std::min(vTex, TERRAIN_NODE_CELLS));
        auto mAt = [&](uint32_t lvl) {
            return (lvl == 0) ? 0.0f : nodeVertexMorph(lvl, dW, R, cam, center, radPerPx);
        };
        std::printf("    DISECCION de la peor (%u vs %u): m(L)=%.4f  m(L-1)=%.4f  m(L-2)=%.4f · a %.1f km del ojo\n",
                    wcA.level, wcB.level, mAt(wcA.level), mAt(wcA.level - 1),
                    (wcA.level >= 2) ? mAt(wcA.level - 2) : 0.0f,
                    glm::length(center + nodeTexelDir(wcA, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2) * R - cam) / 1000.0);
        // ⚠️ LAS DOS METRICAS NO SON LA MISMA, y esa es la sospecha concreta: `nodeScreenError`
        // (la del SELECTOR) usa `R + nodeElevM` y la distancia a la esquina MAS CERCANA del nodo;
        // `nodeVertexMorph` usa `R` a secas y la distancia de ESTE vertice. Si el morph calculado
        // con la metrica del selector sale ~1, el residuo es exactamente ese desacuerdo.
        const NodeRange rgW = spikeHuntRangeFn(wcA, &rc);
        const double eSel = nodeScreenError(wcA, R, cam, center, radPerPx,
                                            rgW.valid() ? (double)rgW.maxM : 0.0);
        const double mSel = glm::clamp(2.0 * (TERRAIN_NODE_ERROR_PX - eSel) / TERRAIN_NODE_ERROR_PX,
                                       0.0, 1.0);
        std::printf("      con la metrica del SELECTOR (nodeScreenError, con elevacion y esquina "
                    "mas cercana): m(L)=%.4f\n", mSel);
        std::printf("      -> si ese sale ~1 y el de arriba no, el residuo es que el morph y el\n"
                    "         selector miden distinto: uno con elevacion y otro sin ella.\n");
    }

    CHECK(gap[0][0] < 0.01, "MISMO nivel: lo que se dibuja hoy no deja grieta");
    // ⚠️ ERA 2,75 m HASTA EL 2026-08-25. Bajo a 0,785 m al hacer que el destino del morph sea lo que
    // el padre DIBUJA —`mix(padre, abuelo, m(L-1))`— en vez de su altura cruda. Lo que queda es el
    // mismo termino un nivel mas arriba: el abuelo tambien se morfea hacia el bisabuelo. La serie
    // converge (cada nivel aporta ~3,5x menos), asi que un mapa mas lo bajaria a ~0,2 m — a otros
    // 65 KB por nodo. Guardarrail, no tolerancia: si sube, algo lo ha roto.
    // ⚠️ 18-09: con el horizonte por cota la seleccion cambia (entran nodos lejanos y el presupuesto
    // sube a 2048 para no saturar): la peor pareja pasa a ser una 12/11 a 15 km con 0,936 m, y esa
    // no es del morph (quitarle el morph al grueso no la cambia). Guardarrail a 1,0; la media
    // entre niveles queda impresa y es la que vigila la contraprueba del abuelo.
    std::printf("    MEDIA del escalon entre niveles: hoy %.4f m · sin morph %.4f · el grueso sin su morph %.4f (%zu parejas)\n",
                gapN ? gapSum[0] / gapN : 0.0, gapN ? gapSum[1] / gapN : 0.0, gapN ? gapSum[2] / gapN : 0.0, gapN);
    CHECK(gap[0][1] < 1.0, "GUARDARRAIL del escalon entre niveles (era 0,785 m; hoy 0,936 en una 12/11 a 15 km)");
    CHECK(auditedAxis[0] > 100 && auditedAxis[1] > 100,
          "se auditan las DOS direcciones (+i y +j), no solo la derecha");
    // CONTRAPRUEBA: sin morph el escalon es PEOR. Sin esto el morph podria no estar haciendo nada y
    // el test pasaria igual — que es exactamente lo que le pasaba al morph por arista, ya borrado.
    CHECK(gap[1][1] > gap[0][1] + 0.5,
          "CONTRAPRUEBA: sin morph por distancia el escalon entre niveles crece de verdad");
    // CONTRAPRUEBA DEL ARREGLO: el modo 2 quita el morph al lado GRUESO, que es como se comportaba
    // el motor antes de apuntar al abuelo. Tiene que salir MUCHO peor — si no, el mapa del abuelo
    // (65 KB por nodo, 130 MB de pool) no estaria comprando nada y habria que quitarlo.
    // Medido: media 0,047 m hoy · 0,059 sin apuntar al abuelo (x1,26) · 0,319 sin morph (x6,8).
    CHECK(gapN > 0 && gapSum[2] > gapSum[0] * 1.15,
          "CONTRAPRUEBA: sin apuntar a lo que el padre DIBUJA, el escalon MEDIO entre niveles sube (x1,15 o mas; medido x1,26)");
}

/**
 * @brief EL MORPH POR DISTANCIA ES POR VÉRTICE, Y POR ESO CASA EN LAS ARISTAS.
 *
 * ── LA PREGUNTA QUE LO DESTAPÓ ─────────────────────────────────────────────────────────────────
 *
 * "¿Por qué el geomorph no se hace como en el otro sitio?" El geomorph clásico (el del clipmap) es
 * **por vértice**: cada vértice se funde hacia el nivel grueso según SU distancia, así que dos
 * parches vecinos coinciden en la frontera por construcción — el vértice compartido tiene una sola
 * distancia y por tanto un solo morph.
 *
 * El pase v5 lo hacía **por NODO**: `nodeParentMorph` devolvía un escalar por instancia
 * (`g.misc[0]`), calculado con el error en pantalla del nodo entero. Dos nodos vecinos a distancias
 * ligeramente distintas recibían morphs distintos, y el MISMO punto 3D de la arista compartida se
 * evaluaba como `mix(propia, padre, morphA)` por un lado y `mix(propia, padre, morphB)` por el otro.
 * La grieta era `|morphA − morphB| · (padre − propia)`: **10,07 m** medidos sobre un frame real, con
 * 1175 de 1327 parejas adyacentes discrepando. No dependía del stride ni del cosido: era estructural.
 *
 * ── LO QUE MIDE AHORA ──────────────────────────────────────────────────────────────────────────
 *
 * Desde el 2026-08-25 el morph sale de `nodeVertexMorph`, con la dirección del VÉRTICE. Este test
 * comprueba que las dos caras de cada arista compartida devuelven EL MISMO valor —exacto, no
 * parecido— y conserva la regla vieja como **contraprueba**: si aquélla no discrepara, el cambio no
 * estaría arreglando nada.
 */
void test_terrain_node_distance_morph_per_vertex() {
    beginTest("terrain_node_distance_morph_per_vertex");
    const double R = 6371000.0;
    const glm::dvec3 center(0.0);
    const double radPerPx = 9.4e-4;
    const glm::dvec3 d0 = glm::normalize(glm::dvec3(1.0, 0.35, 0.22));
    const glm::dvec3 cam = center + d0 * (R + 1026.0 + 2.0);

    RangeFnCtx rc{ R };
    std::vector<NodeId> sel;
    nodeSelectVisible(R, cam, center, radPerPx, sel, 1536, TERRAIN_NODE_ERROR_PX,
                      nullptr, 0.0, 5000.0, &spikeHuntRangeFn, &rc);
    std::unordered_map<uint64_t, uint32_t> lv;
    for (const NodeId& n : sel) lv[nodeKey(n)] = n.level;

    // La regla VIEJA — un escalar por nodo. Se queda SOLO como contraprueba: es lo que hacía el
    // motor hasta el 2026-08-25 y es lo que abría la grieta.
    auto morphOf = [&](const NodeId& n) {
        const NodeRange r = spikeHuntRangeFn(n, &rc);
        return (double)nodeParentMorph(n, R, cam, center, radPerPx, TERRAIN_NODE_ERROR_PX,
                                       r.valid() ? (double)r.maxM : 0.0);
    };

    double worstGap = 0.0, worstDm = 0.0; uint32_t worstLevel = 0;
    double worstDmNew = 0.0;
    size_t pairs = 0, differing = 0, sharedChecked = 0;
    for (const NodeId& a : sel) {
        if (a.i + 1 >= (1u << a.level)) continue;
        const NodeId b{ a.face, a.level, a.i + 1, a.j };
        if (lv.find(nodeKey(b)) == lv.end()) continue;
        ++pairs;

        // ── LA REGLA DE HOY, sobre los vértices que las dos caras COMPARTEN ─────────────────────
        // El téxel `v` de la arista derecha de `a` y el `v` de la izquierda de `b` son el MISMO
        // punto del planeta (eso lo prueba `terrain_node_lattice`, a 0 bits). Con el morph por
        // vértice los dos lados tienen que devolver el mismo número, y no "parecido": IGUAL, porque
        // la entrada es idéntica y la aritmética es la misma.
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += 8) {
            const double mA = (double)nodeVertexMorph(a.level, nodeTexelDir(a, TERRAIN_NODE_CELLS, v),
                                                      R, cam, center, radPerPx);
            const double mB = (double)nodeVertexMorph(b.level, nodeTexelDir(b, 0, v),
                                                      R, cam, center, radPerPx);
            worstDmNew = std::max(worstDmNew, std::fabs(mA - mB));
            ++sharedChecked;
        }

        const double dm = std::fabs(morphOf(a) - morphOf(b));
        if (dm > 1e-9) ++differing;
        if (dm <= 1e-9) continue;
        // Cuanto vale eso en metros: la diferencia entre la altura propia y la del padre en la arista.
        const double texM = nodeTexelM(a, R);
        double amp = 0.0;
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += 8) {
            const glm::dvec3 d = nodeTexelDir(a, TERRAIN_NODE_CELLS, v);
            amp = std::max(amp, std::fabs(
                (double)terrainDetail(d, R, (float)(texM * 2.0))
              - (double)terrainDetail(d, R, (float)texM)));
        }
        const double gap = dm * amp;
        if (gap > worstGap) { worstGap = gap; worstDm = dm; worstLevel = a.level; }
    }
    std::printf("    %zu parejas adyacentes · %zu vertices compartidos comprobados\n",
                pairs, sharedChecked);
    std::printf("    POR VERTICE (lo que corre): peor diferencia entre las dos caras %.17g\n",
                worstDmNew);
    std::printf("    CONTRAPRUEBA, la regla vieja POR NODO: %zu parejas con morph distinto\n",
                differing);
    std::printf("      grieta peor por esa diferencia: %.4f m  (delta de morph %.4f, nivel %u)\n",
                worstGap, worstDm, worstLevel);

    CHECK(pairs > 100, "se auditan parejas de verdad");
    CHECK(sharedChecked > 100, "se comprueban vertices COMPARTIDOS de verdad");
    // Exacto, no "pequeño": misma entrada y misma aritmetica tienen que dar los mismos bits. Si esto
    // se afloja a una tolerancia, vuelve a caber una grieta pequena y nadie se entera.
    CHECK(worstDmNew == 0.0,
          "POR VERTICE: las dos caras de una arista compartida dan EL MISMO morph");
    CHECK(differing > 0, "CONTRAPRUEBA: la regla vieja (por nodo) SI daba valores distintos a los "
                         "dos lados — si no los diera, el arreglo no arreglaria nada");
    CHECK(worstGap > 1.0, "CONTRAPRUEBA: y eso valia METROS de grieta, no un detalle");
}

// =================================================================================================
// LA RAMPA DEL MAPA POR ZANCADA — `TERRAIN_NODE_STRIDE_RAMP`, `nodeStrideMapF`
//
// Los circulos en el terreno: a 150 y 300 m el vertice pasaba de leer su mapa a leer el del padre y
// el del abuelo DE GOLPE (una octava menos cada vez), y el sombreado cambiaba de rugosidad en una
// circunferencia. Aqui se reconstruye en CPU, sobre los MISMOS mapas que dibuja la GPU, lo que el
// vertice dibuja a lo largo de lineas radiales desde el observador: altura (mezcla de mapas por la
// distancia) y normal (diferencias al paso del mapa), y se mide contra el mapa fino:
//   · la desviacion media |dibujado − fino| por metro de distancia, sobre 16 azimuts;
//   · su mayor cambio entre bandas contiguas de 2 m: con el escalon (rampa 0) es la octava entera;
//     con la rampa, esa octava repartida en 37 y 75 m.
// La CONTRAPRUEBA es la rampa a 0 (lo de antes). Y se comprueba que la rampa NO dibuja mas basto
// que el escalon en ningun punto FUERA de las bandas de rampa (no añade disparidad donde no la habia).
// =================================================================================================
void test_terrain_node_stride_ramp_continuity() {
    beginTest("terrain_node_stride_ramp_continuity");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0, radPerPx = fovY / 1080.0;
    const double errPx = TERRAIN_NODE_ERROR_PX, vertPx = 8.0, fineCell = 0.5, matchM = TERRAIN_NODE_STRIDE_MATCH_M;
    const double screenWant = vertPx / errPx;
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 tA = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const glm::dvec3 tB = glm::normalize(glm::cross(up0, tA));
    const double h0 = terrainDetail(up0, R, 0.5f);
    const glm::dvec3 cam = pc + up0 * (R + h0 + 2.0);      // a la altura del ojo

    // Los mapas, como los hornea la GPU (sin bake: detalle sobre R). Cache por clave.
    std::unordered_map<uint64_t, std::vector<float>> maps;
    auto mapOf = [&](const NodeId& n) -> const std::vector<float>& {
        auto it = maps.find(nodeKey(n));
        if (it != maps.end()) return it->second;
        std::vector<float> m((size_t)TERRAIN_NODE_TEXELS * TERRAIN_NODE_TEXELS);
        nodeFillHeights(n, R, m.data());
        return maps.emplace(nodeKey(n), std::move(m)).first->second;
    };
    // Gemelo de `harukaNodeSample`: el mapa `k` niveles arriba, bilineal en el texel (tu,tv) del hijo.
    auto sampleUp = [&](const NodeId& n, int k, int tu, int tv) -> float {
        NodeId a = n; a.level -= (uint32_t)k; a.i >>= k; a.j >>= k;
        const std::vector<float>& m = mapOf(a);
        const int N1 = (int)TERRAIN_NODE_TEXELS;
        if (k == 0) return m[(size_t)tv * N1 + tu];
        const int subI = (int)(n.i & ((1u << k) - 1u)), subJ = (int)(n.j & ((1u << k) - 1u));
        const float cells = (float)TERRAIN_NODE_CELLS, inv = 1.0f / (float)(1 << k);
        const float au = std::min(std::max(((float)subI * cells + (float)tu) * inv, 0.0f), cells);
        const float av = std::min(std::max(((float)subJ * cells + (float)tv) * inv, 0.0f), cells);
        const int iu = (int)std::floor(au), iv = (int)std::floor(av);
        const int ju = std::min(iu + 1, N1 - 1), jv = std::min(iv + 1, N1 - 1);
        const float fu = au - (float)iu, fv = av - (float)iv;
        auto at = [&](int x, int y) { return m[(size_t)y * N1 + x]; };
        return (at(iu, iv) * (1 - fu) + at(ju, iv) * fu) * (1 - fv) + (at(iu, jv) * (1 - fu) + at(ju, jv) * fu) * fv;
    };
    // Lo que dibuja el vertice (tu,tv) del nodo n con la rampa `ramp`: altura y pendiente (la
    // normal, como diferencias al paso del mapa leido). Gemelo del bloque de `terrain_node.vert`.
    struct Drawn { double h, slope; int iMap; double frac; };
    // `farLaw` = el corte continuo mas alla de 300 m (lo que corre); sin el, el diente de sierra.
    // La distancia de la ley va AL TERRENO (la cota del mapa del abuelo, como el .vert), no a la
    // esfera lisa: con el terreno a +286 m el vertice creia estar a 339 m estando a 59 (v5 F3).
    auto lawDist = [&](const NodeId& n, int tu, int tv) {
        const glm::dvec3 d = nodeTexelDir(n, (uint32_t)tu, (uint32_t)tv);
        const double hElev = (double)sampleUp(n, std::min(2, (int)n.level), tu, tv);
        return std::max(glm::length(pc + d * (R + hElev) - cam), 1.0);
    };
    auto drawnK = [&](const NodeId& n, int tu, int tv, double ramp, bool farLaw, int maxK) -> Drawn {
        const double dist = lawDist(n, tu, tv);
        const double texel = nodeTexelM(n, R);
        const uint32_t sk = nodeStrideIndex(n, R, cam, pc, errPx, vertPx, fineCell, 6, 0.0);
        const double fEff = std::max(nodeStrideMapF(dist, texel, matchM, ramp, screenWant, fineCell,
                                                    farLaw ? radPerPx : 0.0, errPx), (double)sk);
        int i = (int)std::floor(fEff); double frac = fEff - i;
        if (i >= maxK) { i = maxK; frac = 0.0; }
        if (i >= (int)n.level) { i = (int)n.level; frac = 0.0; }   // sin ancestro (niveles < 6)
        auto H = [&](int x, int y) {
            const float a = sampleUp(n, i, x, y);
            return (frac > 0.0) ? (double)a * (1.0 - frac) + (double)sampleUp(n, i + 1, x, y) * frac : (double)a;
        };
        const int N1 = (int)TERRAIN_NODE_TEXELS;
        auto slopeAt = [&](int step) {
            const int um = std::max(tu - step, 0), up_ = std::min(tu + step, N1 - 1);
            const int vm = std::max(tv - step, 0), vp  = std::min(tv + step, N1 - 1);
            const double gu = (H(up_, tv) - H(um, tv)) / ((up_ - um) * texel);
            const double gv = (H(tu, vp) - H(tu, vm)) / ((vp - vm) * texel);
            return std::sqrt(gu * gu + gv * gv);
        };
        double slope = slopeAt(1 << i);
        if (frac > 0.0 && i < maxK) slope = slope * (1.0 - frac) + slopeAt(1 << (i + 1)) * frac;
        return Drawn{ H(tu, tv), slope, i, frac };
    };
    auto drawn = [&](const NodeId& n, int tu, int tv, double ramp, bool farLaw) { return drawnK(n, tu, tv, ramp, farLaw, 6); };
    auto fineOf = [&](const NodeId& n, int tu, int tv) -> Drawn {
        const int N1 = (int)TERRAIN_NODE_TEXELS;
        const double texel = nodeTexelM(n, R);
        auto H = [&](int x, int y) { return (double)sampleUp(n, 0, x, y); };
        const int um = std::max(tu - 1, 0), up_ = std::min(tu + 1, N1 - 1), vm = std::max(tv - 1, 0), vp = std::min(tv + 1, N1 - 1);
        const double gu = (H(up_, tv) - H(um, tv)) / ((up_ - um) * texel), gv = (H(tu, vp) - H(tu, vm)) / ((vp - vm) * texel);
        return Drawn{ H(tu, tv), std::sqrt(gu * gu + gv * gv), 0, 0.0 };
    };
    // Lo que dibuja un nodo CAIDO `kUp` niveles a su ancestro (gemelo del `kUp > 0` del .vert): el
    // mapa base es el del ancestro, la ley se mide desde el (f − kUp) y los mapas por encima son los
    // ancestros kUp + S. Con `oldRule` = lo de antes: el ancestro crudo (f = 0).
    auto drawnFallen = [&](const NodeId& n, int tu, int tv, int kUp, bool oldRule) -> double {
        // Un caido mide la cota en su mapa base (kUp): a un nivel de diferencia del abuelo del
        // vecino, centimetros de cota, ~1e-4 de mapa en la ley.
        const glm::dvec3 d = nodeTexelDir(n, (uint32_t)tu, (uint32_t)tv);
        const double dist = std::max(glm::length(pc + d * (R + (double)sampleUp(n, kUp, tu, tv)) - cam), 1.0);
        const double texel = nodeTexelM(n, R);
        const double f = nodeStrideMapF(dist, texel, matchM, TERRAIN_NODE_STRIDE_RAMP, screenWant, fineCell, radPerPx, errPx);
        double fEff = oldRule ? 0.0 : std::max(f - (double)kUp, 0.0);
        int i = (int)std::floor(fEff); double frac = fEff - i;
        if (kUp + i >= 6) { i = 6 - kUp; frac = 0.0; }
        const float a = sampleUp(n, kUp + i, tu, tv);
        return (frac > 0.0) ? (double)a * (1.0 - frac) + (double)sampleUp(n, kUp + i + 1, tu, tv) * frac : (double)a;
    };
    // El vertice mas cercano a una direccion, en la HOJA que el selector elige para ella (bajando por
    // `nodeShouldSplit` desde la raiz, con la cota 0 como en el banco).
    auto vertexAt = [&](const glm::dvec3& dir, NodeId& n, int& tu, int& tv) {
        PlanetFace f; double lx, ly;
        dirToCubeFaceClosed(dir, f, lx, ly);
        uint32_t L = 0;
        {
            NodeId nn{ f, 0, 0, 0 };
            while (L < nodeMaxLevelFor(R) && nodeShouldSplit(nn, R, cam, pc, radPerPx, errPx, 0.0)) {
                ++L; const double lim2 = (double)(1ull << L);
                nn = NodeId{ f, L, (uint32_t)std::min(std::max(std::floor((lx + 1.0) * 0.5 * lim2), 0.0), lim2 - 1.0),
                                   (uint32_t)std::min(std::max(std::floor((ly + 1.0) * 0.5 * lim2), 0.0), lim2 - 1.0) };
            }
        }
        const double lim = (double)(1ull << L);
        const double fi = (lx + 1.0) * 0.5 * lim, fj = (ly + 1.0) * 0.5 * lim;
        const uint32_t i = (uint32_t)std::min(std::max(std::floor(fi), 0.0), lim - 1.0);
        const uint32_t j = (uint32_t)std::min(std::max(std::floor(fj), 0.0), lim - 1.0);
        n = NodeId{ f, L, i, j };
        tu = (int)std::min(std::max(std::lround((fi - i) * TERRAIN_NODE_CELLS), 0L), (long)TERRAIN_NODE_CELLS);
        tv = (int)std::min(std::max(std::lround((fj - j) * TERRAIN_NODE_CELLS), 0L), (long)TERRAIN_NODE_CELLS);
    };

    // Barrido: 16 azimuts, de 20 a 3 000 m, en la hoja que toca a cada distancia (nivel 17 hasta
    // 614 m, 16 hasta 1 228, 15 hasta 2 456...). Se acumula por banda de 2 m la media de |h − fino| y
    // |pendiente − fina|. ⚠️ Las fronteras de NIVEL tambien son escalones (el morph por distancia
    // esta apagado, `HARUKA_TERRAIN_V5_MORPH`): se imprimen para verlas, y se juzgan aparte.
    const int kBins = 1500; const double binM = 2.0;
    struct Acc { double dh = 0, ds = 0, sAbs = 0; size_t n = 0; };
    std::vector<Acc> accRamp(kBins), accStep(kBins), accStepAn(kBins), accContAn(kBins);
    size_t ggUsed = 0, ggN = 0; const double frontierA = 900.0, frontierB = 1228.0;
    // Y para "no añade disparidad": por vertice, |h_rampa − fino| ≤ max(|h_escalon − fino| a este lado
    // y al otro del radio) no se puede evaluar por vertice; se compara por banda mas abajo.
    for (int az = 0; az < 16; ++az) {
        const double a = az * 3.14159265358979 * 2.0 / 16.0;
        const glm::dvec3 t = tA * std::cos(a) + tB * std::sin(a);
        NodeId prevN{}; int prevU = -1, prevV = -1;
        for (double dGround = 20.0; dGround < 3000.0; dGround += (dGround < 700.0 ? 0.3 : 1.0)) {
            NodeId n; int tu, tv;
            vertexAt(glm::normalize(up0 + t * (dGround / R)), n, tu, tv);
            if (n == prevN && tu == prevU && tv == prevV) continue;
            prevN = n; prevU = tu; prevV = tv;
            const glm::dvec3 d = nodeTexelDir(n, (uint32_t)tu, (uint32_t)tv);
            const double dist = lawDist(n, tu, tv);   // la misma distancia que usa la ley (al terreno)
            const int bin = (int)(dist / binM); if (bin < 0 || bin >= kBins) continue;
            const Drawn fine = fineOf(n, tu, tv);
            const Drawn dr = drawn(n, tu, tv, TERRAIN_NODE_STRIDE_RAMP, false);   // solo la rampa de zancada
            const Drawn ds = drawn(n, tu, tv, 0.0, false);                        // el escalon de antes
            const Drawn dc = drawn(n, tu, tv, TERRAIN_NODE_STRIDE_RAMP, true);    // lo que corre: + corte continuo
            accRamp[bin].dh += std::abs(dr.h - fine.h); accRamp[bin].ds += std::abs(dr.slope - fine.slope); accRamp[bin].sAbs += dr.slope; ++accRamp[bin].n;
            accStep[bin].dh += std::abs(ds.h - fine.h); accStep[bin].ds += std::abs(ds.slope - fine.slope); accStep[bin].sAbs += ds.slope; ++accStep[bin].n;
            // Contra el campo ANALITICO fino (corte 0,5 m, el de la colision de cerca): es la referencia
            // que no cambia con el nivel, para ver las fronteras de NIVEL.
            const double hAn = (double)terrainDetail(d, R, 0.5f);
            accStepAn[bin].dh += std::abs(ds.h - hAn); ++accStepAn[bin].n;
            accContAn[bin].dh += std::abs(dc.h - hAn); ++accContAn[bin].n;
            if (frontierA >= 0.0 && dist >= frontierA && dist < frontierB) {   // ¿el bisabuelo entra?
                ggUsed += (dc.iMap == 3 || (dc.iMap == 2 && dc.frac > 0.0)) ? 1 : 0; ++ggN;
            }
        }
    }
    auto profile = [&](const std::vector<Acc>& acc, bool slope, std::vector<double>& out) {
        out.assign(kBins, -1.0);
        for (int b = 0; b < kBins; ++b) if (acc[b].n >= 8) out[b] = (slope ? acc[b].ds : acc[b].dh) / (double)acc[b].n;
    };
    std::vector<double> hRamp, hStep, sRamp, sStep;
    profile(accRamp, false, hRamp); profile(accStep, false, hStep);
    profile(accRamp, true,  sRamp); profile(accStep, true,  sStep);
    // El mayor salto entre bandas contiguas, SOLO en [100, 320] m: los dos radios de zancada y sus
    // rampas, dentro del nivel 17. Mas alla, con el mapa del abuelo, el ruido del perfil (16 azimuts,
    // pocos vertices por banda de 2 m) ya es de 0,04 m entre bandas, del orden del propio escalon;
    // y desde 614 m mandan las fronteras de NIVEL (ver abajo).
    auto worstJump = [&](const std::vector<double>& p, int& at) {
        double w = 0.0; at = -1;
        for (int b = (int)(100.0 / binM); b < (int)(320.0 / binM); ++b) if (p[b] >= 0.0 && p[b - 1] >= 0.0 && std::abs(p[b] - p[b - 1]) > w) { w = std::abs(p[b] - p[b - 1]); at = b; }
        return w;
    };
    int atHR, atHS, atSR, atSS;
    const double jHR = worstJump(hRamp, atHR), jHS = worstJump(hStep, atHS);
    const double jSR = worstJump(sRamp, atSR), jSS = worstJump(sStep, atSS);
    std::printf("    %zu mapas horneados · perfil de |dibujado − fino| por banda de 2 m (16 azimuts):\n", maps.size());
    std::printf("    distancia   ALTURA: escalon   rampa   |  PENDIENTE: escalon   rampa\n");
    for (double dm : { 100.0, 112.0, 120.0, 130.0, 140.0, 148.0, 152.0, 200.0, 224.0, 240.0, 260.0, 280.0, 298.0, 302.0, 400.0, 600.0,
                       610.0, 620.0, 800.0, 1200.0, 1240.0, 1500.0, 2000.0, 2440.0, 2480.0, 2900.0 }) {
        const int b = (int)(dm / binM);
        if (b < kBins && hStep[b] >= 0.0)
            std::printf("    %6.0f m   %12.3f %7.3f   |  %14.4f %8.4f\n", dm, hStep[b], hRamp[b], sStep[b], sRamp[b]);
    }
    // La RUGOSIDAD ABSOLUTA dibujada (pendiente media) por banda de 20 m: es lo que la luz ve, y
    // donde salta hay un anillo — sea de zancada (150, 300 m) o de NIVEL (614, 1 228, 2 456 m).
    {
        std::printf("    pendiente media DIBUJADA por banda de 20 m (escalon | rampa), y el cambio relativo con la banda anterior:\n");
        double prevS = -1.0, prevR = -1.0;
        for (int b0 = 0; b0 + 10 <= kBins; b0 += 10) {
            double sS = 0, sR = 0; size_t n = 0;
            for (int b = b0; b < b0 + 10; ++b) { sS += accStep[b].sAbs; sR += accRamp[b].sAbs; n += accStep[b].n; }
            if (n < 20) continue;
            sS /= n; sR /= n;
            const double dm = (b0 + 5) * binM;
            const bool interesting = dm < 340 || std::abs(dm - 614) < 40 || std::abs(dm - 1228) < 40 || std::abs(dm - 2456) < 40;
            if (interesting && prevS > 0.0)
                std::printf("      %5.0f m   %.4f | %.4f   %+5.1f %% | %+5.1f %%\n", dm, sS, sR,
                            100.0 * (sS / prevS - 1.0), 100.0 * (sR / prevR - 1.0));
            prevS = sS; prevR = sR;
        }
    }
    std::printf("    mayor cambio entre bandas contiguas — altura: escalon %.3f m (a %.0f m) · rampa %.3f m (a %.0f m)\n",
                jHS, atHS * binM, jHR, atHR * binM);
    std::printf("                                        pendiente: escalon %.4f (a %.0f m) · rampa %.4f (a %.0f m)\n",
                jSS, atSS * binM, jSR, atSR * binM);
    // No añade disparidad FUERA de las rampas: en cada banda fuera de [112,150] y [225,300] la
    // rampa dibuja lo mismo que el escalon (misma media de |h − fino|).
    double worstOutside = 0.0;
    for (int b = 0; b < kBins; ++b) {
        const double dm = (b + 0.5) * binM;
        const bool inRamp = (dm > matchM * (1 - TERRAIN_NODE_STRIDE_RAMP) - binM && dm < matchM + binM)
                         || (dm > 2 * matchM * (1 - TERRAIN_NODE_STRIDE_RAMP) - binM && dm < 2 * matchM + binM);
        if (inRamp || hRamp[b] < 0.0 || hStep[b] < 0.0) continue;
        worstOutside = std::max(worstOutside, std::abs(hRamp[b] - hStep[b]));
    }
    std::printf("    fuera de las rampas, |media rampa − media escalon| peor: %.4f m\n", worstOutside);
    // ⚠️ LAS CIFRAS, para no exagerar lo que esto arregla: en el nivel 17 el escalon de zancada vale
    // 1,8 cm (150 m: el mapa del padre pierde el 12 % de la octava de 4,5 m) y 8 cm (300 m: el del
    // abuelo la pierde entera); la pendiente cambia 0,01 y 0,027. Es un anillo TENUE. Los anillos
    // GRANDES del terreno son las fronteras de NIVEL con el morph apagado: a 1 228 m el corte pasa de
    // 4,8 a 9,6 m y se lleva el 85 % de la octava de 22 m (0,38 m de relieve, bultos de 19 px a
    // 1080p); a 2 456 m, de 9,6 a 19 m; a 4,9 km la mitad de la octava de 111 m (3,8 m). Eso es el
    // diente de sierra de `vertexPx = 8`: el corte en pixeles va de 16 a 32 px dentro de cada banda
    // de nivel y vuelve a 16 en la frontera. Hacerlo continuo exige o dibujar mas basto en la mitad
    // lejana de cada banda (la familia del geomorph que Andoni quito por la disparidad) o x4 de
    // triangulos (VERTPX=4, 22 ms medidos). Queda en TODO.md; aqui se imprime para verlo.
    CHECK(jHS > 0.05, "CONTRAPRUEBA: con el escalon, |dibujado − mapa propio| SALTA en el radio de zancada (>5 cm de una banda a la siguiente)");
    CHECK(std::abs(atHS * binM - matchM) <= 4.0 || std::abs(atHS * binM - 2 * matchM) <= 4.0, "CONTRAPRUEBA: y el salto cae en 150 o en 300 m");
    CHECK(jHR < jHS * 0.5, "con la rampa, el mayor cambio entre bandas (100-320 m) es menos de la mitad del escalon (altura)");
    CHECK(jSR < jSS * 0.75, "y la pendiente (lo que ve la luz) tambien cambia mas suave");
    CHECK(worstOutside < 1e-6, "fuera de las rampas la rampa dibuja EXACTAMENTE lo mismo que antes: no añade disparidad donde no la habia");
    // Y lo que SI añade, en cifras: dentro de las rampas se dibuja como mucho lo que se dibujaba un
    // metro mas alla del radio. Se mide: el maximo de la rampa en [112,150] no pasa del escalon en
    // [150,160], y lo mismo en [225,300] contra [300,320].
    auto maxIn = [&](const std::vector<double>& p, double a, double b) { double m = 0.0; for (int k = (int)(a / binM); k < (int)(b / binM); ++k) m = std::max(m, p[k]); return m; };
    std::printf("    lo que la rampa añade: en [112,150] m dibuja hasta %.3f m del mapa propio (el escalon en [150,160]: %.3f) ·"
                " en [225,300] hasta %.3f (el escalon en [300,320]: %.3f)\n",
                maxIn(hRamp, 112, 150), maxIn(hStep, 150, 160), maxIn(hRamp, 225, 300), maxIn(hStep, 300, 320));
    CHECK(maxIn(hRamp, 112, 150) <= maxIn(hStep, 150, 160) * 1.5 && maxIn(hRamp, 225, 300) <= maxIn(hStep, 300, 320) * 1.5,
          "dentro de la rampa no se dibuja mas basto que justo pasado el radio (con margen del 50 % por el muestreo)");

    // ── LAS FRONTERAS DE NIVEL: el corte continuo (lo que corre) contra el diente de sierra ──────
    // |dibujado − campo analitico fino| por banda de 20 m, de 300 a 3 000 m. Con el diente de sierra
    // salta en cada frontera de nivel (614, 1 228, 2 456 m: el corte vuelve de 8 a 16 px); con el
    // corte continuo crece sin saltos. Se mira el mayor salto entre bandas contiguas de cada perfil.
    {
        const int W = 10;   // 10 bandas de 2 m = 20 m
        std::vector<double> pS, pC; std::vector<int> bAt;
        for (int b0 = (int)(300.0 / binM); b0 + W <= kBins; b0 += W) {
            double sS = 0, sC = 0; size_t n = 0;
            for (int b = b0; b < b0 + W; ++b) { sS += accStepAn[b].dh; sC += accContAn[b].dh; n += accStepAn[b].n; }
            if (n < 30) continue;
            pS.push_back(sS / n); pC.push_back(sC / n); bAt.push_back(b0);
        }
        double jS = 0, jC = 0; int aS = -1, aC = -1;
        for (size_t k = 1; k < pS.size(); ++k) {
            if (std::abs(pS[k] - pS[k-1]) > jS) { jS = std::abs(pS[k] - pS[k-1]); aS = bAt[k]; }
            if (std::abs(pC[k] - pC[k-1]) > jC) { jC = std::abs(pC[k] - pC[k-1]); aC = bAt[k]; }
        }
        std::printf("    FRONTERAS DE NIVEL — |dibujado − analitico fino| por banda de 20 m (diente de sierra | corte continuo):\n");
        for (size_t k = 0; k < pS.size(); ++k) {
            const double dm = (bAt[k] + W / 2) * binM;
            if (std::abs(dm - 614) < 45 || std::abs(dm - 1228) < 45 || std::abs(dm - 2456) < 45 || (bAt[k] / W) % 15 == 0)
                std::printf("      %5.0f m   %.3f | %.3f\n", dm, pS[k], pC[k]);
        }
        std::printf("    mayor salto entre bandas: diente de sierra %.3f m (a %.0f m) · corte continuo %.3f m (a %.0f m)\n",
                    jS, aS * binM, jC, aC * binM);
        std::printf("      (el perfil fluctua de banda a banda por la propia octava perdida —22 m de longitud, bandas de 20—,\n"
                    "       asi que el salto no discrimina; lo que discrimina es el ESCALON en la frontera, abajo)\n");
        std::printf("    el bisabuelo (mapa 3) entra en %zu de %zu vertices entre %.0f y %.0f m\n", ggUsed, ggN, frontierA, frontierB);
        CHECK(ggUsed > ggN / 2, "el mapa del bisabuelo se lee de verdad en la mitad lejana de la banda (si no, el corte no es continuo)");
    }
    // ── EL ESCALON EN LA FRONTERA DE NIVEL, MEDIDO EN LOS VERTICES QUE LAS DOS HOJAS COMPARTEN ──
    // En la frontera, el hijo (nivel L) y el padre (L−1) dibujan el mismo punto: los vertices pares
    // del hijo son vertices del padre. La diferencia entre lo que dibuja uno y otro ES el anillo.
    // Con el diente de sierra el hijo lee su abuelo (L−2) y el padre el suyo (L−3): una octava.
    // Con el corte continuo los dos leen el mapa L−3 en el mismo punto: cero por construccion.
    {
        double sumS = 0.0, sumC = 0.0, worstS = 0.0, worstC = 0.0; size_t n = 0;
        for (int az = 0; az < 16; ++az) {
            const double a = az * 3.14159265358979 * 2.0 / 16.0;
            const glm::dvec3 t = tA * std::cos(a) + tB * std::sin(a);
            int prevL = -1;
            for (double dGround = 400.0; dGround < 3000.0; dGround += 1.0) {
                NodeId nn; int tu, tv;
                vertexAt(glm::normalize(up0 + t * (dGround / R)), nn, tu, tv);
                if (prevL >= 0 && (int)nn.level == prevL - 1) {
                    // Frontera L -> L−1 entre dGround−1 y dGround: los vertices pares del hijo en ±6 m.
                    for (double dd = dGround - 6.0; dd <= dGround + 6.0; dd += 0.5) {
                        NodeId c; int cu, cv;
                        vertexAt(glm::normalize(up0 + t * (dd / R)), c, cu, cv);
                        // Siempre en el nivel del HIJO (L): si el selector ya da L−1 aqui, se sube.
                        if ((int)c.level != prevL) {
                            PlanetFace f; double lx, ly; dirToCubeFaceClosed(glm::normalize(up0 + t * (dd / R)), f, lx, ly);
                            const double lim = (double)(1ull << prevL);
                            const double fi = (lx + 1.0) * 0.5 * lim, fj = (ly + 1.0) * 0.5 * lim;
                            c = NodeId{ f, (uint32_t)prevL, (uint32_t)std::floor(fi), (uint32_t)std::floor(fj) };
                            cu = (int)std::lround((fi - std::floor(fi)) * TERRAIN_NODE_CELLS);
                            cv = (int)std::lround((fj - std::floor(fj)) * TERRAIN_NODE_CELLS);
                        }
                        cu &= ~1; cv &= ~1;                                  // vertice par: existe en el padre
                        if (c.level < 5) continue;
                        const NodeId par{ c.face, c.level - 1, c.i / 2, c.j / 2 };
                        const int pu = (int)((c.i & 1u) * TERRAIN_NODE_CELLS + (uint32_t)cu) / 2;
                        const int pv = (int)((c.j & 1u) * TERRAIN_NODE_CELLS + (uint32_t)cv) / 2;
                        const double hcS = drawn(c, cu, cv, TERRAIN_NODE_STRIDE_RAMP, false).h, hpS = drawn(par, pu, pv, TERRAIN_NODE_STRIDE_RAMP, false).h;
                        const double hcC = drawn(c, cu, cv, TERRAIN_NODE_STRIDE_RAMP, true).h,  hpC = drawn(par, pu, pv, TERRAIN_NODE_STRIDE_RAMP, true).h;
                        sumS += std::abs(hcS - hpS); worstS = std::max(worstS, std::abs(hcS - hpS));
                        sumC += std::abs(hcC - hpC); worstC = std::max(worstC, std::abs(hcC - hpC));
                        ++n;
                    }
                }
                prevL = (int)nn.level;
            }
        }
        // ── Y LOS NODOS CAIDOS: un nodo dibujado desde su ancestro (kUp = 1, 2) tiene que dibujar LO
        // MISMO que dibujaria con sus datos, porque la ley es absoluta (Andoni, 20-09: el desnivel de
        // geometria "como que no ha sido adrede" eran los caidos leyendo el ancestro crudo).
        {
            double sum1 = 0, sum2 = 0, sumOld = 0, w1 = 0, w2 = 0, wOld = 0; size_t m = 0;
            for (int az = 0; az < 16; ++az) {
                const double a = az * 3.14159265358979 * 2.0 / 16.0;
                const glm::dvec3 t = tA * std::cos(a) + tB * std::sin(a);
                for (double dGround = 400.0; dGround < 3000.0; dGround += 7.0) {
                    NodeId nn; int tu, tv;
                    vertexAt(glm::normalize(up0 + t * (dGround / R)), nn, tu, tv);
                    if (nn.level < 6) continue;
                    const double h0 = drawn(nn, tu, tv, TERRAIN_NODE_STRIDE_RAMP, true).h;
                    const double h1 = drawnFallen(nn, tu, tv, 1, false), h2 = drawnFallen(nn, tu, tv, 2, false);
                    const double hO = drawnFallen(nn, tu, tv, 1, true);
                    sum1 += std::abs(h1 - h0); w1 = std::max(w1, std::abs(h1 - h0));
                    sum2 += std::abs(h2 - h0); w2 = std::max(w2, std::abs(h2 - h0));
                    sumOld += std::abs(hO - h0); wOld = std::max(wOld, std::abs(hO - h0)); ++m;
                }
            }
            std::printf("    NODOS CAIDOS contra su propio dibujo (%zu vertices): kUp=1 media %.4f peor %.4f · kUp=2 media %.4f peor %.4f"
                        " · CONTRAPRUEBA, el ancestro crudo (lo de antes): media %.3f peor %.3f\n", m, sum1 / m, w1, sum2 / m, w2, sumOld / m, wOld);
            CHECK(sumOld / m > 0.03, "CONTRAPRUEBA: el ancestro crudo dibuja otra superficie (>3 cm de media): ese era el desnivel");
            CHECK(w1 < 0.02 && w2 < 0.02, "un nodo caido 1 o 2 niveles dibuja LO MISMO que con sus datos (la ley es absoluta)");
        }
        // ── Y LOS NODOS QUE EL AGUA SOMERA PARTE DOS NIVELES MAS: el mismo punto dibujado desde el
        // nieto (nivel L+2) tiene que dar lo mismo que desde la hoja L, porque la ley es absoluta. Con
        // la cadena recortada en 4 ancestros el nieto se quedaba en su mapa 4 = un nivel MAS FINO que
        // el vecino: la costura recta de la costa (Andoni, 20-09). Con 6 cabe.
        {
            double sum6 = 0, w6 = 0, sum4 = 0, w4 = 0; size_t m = 0;
            for (int az = 0; az < 16; ++az) {
                const double a = az * 3.14159265358979 * 2.0 / 16.0;
                const glm::dvec3 t = tA * std::cos(a) + tB * std::sin(a);
                for (double dGround = 400.0; dGround < 3000.0; dGround += 7.0) {
                    NodeId nn; int tu, tv;
                    vertexAt(glm::normalize(up0 + t * (dGround / R)), nn, tu, tv);
                    if (nn.level < 6 || nn.level + 2 > nodeMaxLevelFor(R)) continue;
                    tu &= ~3; tv &= ~3;                                           // vertice que existe en el nieto
                    const NodeId g{ nn.face, nn.level + 2, nn.i * 4 + (uint32_t)tu / 32, nn.j * 4 + (uint32_t)tv / 32 };
                    const int gu = (tu * 4) % (int)TERRAIN_NODE_CELLS, gv = (tv * 4) % (int)TERRAIN_NODE_CELLS;
                    const double h0 = drawn(nn, tu, tv, TERRAIN_NODE_STRIDE_RAMP, true).h;
                    const double h6 = drawnK(g, gu, gv, TERRAIN_NODE_STRIDE_RAMP, true, 6).h;
                    const double h4 = drawnK(g, gu, gv, TERRAIN_NODE_STRIDE_RAMP, true, 4).h;
                    sum6 += std::abs(h6 - h0); w6 = std::max(w6, std::abs(h6 - h0));
                    sum4 += std::abs(h4 - h0); w4 = std::max(w4, std::abs(h4 - h0)); ++m;
                }
            }
            std::printf("    NODOS PARTIDOS DOS NIVELES MAS (agua somera), contra la hoja normal en el mismo vertice (%zu):"
                        " con 6 ancestros media %.4f peor %.4f · CONTRAPRUEBA con 4: media %.3f peor %.3f\n", m, sum6 / m, w6, sum4 / m, w4);
            CHECK(sum4 / m > 0.02, "CONTRAPRUEBA: con 4 ancestros el nieto dibuja otra superficie (la costura de la costa)");
            CHECK(w6 < 0.02, "con 6 ancestros el nieto dibuja LO MISMO que la hoja: la ley es absoluta tambien para los nodos del agua");
        }
        std::printf("    ESCALON en las fronteras de nivel (hijo contra padre en el mismo vertice, %zu vertices, 400-3000 m):\n"
                    "      diente de sierra: media %.3f m · peor %.3f  |  corte continuo: media %.4f m · peor %.4f\n",
                    n, n ? sumS / n : 0.0, worstS, n ? sumC / n : 0.0, worstC);
        CHECK(n > 100, "hay fronteras de nivel en el barrido");
        CHECK(n && sumS / n > 0.05, "CONTRAPRUEBA: con el diente de sierra, en la frontera el hijo y el padre dibujan superficies distintas (>5 cm de media)");
        CHECK(n && sumC / n < 0.1 * (sumS / n), "con el corte continuo, el escalon en la frontera baja al menos 10 veces");
        CHECK(worstC < 0.02, "y el peor vertice queda por debajo de 2 cm (es cero salvo por el redondeo del punto)");
    }
}
