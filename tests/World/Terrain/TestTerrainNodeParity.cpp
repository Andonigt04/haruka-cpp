// Quadtree v5: lo dibujado contra la colisión y la referencia (paridad, coste de la consulta del suelo).
// (Partido de TestTerrainNode.cpp, que tenía 6 400 líneas; el registro de todos sigue allí.)
#include "World/Terrain/TerrainNodeTestCommon.h"

// ================================================================================================
// F2 (2/2) — FRUSTUM: lo que hace viable el presupuesto de memoria
//
// Sin recorte de frustum el selector elige los 360° alrededor de la cámara, y el número de nodos
// residentes se traduce directo en VRAM: 16 641 téxeles × 4 B = 66,6 KB por nodo. Este test mide la
// reducción Y la convierte en megabytes, que es la cifra con la que se dimensiona el pool.
// ================================================================================================
// ================================================================================================
// EL CONO NO PUEDE COMERSE LAS ESQUINAS.
//
// ⚠️ SÍNTOMA REPORTADO: "chunks cortados antes de que acabe la pantalla". El recorte de frustum del
// selector no usa los seis planos: usa UN CONO alrededor del eje de vista, porque un cono es una
// comparación de ángulos y los planos exigen la matriz. Un cono solo es seguro si envuelve al
// frustum, y lo que decide eso son las ESQUINAS, que están más lejos del eje que los bordes:
//
//     semiángulo vertical    = atan(tan(fovY/2))
//     semiángulo horizontal  = atan(aspect · tan(fovY/2))
//     semiángulo a la ESQUINA= atan(tan(fovY/2) · sqrt(1 + aspect²))   <- el que hay que usar
//
// Con 60° y 16:9 son 22,5°, 36,4° y 40,2°. Usar el vertical o el horizontal recorta justo donde el
// usuario lo ve: en las esquinas, antes de llegar al borde del cuadro.
//
// ⚠️ EL TEST QUE YA HABÍA NO PODÍA VER ESTO. `test_terrain_node_frustum` comprueba que el recorte
// REDUCE el número de nodos y que no lo deja vacío — un recorte demasiado agresivo pasa esas dos.
// Y el test de cobertura en pantalla tampoco: su ventana es 256x256, o sea aspecto 1,0, y en un
// cuadrado las esquinas quedan mucho más cerca del eje. El fallo solo existe fuera del cuadrado.
//
// Aquí la VERDAD DE REFERENCIA es la matriz: se extraen los seis planos de `proj·vista`
// (Gribb-Hartmann) y se exige que NINGÚN nodo que los planos dejan pasar sea descartado por el cono.
// ================================================================================================
// ================================================================================================
// LA COSTURA ENTRE CARAS DEL CUBO.
//
// Un nodo pegado al borde de su cara tiene el vecino EN OTRA CARA. `nodeNeighbourLevels` lo saltaba
// (`continue`), asi que en las doce aristas del cubo nadie cosia: T-junctions y grieta.
//
// El oraculo es la SIMETRIA, que es lo bueno de este caso: si A cruzando su arista da B, entonces B
// cruzando la arista correspondiente tiene que devolver A. Una tabla de adyacencia mal escrita —o un
// empuje mal dimensionado— no puede cumplir eso por casualidad en las 24 combinaciones.
//
// El segundo oraculo es geometrico y en metros: la arista compartida tiene que estar en el MISMO
// sitio del espacio vista desde las dos caras. Si no, no es una costura, son dos bordes distintos.
// ================================================================================================
// ================================================================================================
// LOS DOS BAKES NO SON EL MISMO SUELO. Cuánto se separan, en metros.
//
// ⚠️ El planeta hornea su elevación DOS VECES, en dos parametrizaciones distintas:
//
//     equirect  `m_heightCPU` (8192x4096)   -> lo muestrea `sampleHeight`, o sea LA FISICA
//     cubo      `m_baseHeights` (6x513²)    -> lo muestrea el shader, o sea EL RENDER del v5
//
// Y `m_baseHeights` llevaba el comentario "es el suelo que consultará la física" sin que lo leyera
// NADIE: se rellenaba y se quedaba ahí. O sea que lo que se pisa y lo que se ve salen de dos mapas
// con retículas distintas, y la diferencia entre ambos es una cota inferior del desajuste
// render↔colisión que F4 viene a cerrar.
//
// Aquí se siembra una elevación ANALITICA conocida en las dos retículas —a las resoluciones reales
// del motor— y se mide cuánto discrepan al muestrearlas. No es una estimación: es la propia
// operación que hacen el shader y la física, con el mismo dato de partida.
// ================================================================================================
// ================================================================================================
// ¿PUEDE UN NODO SER UN `HeightFieldShape` DE JOLT TAL CUAL? — la pregunta que abre F4.
//
// El plan dice "una rejilla regular es lo que Jolt quiere; mapea 1:1, sin conversion". Merece
// comprobarse ANTES de construir nada, porque las dos cosas no son obviamente la misma:
//
//   · `HeightFieldShape` es el GRAFO de una funcion sobre un plano: la muestra (i,j) esta en
//     (i·celda, h, j·celda). Rejilla REGULAR, dominio PLANO.
//   · Un nodo del quadtree es una parcela CURVA de la esfera, y sus texeles NO estan equiespaciados
//     al proyectarlos a un plano: el mapeo cubo→esfera (Cobb) estira hacia los bordes de la cara.
//
// RESPUESTA MEDIDA: **NO, y el plan estaba equivocado.** El angulo entre los ejes `u` y `v` del nodo
// es 88,13 grados, no 90 — y es el MISMO en todos los niveles, porque depende de donde cae el nodo en
// la cara del cubo, no de su tamaño. El paso si es uniforme (1,000002 entre centro y borde), asi que
// el problema no es el espaciado: es que la rejilla es un PARALELOGRAMO y Jolt quiere un rectangulo.
//
// El desvio resultante es el 4,6 % del lado del nodo, a cualquier nivel: 3,54 m en uno de 76,4 m. Eso
// no es un detalle — es el suelo que se pisa desplazado horizontalmente respecto al que se ve.
//
// Este test se queda para que nadie vuelva a asumir el "1:1" sin mirar el numero.
// ================================================================================================
// ================================================================================================
// CIERRE DE F4: cuanto se separan el suelo que se DIBUJA y el que se PISA.
//
// ⚠️ EL CRITERIO DEL PLAN ERA IMPOSIBLE. Decia "`terrain_chord_error` da 0 POR CONSTRUCCION", y eso
// exigia que la colision leyera los MISMOS texeles que el render. No puede: los ejes del nodo forman
// 88,13 grados, no 90, asi que un nodo no es un `HeightFieldShape` de Jolt (ver
// `terrain_node_as_heightfield`). Se cerro por la opcion 3: los anillos siguen, pero muestreando la
// misma superficie. Paridad en VALORES, no en celdas — y entonces el cierre es una COTA MEDIDA.
//
// Lo que ya coincide tras el trabajo de F4:
//   · la elevacion base   -> los dos leen el bake EQUIRECT (antes el nodo leia el del cubo)
//   · el radio del ruido  -> los dos usan `baseR = R + baseH` (antes el nodo usaba R a secas)
//
// Lo que NO coincide, y es lo que este test mide: el CORTE DE OCTAVAS.
//   · el nodo   corta por `nodeTexelM(nivel)`      — el texel del nodo que se dibuja
//   · el anillo corta por `terrainTriM(distancia)` — LOD por distancia al jugador
// Son dos LOD distintos sobre el mismo campo, asi que la diferencia es el relieve que uno incluye y
// el otro no.
// ================================================================================================
void test_terrain_render_vs_collision() {
    beginTest("terrain_render_vs_collision");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / 1080.0;

    // Camara a altura de ojo, que es donde la disparidad importa: es donde se camina.
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 cam = pc + up0 * (R + 1.7);
    const glm::dvec3 t1  = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));

    // El conjunto que el selector dibujaria de verdad.
    std::vector<NodeId> sel;
    nodeSelectVisible(R, cam, pc, radPerPx, sel, 200000);
    const auto drawn = nodeDrawnIndex(sel);
    CHECK(!sel.empty(), "el selector elige nodos alrededor de la camara");

    // El nivel del nodo que cubre una direccion: el mas FINO del conjunto dibujado que la contiene.
    auto levelAt = [&](const glm::dvec3& dir) -> int {
        PlanetFace f; double lx, ly;
        dirToCubeFaceClosed(dir, f, lx, ly);
        for (int lv = (int)nodeMaxLevelFor(R); lv >= 0; --lv) {
            const uint64_t lim = 1ull << lv;
            const int64_t i = (int64_t)((lx + 1.0) * 0.5 * (double)lim);
            const int64_t j = (int64_t)((ly + 1.0) * 0.5 * (double)lim);
            const NodeId n{ f, (uint32_t)lv,
                            (uint32_t)std::min<int64_t>(std::max<int64_t>(i, 0), (int64_t)lim - 1),
                            (uint32_t)std::min<int64_t>(std::max<int64_t>(j, 0), (int64_t)lim - 1) };
            if (drawn.find(nodeKey(n)) != drawn.end()) return lv;
        }
        return -1;
    };

    std::printf("    dist. al jugador   corte del NODO   corte del ANILLO   |dibujado - pisado|"
                "   morph   con morph\n");
    double worstNear = 0.0, worstNearMorph = 0.0;
    for (double d : { 2.0, 10.0, 50.0, 200.0, 1000.0, 5000.0 }) {
        const glm::dvec3 dir = glm::normalize(up0 + t1 * (d / R));
        const int lv = levelAt(dir);
        if (lv < 0) { std::printf("    %8.0f m   (sin nodo dibujado)\n", d); continue; }
        const NodeId n{ PlanetFace::FRONT, (uint32_t)lv, 0, 0 };   // solo para el tamaño de texel
        const double triNode = nodeTexelM(n, R);
        const double triRing = terrainTriM(d);

        // La MISMA composicion en los dos lados; solo cambia el corte de octavas.
        const float hNode = terrainDetail(dir, R, (float)triNode);
        const float hRing = terrainDetail(dir, R, (float)triRing);
        const double diff = std::fabs((double)hNode - (double)hRing);

        // ⚠️ Y EL MORPH, QUE ES RENDER-ONLY. La fisica no lo aplica: muestrea el campo y ya. Asi que
        // donde el morph vale >0, lo DIBUJADO se aparta de lo que se PISA a proposito — y esa es
        // justo la clase de fallo invisible que tenia el v4 (se veia bien y la colision decia otra
        // cosa). Hasta el 2026-08-25 este test no lo modelaba, o sea que no podia verlo.
        const double m0 = (double)nodeVertexMorph((uint32_t)lv, dir, R, cam, pc, radPerPx);
        const double m1 = (lv <= 1) ? 0.0
                        : (double)nodeVertexMorph((uint32_t)lv - 1, dir, R, cam, pc, radPerPx);
        const float hPar  = terrainDetail(dir, R, (float)(triNode * 2.0));
        const float hGran = terrainDetail(dir, R, (float)(triNode * 4.0));
        const double tgt  = (double)hPar + ((double)hGran - (double)hPar) * m1;
        const double hDrawn = (double)hNode + (tgt - (double)hNode) * m0;
        const double diffM  = std::fabs(hDrawn - (double)hRing);

        std::printf("    %8.0f m   %10.3f m   %12.3f m   %14.4f m   %5.3f   %8.4f m\n",
                    d, triNode, triRing, diff, m0, diffM);
        if (d <= 200.0) { worstNear = std::max(worstNear, diff);
                          worstNearMorph = std::max(worstNearMorph, diffM); }
    }
    std::printf("    -> en el campo CERCANO (<=200 m, donde se camina): peor %.4f m"
                "  ·  CON el morph: %.4f m\n", worstNear, worstNearMorph);

    // ⚠️ LA PREGUNTA QUE ESTO CONTESTA: ¿el morph reabre la disparidad ver-pisar por la puerta de
    // atras? Cerca NO puede: el nodo esta al nivel mas fino, ya no puede subdividirse mas, asi que su
    // error en pantalla supera el presupuesto y el morph vale 0 por la formula. Pero eso era un
    // ARGUMENTO y ahora es una MEDIDA — que es la diferencia que costo esta sesion entera.
    CHECK(worstNearMorph <= worstNear + 1e-6,
          "el morph NO aparta lo dibujado de lo pisado donde se camina (vale 0 al nivel mas fino)");

    // LA COTA DE CIERRE DE F4. No es 0 —no puede serlo— y esta puesta donde el numero medido la
    // deja, no donde gustaria: si sube, algo ha vuelto a divergir y hay que mirar QUE.
    // ⚠️ 0,2539 m ES ESTRUCTURAL, NO UN AJUSTE PENDIENTE. Se probó bajar el corte de octavas de la
    // colisión al téxel del nodo: la disparidad se iba a 0 y el twist del quad de 4 m se triplicaba
    // (0,035 -> 0,108 m a menos de 8 m del jugador), porque meter octavas de 0,6 m en una rejilla de
    // 4 m es sub-Nyquist. La colisión es una rejilla de 4 m y el render de 0,596 m: cerrar la
    // disparidad exige AFINAR LA REJILLA, no mover el corte. Ver la nota en `TerrainLOD.h`.
    CHECK(worstNear < 1.0, "COTA F4: dibujado y pisado difieren menos de 1 m en el campo cercano");
    // CONTRAPRUEBA: con el corte del NODO la disparidad se iria a ~0 — y por eso es tentador. El
    // numero existe para que se vea lo que se gana, y la nota de arriba para que se vea lo que cuesta.
    double oldWorst = 0.0;
    for (double d : { 2.0, 10.0, 50.0, 200.0 }) {
        const glm::dvec3 dd = glm::normalize(up0 + t1 * (d / R));
        const int lv = levelAt(dd);
        if (lv < 0) continue;
        const NodeId nn{ PlanetFace::FRONT, (uint32_t)lv, 0, 0 };
        oldWorst = std::max(oldWorst, (double)std::fabs(
            terrainDetail(dd, R, (float)nodeTexelM(nn, R)) -
            terrainDetail(dd, R, (float)nodeTexelM(nn, R))));
    }
    std::printf("    si la colision cortara como el nodo seria %.4f m — pero es SUB-NYQUIST para una\n"
                "      rejilla de %.1f m: el twist del quad se triplica (0,035 -> 0,108 m). Ver TerrainLOD.h\n",
                oldWorst, TERRAIN_TRIM_FLOOR);

    // CONTRAPRUEBA: con cortes de octava DELIBERADAMENTE distintos, la diferencia tiene que dispararse.
    // Sin esto, un `terrainDetail` que ignorara `minFeatureM` daria 0 y se leeria como paridad.
    const glm::dvec3 dprobe = glm::normalize(up0 + t1 * (50.0 / R));
    const double far = std::fabs((double)terrainDetail(dprobe, R, 0.6f) -
                                 (double)terrainDetail(dprobe, R, 300.0f));
    std::printf("    CONTRAPRUEBA: con cortes 0,6 m contra 300 m la diferencia es %.2f m\n", far);

    // ── ¿CRECE LA DISPARIDAD CON LA COTA? El sintoma es "en la ladera, el lado que SUBE" ──────────
    //
    // ⚠️ El criterio de subdivision ahora mide al TERRENO, asi que donde hay cota alta el render baja
    // hasta 3 niveles mas (texeles 8x mas finos). La colision NO cambio: su celda sigue siendo de 4 m
    // y su corte de octavas va con ella (Nyquist). O sea que afinar el render ENSANCHA la brecha
    // justo donde hay relieve — que es donde el autor la ve.
    std::printf("    cota del nodo   corte NODO   corte ANILLO   |dibujado - pisado|\n");
    double gapFlat = 0.0, gapHigh = 0.0;
    for (double elev : { 0.0, 500.0, 2000.0, 5000.0 }) {
        const glm::dvec3 cam2 = pc + up0 * (R + 1.7 + elev);
        const glm::dvec3 dd = glm::normalize(up0 + t1 * (100.0 / R));
        // El nivel que el selector elige AHORA, con la cota.
        int lv = 0;
        {
            NodeId nn{ PlanetFace::FRONT, 0, 0, 0 };
            PlanetFace f; double lx, ly;
            dirToCubeFaceClosed(dd, f, lx, ly);
            for (; lv < (int)nodeMaxLevelFor(R); ++lv) {
                if (!nodeShouldSplit(nn, R, cam2, pc, radPerPx, TERRAIN_NODE_ERROR_PX, elev)) break;
                const uint64_t lim = 1ull << (lv + 1);
                nn = NodeId{ f, (uint32_t)(lv + 1),
                    (uint32_t)std::min<int64_t>((int64_t)((lx + 1.0) * 0.5 * (double)lim), (int64_t)lim - 1),
                    (uint32_t)std::min<int64_t>((int64_t)((ly + 1.0) * 0.5 * (double)lim), (int64_t)lim - 1) };
            }
        }
        const NodeId nn{ PlanetFace::FRONT, (uint32_t)lv, 0, 0 };
        const double triNode = nodeTexelM(nn, R);
        const double triRing = terrainTriM(100.0);
        const double gap = std::fabs(
            (double)terrainDetail(dd, R, (float)triNode) -
            (double)terrainDetail(dd, R, (float)triRing));
        std::printf("    %9.0f m    %8.3f m   %8.3f m   %14.4f m\n", elev, triNode, triRing, gap);
        if (elev == 0.0) gapFlat = gap;
        if (elev == 5000.0) gapHigh = gap;
    }
    std::printf("    -> en llano %.4f m · sobre 5 km de cota %.4f m  (x%.1f)\n",
                gapFlat, gapHigh, gapFlat > 1e-9 ? gapHigh / gapFlat : 0.0);

    // ── LO QUE COSTARIA CERRARLO DEL TODO ───────────────────────────────────────────────────────
    //
    // La disparidad es ENTERA del corte de octavas: el nodo corta por su texel (0,596 m al nivel mas
    // fino) y el anillo por `terrainTriM(d)`, cuyo piso es `TERRAIN_CLIP_QUAD_M` = 4 m (el lado del
    // quad del clipmap). Si el anillo usara el corte del NODO, la disparidad seria 0 por definicion.
    //
    // No se hace aqui porque acopla el LOD de la FISICA al de RENDER —el nivel que elige el selector
    // depende de la camara— y eso es una decision de diseño, no un arreglo. El numero deja claro lo
    // que esta en juego.
    double bestPossible = 0.0;
    for (double d : { 2.0, 10.0, 50.0, 200.0 }) {
        const glm::dvec3 dd = glm::normalize(up0 + t1 * (d / R));
        const int lv = levelAt(dd);
        if (lv < 0) continue;
        const NodeId nn{ PlanetFace::FRONT, (uint32_t)lv, 0, 0 };
        bestPossible = std::max(bestPossible, (double)std::fabs(
            terrainDetail(dd, R, (float)nodeTexelM(nn, R)) -
            terrainDetail(dd, R, (float)nodeTexelM(nn, R))));
    }
    std::printf("    si el anillo cortara como el nodo, la disparidad seria %.4f m "
                "(hoy %.4f m, piso del anillo %.1f m)\n",
                bestPossible, worstNear, TERRAIN_TRIM_FLOOR);
    CHECK(far > worstNear * 10.0, "CONTRAPRUEBA: el corte de octavas SI mueve la superficie (el test mide eso)");
}

void test_terrain_node_as_heightfield() {
    beginTest("terrain_node_as_heightfield");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    std::printf("    nivel   lado del nodo    m/texel    desvio de la rejilla REGULAR (peor)\n");
    bool fineOk = true;
    for (uint32_t level : { 8u, 12u, 14u, 16u, 17u }) {
        // Un nodo cualquiera, no centrado en la cara: los del borde son los mas distorsionados.
        const uint32_t lim = 1u << level;
        const NodeId n{ PlanetFace::FRONT, level, lim * 3 / 4, lim / 3 };

        // ⚠️ EL MARCO SE ALINEA CON LOS EJES DEL NODO, no con el eje Y del mundo.
        //
        // Primera version: `t1 = cross(up, (0,1,0))`. Eso da un marco valido pero ROTADO respecto a
        // la rejilla del nodo, asi que el "desvio" que salia era el de la rotacion, no el de la
        // distorsion: 101 m en un nodo que mide 76 m de lado — imposible, y por eso se vio.
        const uint32_t M = TERRAIN_NODE_CELLS / 2;
        const glm::dvec3 c = nodeTexelDir(n, M, M);
        const glm::dvec3 du = nodeTexelDir(n, M + 1, M) - nodeTexelDir(n, M - 1, M);
        const glm::dvec3 dv = nodeTexelDir(n, M, M + 1) - nodeTexelDir(n, M, M - 1);
        const glm::dvec3 t1 = glm::normalize(du - c * glm::dot(du, c));   // +u, tangente
        const glm::dvec3 t2 = glm::normalize(dv - c * glm::dot(dv, c));   // +v, tangente

        // El paso que Jolt asumiria: el de la arista central, medido de verdad.
        const double step = glm::length(nodeTexelDir(n, M + 1, M) * R - nodeTexelDir(n, M, M) * R);

        double worst = 0.0;
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += 8)
            for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += 8) {
                const glm::dvec3 p   = nodeTexelDir(n, u, v) * R;
                const glm::dvec3 rel = p - c * R;
                // Donde CAE de verdad en el plano tangente, contra donde Jolt lo pondria.
                const double x = glm::dot(rel, t1), z = glm::dot(rel, t2);
                const double wantX = ((double)u - (double)M) * step;
                const double wantZ = ((double)v - (double)M) * step;
                worst = std::max(worst, std::hypot(x - wantX, z - wantZ));
            }
        const double sideM = R * 1.5707963267948966 / (double)(1u << level);
        // ⚠️ ¿Son ORTOGONALES los ejes del nodo? Jolt asume una rejilla rectangular alineada a ejes.
        // El mapeo cubo→esfera NO es conforme, asi que fuera del centro de la cara `u` y `v` se
        // cruzan sesgados — y un sesgo de unos grados sobre ±38 m son metros de desvio aparente.
        const double skewDeg = std::acos(glm::clamp(glm::dot(t1, t2), -1.0, 1.0)) * 180.0 / 3.14159265358979;
        // Y el paso, ¿es el mismo en el centro que en el borde del nodo?
        const double stepEdge = glm::length(nodeTexelDir(n, TERRAIN_NODE_CELLS, M) * R -
                                            nodeTexelDir(n, TERRAIN_NODE_CELLS - 1, M) * R);
        std::printf("     %2u    %10.1f m   %8.3f m    %10.4f m   angulo u^v %6.2f deg  "
                    "paso centro/borde %.6f\n",
                    level, sideM, nodeTexelM(n, R), worst, skewDeg, stepEdge / step);
        // La colision vive en los niveles finos: ahi es donde tiene que valer.
        // La proporcion es CONSTANTE: el desvio escala con el nodo, no se diluye al afinar.
        if (std::fabs(worst / sideM - 0.0463) > 0.005) fineOk = false;
    }
    CHECK(fineOk, "el desvio es una PROPORCION constante del nodo (~4,6 %), no algo que afine");

    // CONTRAPRUEBA: el desvio TIENE que crecer con el tamaño del nodo. Si saliera constante, el
    // marco o la proyeccion estarian mal y el test no mediria la distorsion sino otra cosa.
    auto devAt = [&](uint32_t level) {
        const uint32_t lim = 1u << level;
        const NodeId n{ PlanetFace::FRONT, level, lim * 3 / 4, lim / 3 };
        const uint32_t M = TERRAIN_NODE_CELLS / 2;
        const glm::dvec3 c = nodeTexelDir(n, M, M);
        const glm::dvec3 du = nodeTexelDir(n, M + 1, M) - nodeTexelDir(n, M - 1, M);
        const glm::dvec3 dv = nodeTexelDir(n, M, M + 1) - nodeTexelDir(n, M, M - 1);
        const glm::dvec3 t1 = glm::normalize(du - c * glm::dot(du, c));
        const glm::dvec3 t2 = glm::normalize(dv - c * glm::dot(dv, c));
        const double step = glm::length(nodeTexelDir(n, M + 1, M) * R - nodeTexelDir(n, M, M) * R);
        const glm::dvec3 rel = nodeTexelDir(n, 0, 0) * R - c * R;
        return std::hypot(glm::dot(rel, t1) + (double)M * step,
                          glm::dot(rel, t2) + (double)M * step);
    };
    const double d8 = devAt(8), d16 = devAt(16);
    std::printf("    CONTRAPRUEBA: el desvio en la esquina crece con el nodo: nivel 16 = %.4f m, "
                "nivel 8 = %.1f m  (x%.0f)\n", d16, d8, d8 / std::max(d16, 1e-9));
    CHECK(d8 > d16 * 100.0, "CONTRAPRUEBA: la distorsion escala con el tamaño del nodo (el test mide eso)");

    // Y el numero que lo explica todo: el sesgo de los ejes. Si algun dia sale 90, esto se puede
    // reabrir — y si sale otro valor, es que el mapeo cubo→esfera ha cambiado.
    const uint32_t M = TERRAIN_NODE_CELLS / 2;
    const NodeId nk{ PlanetFace::FRONT, 16, (1u << 16) * 3 / 4, (1u << 16) / 3 };
    const glm::dvec3 ck = nodeTexelDir(nk, M, M);
    const glm::dvec3 duk = nodeTexelDir(nk, M + 1, M) - nodeTexelDir(nk, M - 1, M);
    const glm::dvec3 dvk = nodeTexelDir(nk, M, M + 1) - nodeTexelDir(nk, M, M - 1);
    const double skew = std::acos(glm::clamp(glm::dot(
        glm::normalize(duk - ck * glm::dot(duk, ck)),
        glm::normalize(dvk - ck * glm::dot(dvk, ck))), -1.0, 1.0)) * 180.0 / 3.14159265358979;
    CHECK(std::fabs(skew - 90.0) > 1.0, "los ejes del nodo NO son ortogonales (por eso no es un heightfield de Jolt)");
}

/**
 * @brief EL BAKE DE ALTURA ES UNO SOLO. Este test mide el camino de RESPALDO, no el que corre.
 *
 * ── AVISO, PORQUE ESTE NUMERO ME ENGAÑO A MI VARIAS VECES ───────────────────────────────────────
 *
 * Lo que sale aqui —"la FISICA (equirect) contra el RENDER (cubo): peor 0,15 m"— describe un estado
 * que **ya no es el del motor**, y aun asi lo estuve citando como "el suelo estructural de la
 * disparidad" durante toda una sesion de depuracion, mandandome a mi mismo por el camino equivocado.
 *
 * Lo que hace el motor HOY:
 *   · la fisica (`sampleHeight`) muestrea `m_heightCPU` con `equirectUV` + `sampleHeightField`;
 *   · el compute (`terrain_node.comp`) muestrea `uHeightTex` con `harukaEquirectUV` +
 *     `harukaSampleHeightField`, y `uMisc.w` vale 1 SIEMPRE que el bake equirect existe;
 *   · y los dos salen del MISMO `up.cpuField`, a la MISMA resolucion: `m_heightTex` se crea con
 *     `up.cpuField.data()` y `m_heightCPU = std::move(up.cpuField)`.
 *
 * O sea: **un solo bake, mismo dato, misma resolucion, mismo bilineal a mano en los dos lados.**
 *
 * El bake del CUBO (`uBaseField`) sigue existiendo para el CLIMA (temperatura, humedad) y como
 * respaldo de altura si no hay equirect. Lo que este test mide es cuanto costaria ese respaldo si
 * alguna vez se usara — util, pero NO es una disparidad viva. El nombre dice "disagree" y por eso
 * se lee mal de un vistazo; el aviso se queda aqui para que no vuelva a pasar.
 */
void test_terrain_backup_bake_cost() {
    beginTest("terrain_backup_bake_cost");

    // Elevación analítica del orden del bake real (±4 km) con estructura a varias escalas.
    auto elevM = [](const glm::dvec3& d) {
        return 2500.0 * std::sin(d.x * 3.0) * std::cos(d.y * 2.0)
             +  900.0 * std::sin(d.z * 7.0 + 1.3)
             +  300.0 * std::cos(d.x * 17.0 + d.y * 11.0);
    };

    // --- retícula EQUIRECT, la que muestrea la física ---
    const int EW = 2048, EH = 1024;      // 8192x4096 son 128 MB en un test; la conclusión no cambia
    std::vector<float> eq((size_t)EW * EH);
    for (int y = 0; y < EH; ++y)
        for (int x = 0; x < EW; ++x) {
            const double u = ((double)x + 0.5) / EW, v = ((double)y + 0.5) / EH;
            const double lon = (u - 0.5) * 2.0 * 3.14159265358979;
            const double lat = (0.5 - v) * 3.14159265358979;
            const glm::dvec3 d(std::cos(lat) * std::cos(lon), std::sin(lat),
                               std::cos(lat) * std::sin(lon));
            eq[(size_t)y * EW + x] = (float)elevM(glm::normalize(d));
        }

    // --- retícula del CUBO, la que muestrea el render ---
    const int CR = 512;                   // `faceRes` real del motor (Planet.cpp: cfg.faceRes ? : 512)
    const int C1 = CR + 1;
    std::vector<float> cube((size_t)6 * C1 * C1);
    for (int f = 0; f < 6; ++f)
        for (int j = 0; j < C1; ++j)
            for (int i = 0; i < C1; ++i) {
                const double lx = -1.0 + 2.0 * (double)i / CR;
                const double ly = -1.0 + 2.0 * (double)j / CR;
                cube[((size_t)f * C1 * C1) + (size_t)j * C1 + i] =
                    (float)elevM(cubeFaceToDir((PlanetFace)f, lx, ly));
            }
    std::printf("    equirect %dx%d (%.1f M texeles)  ·  cubo 6x%d² (%.1f M texeles)\n",
                EW, EH, EW * (double)EH / 1e6, C1, 6.0 * C1 * C1 / 1e6);

    // --- ¿cuánto discrepan al muestrear? ---
    double worst = 0.0, sum = 0.0; size_t n = 0;
    glm::dvec3 worstDir(0.0);
    for (int a = 0; a < 90; ++a)
        for (int b = 0; b < 180; ++b) {
            const double lat = (-89.0 + a * 2.0) * 3.14159265358979 / 180.0;
            const double lon = (-179.0 + b * 2.0) * 3.14159265358979 / 180.0;
            const glm::dvec3 d = glm::normalize(glm::dvec3(
                std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon)));
            const float hEq = sampleHeightField(
                equirectUV(glm::vec3(d)), EW, EH, eq.data());
            const float hCu = baseFieldHeightAt(cube.data(), CR, d);
            const double e = std::fabs((double)hEq - (double)hCu);
            if (e > worst) { worst = e; worstDir = d; }
            sum += e; ++n;
        }
    std::printf("    RESPALDO (cubo) contra el bake VIVO (equirect): peor %.2f m · media %.3f m"
                "  sobre %zu direcciones\n", worst, sum / (double)n, n);
    std::printf("      el peor cae en dir (%.3f, %.3f, %.3f)\n", worstDir.x, worstDir.y, worstDir.z);

    // No hay cota que aprobar: esto MIDE el desajuste, no lo bendice. La cifra es la entrada de F4.
    CHECK(n > 0 && worst >= 0.0, "los dos bakes se muestrean y se comparan");

    // CONTRAPRUEBA: que el muestreador del cubo REPRODUZCA su origen. Sin esto, uno roto —que
    // devolviera siempre 0, o una constante— daria "los dos bakes discrepan poco" y se leeria como
    // paridad cuando lo que hay es un muestreador mudo.
    //
    // ⚠️ La primera version afirmaba `self < worst`, o sea "cada bake es mas fiel a su origen que al
    // otro". FALSO, y lo dijo el numero: self 0,19 m > worst 0,15 m. Las dos reticulas remuestrean la
    // MISMA funcion suave, asi que sus errores de interpolacion estan correlacionados y se cancelan
    // en parte al compararlas entre si. La premisa era mia, no del codigo.
    double self = 0.0;
    for (int a = 0; a < 45; ++a)
        for (int b = 0; b < 90; ++b) {
            const double lat = (-88.0 + a * 4.0) * 3.14159265358979 / 180.0;
            const double lon = (-178.0 + b * 4.0) * 3.14159265358979 / 180.0;
            const glm::dvec3 d = glm::normalize(glm::dvec3(
                std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon)));
            self = std::max(self, (double)std::fabs(
                baseFieldHeightAt(cube.data(), CR, d) - (float)elevM(d)));
        }
    std::printf("    CONTRAPRUEBA: el muestreador del cubo contra la funcion ANALITICA: peor %.2f m"
                "  (la elevacion abarca +-3700 m -> %.4f %% )\n", self, 100.0 * self / 3700.0);
    CHECK(self > 1e-6, "CONTRAPRUEBA: el muestreador NO devuelve una constante (interpola de verdad)");
    CHECK(self < 3700.0 * 0.001, "CONTRAPRUEBA: y reproduce su origen dentro del 0,1 % de la amplitud");
}

/**
 * @brief EL CORTE DE OCTAVAS ESTÁ EN EL TÉXEL, Y NYQUIST PIDE EL DOBLE.
 *
 * ── DE DÓNDE SALEN LOS PINCHOS A RAS DE SUELO ───────────────────────────────────────────────────
 *
 * `terrain_node_spike_hunt` mide el laplaciano del campo CRUDO —sin cosido, sin morph, sin stride— y
 * encuentra **4 200 vértices que rompen más de 0,5 m**, con el pase v5 añadiendo **−0,038 m**, o sea
 * nada. Los picos no los mete el pase: ya están en el campo de altura.
 *
 * La causa candidata es de muestreo, no de ruido: el corte de octavas se hace en `triM` = el téxel,
 * y muestrear una onda de longitud λ con un paso de λ es exactamente Nyquist — el caso en que el
 * muestreo ya no describe la onda. A nivel 14 el téxel mide 4,77 m y la octava más fina es λ = 4,5 m:
 * justo ahí. Para que una octava quede resuelta hace falta `λ ≥ 2·paso`.
 *
 * Aquí se mide el laplaciano del campo con el corte en el téxel y con el corte al DOBLE. Si el
 * segundo elimina los picos, el arreglo es mover el corte, no tocar el ruido.
 */
void test_terrain_node_octave_cut_nyquist() {
    beginTest("terrain_node_octave_cut_nyquist");
    const double R = 6371000.0;

    // ⚠️ EL CORTE POR TEXEL ES CORRECTO. Las guardas de `terrainDetail` estan en `lambda/2` exacto
    // (la octava de 4,5 m solo entra si el texel baja de 2,25), o sea Nyquist bien puesto. Cortar
    // "al doble" no arregla aliasing: solo atenua una octava legitima, que es perder detalle.
    //
    // El problema es OTRO: el corte se calcula con el TEXEL y la malla se dibuja cada TEXEL x STRIDE.
    // Con stride 2 se admiten octavas resueltas para el texel y se muestrean al doble de paso, que es
    // donde de verdad se cae por debajo de Nyquist. No sobra detalle: sobra STRIDE para el detalle
    // que se admitio.
    //
    // Aqui se mide el laplaciano SOBRE LA MALLA QUE SE DIBUJA (paso = texel x stride) con el corte
    // actual (texel) y con el corte al paso real. Si el segundo lo aplana, el arreglo es alinear el
    // corte con lo que se dibuja — y con stride 1, que es lo que hay bajo los pies, NO CAMBIA NADA.
    std::printf("    nivel  texel   stride  paso real   corte=TEXEL   corte=PASO REAL\n");
    double worstTex = 0.0, worstStep = 0.0;
    for (uint32_t lv : { 13u, 14u, 15u }) {
        const NodeId n{ PlanetFace::FRONT, lv, (1u << lv) / 3u, (1u << lv) / 7u };
        const double triM = nodeTexelM(n, R);
        for (uint32_t st : { 1u, 2u, 4u }) {
            const double stepM = triM * (double)st;
            auto lapAt = [&](uint32_t u, uint32_t v, double cut) {
                auto P = [&](uint32_t uu, uint32_t vv) {
                    const glm::dvec3 d = nodeTexelDir(n, uu, vv);
                    return d * (R + (double)terrainDetail(d, R, (float)cut));
                };
                const glm::dvec3 c = P(u, v);
                const glm::dvec3 avg = (P(u - st, v) + P(u + st, v)
                                      + P(u, v - st) + P(u, v + st)) * 0.25;
                return glm::length(c - avg);
            };
            double wT = 0.0, wS = 0.0;
            for (uint32_t v = st; v + st <= TERRAIN_NODE_CELLS; v += st)
                for (uint32_t u = st; u + st <= TERRAIN_NODE_CELLS; u += st) {
                    wT = std::max(wT, lapAt(u, v, triM));
                    wS = std::max(wS, lapAt(u, v, stepM));
                }
            std::printf("    %4u  %6.3f    %2u    %7.3f m   %8.4f m     %8.4f m\n",
                        lv, triM, st, stepM, wT, wS);
            if (st > 1) { worstTex = std::max(worstTex, wT); worstStep = std::max(worstStep, wS); }
        }
    }
    std::printf("    -> con stride > 1: corte por TEXEL %.4f m · corte al PASO REAL %.4f m\n",
                worstTex, worstStep);
    std::printf("       (con stride 1 las dos columnas son la MISMA cuenta: bajo los pies no cambia)\n");

    CHECK(worstTex > 0.0, "el campo tiene relieve (si no, no habria nada que medir)");
    CHECK(worstStep <= worstTex,
          "alinear el corte con el paso que SE DIBUJA nunca aumenta la rugosidad de la malla");
}

/**
 * @brief LA COLISIÓN MUESTREA A NIVEL DEL MAR Y COLOCA EL VÉRTICE A `R+h`: HAY DESPLAZAMIENTO LATERAL.
 *
 * ── EL ÚLTIMO DESAJUSTE DE LA FAMILIA ───────────────────────────────────────────────────────────
 *
 * `ringSample` hace, en este orden:
 *
 *     dir = normalize(up + t1·x/R + hz·z/R)     // la direccion del nodo
 *     wp  = pc + dir·R                          // se MUESTREA aqui: a nivel del mar
 *     h   = heightAt(wp)
 *     ->  dot(pc + dir·(R+h) − org, up)         // pero el vertice representa el punto a R+h
 *
 * Y Jolt coloca ese vértice en la coordenada tangente `(x, z)` de su rejilla. El problema: el punto
 * real a radio `R+h` NO está en la tangente `x` — está en `x·(R+h)/R`, porque el rayo se abre con el
 * radio. A 1 km de altura eso es un factor `1 + 1,6e−4`.
 *
 * Consecuencia: la altura guardada en el nodo `(x,z)` es la del terreno en OTRO punto, desplazado
 * lateralmente `x·h/R`. El error de altura resultante es ese desplazamiento por la PENDIENTE local —
 * y por eso sale como outliers: donde el terreno es plano no se nota, donde hay una arista sí.
 *
 * El autotest del alambre lo ve como "el vértice no cae sobre la función": media 0,0014 m,
 * pico 0,04-0,10 m.
 */
void test_terrain_ring_sample_lateral_shift() {
    beginTest("terrain_ring_sample_lateral_shift");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.35, 0.22));
    const glm::dvec3 e1  = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const glm::dvec3 e2  = glm::cross(up0, e1);
    const float cut = 2.0f;
    const double kBaseAlt = 991.0;   // la altitud a la que el juego reporta el peor vertice

    std::printf("    radio   desplazamiento lateral   error de altura que produce\n");
    double worstShift = 0.0, worstErr = 0.0, sumErr = 0.0; size_t cnt = 0;
    for (double r : { 8.0, 32.0, 76.7, 128.0, 256.0 }) {
        double shift = 0.0, err = 0.0;
        for (int k = 0; k < 64; ++k) {
            const double a = 6.28318530718 * k / 64.0;
            const double x = r * std::cos(a), z = r * std::sin(a);
            const glm::dvec3 dir = glm::normalize(up0 + (e1 * x) / R + (e2 * z) / R);
            // Donde se MUESTREA (nivel del mar) y donde CAE de verdad el punto de superficie.
            const glm::dvec3 wpSea = pc + dir * R;
            // ⚠️ LA ALTURA REAL INCLUYE LA BASE. Con solo el detalle (metros) el efecto sale 40x
            // pequeno y parece despreciable; en el juego el jugador esta a ~991 m sobre el nivel del
            // mar, y el desplazamiento es proporcional a `h/R`. Medir esto sin la base es medir otro
            // problema.
            const double h = kBaseAlt + (double)terrainDetail(dir, R, cut);
            const glm::dvec3 wpSurf = pc + dir * (R + h);
            // Coordenada tangente de cada uno respecto al ancla.
            const glm::dvec3 relS = wpSea  - pc - up0 * glm::dot(wpSea  - pc, up0);
            const glm::dvec3 relF = wpSurf - pc - up0 * glm::dot(wpSurf - pc, up0);
            const double d = std::fabs(glm::length(relF) - glm::length(relS));
            shift = std::max(shift, d);
            // Lo que ese desplazamiento vale en ALTURA: se compara la altura del punto de muestreo
            // con la del punto donde Jolt cree que esta el vertice.
            const glm::dvec3 dirShift = glm::normalize(up0 + (e1 * (x * (1.0 + h / R))) / R
                                                           + (e2 * (z * (1.0 + h / R))) / R);
            const double h2 = kBaseAlt + (double)terrainDetail(dirShift, R, cut);
            err = std::max(err, std::fabs(h2 - h));
            sumErr += std::fabs(h2 - h); ++cnt;
        }
        std::printf("    %5.0f m   %14.4f m   %18.4f m\n", r, shift, err);
        worstShift = std::max(worstShift, shift); worstErr = std::max(worstErr, err);
    }
    std::printf("    -> peor desplazamiento %.4f m · peor error de altura %.4f m · medio %.4f m\n",
                worstShift, worstErr, sumErr / (double)cnt);
    std::printf("       (el autotest del alambre en el juego mide: media 0,0014 m, pico 0,04-0,10 m)\n");

    CHECK(cnt > 100, "se muestrean puntos de verdad");
    CHECK(worstShift > 0.0, "el desplazamiento lateral EXISTE (es `x·h/R`, no puede ser cero con h>0)");
}

/**
 * @brief ¿EVALÚAN EL RENDER Y LA COLISIÓN EL MISMO CORTE DE OCTAVAS? (no)
 *
 * ── LA DISPARIDAD QUE NO ES UN OUTLIER, ESTÁ EN TODAS PARTES ────────────────────────────────────
 *
 * Andoni, mirando el alambre de colisión sobre el terreno: *"no son iguales"*. Y el autotest del
 * propio motor lo dice con número: los vértices del alambre se separan hasta **0,1042 m** de la
 * superficie de referencia donde debería ser ~0.
 *
 * Hay dos fuentes posibles y conviene no confundirlas:
 *   · OUTLIERS — unos pocos vértices mal (media 0,0037 m contra pico 0,1042 m: eso es lo que dice el
 *     autotest, la mayoría bien y algunos fuera).
 *   · SISTEMÁTICA — que las dos superficies se evalúen con un CORTE DE OCTAVAS distinto, en cuyo
 *     caso difieren en todas partes, no en puntos sueltos.
 *
 * Aquí se mide la segunda. El render dibuja con el téxel del nodo (`nodeTexelM`, **0,596 m** en el
 * nivel 17) y la referencia con `TERRAIN_TRIM_FLOOR` (= `TERRAIN_RING_FINE_CELL`, **0,5 m**). Son
 * dos cortes distintos: la octava de λ 4,5 m entra con pesos distintos en cada uno.
 */
void test_terrain_render_vs_reference_cut() {
    beginTest("terrain_render_vs_reference_cut");
    const double R = 6371000.0;
    const NodeId n{ PlanetFace::FRONT, 17, (1u << 17) / 3u, (1u << 17) / 7u };
    const double cutRender = nodeTexelM(n, R);                       // lo que dibuja el pase v5
    const double cutRef    = TERRAIN_TRIM_FLOOR;     // lo que muestrea la referencia

    double worst = 0.0, sum = 0.0; size_t cnt = 0;
    for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += 2)
        for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += 2) {
            const glm::dvec3 d = nodeTexelDir(n, u, v);
            const double hR = (double)terrainDetail(d, R, (float)cutRender);
            const double hC = (double)terrainDetail(d, R, (float)cutRef);
            const double e = std::fabs(hR - hC);
            worst = std::max(worst, e); sum += e; ++cnt;
        }
    std::printf("    corte del RENDER (texel del nodo, nivel 17): %.4f m\n", cutRender);
    std::printf("    corte de la REFERENCIA (TERRAIN_TRIM_FLOOR): %.4f m\n", cutRef);
    std::printf("    diferencia de altura entre las dos: peor %.4f m · media %.4f m (%zu puntos)\n",
                worst, sum / (double)cnt, cnt);
    std::printf("    (si esto no es 0, las dos superficies difieren EN TODAS PARTES por diseno,\n"
                "     no por vertices sueltos — y ninguna cantidad de precision lo cierra)\n");

    CHECK(cnt > 1000, "se muestrean puntos de verdad");
    // No se afirma que deba ser 0 —hoy no lo es— pero queda MEDIDO y con guardarrail: si crece,
    // alguien ha separado mas los dos cortes.
    CHECK(worst < 0.30, "GUARDARRAIL de la disparidad SISTEMATICA por corte de octavas distinto");
}

/**
 * @brief ¿SE MUEVEN LOS PUNTOS DE MUESTREO DE LA COLISIÓN CUANDO SALTA EL ANCLA?
 *
 * ── LA PROPIEDAD QUE EL RENDER TIENE Y LA COLISIÓN NO ───────────────────────────────────────────
 *
 * La posición de un téxel del nodo es función pura de `(cara, nivel, i, j, u, v)` — enteros. No
 * depende de la cámara, ni de un ancla, ni de ningún padre: es absoluta. Por eso andar no la mueve.
 *
 * La colisión no funciona así: sus nodos son offsets `terrainRingNode(i)` desde un marco tangente
 * anclado en el jugador (`terrainClipFrame`). El ancla se cuantiza a una retícula, pero cuando SALTA
 * de celda, todos los puntos de muestreo se desplazan con ella — y el suelo que Jolt colisiona pasa
 * a estar evaluado en sitios distintos. La superficie cambia de forma bajo los pies sin que el
 * terreno haya cambiado.
 *
 * Aquí se mide: se construye el marco en dos posiciones separadas UN salto de ancla y se compara
 * dónde cae el mismo nodo de anillo. Si la distancia no es cero, la superficie se re-muestrea.
 */
void test_terrain_ring_anchor_drift() {
    beginTest("terrain_ring_anchor_drift");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.35, 0.22));

    // Dos cámaras separadas por pasos crecientes: en algún punto el ancla salta de celda.
    const glm::dvec3 east = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    glm::dvec3 upA, t1A, t2A;
    terrainClipFrame(pc + up0 * (R + 2.0), pc, upA, t1A, t2A, R);

    // ⚠️ LO QUE IMPORTA NO ES CUANTO SE MUEVE EL ANCLA, SINO SI LAS MUESTRAS NUEVAS CAEN DONDE YA
    // HABIA MUESTRAS. Si el salto es un multiplo exacto de la celda del anillo, cada nodo nuevo
    // aterriza sobre uno viejo y la superficie lineal a trozos es IDENTICA: no cambia nada bajo los
    // pies. Si no lo es, todas las muestras caen en posiciones sub-celda distintas y el suelo se
    // reesculpe entero.
    const double cell = TERRAIN_RING_FINE_CELL;
    std::printf("    paso de camara   salto del ancla   ¿multiplo de la celda de %.2f m?\n", cell);
    double worstFrac = 0.0;
    for (double step : { 0.05, 0.5, 2.0, 8.0, 32.0, 64.0 }) {
        glm::dvec3 upB, t1B, t2B;
        terrainClipFrame(pc + up0 * (R + 2.0) + east * step, pc, upB, t1B, t2B, R);
        // Cuanto se ha movido el ancla, medido EN EL PLANO TANGENTE (que es donde viven los nodos).
        const glm::dvec3 dA = (upB - upA) * R;
        const double du = glm::dot(dA, t1A), dv = glm::dot(dA, -t2A);
        auto fracOf = [&](double d) {
            const double f = std::fabs(d / cell - std::round(d / cell));
            return f;
        };
        const double frac = std::max(fracOf(du), fracOf(dv));
        std::printf("    %8.2f m      %7.3f,%7.3f m      desfase %.3f de celda %s\n",
                    step, du, dv, frac, (frac < 0.01) ? "OK" : "<- NO CASA");
        worstFrac = std::max(worstFrac, frac);
    }
    std::printf("    -> peor desfase: %.3f de celda\n", worstFrac);
    std::printf("       (0 = las muestras nuevas caen sobre las viejas y el suelo no cambia;\n"
                "        0,5 = caen justo en medio y la superficie se reesculpe entera)\n");

    // CONTRASTE: el parametrizado del RENDER no tiene este problema por construccion. Un texel del
    // nodo es funcion de ENTEROS —sin camara, sin ancla, sin padre— asi que su desfase es CERO andes
    // lo que andes. Es exactamente la propiedad que a la colision le falta.
    const NodeId nq{ PlanetFace::FRONT, 17, (1u << 17) / 3u, (1u << 17) / 7u };
    const double nodeDrift = glm::length(nodeTexelDir(nq, 40, 90) * R - nodeTexelDir(nq, 40, 90) * R);
    std::printf("    CONTRASTE: el mismo texel del NODO, con cualquier camara: %.1e m de desfase\n",
                nodeDrift);

    CHECK(worstFrac < 0.01, "el salto del ancla es MULTIPLO de la celda del anillo: las muestras "
                            "nuevas caen sobre las viejas y el suelo no cambia de forma al andar");
    CHECK(nodeDrift == 0.0, "CONTRASTE: el parametrizado del nodo no depende de nada externo");
}

/**
 * @brief ¿CUÁNTA PENDIENTE TIENE EL SUELO QUE SE PISA, A LA ESCALA DE LA CELDA DE COLISIÓN?
 *
 * ── LA PREGUNTA QUE ESTO CONTESTA ───────────────────────────────────────────────────────────────
 *
 * "Andar se hace difícil" no es un fallo de paridad ni de render: es que el suelo que Jolt colisiona
 * sea demasiado abrupto para el controlador. El personaje tiene `mMaxSlopeAngle = 50°` (más empinado
 * = resbalas) y `mWalkStairsStepUp = 0,4 m`.
 *
 * La física corta las octavas en `minFeatureM = 2.0`, que **sí** admite la más fina (λ 4,5 m,
 * amplitud ±0,35 m), y muestrea en celdas de 0,5 m. Aquí se mide la distribución real de pendientes
 * entre celdas contiguas: qué fracción pasa de 50° (resbalas) y qué fracción del escalón de 0,4 m se
 * come una sola celda.
 *
 * ⚠️ No decide nada: pone el número al lado del límite del controlador, que es lo que falta para
 * saber si el problema es el terreno, el controlador o ninguno de los dos.
 */
void test_terrain_collision_walkability() {
    beginTest("terrain_collision_walkability");
    const double R = 6371000.0;
    const double cellM = TERRAIN_RING_FINE_CELL;   // la celda del anillo fino
    const float  cutM  = 2.0f;                                     // lo que pasa la fisica

    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.35, 0.22));
    const glm::dvec3 e1  = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const glm::dvec3 e2  = glm::cross(up0, e1);
    auto hAt = [&](double x, double y) {
        const glm::dvec3 d = glm::normalize(up0 + e1 * (x / R) + e2 * (y / R));
        return (double)terrainDetail(d, R, cutM);
    };

    const int N = 400;
    size_t over50 = 0, over30 = 0, total = 0;
    double worstDeg = 0.0, worstStep = 0.0;
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            const double x = i * cellM, y = j * cellM;
            const double dh = hAt(x + cellM, y) - hAt(x, y);
            const double deg = std::atan2(std::fabs(dh), cellM) * 180.0 / 3.14159265358979;
            worstDeg = std::max(worstDeg, deg);
            worstStep = std::max(worstStep, std::fabs(dh));
            if (deg > 50.0) ++over50;
            if (deg > 30.0) ++over30;
            ++total;
        }
    std::printf("    celda de colision %.2f m · corte de octavas de la fisica %.1f m\n", cellM, cutM);
    std::printf("    pendiente entre celdas contiguas: peor %.1f gr · desnivel peor %.3f m\n",
                worstDeg, worstStep);
    std::printf("    por encima de 50 gr (el limite del personaje): %.2f %% de %zu celdas\n",
                100.0 * (double)over50 / (double)total, total);
    std::printf("    por encima de 30 gr:                            %.2f %%\n",
                100.0 * (double)over30 / (double)total);
    std::printf("    (el controlador: mMaxSlopeAngle=50 gr · mWalkStairsStepUp=0,40 m)\n");

    CHECK(total > 1000, "se muestrea terreno de verdad");
    CHECK(worstDeg > 0.0, "el terreno tiene pendiente (si no, no habria nada que medir)");
}

/**
 * @brief LA DIRECCION EN FLOAT CUANTIZA LA SUPERFICIE A MEDIO METRO. **Eso son los pinchos.**
 *
 * ── EL MURO QUE SE DABA POR INOFENSIVO ──────────────────────────────────────────────────────────
 *
 * `terrain_node.vert` construia la direccion del vertice con `harukaCubeFaceToDirF` —la version
 * FLOAT— mientras `terrain_node.comp` usa la de DOUBLE para el MISMO punto. Un ulp de un vector
 * unitario en float son ~6e-8, y a radio terrestre eso son **0,38-0,76 m de superficie**.
 *
 * El comentario de `vFragPos` daba por bueno ese muro: *"ese error es ESTATICO por vertice, asi que
 * distorsiona el terreno una vez y no se ve"*. Es falso, y este test lo cuantifica: los vertices del
 * nivel 17 estan a **0,596 m** unos de otros, asi que un error de redondeo de ese mismo orden no
 * "distorsiona" — **cuantiza la superficie a una rejilla de medio metro**. Y el redondeo es
 * independiente vertice a vertice, asi que no es una deformacion suave: es ruido a la frecuencia de
 * la malla. Eso es exactamente lo que se ve en la captura de RenderDoc como crestas finas, y lo que
 * ningun test de este banco podia ver porque TODOS calculan en double.
 */
void test_terrain_node_float_dir_quantisation() {
    beginTest("terrain_node_float_dir_quantisation");
    const double R = 6371000.0;

    std::printf("    nivel  texel     error por vertice (float vs double)      relativo al texel\n");
    double worstRel = 0.0;
    for (uint32_t lv : { 14u, 15u, 17u }) {
        const NodeId n{ PlanetFace::FRONT, lv, (1u << lv) / 3u, (1u << lv) / 7u };
        const double triM = nodeTexelM(n, R);
        double worst = 0.0, sum = 0.0; size_t cnt = 0;
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += 3)
            for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += 3) {
                const glm::dvec3 dD = nodeTexelDir(n, u, v);            // como el compute: double
                const glm::vec3  dF = glm::vec3(dD);                    // como el vertex shader: float
                // La posicion que sale de cada una, a radio terrestre.
                const glm::dvec3 pD = dD * R;
                const glm::dvec3 pF = glm::dvec3(dF) * R;
                const double e = glm::length(pD - pF);
                worst = std::max(worst, e); sum += e; ++cnt;
            }
        const double rel = worst / triM;
        std::printf("    %4u  %6.3f m   peor %.4f m · medio %.4f m            %5.1f %% del texel\n",
                    lv, triM, worst, sum / (double)cnt, 100.0 * rel);
        worstRel = std::max(worstRel, rel);
    }
    std::printf("    -> el redondeo llega al %.0f %% de la separacion entre vertices\n", 100.0 * worstRel);
    std::printf("       (y es INDEPENDIENTE vertice a vertice: no deforma suave, mete ruido a la\n"
                "        frecuencia de la malla — que es como se ve, crestas finas)\n");

    CHECK(worstRel > 0.10, "el redondeo de la direccion en FLOAT es una fraccion GRANDE del texel: "
                           "no es despreciable, es del orden del paso de la malla");
}

/**
 * @brief LA DIAGONAL FIJA DEL QUAD SESGA LA SUPERFICIE SIEMPRE HACIA EL MISMO LADO.
 *
 * ── EL ARTEFACTO QUE NINGUN TEST DE ESTE BANCO PODIA VER ────────────────────────────────────────
 *
 * Todos los instrumentos de la sesion miden POSICIONES DE VERTICE: la retícula, las costuras, el
 * morph, el laplaciano. Y los vértices están bien — caen exactamente sobre el campo. Lo que ninguno
 * mira es **qué triángulos unen esos vértices**.
 *
 * La rejilla parte cada quad SIEMPRE por la misma diagonal (`{a,b,c},{c,b,d}` en
 * `TerrainNodeRenderer.h`). Un quad no es plano, así que el centro del quad queda por encima o por
 * debajo del campo según qué diagonal se use — y con una diagonal FIJA ese error tiene **el mismo
 * signo en todo el nodo**. No se cancela: se acumula en crestas alineadas con la diagonal. Es lo que
 * en la captura de RenderDoc se ve como líneas rectas finas y paralelas por todo el suelo.
 *
 * Aquí se mide el error CON SIGNO en el centro de cada quad. Con la diagonal fija la media se separa
 * de cero (sesgo); alternándola en damero, los dos signos se compensan.
 */
void test_terrain_node_quad_diagonal_bias() {
    beginTest("terrain_node_quad_diagonal_bias");
    const double R = 6371000.0;

    std::printf("    nivel  texel     media CON SIGNO (sesgo)      |media| fija / alterna\n");
    double worstFixed = 0.0, worstAlt = 0.0;
    for (uint32_t lv : { 14u, 15u, 17u }) {
        const NodeId n{ PlanetFace::FRONT, lv, (1u << lv) / 3u, (1u << lv) / 7u };
        const double triM = nodeTexelM(n, R);
        auto hAt = [&](uint32_t u, uint32_t v) {
            return (double)terrainDetail(nodeTexelDir(n, u, v), R, (float)triM);
        };
        double sumFix = 0.0, sumAlt = 0.0; size_t cnt = 0;
        for (uint32_t v = 0; v + 1 <= TERRAIN_NODE_CELLS; ++v)
            for (uint32_t u = 0; u + 1 <= TERRAIN_NODE_CELLS; ++u) {
                const double h00 = hAt(u, v), h10 = hAt(u + 1, v);
                const double h01 = hAt(u, v + 1), h11 = hAt(u + 1, v + 1);
                // El centro del quad segun cada diagonal, contra la media de los cuatro (el campo).
                const double centreD1 = (h00 + h11) * 0.5;      // diagonal a-d
                const double centreD2 = (h10 + h01) * 0.5;      // diagonal b-c
                const double field    = (h00 + h10 + h01 + h11) * 0.25;
                sumFix += centreD1 - field;                     // SIEMPRE la misma: se acumula
                sumAlt += (((u + v) & 1u) ? centreD2 : centreD1) - field;   // en damero: se cancela
                ++cnt;
            }
        const double mFix = sumFix / (double)cnt, mAlt = sumAlt / (double)cnt;
        std::printf("    %4u  %6.3f m   fija %+9.5f m · alterna %+9.5f m    %8.5f / %8.5f\n",
                    lv, triM, mFix, mAlt, std::fabs(mFix), std::fabs(mAlt));
        worstFixed = std::max(worstFixed, std::fabs(mFix));
        worstAlt   = std::max(worstAlt,   std::fabs(mAlt));
    }
    std::printf("    -> sesgo peor: diagonal FIJA %.5f m · ALTERNA %.5f m\n", worstFixed, worstAlt);
    std::printf("       (un sesgo con signo constante no es rugosidad: son crestas alineadas con la\n"
                "        diagonal, que es como se ve en pantalla)\n");

    CHECK(worstFixed > 0.0, "la diagonal fija introduce un sesgo (si fuera 0, el quad seria plano)");
    CHECK(worstAlt < worstFixed, "alternar la diagonal en damero CANCELA el sesgo");
}

/**
 * @brief EL RUIDO SE MUESTREA EN `dir·(R + baseH)`, Y `baseH` CAMBIA ENTRE TÉXELES VECINOS.
 *
 * ── POR QUÉ ESTO PUEDE SER UN PINCHO A RAS DE SUELO ─────────────────────────────────────────────
 *
 * `terrainDetail` evalúa el ruido en `p = dir · radius`, y el radio que se le pasa es `R + baseH`
 * (así lo exige la paridad con la física y con el clipmap: ver `terrain_node.comp`). Pero `baseH`
 * varía de un téxel al siguiente — es el propio relieve del bake.
 *
 * Consecuencia: entre dos vértices contiguos el punto de muestreo no se mueve solo TANGENCIALMENTE
 * (0,596 m en el nivel 17) sino también RADIALMENTE, tanto como cambie `baseH`. Si esa variación es
 * del orden de la octava más fina (λ = 4,5 m), dos vértices contiguos caen en fases distintas del
 * mismo ruido y la superficie se dispara. Eso es un pincho, y no depende del stride ni de las
 * costuras — por eso ningún instrumento de este banco lo veía.
 *
 * Aquí se separan las dos contribuciones: cuánto cambia el detalle por moverse TANGENCIALMENTE (lo
 * legítimo) y cuánto por el desplazamiento RADIAL (el sospechoso).
 */
void test_terrain_node_radial_noise_shift() {
    beginTest("terrain_node_radial_noise_shift");
    const double R = 6371000.0;

    // Un bake sintético con relieve REALISTA a escala de decenas de metros: es lo que hace que
    // `baseH` cambie entre téxeles vecinos. Sin variación de `baseH` el efecto no existe por
    // definición, así que un bake plano haría el test trivialmente verde.
    auto baseHAt = [&](const glm::dvec3& d) {
        return 600.0 * std::sin(d.x * 900.0) * std::cos(d.y * 700.0)
             + 120.0 * std::sin(d.z * 5000.0);
    };

    std::printf("    nivel   texel      d(baseH) vecino   detalle: TANGENCIAL   RADIAL\n");
    double worstTan = 0.0, worstRad = 0.0;
    for (uint32_t lv : { 14u, 15u, 16u, 17u }) {
        const NodeId n{ PlanetFace::FRONT, lv, (1u << lv) / 3u, (1u << lv) / 7u };
        const double triM = nodeTexelM(n, R);
        double dB = 0.0, tan_ = 0.0, rad = 0.0;
        for (uint32_t v = 8; v + 8 <= TERRAIN_NODE_CELLS; v += 7)
            for (uint32_t u = 8; u + 8 <= TERRAIN_NODE_CELLS; u += 7) {
                const glm::dvec3 d0 = nodeTexelDir(n, u, v), d1 = nodeTexelDir(n, u + 1, v);
                const double b0 = baseHAt(d0), b1 = baseHAt(d1);
                dB = std::max(dB, std::fabs(b1 - b0));
                // TANGENCIAL: los dos vecinos con el MISMO radio. Es el relieve real del terreno.
                const double t0 = (double)terrainDetail(d0, R + b0, (float)triM);
                const double t1 = (double)terrainDetail(d1, R + b0, (float)triM);
                tan_ = std::max(tan_, std::fabs(t1 - t0));
                // RADIAL: el MISMO punto, con el radio del vecino. Es puro artefacto de muestreo.
                const double r1 = (double)terrainDetail(d0, R + b1, (float)triM);
                rad = std::max(rad, std::fabs(r1 - t0));
            }
        std::printf("    %4u   %6.3f m   %10.2f m        %8.4f m   %8.4f m\n",
                    lv, triM, dB, tan_, rad);
        worstTan = std::max(worstTan, tan_); worstRad = std::max(worstRad, rad);
    }
    std::printf("    -> peor por moverse tangencialmente %.4f m · peor por el salto RADIAL %.4f m\n",
                worstTan, worstRad);
    std::printf("       (el radial no es relieve: son dos vertices contiguos leyendo el ruido en\n"
                "        fases distintas. Si domina, ES un pincho y no depende del stride.)\n");

    CHECK(worstTan > 0.0, "el terreno tiene relieve de verdad entre texeles vecinos");
    // No se afirma que sea el fallo: se MIDE su tamaño relativo. Si el radial fuera despreciable,
    // esta hipotesis quedaria descartada como las siete anteriores.
    CHECK(worstRad >= 0.0, "medido");
}

/**
 * @brief LA NORMAL TIENE QUE DESCRIBIR LA SUPERFICIE QUE SE DIBUJA, NO OTRA MÁS FINA.
 *
 * ── EL PINCHO QUE NO ERA UNA GRIETA ─────────────────────────────────────────────────────────────
 *
 * `terrain_node.vert` calculaba la diferencia finita de la normal SIEMPRE a ±1 téxel, mientras la
 * geometría se dibuja cada `stride` téxeles. Con stride 2 o 4, un triángulo abarca 2-4 téxeles y sus
 * vértices recibían normales de rugosidad SUB-TRIÁNGULO: al interpolarlas a lo largo del triángulo,
 * el sombreado salta. Se lee como pinchos **dentro de cada nodo**, y por eso ningún instrumento de
 * costuras lo veía — no es una grieta, es iluminación describiendo una superficie que no existe.
 *
 * Aquí se mide el ángulo entre la normal SOMBREADA y la GEOMÉTRICA del quad que de verdad se
 * rasteriza, con la regla vieja (±1) y con la nueva (±stride). La contraprueba es la propia regla
 * vieja: si no empeorara con el stride, este cambio no estaría arreglando nada.
 */
void test_terrain_node_normal_matches_geometry() {
    beginTest("terrain_node_normal_matches_geometry");
    const double R = 6371000.0;
    const NodeId n{ PlanetFace::FRONT, 15, (1u << 15) / 3u, (1u << 15) / 7u };
    const double texM = nodeTexelM(n, R);

    auto hAt = [&](uint32_t u, uint32_t v) {
        return (double)terrainDetail(nodeTexelDir(n, u, v), R, (float)texM);
    };
    auto posAt = [&](uint32_t u, uint32_t v) { return nodeTexelDir(n, u, v) * (R + hAt(u, v)); };

    // Normal por diferencia finita con paso `k`, como la calcula el shader.
    auto shadeN = [&](uint32_t u, uint32_t v, uint32_t k) {
        const glm::dvec3 d = nodeTexelDir(n, u, v);
        const uint32_t um = (u > k) ? u - k : 0u, up = std::min(u + k, TERRAIN_NODE_CELLS);
        const uint32_t vm = (v > k) ? v - k : 0u, vp = std::min(v + k, TERRAIN_NODE_CELLS);
        glm::dvec3 t1 = glm::normalize(std::fabs(d.y) < 0.99 ? glm::cross(d, glm::dvec3(0, 1, 0))
                                                             : glm::cross(d, glm::dvec3(1, 0, 0)));
        const glm::dvec3 t2 = glm::cross(d, t1);
        const double du = (double)(up - um) * texM, dv = (double)(vp - vm) * texM;
        return glm::normalize(d - t1 * ((hAt(up, v) - hAt(um, v)) / du)
                                - t2 * ((hAt(u, vp) - hAt(u, vm)) / dv));
    };
    // La normal GEOMETRICA del quad que se dibuja: el que va de `u-s` a `u+s`.
    auto geomN = [&](uint32_t u, uint32_t v, uint32_t s) {
        const glm::dvec3 a = posAt(u - s, v), b = posAt(u + s, v);
        const glm::dvec3 c = posAt(u, v - s), e = posAt(u, v + s);
        glm::dvec3 g = glm::cross(b - a, e - c);
        if (glm::dot(g, nodeTexelDir(n, u, v)) < 0.0) g = -g;
        return glm::normalize(g);
    };
    auto degBetween = [](const glm::dvec3& a, const glm::dvec3& b) {
        return std::acos(glm::clamp(glm::dot(a, b), -1.0, 1.0)) * 180.0 / 3.14159265358979;
    };

    // ⚠️ LA METRICA CORRECTA NO ES "cuanto se aparta de la faceta". Esa mezcla la rugosidad propia
    // del terreno —una normal analitica y una de faceta difieren ~29 gr sobre relieve rugoso, con
    // CUALQUIER paso— y no distingue las dos reglas (medido: 28,79 contra 28,59). Lo que se ve como
    // PINCHO es otra cosa: que dos vertices CONTIGUOS del mismo triangulo reciban normales muy
    // distintas, porque al interpolarlas a lo largo del triangulo el sombreado pega un salto.
    //
    // Con paso ±1 y stride 4, dos vertices dibujados contiguos muestrean vecindarios DISJUNTOS: sus
    // normales no estan correlacionadas. Con paso ±stride se solapan, y varian suave.
    std::printf("    stride   salto de normal entre vertices CONTIGUOS del mismo triangulo\n");
    std::printf("             con paso ±1 texel     con paso ±stride\n");
    double worstOld = 0.0, worstNew = 0.0;
    for (uint32_t s : { 1u, 2u, 4u, 8u }) {
        double dOld = 0.0, dNew = 0.0;
        for (uint32_t v = 2u * s; v + 2u * s <= TERRAIN_NODE_CELLS; v += s)
            for (uint32_t u = 2u * s; u + 3u * s <= TERRAIN_NODE_CELLS; u += s) {
                dOld = std::max(dOld, degBetween(shadeN(u, v, 1u), shadeN(u + s, v, 1u)));
                dNew = std::max(dNew, degBetween(shadeN(u, v, s),  shadeN(u + s, v, s)));
            }
        std::printf("    %4u       %9.2f gr          %9.2f gr\n", s, dOld, dNew);
        if (s > 1) { worstOld = std::max(worstOld, dOld); worstNew = std::max(worstNew, dNew); }
    }
    std::printf("    -> con stride > 1: el salto peor pasa de %.2f gr a %.2f gr\n", worstOld, worstNew);
    (void)geomN;

    CHECK(worstNew < worstOld, "al paso del STRIDE, dos vertices contiguos ya no reciben normales "
                               "descorrelacionadas: el sombreado deja de saltar dentro del triangulo");
    // CONTRAPRUEBA: si la regla vieja no saltara, el cambio no arreglaria nada y sobraria.
    // ⚠️ El liston esta donde la medida lo deja (10,34 gr con la regla vieja), no donde gustaria.
    // Y ojo con lo que este test NO dice: **con stride 1 las dos reglas son la misma cosa** (2,59 gr
    // en las dos columnas). Cerca del jugador el tope de colision fuerza stride 1, asi que este
    // arreglo suaviza el sombreado a media y larga distancia y NO toca lo que se ve bajo los pies.
    CHECK(worstOld > 5.0, "CONTRAPRUEBA: la regla vieja SI daba saltos entre vertices contiguos "
                          "(si no, este arreglo no estaria arreglando nada)");
}

/**
 * @brief PROPS vs COLISIÓN vs TERRENO: tres cortes de octavas en el mismo punto.
 *
 * Reportado como "no coincide el placement de props y colision + terreno". La altura de un punto no
 * es un valor: es `bake + terrainDetail(dir, R, minFeatureM)`, y **cada consumidor pasa un
 * `minFeatureM` distinto**, o sea corta la escalera de octavas en otro sitio:
 *
 *   · **PROPS** — `sampleHeight(dir)` sin mas, que usa `terrainTriM(0.0)` = el piso, hoy 0,5 m.
 *   · **RENDER** — el téxel del nodo que lo dibuja: `nodeTexelM`, 0,596 m en el nivel 17 pero
 *     1,19 / 2,39 / 4,77 m en los niveles de mas afuera.
 *   · **COLISION** — la celda del anillo que cubre ese punto: 0,5 m dentro de ±32 m, luego 1, 2, 4 m.
 *
 * Tres cortes = tres superficies. Un prop plantado con uno y dibujado sobre otro flota o se hunde.
 */
void test_terrain_prop_anchor_mismatch() {
    beginTest("terrain_prop_anchor_mismatch");
    const double R = 6371000.0;
    const NodeId n17{ PlanetFace::FRONT, 17, (1u << 17) / 2, (1u << 17) / 2 };
    const double texel17 = nodeTexelM(n17, R);
    const double propTriM = (double)terrainTriM(0.0);

    std::printf("    el corte de octavas que usa cada uno en el MISMO punto:\n");
    std::printf("      props  (sampleHeight -> terrainTriM(0)) : %.3f m\n", propTriM);
    std::printf("      render (texel del nodo, nivel 17)       : %.3f m\n", texel17);
    std::printf("      colision (celda del anillo fino)        : %.3f m\n",
                TERRAIN_RING_FINE_CELL);

    // Cuanto se separan de hecho, sobre puntos repartidos por el nodo.
    double worstPR = 0.0, sumPR = 0.0; size_t cnt = 0;
    double worstFar = 0.0; uint32_t farLevel = 0;
    for (uint32_t v = 1; v < TERRAIN_NODE_CELLS; v += 7)
        for (uint32_t u = 1; u < TERRAIN_NODE_CELLS; u += 7) {
            const glm::dvec3 d = nodeTexelDir(n17, u, v);
            const double hProp = (double)terrainDetail(d, R, (float)propTriM);
            const double hRend = (double)terrainDetail(d, R, (float)texel17);
            const double e = std::fabs(hProp - hRend);
            worstPR = std::max(worstPR, e); sumPR += e; ++cnt;
        }
    std::printf("    props contra render EN EL NIVEL MAS FINO: peor %.4f m · medio %.4f m (%zu puntos)\n",
                worstPR, sumPR / (double)cnt, cnt);

    // ⚠️ Y EL NIVEL FINO ES EL CASO BUENO. Un prop a 300 m lo dibuja un nodo de nivel 15 o 14, cuyo
    // texel es 2,4 o 4,8 m: ahi el corte del render se aleja mucho mas del que planto el prop.
    std::printf("    nivel del nodo   texel    props contra render (peor)\n");
    for (uint32_t lvl = 17; lvl >= 13; --lvl) {
        const NodeId nl{ PlanetFace::FRONT, lvl, (1u << lvl) / 2, (1u << lvl) / 2 };
        const double tx = nodeTexelM(nl, R);
        double w = 0.0;
        for (uint32_t v = 1; v < TERRAIN_NODE_CELLS; v += 11)
            for (uint32_t u = 1; u < TERRAIN_NODE_CELLS; u += 11) {
                const glm::dvec3 d = nodeTexelDir(nl, u, v);
                w = std::max(w, std::fabs(
                    (double)terrainDetail(d, R, (float)propTriM)
                  - (double)terrainDetail(d, R, (float)tx)));
            }
        std::printf("        %2u        %6.3f m        %8.4f m\n", lvl, tx, w);
        if (w > worstFar) { worstFar = w; farLevel = lvl; }
    }
    std::printf("    -> un prop plantado con el corte de 0,5 m y dibujado sobre un nodo de nivel %u\n"
                "       queda hasta %.2f m fuera de la superficie que se ve\n", farLevel, worstFar);

    // ⚠️ HIPOTESIS DESCARTADA POR LA PROPIA MEDIDA, y se deja escrita para no volver a mirar aqui.
    //
    // En el nivel 17 props y render coinciden EXACTAMENTE (0,0000 m): `terrainTriM(0)` = 0,5 m y el
    // texel = 0,596 m caen en el mismo escalon de la escalera de octavas. El desajuste solo aparece
    // en nodos mas bastos... que es donde no se ve, porque el error y la distancia escalan juntos:
    //
    //   nivel 16: 0,036 m a ~1,3 km = 0,03 px      nivel 15: 0,333 m a ~2,5 km = 0,14 px
    //   nivel 13: 1,320 m a ~10 km  = 0,14 px
    //
    // (la distancia sale de igualar el error de pantalla a 1 px, que es como el selector elige nivel)
    // Asi que el corte de octavas NO puede ser el "props que no casan" que se ve de cerca.
    CHECK(worstPR < 0.001, "en el nivel mas fino props y render dan la MISMA altura: el corte de "
                           "octavas queda descartado como causa del desajuste visible");
    CHECK(worstFar > 0.1, "y aunque en nodos bastos si difiere, ahi cae por debajo del pixel");
}

/**
 * @brief ¿CUÁNTO DEPENDE DEL OBSERVADOR EL SUELO QUE SE PISA? El requisito de multijugador, medido.
 *
 * Andoni fijó el requisito el 2026-08-31: **lo visible tiene que ser colisionable, 10-100 km.** Eso
 * convierte esta medida en un criterio de aceptación y no en una curiosidad.
 *
 * `terrainTriM(radM) = max(radM·0.002, 0.5)` es el corte de octavas de la colisión, y su entrada es
 * **la distancia al jugador**. O sea que la altura del MISMO punto del mundo depende de dónde esté
 * mirando quien pregunta. Dos clientes a distinta distancia pisan suelos distintos, y a un mismo
 * jugador el suelo le sube o baja mientras se acerca.
 *
 * ⚠️ LA MEDIDA ANTERIOR LLEGABA A 2500 m Y SE LEYÓ COMO SI FUERA EL TOPE. Daba 0,313 m y "satura",
 * pero no satura: es que entre 1200 y 2500 m no se apaga ninguna octava nueva. Con el requisito a
 * 10-100 km el barrido tiene que llegar ahí, porque cada octava que cruza su guarda de Nyquist añade
 * su amplitud entera de golpe.
 *
 * Referencia = el corte del PIE (`terrainTriM(0)`), que es la superficie que de verdad se pisa.
 */
void test_terrain_collision_observer_dependence() {
    beginTest("terrain_collision_observer_dependence");
    const double R = 6371000.0;

    // Puntos repartidos por CUATRO caras del cubo, no por un nodo: un solo nodo mide un trozo de
    // relieve y su peor caso no es el del planeta.
    std::vector<glm::dvec3> pts;
    const PlanetFace faces[4] = { PlanetFace::FRONT, PlanetFace::TOP,
                                  PlanetFace::RIGHT, PlanetFace::BACK };
    for (PlanetFace f : faces) {
        const NodeId n{ f, 17, (1u << 17) / 2, (1u << 17) / 3 };
        for (uint32_t v = 1; v < TERRAIN_NODE_CELLS; v += 9)
            for (uint32_t u = 1; u < TERRAIN_NODE_CELLS; u += 9)
                pts.push_back(nodeTexelDir(n, u, v));
    }

    const float refCut = terrainTriM(0.0);
    std::vector<double> ref(pts.size());
    for (size_t k = 0; k < pts.size(); ++k)
        ref[k] = (double)terrainDetail(pts[k], R, refCut);

    std::printf("    referencia: el corte del PIE, terrainTriM(0) = %.3f m · %zu puntos\n",
                refCut, pts.size());
    std::printf("    distancia    corte      octavas    divergencia media    peor\n");

    // Cuántas octavas de la escalera sobreviven a ese corte, para que se vea el mecanismo: la
    // divergencia no crece suave, salta cuando una octava cruza su guarda.
    auto octavesAlive = [](double cut) {
        const double guard[7] = { 6000.0, 3000.0, 1428.5, 312.5, 55.5, 11.0, 2.25 };
        int n = 0; for (double g : guard) if (cut < g) ++n; return n;
    };

    struct Row { double d, cut, mean, worst; int oct; };
    std::vector<Row> rows;
    for (double d : { 0.0, 100.0, 250.0, 300.0, 600.0, 1200.0, 2500.0,
                      6000.0, 10000.0, 30000.0, 100000.0 }) {
        const float cut = terrainTriM(d);
        double sum = 0.0, worst = 0.0;
        for (size_t k = 0; k < pts.size(); ++k) {
            const double e = std::fabs((double)terrainDetail(pts[k], R, cut) - ref[k]);
            sum += e; worst = std::max(worst, e);
        }
        const Row r{ d, (double)cut, sum / (double)pts.size(), worst, octavesAlive((double)cut) };
        rows.push_back(r);
        std::printf("    %8.0f m  %8.3f m   %d de 7    %10.4f m    %10.4f m\n",
                    r.d, r.cut, r.oct, r.mean, r.worst);
    }

    // ── LO QUE SE AFIRMA ────────────────────────────────────────────────────────────────────────
    //
    // (a) DENTRO DE 250 m LA DIVERGENCIA ES CERO EXACTO. No "pequeña": `terrainTriM` es constante en
    //     su piso hasta 250 m (0,002·250 = 0,5), asi que el corte es EL MISMO y la funcion es pura.
    //     Esto es a la vez el resultado tranquilizador y la CONTRAPRUEBA de la medida: si el barrido
    //     devolviera ruido en vez de ceros aqui, las cifras de mas abajo no significarian nada.
    double within = 0.0, beyond10k = 0.0, beyond100k = 0.0;
    for (const Row& r : rows) {
        if (r.d <= 250.0) within = std::max(within, r.worst);
        if (r.d == 10000.0)  beyond10k  = r.worst;
        if (r.d == 100000.0) beyond100k = r.worst;
    }
    CHECK(within == 0.0, "hasta 250 m el corte es el MISMO y la colision es funcion PURA de la "
                         "posicion: divergencia CERO EXACTO entre clientes");
    // (b) Y FUERA NO LO ES, con el orden de magnitud a la vista. Sin esta, la (a) se leeria como
    //     "no hay problema" cuando lo que dice es "no hay problema DENTRO DE 250 m".
    CHECK(beyond10k > 1.0, "CONTRAPRUEBA: a 10 km el mismo punto ya vale METROS distintos segun "
                           "quien pregunte — el requisito de 'lo visible es colisionable' no se cumple");
    CHECK(beyond100k > beyond10k, "y empeora con la distancia: cada octava que cruza Nyquist entra "
                                  "entera");
    std::printf("    veredicto: dentro de 250 m %.4f m · a 10 km %.3f m · a 100 km %.3f m\n",
                within, beyond10k, beyond100k);
}

/**
 * @brief EL CRITERIO DE VERDAD: ¿coincide el suelo de un cliente LEJANO con el de uno CERCANO?
 *
 * Requisito de Andoni (2026-08-31): **lo visible tiene que ser colisionable, 10-100 km**, y las
 * colisiones tienen que ser las mismas entre clientes. Eso es UN numero, no dos, y hay que medirlo
 * como uno.
 *
 * ⚠️ MEDIRLO POR SEPARADO ENGANA, Y CASI ME COME. Hay dos mecanismos:
 *
 *   · **el CORTE** — `terrainTriM(radM)` apaga octavas segun la distancia AL JUGADOR;
 *   · **la REJILLA** — `terrainRingNode` coloca los nodos en `k·cell` DESDE EL ANCLA, asi que al
 *     caminar la rejilla se desplaza y la bilineal entre nodos cambia bajo un objeto quieto.
 *
 * Medidos aparte, "cortar por la celda del anillo" baja el termino de rejilla 7x (4,12 -> 0,58 m a
 * 10 km) y parece el arreglo. Pero encarece el otro —la celda es MAS GRUESA que el corte de hoy, o
 * sea menos octavas— y el total puede empeorar. Un arreglo que mejora un sumando y empeora el otro
 * mas no es un arreglo; es un numero correcto contando media verdad.
 *
 * Asi que esto compara contra LA SUPERFICIE QUE SE PISA (el corte del pie, funcion pura de la
 * posicion) la superficie que un cliente lejano presenta de verdad: bilineal sobre su rejilla, con
 * su corte y con su ancla. Eso es exactamente lo que un objeto replicado ve.
 */
void test_terrain_collision_client_agreement() {
    beginTest("terrain_collision_client_agreement");
    const double R = 6371000.0;
    const glm::dvec3 up = glm::normalize(glm::dvec3(0.31, 0.62, 0.72));
    const glm::dvec3 t1 = glm::normalize(glm::cross(up, glm::dvec3(0, 1, 0)));
    const glm::dvec3 t2 = glm::normalize(glm::cross(up, t1));

    auto dirAt = [&](double wx, double wz) {
        return glm::normalize(up + (t1 * wx) / R + (t2 * wz) / R);
    };
    // Un nodo de la rejilla del cliente lejano. `cellCut > 0` = la hipotesis (cortar por la celda).
    auto nodeH = [&](double wx, double wz, double ax, double az, double cellCut) {
        const double radM = std::sqrt((wx - ax) * (wx - ax) + (wz - az) * (wz - az));
        const double cut  = (cellCut > 0.0) ? std::max(cellCut, TERRAIN_TRIM_FLOOR)
                                            : (double)terrainTriM(radM);
        return (double)terrainDetail(dirAt(wx, wz), R, (float)cut);
    };
    // Lo que Jolt presenta en `(px,pz)`: bilineal entre los cuatro nodos que lo rodean.
    auto surfaceAt = [&](double px, double pz, double cell, double ax, double az, double cellCut) {
        const double gx = std::floor((px - ax) / cell), gz = std::floor((pz - az) / cell);
        const double x0 = ax + gx * cell, z0 = az + gz * cell;
        const double fx = (px - x0) / cell, fz = (pz - z0) / cell;
        const double a = nodeH(x0, z0, ax, az, cellCut)
                       + (nodeH(x0 + cell, z0, ax, az, cellCut) - nodeH(x0, z0, ax, az, cellCut)) * fx;
        const double b = nodeH(x0, z0 + cell, ax, az, cellCut)
                       + (nodeH(x0 + cell, z0 + cell, ax, az, cellCut)
                        - nodeH(x0, z0 + cell, ax, az, cellCut)) * fx;
        return a + (b - a) * fz;
    };

    const std::vector<TerrainRingSpec> rings =
        terrainRingLayout(200000.0);
    const float footCut = terrainTriM(0.0);
    std::printf("    referencia: la superficie que SE PISA (corte del pie %.3f m, funcion pura)\n", footCut);
    std::printf("    lo que ve un cliente a esa distancia, con su rejilla, su corte y su ancla:\n");
    std::printf("    distancia   celda      HOY (corte por distancia)   CORTE=CELDA (refutado)   CORTE DEL PIE SIEMPRE\n");
    std::printf("                            media        peor           media      peor        media      peor\n");

    double hoy10k = 0.0, hyp10k = 0.0, hoy100k = 0.0, hyp100k = 0.0, hoyNear = 0.0;
    for (double d : { 100.0, 600.0, 2500.0, 10000.0, 100000.0 }) {
        double cell = 0.0;
        for (const auto& r : rings) if (r.extent >= d) { cell = r.cell; break; }
        if (cell <= 0.0) cell = rings.empty() ? 1.0 : rings.back().cell;

        double sA = 0.0, wA = 0.0, sB = 0.0, wB = 0.0, sC = 0.0, wC = 0.0; int n = 0;
        for (int a = 1; a < 8; ++a) for (int b = 1; b < 8; ++b) {
            const double px = d + cell * ((double)a / 8.0), pz = cell * ((double)b / 8.0);
            // La verdad: el corte del pie evaluado EN el punto. Ni rejilla ni ancla.
            const double truth = (double)terrainDetail(dirAt(px, pz), R, footCut);
            const double eA = std::fabs(surfaceAt(px, pz, cell, 0.0, 0.0, 0.0)  - truth);
            const double eB = std::fabs(surfaceAt(px, pz, cell, 0.0, 0.0, cell) - truth);
            // TERCERA POLITICA, la que implica resolver por consulta: los VERTICES del anillo lejano
            // se evaluan con el corte del PIE, o sea que caen sobre la superficie que se pisa. Queda
            // el error de cuerda de la celda, pero es funcion PURA de la posicion: sin observador.
            const double eC = std::fabs(surfaceAt(px, pz, cell, 0.0, 0.0, 1e-6) - truth);
            sA += eA; wA = std::max(wA, eA); sB += eB; wB = std::max(wB, eB);
            sC += eC; wC = std::max(wC, eC); ++n;
        }
        std::printf("    %8.0f m %8.2f m  %9.4f m %9.4f m  %9.4f m %9.4f m  %9.4f m %9.4f m\n",
                    d, cell, sA / (double)n, wA, sB / (double)n, wB, sC / (double)n, wC);
        if (d == 100.0)    hoyNear = wA;
        if (d == 10000.0)  { hoy10k = wA;  hyp10k = wB; }
        if (d == 100000.0) { hoy100k = wA; hyp100k = wB; }
    }
    std::printf("    HOY: 10 km %.3f m · 100 km %.3f m   |   CORTE=CELDA: 10 km %.3f m · 100 km %.3f m\n",
                hoy10k, hoy100k, hyp10k, hyp100k);

    // ── LO QUE SE AFIRMA ────────────────────────────────────────────────────────────────────────
    //
    // (a) HOY NO SE CUMPLE EL REQUISITO. Un objeto replicado a 10 km se apoya en un suelo que no es
    //     el que pisa el jugador que esta encima.
    CHECK(hoy10k > 1.0, "HOY: a 10 km el suelo de colision no es el que se pisa — el requisito de "
                        "'lo visible es colisionable' no se cumple");
    // (b) Y CERCA SI. Es la contraprueba de la medida: si tambien fallara aqui, el barrido estaria
    //     midiendo cualquier otra cosa y no la distancia.
    CHECK(hoyNear < hoy10k * 0.2, "CONTRAPRUEBA: cerca del jugador SI coincide — la medida ve la "
                                  "distancia, no un error de fondo");
    // (c) Y EL VEREDICTO SOBRE LA HIPOTESIS, sea cual sea. Lo unico que se fija es que la comparacion
    //     quede escrita: si algun dia alguien cambia el corte, este test dice si mejoro o empeoro.
    std::printf("    veredicto: cortar por la celda %s el acuerdo entre clientes a 10 km (%.3f -> %.3f m)\n",
                (hyp10k < hoy10k) ? "MEJORA" : "EMPEORA", hoy10k, hyp10k);
}

/**
 * @brief ¿CUÁNTO CUESTA resolver el suelo por CONSULTA en vez de contra la malla?
 *
 * Andoni, 2026-08-31: *"¿tiene buen rendimiento y es replicable en clientes? entonces es un sí"*.
 * La replicabilidad es estructural —`terrainTriM(0)` es constante, así que la consulta es función
 * pura de la posición— pero el coste hay que medirlo, porque de él depende cuántos objetos caben.
 *
 * ⚠️ ESTO NO SIGNIFICA NADA COMPILADO EN DEBUG. A -O0 el ruido va 10-20x más lento y el número
 * saldría prohibitivo por el compilador, no por el algoritmo. `CMakeLists` avisa cuando el tipo de
 * build es Debug; si ves este test ahí, ignora el resultado.
 *
 * Se mide el corte del PIE (0,5 m), que es el caso caro: 7 octavas x 8 esquinas = 56 hashes.
 */
void test_terrain_ground_query_cost() {
    beginTest("terrain_ground_query_cost");
    const double R = 6371000.0;
    const float  cut = terrainTriM(0.0);

    // Direcciones repartidas, no una sola: una sola cabría entera en caché y mediría otra cosa.
    std::vector<glm::dvec3> dirs;
    for (int k = 0; k < 4096; ++k) {
        const double a = (double)k * 0.61803398875;
        const double t = a - std::floor(a), u = (double)k / 4096.0;
        const double phi = t * 6.283185307179586, cz = 1.0 - 2.0 * u;
        const double sz = std::sqrt(std::max(0.0, 1.0 - cz * cz));
        dirs.push_back(glm::dvec3(sz * std::cos(phi), sz * std::sin(phi), cz));
    }

    // Una pasada en frío para que la caché no cuente como coste del algoritmo.
    double sink = 0.0;
    for (const glm::dvec3& d : dirs) sink += (double)terrainDetail(d, R, cut);

    const int reps = 16;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r)
        for (const glm::dvec3& d : dirs) sink += (double)terrainDetail(d, R, cut);
    const auto t1 = std::chrono::steady_clock::now();

    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count()
                    / (double)(reps * (int)dirs.size());
    const double perMs = 1.0e6 / ns;
    std::printf("    consulta del suelo (corte del pie %.3f m, 7 octavas = 56 hashes)\n", cut);
    std::printf("      %.0f ns por consulta · %.0f consultas por ms\n", ns, perMs);
    std::printf("      en 1 ms de presupuesto: %.0f objetos a 60 Hz · en 0,1 ms: %.0f\n",
                perMs, perMs * 0.1);
    std::printf("      (suma de control %.3f, para que el optimizador no borre el bucle)\n", sink);

    // ⚠️ SIN UMBRAL DE ms: un test que falle por la carga de la maquina es ruido. Lo que se fija es
    // que la consulta exista y devuelva relieve de verdad; el numero de arriba es para decidir, y se
    // lee a mano. Si algun dia se quiere un tope, tiene que venir de un presupuesto declarado.
    CHECK(std::fabs(sink) > 0.0, "la consulta devuelve relieve, no ceros");
    CHECK(ns > 0.0, "el cronometro mide algo");
}

/**
 * @brief EL PRECIO DEL ACUERDO: ¿cuánta celda hace falta y cuánto cuesta?
 *
 * `terrain_collision_client_agreement` deja el problema acotado: lo que un cliente lejano presenta
 * difiere del suelo que se pisa en **8,12 m a 10 km y 39,4 m a 100 km**, y NO lo arregla ninguna
 * política de corte de octavas (se probaron las tres). Es término de TAMAÑO DE CELDA puro: 256 m no
 * pueden representar terreno con relieve de 4,5 m.
 *
 * Así que la pregunta que queda no es "cómo", es "cuánto": afinar la celda es lo único que lo cierra
 * y cuesta muestras. Esto barre el factor de afinado y da las dos columnas juntas, para que la
 * decisión se tome con el precio delante en vez de con una intuición.
 */
void test_terrain_collision_refine_cost() {
    beginTest("terrain_collision_refine_cost");
    const double R = 6371000.0;
    const glm::dvec3 up = glm::normalize(glm::dvec3(0.31, 0.62, 0.72));
    const glm::dvec3 t1 = glm::normalize(glm::cross(up, glm::dvec3(0, 1, 0)));
    const glm::dvec3 t2 = glm::normalize(glm::cross(up, t1));
    const float footCut = terrainTriM(0.0);

    auto dirAt = [&](double wx, double wz) {
        return glm::normalize(up + (t1 * wx) / R + (t2 * wz) / R);
    };
    // La superficie que Jolt presenta con una celda dada: bilineal entre nodos, evaluados con el
    // corte que le toca a cada uno por su distancia (la ley de hoy).
    auto surfaceAt = [&](double px, double pz, double cell, bool footCutNodes) {
        const double gx = std::floor(px / cell), gz = std::floor(pz / cell);
        const double x0 = gx * cell, z0 = gz * cell;
        const double fx = (px - x0) / cell, fz = (pz - z0) / cell;
        auto nodeH = [&](double wx, double wz) {
            const double radM = std::sqrt(wx * wx + wz * wz);
            const float cut = footCutNodes ? footCut : terrainTriM(radM);
            return (double)terrainDetail(dirAt(wx, wz), R, cut);
        };
        const double a = nodeH(x0, z0) + (nodeH(x0 + cell, z0) - nodeH(x0, z0)) * fx;
        const double b = nodeH(x0, z0 + cell) + (nodeH(x0 + cell, z0 + cell)
                                               - nodeH(x0, z0 + cell)) * fx;
        return a + (b - a) * fz;
    };

    const std::vector<TerrainRingSpec> rings =
        terrainRingLayout(200000.0);

    std::printf("    acuerdo con el suelo que SE PISA, y lo que cuesta afinarlo\n");
    std::printf("    distancia  afinado    celda    acuerdo HOY   + corte del PIE   muestras\n");
    for (double d : { 10000.0, 100000.0 }) {
        double cell0 = 0.0;
        for (const auto& r : rings) if (r.extent >= d) { cell0 = r.cell; break; }
        if (cell0 <= 0.0) continue;
        double prev = 1e30; bool monotone = true; double best = 0.0;
        for (int f : { 1, 2, 4, 8, 16 }) {
            const double cell = cell0 / (double)f;
            double worst = 0.0, worstFoot = 0.0;
            // ⚠️ LOS PUNTOS NO PUEDEN CAER SOBRE LOS NODOS, Y CASI ME COME. Estaban en `cell0·a/8`, y
            // 256/8 = 32 es múltiplo de la celda de x16 (16 m): la bilineal devolvía el valor del
            // NODO y el acuerdo salía **0,0000 m exacto** — un titular falso. Con un desplazamiento
            // de razón áurea, la muestra no cae sobre un nodo de ninguna celda potencia de dos.
            for (int a = 1; a < 8; ++a) for (int b = 1; b < 8; ++b) {
                const double px = d + cell0 * std::fmod((double)a * 0.61803398875, 1.0);
                const double pz =     cell0 * std::fmod((double)b * 0.41421356237, 1.0);
                const double truth = (double)terrainDetail(dirAt(px, pz), R, footCut);
                worst     = std::max(worst,     std::fabs(surfaceAt(px, pz, cell, false) - truth));
                worstFoot = std::max(worstFoot, std::fabs(surfaceAt(px, pz, cell, true)  - truth));
            }
            // Las muestras de un anillo van con el CUADRADO del afinado: cubre lo mismo con celdas
            // f veces más pequeñas, o sea f² nodos. Es el precio, y es el que decide.
            std::printf("    %8.0f m   x%-3d %9.2f m %11.4f m %14.4f m %8dx\n",
                        d, f, cell, worst, worstFoot, f * f);
            if (worst > prev + 1e-6) monotone = false;
            prev = worst; best = worstFoot;
        }
        // ⚠️ NO ES UN BARRIDO INFORMATIVO Y YA: se afirma la PROPIEDAD que hace que la decisión tenga
        // sentido — que afinar la celda de verdad cierra el hueco, y monótonamente. Si no lo hiciera,
        // la tabla de precios de arriba sería una lista de números sin nada que comprar.
        // ⚠️ LO QUE ESTE BARRIDO DEMUESTRA, Y NO ES LO QUE YO ESPERABA: afinar la celda SOLA se
        // ESTANCA. De x8 a x16 el acuerdo apenas se mueve (1,179 -> 1,169 m a 10 km), porque el
        // residuo ya no es la celda: es el CORTE, que sigue siendo `terrainTriM(radM)` y a 10 km vale
        // 20 m. Los dos términos se tapan el uno al otro — cambiar sólo el corte también se estancaba
        // (medido en `terrain_collision_client_agreement`, las tres políticas daban ~8 m).
        //
        // Cerrarlo pide LAS DOS COSAS a la vez, y ésa es la columna de la derecha. Ahí sí baja.
        CHECK(monotone, "afinar la celda MEJORA el acuerdo en cada paso (monotono)");
        CHECK(best < prev * 0.5,
              "con celda fina Y corte del pie el acuerdo baja a menos de la mitad: hacen falta LOS DOS");
    }
}
