// ================================================================================================
// F1 del PLAN TERRENO v5 — el direccionamiento de nodos del quadtree
//
// Lo que se verifica aquí NO es "que los números salgan parecidos": es que salgan **idénticos bit a
// bit**. Toda la arquitectura del v5 se apoya en tres propiedades que solo valen si son exactas:
//
//   · grueso ⊂ fino  -> un nodo de nivel L y uno de L+1 comparten téxeles SIN interpolar
//   · sin grietas    -> dos nodos vecinos evalúan su arista compartida con el MISMO double
//   · determinismo   -> mismos enteros, mismos bits, en cualquier máquina
//
// Con tolerancia, las tres se cumplen "casi" y el quadtree tiene grietas de submilímetro que la
// iluminación amplifica. Por eso todas las comprobaciones de este fichero son `==` sobre `double`,
// que es lo que normalmente sería un error y aquí es justo el punto.
//
// Cada afirmación lleva su CONTRAPRUEBA: una variante que TIENE que fallar. Sin ella, comparar una
// fórmula consigo misma da 0.0 y no demuestra nada — el fallo que dejó inútil a `detail_triM_parity`.
// ================================================================================================
#include "World/Terrain/TerrainNodeTestCommon.h"

namespace {

// Igualdad BIT A BIT de dos doubles. `==` bastaría, pero esto deja explícito que no hay epsilon y
// atrapa además el caso NaN (que con `==` pasaría desapercibido como "distinto" en vez de romper).
bool sameBits(double a, double b) {
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}
bool sameBits(const glm::dvec3& a, const glm::dvec3& b) {
    return sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.z, b.z);
}

// ── LA CONTRAPRUEBA: el camino que el v5 SUSTITUYE ──────────────────────────────────────────────
//
// Reconstruye la dirección de un punto como lo hacía el clipmap: proyectándola sobre un marco
// tangente anclado y rehaciéndola en FLOAT (`ringSample` sigue haciendo exactamente esto).
// Es el camino real que el v5 sustituye.
//
// ⚠️ Y es la contraprueba CORRECTA tras dos intentos fallidos, que se anotan porque el motivo enseña
// más que el test:
//   · "acumular el paso en vez de dividir" -> con paso potencia de dos exacta, acumular TAMBIÉN
//     sale exacto. No rompe nada.
//   · "usar un número de celdas que no sea potencia de dos" -> tampoco rompe: el hijo calcula
//     `2a/2b` y el padre `a/b`, y duplicar numerador y denominador es exacto en base 2.
// De ahí la corrección al diseño: la exactitud NO la da la potencia de dos, la da calcular el
// numerador en ENTEROS y dividir UNA sola vez. La potencia de dos aporta que además cada valor sea
// exactamente representable, que es lo que importa aguas abajo (float, GPU).
glm::dvec3 dirViaTangentFrame(const glm::dvec3& d, const glm::dvec3& anchor) {
    // Marco tangente en el ancla, como `terrainClipFrame`.
    const glm::dvec3 up = glm::normalize(anchor);
    glm::dvec3 t1 = glm::cross(glm::abs(up.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0), up);
    t1 = glm::normalize(t1);
    const glm::dvec3 t2 = glm::cross(up, t1);
    // Ida y vuelta por el marco, con el paso por FLOAT que hace el shader.
    const float x = (float)glm::dot(d, t1);
    const float y = (float)glm::dot(d, t2);
    const float z = (float)glm::dot(d, up);
    const glm::vec3 f = glm::normalize(glm::vec3(t1) * x + glm::vec3(t2) * y + glm::vec3(up) * z);
    return glm::normalize(glm::dvec3(f));
}

} // namespace

void test_terrain_node_lattice() {
    beginTest("terrain_node_lattice");

    // La celda potencia de dos no es lo que da la coincidencia padre↔hijo (eso lo da el numerador
    // entero y una sola división — ver la nota de la contraprueba). Lo que da es que cada `lx` sea
    // EXACTAMENTE REPRESENTABLE, que es lo que importa aguas abajo: en float y en la GPU.
    const bool cellsPow2 = (TERRAIN_NODE_CELLS & (TERRAIN_NODE_CELLS - 1u)) == 0u;
    std::printf("    nodo de %u texeles (%u celdas, potencia de dos: %s)\n",
                TERRAIN_NODE_TEXELS, TERRAIN_NODE_CELLS, cellsPow2 ? "si" : "NO");
    CHECK(cellsPow2, "las celdas por nodo son potencia de dos (cada lx exactamente representable)");

    // ── (1) GRUESO ⊂ FINO, BIT A BIT ────────────────────────────────────────────────────────────
    // El téxel `u` de un nodo de nivel L cae sobre el téxel `2u` de su hijo izquierdo (y los de la
    // mitad derecha sobre el hijo derecho). Si esto fuese solo "casi", el heightmap del padre y el
    // del hijo describirían superficies distintas en los mismos puntos y el cambio de nivel daría
    // un salto — el "popping" que el LOD por error en pantalla NO puede evitar por sí solo.
    int checked = 0, mismatches = 0;
    for (int f = 0; f < 6; ++f) {
        const NodeId parent{ (PlanetFace)f, 3, 5, 2 };
        NodeId kids[4]; nodeChildren(parent, kids);
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; ++v)
            for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; ++u) {
                // A qué hijo y a qué téxel del hijo corresponde este téxel del padre.
                const uint32_t child = (u >= TERRAIN_NODE_CELLS / 2 ? 1u : 0u)
                                     + (v >= TERRAIN_NODE_CELLS / 2 ? 2u : 0u);
                const uint32_t cu = (u - (child & 1u) * (TERRAIN_NODE_CELLS / 2)) * 2u;
                const uint32_t cv = (v - ((child >> 1) & 1u) * (TERRAIN_NODE_CELLS / 2)) * 2u;
                const glm::dvec3 a = nodeTexelDir(parent, u, v);
                const glm::dvec3 b = nodeTexelDir(kids[child], cu, cv);
                ++checked;
                if (!sameBits(a, b)) ++mismatches;
            }
    }
    std::printf("    grueso ⊂ fino: %d texeles comparados padre↔hijo · %d distintos\n",
                checked, mismatches);
    CHECK(mismatches == 0, "cada texel del padre cae BIT A BIT sobre uno del hijo");

    // ── (2) SIN GRIETAS: la arista compartida de dos vecinos ────────────────────────────────────
    // El borde derecho del nodo (i,j) es el borde izquierdo del (i+1,j). Los dos tienen téxel propio
    // ahí (por eso el nodo es 2^k+1 y no 2^k), y tienen que dar el MISMO double. Si difirieran, la
    // costura se abre — y con normales analíticas la grieta se ve aunque mida micras.
    int edge = 0, edgeBad = 0;
    for (int f = 0; f < 6; ++f) {
        const NodeId left { (PlanetFace)f, 4, 3, 6 };
        const NodeId right{ (PlanetFace)f, 4, 4, 6 };
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; ++v) {
            const glm::dvec3 a = nodeTexelDir(left,  TERRAIN_NODE_CELLS, v);   // borde derecho
            const glm::dvec3 b = nodeTexelDir(right, 0,                  v);   // borde izquierdo
            ++edge;
            if (!sameBits(a, b)) ++edgeBad;
        }
    }
    std::printf("    costura: %d texeles de arista compartida · %d distintos\n", edge, edgeBad);
    CHECK(edgeBad == 0, "dos nodos vecinos dan la MISMA direccion en su arista compartida");

    // ── (3) COSTURA ENTRE NIVELES DISTINTOS (el caso T-junction) ────────────────────────────────
    // Un nodo fino contra un vecino GRUESO. Los nodos del grueso que tienen gemelo fino deben
    // coincidir bit a bit; los de en medio no existen en el grueso y son los que hay que resolver en
    // el render (faldas o casado de niveles). Aquí se comprueba lo primero, que es lo que permite lo
    // segundo: si ni los gemelos coincidieran, no habría nada a lo que casar.
    int tj = 0, tjBad = 0;
    {
        // `coarse` (nivel 3, i=2) va de lx -0,50 a -0,25; `fine` (nivel 4, i=6) de -0,25 a -0,125.
        // O sea que el borde DERECHO del grueso toca el borde IZQUIERDO del fino. Y en ly, el fino
        // (nivel 4, j=2: -0,750 a -0,625) cubre la MITAD BAJA del grueso (nivel 3, j=1: -0,75 a -0,50),
        // así que el téxel `v` del fino corresponde al `v/2` del grueso.
        const NodeId coarse{ PlanetFace::FRONT, 3, 2, 1 };
        const NodeId fine  { PlanetFace::FRONT, 4, 6, 2 };
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += 2) {
            const glm::dvec3 a = nodeTexelDir(coarse, TERRAIN_NODE_CELLS, v / 2);   // derecho del grueso
            const glm::dvec3 b = nodeTexelDir(fine,   0,                  v);       // izquierdo del fino
            ++tj;
            if (!sameBits(a, b)) ++tjBad;
        }
    }
    std::printf("    T-junction: %d nodos gruesos con gemelo fino · %d distintos\n", tj, tjBad);
    CHECK(tjBad == 0, "los nodos del nivel grueso caen bit a bit sobre los del fino");

    // ── (4) DETERMINISMO ────────────────────────────────────────────────────────────────────────
    // Mismos enteros, mismos bits. Trivial hoy, y por eso mismo conviene fijarlo: es la propiedad
    // que hace que cliente y servidor generen el MISMO nodo, y la que un `fma` mal puesto rompería.
    bool stable = true;
    for (int r = 0; r < 3 && stable; ++r)
        for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS && stable; u += 7) {
            const NodeId n{ PlanetFace::LEFT, 7, 41, 93 };
            if (!sameBits(nodeTexelDir(n, u, 11), nodeTexelDir(n, u, 11))) stable = false;
        }
    CHECK(stable, "misma entrada entera -> mismos bits");

    // ── CONTRAPRUEBA: contra el camino que esto sustituye ───────────────────────────────────────
    // Todo lo de arriba compara la fórmula consigo misma. Lo que le da dientes es medirlo contra el
    // camino REAL que el v5 reemplaza: reconstruir la dirección por un marco tangente en float, que
    // es lo que hacen hoy `ringSample` y `clipmap.tese`.
    int viaFrameBad = 0; double worstM = 0.0;
    const double R = 6371000.0;
    {
        const NodeId n{ PlanetFace::FRONT, 8, 130, 77 };
        const glm::dvec3 anchor = nodeTexelDir(n, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2);
        for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += 4) {
            const glm::dvec3 exact = nodeTexelDir(n, u, 64);
            const glm::dvec3 viaF  = dirViaTangentFrame(exact, anchor);
            if (!sameBits(exact, viaF)) ++viaFrameBad;
            worstM = std::max(worstM, glm::length(exact - viaF) * R);
        }
    }
    std::printf("    CONTRAPRUEBA (reconstruir por marco tangente en float, como hoy):\n");
    std::printf("      %d de %u texeles NO reproducen los bits · peor separacion %.4f m de superficie\n",
                viaFrameBad, TERRAIN_NODE_TEXELS / 4 + 1, worstM);
    CHECK(viaFrameBad > 0,
          "CONTRAPRUEBA: el camino actual (marco tangente en float) NO es bit-exacto");
    std::printf("      -> el direccionamiento por enteros da 0 bits de diferencia donde ese da %.4f m\n",
                worstM);

}

void test_terrain_node_scale() {
    beginTest("terrain_node_scale");
    const double R = 6371000.0;

    std::printf("    nivel   nodos/cara      lado del nodo      m/texel\n");
    for (uint32_t L : { 0u, 4u, 8u, 12u, 16u, 20u }) {
        const NodeId n{ PlanetFace::FRONT, L, 0, 0 };
        std::printf("    %5u   %10llu   %12.1f m   %10.3f m\n",
                    L, (unsigned long long)(1ull << L) * (1ull << L),
                    nodeSpanM(n, R), nodeTexelM(n, R));
    }

    // El nivel más fino tiene que bajar del metro por téxel: por debajo de eso vive la octava más
    // fina de `TerrainDetail.h` (λ 4,5 m), y si el nodo no puede representarla, el quadtree no
    // arregla el "se ve basto" — solo mueve el problema.
    const NodeId finest{ PlanetFace::FRONT, nodeMaxLevelFor(R), 0, 0 };
    const double finestTexel = nodeTexelM(finest, R);
    std::printf("    texel mas fino: %.3f m  (la octava mas fina es lambda 4.5 m -> hacen falta <2.25 m)\n",
                finestTexel);
    CHECK(finestTexel < 2.25, "el nivel mas fino resuelve la octava mas fina del terreno");
    CHECK(nodeMaxLevelFor(R) == 17u, "para la Tierra el tope derivado es el 17 de siempre");

    // ── EL TOPE DEPENDE DEL RADIO ────────────────────────────────────────────────────────────────
    // Era la constante 17, calculada para la Tierra. La regla: el primer nivel cuyo texel baja de
    // lambda/4 = 1,125 m (donde la octava mas fina entra a peso completo). Un planeta 4 veces mayor
    // necesita dos niveles mas para el MISMO texel; uno 4 veces menor, dos menos. Y el texel del tope
    // queda siempre en la misma ventana [lambda/8, lambda/4): ni por encima (perderia la octava) ni
    // un nivel de mas (interpolarse a si mismo, lo que costaba la mitad del pool con el tope 20).
    std::printf("    radio        tope   m/texel en el tope   (ventana: %.3f..%.3f)\n",
                TERRAIN_NODE_FINEST_LAMBDA_M / 8.0, TERRAIN_NODE_FINEST_LAMBDA_M / 4.0);
    int badWindow = 0;
    for (double Rk : { R / 16.0, R / 4.0, 3389.5e3, R, 4.0 * R, 16.0 * R, 100.0 * R }) {
        const uint32_t L = nodeMaxLevelFor(Rk);
        const double tx = nodeTexelM(NodeId{ PlanetFace::FRONT, L, 0, 0 }, Rk);
        std::printf("    %8.0f km   %4u   %10.3f m\n", Rk / 1000.0, L, tx);
        if (!(tx < TERRAIN_NODE_FINEST_LAMBDA_M / 4.0 && tx >= TERRAIN_NODE_FINEST_LAMBDA_M / 8.0)) ++badWindow;
    }
    CHECK(badWindow == 0, "el texel del tope cae en [lambda/8, lambda/4) para cualquier radio");
    CHECK(nodeMaxLevelFor(4.0 * R) == 19u && nodeMaxLevelFor(R / 4.0) == 15u,
          "x4 de radio son +2 niveles; /4 son -2 (mismo texel)");
    // CONTRAPRUEBA: la constante de antes NO escala. Con el tope 17 fijo un planeta de 4 R tendria
    // 2,4 m/texel y perderia la octava de 4,5 m entera.
    const double stuck = nodeTexelM(NodeId{ PlanetFace::FRONT, 17u, 0, 0 }, 4.0 * R);
    std::printf("    CONTRAPRUEBA: con el 17 fijo, 4 R tendria %.2f m/texel (> 2,25: sin la octava fina)\n", stuck);
    CHECK(stuck > 2.25, "la contraprueba muestra lo que perdia la constante");
    // Y el tope duro de la clave (26) solo muerde a partir de ~965 R: no es un caso real.
    CHECK(nodeMaxLevelFor(2000.0 * R) == TERRAIN_NODE_MAX_LEVEL, "el tope duro de la clave recorta a los gigantes");

    // Y el nivel 0 tiene que cubrir la cara entera: es la raíz del quadtree.
    const NodeId root{ PlanetFace::FRONT, 0, 0, 0 };
    double lx0, ly0, lx1, ly1;
    nodeTexelFaceCoord(root, 0, 0, lx0, ly0);
    nodeTexelFaceCoord(root, TERRAIN_NODE_CELLS, TERRAIN_NODE_CELLS, lx1, ly1);
    std::printf("    raiz: lx de %.1f a %.1f · ly de %.1f a %.1f (debe ser -1..1)\n", lx0, lx1, ly0, ly1);
    CHECK(lx0 == -1.0 && lx1 == 1.0 && ly0 == -1.0 && ly1 == 1.0,
          "el nivel 0 cubre la cara entera, exactamente");

    // CONTRAPRUEBA de la cobertura: un nodo interior NO puede cubrir la cara entera. Sin esto, la
    // comprobación de arriba pasaría con una formula que ignorara `i`/`j`.
    const NodeId inner{ PlanetFace::FRONT, 2, 1, 1 };
    double ix0, iy0; nodeTexelFaceCoord(inner, 0, 0, ix0, iy0);
    std::printf("    CONTRAPRUEBA: nodo (nivel 2, i=1, j=1) empieza en lx=%.2f (no en -1)\n", ix0);
    CHECK(ix0 > -1.0 && ix0 < 1.0, "CONTRAPRUEBA: un nodo interior no cubre la cara entera");
}

// ================================================================================================
// F1 (2/2) — EL CONTENIDO DEL NODO: golden + la precisión que la firma tiraba
// ================================================================================================
void test_terrain_node_content() {
    beginTest("terrain_node_content");
    const double R = 6371000.0;
    std::vector<float> buf((size_t)TERRAIN_NODE_TEXELS * TERRAIN_NODE_TEXELS);

    // ── (1) EL NODO TIENE RELIEVE DE VERDAD ─────────────────────────────────────────────────────
    // Un test golden sobre un nodo plano pasaría siempre y no probaría nada. Primero se comprueba
    // que hay algo que hashear.
    const NodeId n{ PlanetFace::FRONT, 14, 4200, 3100 };
    nodeFillHeights(n, R, buf.data());
    float lo = 1e30f, hi = -1e30f;
    for (float h : buf) { lo = std::min(lo, h); hi = std::max(hi, h); }
    std::printf("    nodo nivel %u · %.3f m/texel · relieve de %.2f a %.2f m (recorrido %.2f m)\n",
                n.level, nodeTexelM(n, R), lo, hi, hi - lo);
    CHECK(hi - lo > 1.0f, "el nodo lleva relieve (si no, el golden no probaria nada)");

    // ── (2) GOLDEN: el contenido es estable y reproducible ──────────────────────────────────────
    // El hash va sobre los BITS. Un ulp de deriva —driver, contracción FMA, un refactor
    // "equivalente"— lo cambia, que es justo lo que hay que detectar: entre cliente y servidor, un
    // ulp es una desincronización.
    const uint32_t hash1 = nodeContentHash(buf.data(), buf.size());
    std::vector<float> buf2(buf.size());
    nodeFillHeights(n, R, buf2.data());
    const uint32_t hash2 = nodeContentHash(buf2.data(), buf2.size());
    std::printf("    golden del nodo (%u,%u,%u,%u): 0x%08X\n",
                (unsigned)n.face, n.level, n.i, n.j, hash1);
    CHECK(hash1 == hash2, "generar el mismo nodo dos veces da el MISMO hash");

    // CONTRAPRUEBA: un nodo vecino tiene que dar OTRO hash. Sin esto, el test pasaría con un
    // generador que devolviera ceros o que ignorara el NodeId.
    const NodeId nb{ n.face, n.level, n.i + 1, n.j };
    nodeFillHeights(nb, R, buf2.data());
    const uint32_t hashNb = nodeContentHash(buf2.data(), buf2.size());
    std::printf("    CONTRAPRUEBA: el nodo vecino da 0x%08X (%s)\n",
                hashNb, hashNb != hash1 ? "distinto, bien" : "IGUAL, mal");
    CHECK(hashNb != hash1, "CONTRAPRUEBA: un nodo distinto da un hash distinto");

    // ── (3) LO QUE LA FIRMA `vec3` TIRABA ───────────────────────────────────────────────────────
    // La dirección del téxel se calcula EXACTA desde enteros. Pasarla por la entrada `vec3` de
    // `terrainDetail` la redondea a float antes de multiplicar por el radio — y a radio terrestre un
    // ulp de dirección unitaria son decímetros de superficie. Se mide cuánto cambia la ALTURA.
    double worstDiff = 0.0, sumAbs = 0.0; int cnt = 0;
    const float triM = (float)nodeTexelM(n, R);
    for (uint32_t v = 0; v < TERRAIN_NODE_TEXELS; v += 8)
        for (uint32_t u = 0; u < TERRAIN_NODE_TEXELS; u += 8) {
            const glm::dvec3 d = nodeTexelDir(n, u, v);
            const float hD = terrainDetail(d, R, triM);                  // exacta
            const float hF = terrainDetail(glm::vec3(d), (float)R, triM); // por vec3
            worstDiff = std::max(worstDiff, (double)std::abs(hD - hF));
            sumAbs += std::abs(hD - hF); ++cnt;
        }
    std::printf("    altura con dir EXACTA vs por la firma `vec3`: peor %.4f m · media %.4f m (%d muestras)\n",
                worstDiff, sumAbs / cnt, cnt);
    // No es una aserción de calidad, es la CIFRA que justifica que exista la sobrecarga en double.
    // Si algún día sale 0, la sobrecarga sobra y hay que retirarla en vez de dejarla "por si acaso".
    CHECK(worstDiff > 0.0, "la firma en float SI cambia la altura (por eso existe la de double)");

    // ── (4) EL NIVEL FINO RESUELVE LA OCTAVA FINA ───────────────────────────────────────────────
    // El quadtree solo arregla el "se ve basto" si su téxel baja de 2,25 m, que es la guarda de la
    // octava de λ 4,5 m. Se comprueba con el relieve REAL, no con la aritmética del téxel.
    const NodeId fine{ PlanetFace::FRONT, 18, 100000, 90000 };
    nodeFillHeights(fine, R, buf.data());
    float flo = 1e30f, fhi = -1e30f;
    for (float h : buf) { flo = std::min(flo, h); fhi = std::max(fhi, h); }
    std::printf("    nivel %u (%.3f m/texel): relieve %.3f m — la octava fina %s\n",
                fine.level, nodeTexelM(fine, R), fhi - flo,
                nodeTexelM(fine, R) < 2.25 ? "ENTRA" : "no entra");
    CHECK(nodeTexelM(fine, R) < 2.25, "el nivel fino resuelve la octava de lambda 4,5 m");
    CHECK(fhi - flo > 0.0f, "y produce relieve de verdad a esa escala");

    // ── (5) LA SUPOSICIÓN DEL COMPUTE, VERIFICADA ───────────────────────────────────────────────
    //
    // `terrain_node.comp` no puede usar `int64_t` sin exigir `GL_ARB_gpu_shader_int64`, así que
    // acumula el numerador en DOUBLE: `double(i)·double(celdas) + double(u)` en vez del
    // `uint64_t i·celdas + u` del gemelo C++. El comentario del shader afirma que son el MISMO valor
    // mientras la suma no pase de 2^53.
    //
    // ⚠️ Eso es una afirmación cruzada entre dos lenguajes, del tipo que se rompe en silencio. Aquí
    // se comprueba en C++ para TODO el rango de niveles del plan: si algún día alguien sube
    // `TERRAIN_NODE_MAX_LEVEL`, este test falla antes de que el shader empiece a derivar.
    int numBad = 0; double worstNum = 0.0;
    for (uint32_t level = 0; level <= TERRAIN_NODE_MAX_LEVEL; ++level) {
        const uint64_t maxIdx = (1ull << level) - 1ull;
        for (uint64_t idx : { (uint64_t)0, maxIdx / 3, maxIdx })
            for (uint32_t u : { 0u, 1u, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS }) {
                const double asInt = (double)(idx * TERRAIN_NODE_CELLS + u);          // como el C++
                const double asDbl = (double)idx * (double)TERRAIN_NODE_CELLS + (double)u;  // como el .comp
                if (!sameBits(asInt, asDbl)) { ++numBad; worstNum = std::max(worstNum, std::abs(asInt - asDbl)); }
            }
    }
    std::printf("    numerador entero vs acumulado en double (niveles 0..%u): %d difieren\n",
                TERRAIN_NODE_MAX_LEVEL, numBad);
    CHECK(numBad == 0, "el atajo en double del compute coincide BIT A BIT con el uint64 del C++");

    // Y dónde deja de valer: por encima de 2^53 la suma ya no es exacta. Es la CONTRAPRUEBA y a la
    // vez el margen que queda — el nivel maximo del plan esta muy por debajo.
    const double maxNum = (double)((1ull << TERRAIN_NODE_MAX_LEVEL) - 1ull) * TERRAIN_NODE_CELLS
                        + TERRAIN_NODE_CELLS;
    std::printf("    CONTRAPRUEBA/margen: numerador maximo %.3e contra el limite 2^53 = %.3e (%.0fx de margen)\n",
                maxNum, 9007199254740992.0, 9007199254740992.0 / maxNum);
    CHECK(maxNum < 9007199254740992.0, "el numerador maximo cabe en la parte exacta del double");
}

// ================================================================================================
// F2 (1/2) — SELECCIÓN DE NODOS por error en pantalla
//
// Lo que tiene que cumplir el conjunto seleccionado, y por qué cada cosa importa:
//   · TESELA la esfera — sin huecos (se ve el vacío) ni solapes (z-fighting y coste doble)
//   · el nivel CRECE hacia la cámara — si no, el LOD no está haciendo nada
//   · el error en pantalla queda ACOTADO por el presupuesto — que es la promesa del criterio
//   · el tope de nodos SE RESPETA — sin él, un criterio mal puesto agota la memoria
// ================================================================================================
void test_terrain_node_select() {
    beginTest("terrain_node_select");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    // FOV vertical 60° sobre 1080 px.
    const double radPerPx = (60.0 * 3.14159265358979 / 180.0) / 1080.0;

    // Cámara a 2 m sobre la superficie, sobre la cara FRONT.
    const glm::dvec3 camDir = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 cam    = pc + camDir * (R + 2.0);

    // ⚠️ Tope ALTO a propósito: con el tope por defecto (4096) la selección lo alcanza a esta altura
    // y deja de partir, así que la comprobación del presupuesto mediría el TOPE y no el criterio.
    // El tope se prueba aparte, más abajo.
    std::vector<NodeId> sel;
    nodeSelectVisible(R, cam, pc, radPerPx, sel, 200000);

    // Reparto por nivel: es lo que enseña si el LOD trabaja.
    int perLevel[TERRAIN_NODE_MAX_LEVEL + 1] = {0};
    uint32_t lo = 99, hi = 0;
    for (const NodeId& n : sel) { ++perLevel[n.level]; lo = std::min(lo, n.level); hi = std::max(hi, n.level); }
    std::printf("    camara a 2 m · %zu nodos · niveles %u..%u\n", sel.size(), lo, hi);
    for (uint32_t L = 0; L <= TERRAIN_NODE_MAX_LEVEL; ++L)
        if (perLevel[L]) std::printf("      nivel %2u: %4d nodos (%.3f m/texel)\n",
                                     L, perLevel[L], nodeTexelM(NodeId{PlanetFace::FRONT, L, 0, 0}, R));

    CHECK(!sel.empty(), "selecciona algo");
    CHECK(hi > lo, "hay VARIOS niveles (si no, el LOD no esta haciendo nada)");

    // ── (1) NIVEL CRECIENTE HACIA LA CÁMARA ─────────────────────────────────────────────────────
    // El nodo más cercano tiene que ser MÁS fino que el más lejano. Es la propiedad que justifica
    // todo el quadtree, y falla en cuanto el criterio usa el centro del nodo en vez de su esquina.
    uint32_t nearLevel = 0, farLevel = 99;
    double dNear = 1e300, dFar = 0.0;
    for (const NodeId& n : sel) {
        const glm::dvec3 p = pc + nodeTexelDir(n, TERRAIN_NODE_CELLS / 2, TERRAIN_NODE_CELLS / 2) * R;
        const double d = glm::length(p - cam);
        if (d < dNear) { dNear = d; nearLevel = n.level; }
        if (d > dFar)  { dFar  = d; farLevel  = n.level; }
    }
    std::printf("    nodo mas cercano: nivel %u a %.0f m · mas lejano: nivel %u a %.0f m\n",
                nearLevel, dNear, farLevel, dFar);
    CHECK(nearLevel > farLevel, "el nivel crece hacia la camara");

    // ── (2) EL ERROR EN PANTALLA QUEDA ACOTADO ──────────────────────────────────────────────────
    // Es la promesa del criterio: ninguna hoja debería exceder el presupuesto... salvo las que
    // topan en TERRAIN_NODE_MAX_LEVEL, que no pueden partirse más. Se cuentan aparte porque son un
    // límite declarado, no un fallo.
    int overBudget = 0, atMaxLevel = 0;
    double worstPx = 0.0;
    for (const NodeId& n : sel) {
        double best = 1e300;
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += TERRAIN_NODE_CELLS / 2)
            for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += TERRAIN_NODE_CELLS / 2)
                best = std::min(best, glm::length(pc + nodeTexelDir(n, u, v) * R - cam));
        const double px = nodeTexelM(n, R) / (std::max(best, 1.0) * radPerPx);
        worstPx = std::max(worstPx, px);
        if (px > TERRAIN_NODE_ERROR_PX * 1.001) {
            if (n.level >= nodeMaxLevelFor(R)) ++atMaxLevel; else ++overBudget;
        }
    }
    std::printf("    error peor %.2f px (presupuesto %.1f) · fuera de presupuesto %d · topados en nivel max %d\n",
                worstPx, TERRAIN_NODE_ERROR_PX, overBudget, atMaxLevel);
    CHECK(overBudget == 0, "ninguna hoja excede el presupuesto salvo las topadas por nivel maximo");
    // ⚠️ Y LOS TOPADOS SON MUCHOS, que es un límite REAL del diseño y no un fallo: a 5 m de la
    // cámara, el téxel del nivel 20 (7,5 cm) subtiende 15 px. Para 1 px harían falta téxeles de 5 mm
    // — absurdo para terreno, y por debajo de la octava más fina que existe (λ 4,5 m). O sea que el
    // nivel máximo NO lo fija la pantalla: lo fija la escalera de ruido. A partir de ahí el detalle
    // es cosa de las texturas del material, no de la geometría.
    std::printf("      (los topados son un limite del diseño: el nivel max lo fija la escalera de\n"
                "       ruido, no la pantalla — bajo lambda 4,5 m no hay detalle que representar)\n");

    // ── (3) EL TOPE DE NODOS SE RESPETA ─────────────────────────────────────────────────────────
    std::vector<NodeId> tight;
    nodeSelectVisible(R, cam, pc, radPerPx, tight, 64);
    std::printf("    con tope de 64: %zu nodos\n", tight.size());
    CHECK(tight.size() <= 64, "el tope duro de nodos se respeta");
    CHECK(!tight.empty(), "y aun asi devuelve una teselacion (degrada, no se rinde)");

    // ── (4) DESDE ÓRBITA SELECCIONA MUCHO MENOS ─────────────────────────────────────────────────
    // Es la prueba de que el criterio responde a la pantalla y no a una heurística de altura.
    const glm::dvec3 orbit = pc + camDir * (R + 500000.0);
    std::vector<NodeId> selOrbit;
    nodeSelectVisible(R, orbit, pc, radPerPx, selOrbit);
    std::printf("    desde 500 km: %zu nodos (a 2 m eran %zu)\n", selOrbit.size(), sel.size());
    CHECK(selOrbit.size() < sel.size(), "desde orbita hacen falta MENOS nodos");

    // CONTRAPRUEBA: con un presupuesto de error absurdamente grande, el criterio no debe partir
    // nada — seis caras y ya. Sin esto, el test pasaría con un selector que ignorase `errorPx`.
    std::vector<NodeId> coarse;
    nodeSelectVisible(R, cam, pc, radPerPx, coarse, 4096, 1e9);
    // ⚠️ NO son 6. Con recorte de horizonte, a 2 m de altura solo UNA cara del cubo asoma; las otras
    // cinco están detrás del horizonte. Que salga 1 es la prueba de que las DOS cosas funcionan a la
    // vez: el presupuesto (no subdivide) y el horizonte (descarta lo que no se ve).
    std::printf("    CONTRAPRUEBA: con presupuesto 1e9 px -> %zu nodos (sin subdividir, y solo las\n"
                "      caras sobre el horizonte)\n", coarse.size());
    CHECK(coarse.size() >= 1 && coarse.size() <= 6,
          "CONTRAPRUEBA: el presupuesto de error SI gobierna la subdivision");
    CHECK(coarse.size() < sel.size() / 100,
          "y con presupuesto enorme salen ORDENES DE MAGNITUD menos nodos que con 1 px");
}

// ================================================================================================
// EL RECORTE POR HORIZONTE, CONTRA RAYOS DE PANTALLA.
//
// ⚠️ SINTOMA REPORTADO Y CONFIRMADO EN PANTALLA: "chunks cortados antes de que acabe la pantalla".
// El cono del frustum quedo descartado con numeros (`terrain_node_frustum_corners`: 0 nodos en
// pantalla descartados). Queda el otro recorte, y ninguno de los tests lo tocaba.
//
// `nodeBelowHorizon` sondea el PERIMETRO del nodo, 8 puntos por lado, mas el punto bajo la camara.
// Si la parte visible de un nodo es una franja mas fina que el paso de sonda, ninguna sonda la toca
// y el nodo se descarta ENTERO — un agujero donde si habia suelo.
//
// El oraculo aqui no es una sonda mas: es la GEOMETRIA. Se lanza un rayo por cada pixel del cuadro,
// se corta con la esfera, y si corta, ese suelo SE VE. El nodo que lo contiene no puede descartarse.
// ================================================================================================
// ================================================================================================
// EL CRITERIO DE SUBDIVISION MIDE AL TERRENO, NO AL NIVEL DEL MAR.
//
// ⚠️ SINTOMA REPORTADO: "en una pendiente, mirando cuesta arriba hay disparidad; cuesta abajo no,
// pero se ve inestable". Parecia direccional y no lo era: `nodeShouldSplit` medía la distancia a
// `planetCenter + dir·R` —el nivel del mar— ignorando el relieve. En una ladera de +2 km el suelo
// real esta 2 km MAS CERCA de una camara por encima, asi que el criterio sobreestimaba la distancia
// y subdividia DE MENOS. El error depende de la elevacion local, o sea de hacia donde miras.
//
// Este test mide cuantos NIVELES se pierden por eso, que es lo que se traduce en terreno basto.
// ================================================================================================
// ================================================================================================
// EL POPPING AL ALEJARSE: que el nodo YA SEA su padre cuando le toque fundirse.
//
// ⚠️ SINTOMA REPORTADO: "disparidad sobre todo AL MOVER LA CAMARA". El geomorph de aristas cierra el
// escalon entre dos nodos que se dibujan A LA VEZ; no dice nada de lo que pasa cuando un nodo
// DESAPARECE y lo sustituye su padre. Ahi la superficie salta de una funcion a otra.
//
// `nodeParentMorph` lo cierra en el tiempo. Este test recorre el alejamiento y mide el SALTO entre
// frames consecutivos: si el morph funciona, en el instante del relevo la altura ya es la del padre
// y el salto es 0.
// ================================================================================================
// ================================================================================================
// ¿DEPENDE DE HACIA DONDE MIRAS? La pregunta que ningun otro test hacia.
//
// ⚠️ Reportado: "disparidad, depende del angulo". Y ninguno de los tests del v5 variaba el ANGULO:
// todos ponen el nodo bajo la camara o usan una sola direccion. Este separa las dos causas posibles:
//
//   1. LA SELECCION depende de la direccion -> el suelo cambiaria al girar, y seria un fallo de
//      diseño: `nodeScreenError` solo mira DISTANCIA, asi que NO deberia.
//   2. El RECORTE por cono descarta nodos al girar, el pool los desaloja, y al volver la vista
//      reaparecen como ANCESTRO (mas gruesos) hasta que se regeneran. Eso SI depende del angulo, es
//      transitorio, y en el juego se ve como `por ancestro 8` justo al girar.
//
// El test fija la camara y gira la vista 360 grados, mirando el nivel que le toca a un punto FIJO.
// ================================================================================================
void test_terrain_node_angle_independence() {
    beginTest("terrain_node_angle_independence");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / 1080.0;
    const double aspect = 1920.0 / 1080.0;
    const double cone = nodeFrustumConeHalfAngle(fovY, aspect);

    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 cam = pc + up0 * (R + 1000.0);
    const glm::dvec3 t1  = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const glm::dvec3 t2  = glm::cross(up0, t1);

    // Un punto de suelo a 3 km del jugador: lo bastante lejos para que el nivel importe.
    const glm::dvec3 probe = glm::normalize(up0 + t1 * (3000.0 / R));

    auto levelOf = [&](const std::vector<NodeId>& sel) -> int {
        const auto idx = nodeDrawnIndex(sel);
        PlanetFace f; double lx, ly;
        dirToCubeFaceClosed(probe, f, lx, ly);
        for (int lv = (int)nodeMaxLevelFor(R); lv >= 0; --lv) {
            const uint64_t lim = 1ull << lv;
            const NodeId n{ f, (uint32_t)lv,
                (uint32_t)std::min<int64_t>((int64_t)((lx + 1.0) * 0.5 * (double)lim), (int64_t)lim - 1),
                (uint32_t)std::min<int64_t>((int64_t)((ly + 1.0) * 0.5 * (double)lim), (int64_t)lim - 1) };
            if (idx.find(nodeKey(n)) != idx.end()) return lv;
        }
        return -1;
    };

    int lvFirst = -2, angles = 0, changed = 0, notVisible = 0;
    for (int a = 0; a < 24; ++a) {
        const double th = a * (2.0 * 3.14159265358979 / 24.0);
        const glm::dvec3 fwd = glm::normalize(t1 * std::cos(th) + t2 * std::sin(th));
        std::vector<NodeId> sel;
        nodeSelectVisible(R, cam, pc, radPerPx, sel, 200000, TERRAIN_NODE_ERROR_PX, &fwd, cone);
        const int lv = levelOf(sel);
        ++angles;
        if (lv < 0) { ++notVisible; continue; }          // el cono lo descarto: no esta en cuadro
        if (lvFirst == -2) lvFirst = lv;
        else if (lv != lvFirst) ++changed;
    }
    std::printf("    %d angulos · el punto sale de cuadro en %d · nivel elegido cuando SI se ve: %d\n",
                angles, notVisible, lvFirst);
    std::printf("    veces que el nivel CAMBIA con el angulo: %d\n", changed);
    CHECK(changed == 0, "el nivel de un punto NO depende de hacia donde mires (solo de la distancia)");

    // ⚠️ La primera contraprueba exigia que el cono sacara el punto de cuadro al girar. NO OCURRE, y
    // el numero lo dice: 0 de 24. El punto de sonda esta a 3 km con la camara a 1 km de altura, o sea
    // 18 grados bajo la horizontal, y el cono mide 49,7 — cabe en cualquier azimut. La contraprueba
    // con dientes es la de abajo: que girar SI cambia el conjunto seleccionado.
    //
    // LA OTRA CAUSA, la que si depende del angulo: cuantos nodos entran al girar 90 grados. Esos son
    // los que el pool tiene que regenerar, y hasta que lo hace se dibujan por ANCESTRO.
    // Esos son los que el pool desaloja y que vuelven como ANCESTRO hasta regenerarse.
    std::vector<NodeId> selA, selB;
    nodeSelectVisible(R, cam, pc, radPerPx, selA, 200000, TERRAIN_NODE_ERROR_PX, &t1, cone);
    nodeSelectVisible(R, cam, pc, radPerPx, selB, 200000, TERRAIN_NODE_ERROR_PX, &t2, cone);
    const auto idxA = nodeDrawnIndex(selA);
    size_t nuevos = 0;
    for (const NodeId& n : selB) if (idxA.find(nodeKey(n)) == idxA.end()) ++nuevos;
    std::printf("    al girar 90 grados entran %zu nodos NUEVOS de %zu (%.0f %%): son los que el pool\n"
                "      tiene que regenerar, y hasta que lo hace se dibujan por ANCESTRO\n",
                nuevos, selB.size(), 100.0 * (double)nuevos / (double)selB.size());
    // CONTRAPRUEBA: girar tiene que cambiar el conjunto de verdad. Si `nuevos` fuera ~0, el recorte
    // por cono no estaria haciendo nada y el test de arriba (el nivel no cambia) seria trivial.
    CHECK(nuevos > selB.size() / 10,
          "CONTRAPRUEBA: girar SI cambia el conjunto (>10 %), asi que el test de arriba no es trivial");
}

void test_terrain_node_split_uses_elevation();

void test_terrain_node_split_uses_elevation() {
    beginTest("terrain_node_split_uses_elevation");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double radPerPx = (60.0 * 3.14159265358979 / 180.0) / 1080.0;

    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));

    std::printf("    cota del nodo    altura de camara    nivel SIN cota -> CON cota\n");
    int worstLost = 0;
    for (double elev : { 500.0, 2000.0, 5000.0 })
        for (double alt : { 50.0, 500.0, 3000.0 }) {
            const glm::dvec3 cam = pc + up0 * (R + alt + elev);
            // El nivel al que para de subdividir, por los dos criterios.
            auto deepest = [&](double e) {
                uint32_t lv = 0;
                NodeId n{ PlanetFace::FRONT, 0, 0, 0 };
                // Se baja por el hijo que contiene la direccion de la camara.
                for (; lv < nodeMaxLevelFor(R); ++lv) {
                    if (!nodeShouldSplit(n, R, cam, pc, radPerPx, TERRAIN_NODE_ERROR_PX, e)) break;
                    PlanetFace f; double lx, ly;
                    dirToCubeFaceClosed(up0, f, lx, ly);
                    const uint64_t lim = 1ull << (lv + 1);
                    n = NodeId{ f, lv + 1,
                                (uint32_t)std::min<int64_t>((int64_t)((lx + 1.0) * 0.5 * (double)lim),
                                                            (int64_t)lim - 1),
                                (uint32_t)std::min<int64_t>((int64_t)((ly + 1.0) * 0.5 * (double)lim),
                                                            (int64_t)lim - 1) };
                }
                return lv;
            };
            const uint32_t lvOld = deepest(0.0), lvNew = deepest(elev);
            const int lost = (int)lvNew - (int)lvOld;
            std::printf("    %8.0f m       %8.0f m           %2u -> %2u   %s\n",
                        elev, alt, lvOld, lvNew,
                        lost > 0 ? "(el viejo se quedaba corto)" : "");
            worstLost = std::max(worstLost, lost);
        }
    std::printf("    -> hasta %d niveles de mas al medir al TERRENO (cada nivel dobla la resolucion)\n",
                worstLost);
    CHECK(worstLost >= 1, "medir al terreno subdivide MAS donde hay relieve (era lo que faltaba)");

    // CONTRAPRUEBA: sin relieve (cota 0) los dos criterios tienen que dar EXACTAMENTE lo mismo. Si no,
    // el cambio estaria moviendo el LOD en todas partes y no solo donde hay montaña.
    const glm::dvec3 cam = pc + up0 * (R + 500.0);
    uint32_t a = 0, b = 0;
    NodeId n0{ PlanetFace::FRONT, 0, 0, 0 };
    while (a < nodeMaxLevelFor(R) &&
           nodeShouldSplit(n0, R, cam, pc, radPerPx, TERRAIN_NODE_ERROR_PX, 0.0)) { ++a; break; }
    while (b < nodeMaxLevelFor(R) &&
           nodeShouldSplit(n0, R, cam, pc, radPerPx, TERRAIN_NODE_ERROR_PX)) { ++b; break; }
    std::printf("    CONTRAPRUEBA: con cota 0 los dos criterios coinciden (%u == %u)\n", a, b);
    CHECK(a == b, "CONTRAPRUEBA: sin relieve el criterio NO cambia");
}

void test_terrain_node_horizon_cull() {
    beginTest("terrain_node_horizon_cull");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double aspect = 1920.0 / 1080.0;
    const double tv = std::tan(fovY * 0.5), th = tv * aspect;

    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 fwd = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));   // al horizonte
    const glm::dvec3 right = glm::normalize(glm::cross(fwd, up0));
    const glm::dvec3 vup   = glm::cross(right, fwd);

    // El nivel que el selector usaria a esa distancia; para el recorte solo importa el nodo.
    auto nodeAt = [&](const glm::dvec3& dir, uint32_t level) {
        PlanetFace f; double lx, ly;
        dirToCubeFaceClosed(dir, f, lx, ly);
        const uint64_t lim = 1ull << level;
        const int64_t i = (int64_t)((lx + 1.0) * 0.5 * (double)lim);
        const int64_t j = (int64_t)((ly + 1.0) * 0.5 * (double)lim);
        return NodeId{ f, level,
                       (uint32_t)std::min<int64_t>(std::max<int64_t>(i, 0), (int64_t)lim - 1),
                       (uint32_t)std::min<int64_t>(std::max<int64_t>(j, 0), (int64_t)lim - 1) };
    };

    std::printf("    altura    rayos que TOCAN suelo    caen en nodo DESCARTADO por horizonte\n");
    size_t totalBad = 0;
    for (double alt : { 1.7, 100.0, 2000.0, 50000.0 }) {
        const glm::dvec3 cam = pc + up0 * (R + alt);
        size_t hits = 0, bad = 0;
        double worstAngDeg = 0.0;
        for (int iv = -24; iv <= 24; ++iv)
            for (int iu = -40; iu <= 40; ++iu) {
                const double u = (double)iu / 40.0, v = (double)iv / 24.0;
                const glm::dvec3 d = glm::normalize(fwd + right * (u * th) + vup * (v * tv));
                // Corte rayo-esfera: si hay raiz real positiva, ese suelo se ve.
                const glm::dvec3 oc = cam - pc;
                const double b = glm::dot(oc, d), c = glm::dot(oc, oc) - R * R;
                const double disc = b * b - c;
                if (disc < 0.0) continue;                       // el rayo pasa de largo: cielo
                const double t0 = -b - std::sqrt(disc);
                if (t0 <= 0.0) continue;
                ++hits;
                const glm::dvec3 hit = glm::normalize(oc + d * t0);
                // Se prueba a varios niveles: el recorte se aplica a cualquiera que el selector visite.
                for (uint32_t lv : { 6u, 9u, 12u }) {
                    if (nodeBelowHorizon(nodeAt(hit, lv), R, cam, pc)) {
                        ++bad;
                        const double ang = std::acos(glm::clamp(glm::dot(hit, glm::normalize(oc)),
                                                                -1.0, 1.0)) * 180.0 / 3.14159265358979;
                        worstAngDeg = std::max(worstAngDeg, ang);
                        break;
                    }
                }
            }
        std::printf("    %7.0f m   %10zu             %10zu   %s\n", alt, hits, bad,
                    bad ? "<- AGUJERO" : "");
        totalBad += bad;
    }
    CHECK(totalBad == 0, "ningun rayo que toca suelo cae en un nodo descartado por horizonte");

    // CONTRAPRUEBA: el recorte TIENE que descartar lo que de verdad esta detras del horizonte. Si no,
    // un `nodeBelowHorizon` que devolviera siempre false pasaria el test de arriba sin recortar nada.
    const glm::dvec3 cam = pc + up0 * (R + 2000.0);
    const glm::dvec3 anti = -up0;                       // las antipodas: imposible que se vean
    size_t culled = 0;
    for (uint32_t lv : { 6u, 9u, 12u }) if (nodeBelowHorizon(nodeAt(anti, lv), R, cam, pc)) ++culled;
    std::printf("    CONTRAPRUEBA: las antipodas se descartan en %zu de 3 niveles\n", culled);
    CHECK(culled == 3, "CONTRAPRUEBA: el recorte SI descarta lo que esta detras del planeta");
}

void test_terrain_node_frustum_corners() {
    beginTest("terrain_node_frustum_corners");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double aspect = 1920.0 / 1080.0;
    const double radPerPx = fovY / 1080.0;
    const double cone = nodeFrustumConeHalfAngle(fovY, aspect);

    const double halfV = std::atan(std::tan(fovY * 0.5));
    const double halfH = std::atan(aspect * std::tan(fovY * 0.5));
    const double deg = 180.0 / 3.14159265358979;
    std::printf("    fovY 60 · 16:9  ->  vertical %.1f  horizontal %.1f  ESQUINA %.1f  ·  cono usado %.1f\n",
                halfV * deg, halfH * deg, cone * deg, cone * deg);
    CHECK(cone > halfH - 1e-9, "el cono envuelve el semiangulo HORIZONTAL (si no, corta por los lados)");
    CHECK(cone > halfV - 1e-9, "el cono envuelve el semiangulo VERTICAL");

    const glm::dvec3 up  = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 fwd = glm::normalize(glm::cross(up, glm::dvec3(0, 1, 0)));   // al horizonte

    // ⚠️ EL ORACULO SON DIRECCIONES, NO ESFERAS. Y llegar aqui costo dos intentos.
    //
    // Intento 1: los seis planos sacados de `proj·vista`. Daba 22 nodos "visibles" a 173 grados del
    // eje —detras de la camara— porque con near 1 y far 1e9 en FLOAT el plano cercano degenera.
    // Intento 2: los mismos planos, analiticos y en double. MISMO resultado, 22 nodos. Ahi estaba la
    // leccion: el test esfera-contra-planos es CONSERVADOR por construccion y da falsos positivos
    // justo en las esquinas del frustum. Mi oraculo era mas laxo que lo que pretendia juzgar, asi
    // que acusaba al cono de un fallo que era del oraculo.
    //
    // Lo que el usuario reporta es sobre PIXELES: "cortado antes de que acabe la pantalla". Eso se
    // dice exactamente en direcciones — si un rayo sale por un pixel del cuadro, su direccion tiene
    // que caer dentro del cono. Sin esferas no hay aproximacion y no hay falsos positivos.
    const double tv = std::tan(fovY * 0.5), th = tv * aspect;
    const glm::dvec3 right = glm::normalize(glm::cross(fwd, up));
    const glm::dvec3 vup   = glm::cross(right, fwd);

    double worstDeg = 0.0; int worstU = 0, worstV = 0; size_t outsideCone = 0, sampled = 0;
    for (int iv = -32; iv <= 32; ++iv)
        for (int iu = -32; iu <= 32; ++iu) {
            const double u = (double)iu / 32.0, v = (double)iv / 32.0;   // +-1 = borde del cuadro
            const glm::dvec3 d = glm::normalize(fwd + right * (u * th) + vup * (v * tv));
            const double ang = std::acos(glm::clamp(glm::dot(d, fwd), -1.0, 1.0)) * deg;
            ++sampled;
            if (ang > cone * deg + 1e-9) ++outsideCone;
            if (ang > worstDeg) { worstDeg = ang; worstU = iu; worstV = iv; }
        }
    std::printf("    %zu direcciones del cuadro · la mas alejada del eje: %.2f grados en (u,v)=(%+d,%+d)"
                "  ->  %s\n", sampled, worstDeg, worstU, worstV,
                (worstU == 32 || worstU == -32) && (worstV == 32 || worstV == -32)
                    ? "es una ESQUINA, como debe ser" : "NO es una esquina (revisar)");
    std::printf("    direcciones del cuadro que el cono dejaria fuera: %zu\n", outsideCone);
    CHECK(outsideCone == 0, "ninguna direccion del cuadro cae fuera del cono (nada se corta en pantalla)");
    CHECK(worstDeg <= cone * deg + 1e-9, "la direccion mas alejada (la esquina) cabe en el cono");

    // Y ahora sobre NODOS, tambien exacto: si el CENTRO de un nodo apunta dentro del cuadro, ese
    // nodo se ve, y descartarlo es el fallo reportado. Sin esferas: solo la direccion del centro.
    size_t onScreen = 0, wronglyCulled = 0, totalCulled = 0, examined = 0;
    const double alts[3] = { 2.0, 640.0, 50000.0 };
    for (double alt : alts) {
        const glm::dvec3 cam = pc + up * (R + alt);
        std::vector<NodeId> all;
        nodeSelectVisible(R, cam, pc, radPerPx, all, 200000);
        for (const NodeId& n : all) {
            ++examined;
            glm::dvec3 sc; double sr;
            nodeBoundingSphere(n, R, pc, 5000.0, sc, sr);
            const glm::dvec3 rel = sc - cam;
            const double z = glm::dot(rel, fwd);
            const bool culled = nodeOutsideFrustum(n, R, cam, pc, fwd, cone, 5000.0);
            if (culled) ++totalCulled;
            if (z <= 0.0) continue;                                  // detras: no esta en pantalla
            const double x = glm::dot(rel, right), y = glm::dot(rel, vup);
            if (std::fabs(x) > th * z || std::fabs(y) > tv * z) continue;   // fuera del cuadro
            ++onScreen;
            if (culled) ++wronglyCulled;
        }
    }
    std::printf("    %zu nodos examinados · %zu con el centro DENTRO del cuadro · descartados: %zu\n",
                examined, onScreen, wronglyCulled);
    CHECK(wronglyCulled == 0, "ningun nodo cuyo centro cae en pantalla lo descarta el cono");

    // CONTRAPRUEBA 1: el cono TIENE que seguir recortando. Sin esto, un cono de 180 grados pasaria
    // todo lo de arriba sin recortar nada.
    std::printf("    CONTRAPRUEBA: el cono descarta %zu de %zu nodos (%.0f %%)\n",
                totalCulled, examined, 100.0 * (double)totalCulled / (double)examined);
    CHECK(totalCulled > examined / 4, "el cono sigue recortando de verdad (no es un cono de 180 grados)");

    // CONTRAPRUEBA 2: con el semiangulo VERTICAL en vez del de la esquina, el fallo TIENE que
    // aparecer. Si no apareciera, este test no estaria midiendo lo que dice medir.
    size_t cutWithVertical = 0;
    {
        const glm::dvec3 cam = pc + up * (R + 640.0);
        std::vector<NodeId> all;
        nodeSelectVisible(R, cam, pc, radPerPx, all, 200000);
        for (const NodeId& n : all) {
            glm::dvec3 sc; double sr;
            nodeBoundingSphere(n, R, pc, 5000.0, sc, sr);
            const glm::dvec3 rel = sc - cam;
            const double d = glm::length(rel);
            const double ang = std::acos(glm::clamp(glm::dot(rel / d, fwd), -1.0, 1.0));
            const double angR = std::asin(glm::clamp(sr / d, 0.0, 1.0));
            if ((ang - angR) <= cone && (ang - angR) > halfV) ++cutWithVertical;
        }
    }
    std::printf("    CONTRAPRUEBA: con el semiangulo VERTICAL se perderian %zu nodos que el de esquina conserva\n",
                cutWithVertical);
    CHECK(cutWithVertical > 0, "CONTRAPRUEBA: usar el semiangulo equivocado SI rompe (el test tiene dientes)");
}

void test_terrain_node_frustum() {
    beginTest("terrain_node_frustum");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / 1080.0;
    const double cone = nodeFrustumConeHalfAngle(fovY, 1920.0 / 1080.0);
    std::printf("    cono que envuelve el frustum 60deg 16:9: %.1f grados de semiangulo\n",
                cone * 180.0 / 3.14159265358979);

    const glm::dvec3 camDir = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const double kNodeMB = (double)TERRAIN_NODE_TEXELS * TERRAIN_NODE_TEXELS * 4.0 / (1024.0 * 1024.0);

    // Mirando al HORIZONTE, que es el caso peor: es donde más superficie entra en cuadro.
    const glm::dvec3 up   = camDir;
    glm::dvec3 fwd = glm::normalize(glm::cross(up, glm::dvec3(0, 1, 0)));   // tangente = al horizonte

    struct Case { double altM; const char* what; };
    const Case cases[3] = { { 2.0, "a pie (2 m)" }, { 640.0, "volando (640 m)" },
                            { 500000.0, "orbita (500 km)" } };

    bool allSmaller = true;
    for (const Case& c : cases) {
        const glm::dvec3 cam = pc + camDir * (R + c.altM);
        std::vector<NodeId> all, culled;
        nodeSelectVisible(R, cam, pc, radPerPx, all,    200000);
        nodeSelectVisible(R, cam, pc, radPerPx, culled, 200000, TERRAIN_NODE_ERROR_PX, &fwd, cone);
        const double mbAll = all.size() * kNodeMB, mbCul = culled.size() * kNodeMB;
        std::printf("    %-18s  360deg: %6zu nodos (%7.1f MB)  ->  frustum: %6zu (%6.1f MB)  x%.1f menos\n",
                    c.what, all.size(), mbAll, culled.size(), mbCul,
                    culled.empty() ? 0.0 : (double)all.size() / (double)culled.size());
        if (culled.size() >= all.size()) allSmaller = false;
        CHECK(!culled.empty(), "con frustum sigue seleccionando terreno visible");
    }
    CHECK(allSmaller, "el frustum reduce el numero de nodos en las tres alturas");

    // ── LO QUE CUESTA NO SABER LA COTA DEL NODO ─────────────────────────────────────────────────
    //
    // ⚠️ A 2 m de altura el frustum NO recorta nada (5 439 -> 5 437), y no es un fallo del test ni del
    // algoritmo: es INCERTIDUMBRE REAL. `terrainBoundM` vale 5 000 m porque sin generar el nodo no se
    // sabe su cota (el bake llega a ±4 km), y para un nodo a 100 m de la cámara `asin(5000/100)`
    // satura en 90° — su esfera envolvente tapa medio cielo y no puede quedar fuera del cono.
    //
    // O sea que el recorte de frustum en el campo cercano NO lo desbloquea mejorar el test: lo
    // desbloquea que cada nodo lleve su min/max de elevación, que el generador ya podría emitir de
    // balde (lo tiene delante al llenar el heightmap). Esto mide cuánto vale ese dato.
    const glm::dvec3 camG = pc + camDir * (R + 2.0);
    std::vector<NodeId> loose, tight;
    nodeSelectVisible(R, camG, pc, radPerPx, loose, 200000, TERRAIN_NODE_ERROR_PX, &fwd, cone, 5000.0);
    nodeSelectVisible(R, camG, pc, radPerPx, tight, 200000, TERRAIN_NODE_ERROR_PX, &fwd, cone, 400.0);
    std::printf("    a pie, cota del nodo DESCONOCIDA (+-5000 m): %zu nodos (%.1f MB)\n",
                loose.size(), loose.size() * kNodeMB);
    std::printf("    a pie, cota ACOTADA a +-400 m:               %zu nodos (%.1f MB)  -> x%.1f menos\n",
                tight.size(), tight.size() * kNodeMB,
                tight.empty() ? 0.0 : (double)loose.size() / (double)tight.size());
    CHECK(tight.size() < loose.size(),
          "acotar la cota del nodo SI desbloquea el recorte de frustum en el campo cercano");

    // ── CONTRAPRUEBA: mirar al CIELO no debe seleccionar casi nada ───────────────────────────────
    // Con la cota acotada, apuntar arriba tiene que dejar fuera casi todo el suelo. Sin esta
    // comprobación, un frustum que no recortara nada pasaría los CHECK de arriba igual.
    std::vector<NodeId> sky;
    nodeSelectVisible(R, camG, pc, radPerPx, sky, 200000, TERRAIN_NODE_ERROR_PX, &camDir, cone, 400.0);
    std::printf("    CONTRAPRUEBA: mirando al CIELO %zu nodos · al HORIZONTE %zu (cota acotada)\n",
                sky.size(), tight.size());
    CHECK(sky.size() < tight.size(),
          "CONTRAPRUEBA: mirar al cielo selecciona menos suelo que mirar al horizonte");

    // ── EL PRESUPUESTO, que es para lo que existe este test ─────────────────────────────────────
    const double mb = tight.size() * kNodeMB;
    std::printf("    PRESUPUESTO a pie, mirando al horizonte, cota acotada: %zu nodos = %.1f MB\n",
                tight.size(), mb);
    std::printf("      (%u texeles/nodo x 4 B = %.1f KB por nodo)\n",
                TERRAIN_NODE_TEXELS * TERRAIN_NODE_TEXELS, kNodeMB * 1024.0);
    CHECK(mb < 512.0, "el conjunto visible cabe en un presupuesto de VRAM manejable");
}

// ================================================================================================
// F2 (3/3) — EL RANGO POR NODO con la cota REAL, no una suposición
//
// `terrain_node_frustum` midió que acotar la cota a ±400 m valía ×2,4. Pero ±400 m era una CIFRA
// INVENTADA para ver el potencial. Esto lo repite con la cota de verdad, generando el nodo.
//
// Y comprueba la propiedad que hace posible el esquema: **el rango del hijo cabe en el del padre**.
// Sin ella no se puede heredar el rango antes de generar, y sin heredarlo el círculo no se rompe
// (para saber si un nodo entra hay que acotar su cota; para conocer su cota hay que generarlo).
// ================================================================================================
namespace {
// Caché de rangos calculados de verdad, para el selector del test.
std::vector<std::pair<uint64_t, NodeRange>> g_rangeCache;
NodeRange realRange(const NodeId& n, void* user) {
    const double R = *(const double*)user;
    const uint64_t k = nodeKey(n);
    for (const auto& e : g_rangeCache) if (e.first == k) return e.second;
    // ⚠️ Solo se generan nodos hasta cierto nivel: generar los 129² téxeles de cada candidato del
    // quadtree entero en un test sería carísimo. Por encima, se hereda del ancestro más profundo que
    // sí se generó — que es EXACTAMENTE la política que usará el pool.
    if (n.level > 12) {
        NodeId a = n;
        while (a.level > 12) { a.level--; a.i /= 2; a.j /= 2; }
        return realRange(a, user);
    }
    std::vector<float> buf((size_t)TERRAIN_NODE_TEXELS * TERRAIN_NODE_TEXELS);
    NodeRange rg;
    nodeFillHeights(n, R, buf.data(), &rg);
    g_rangeCache.emplace_back(k, rg);
    return rg;
}
} // namespace

void test_terrain_node_range() {
    beginTest("terrain_node_range");
    double R = 6371000.0;
    std::vector<float> buf((size_t)TERRAIN_NODE_TEXELS * TERRAIN_NODE_TEXELS);

    // ── (1) EL RANGO DEL HIJO CABE EN EL DEL PADRE ──────────────────────────────────────────────
    // Es lo que permite heredarlo antes de generar. Con margen: el padre muestrea más grueso, así
    // que puede no ver un pico que el hijo sí — por eso se comprueba con una tolerancia y se declara.
    const NodeId parent{ PlanetFace::FRONT, 9, 130, 77 };
    NodeRange rp; nodeFillHeights(parent, R, buf.data(), &rp);
    NodeId kids[4]; nodeChildren(parent, kids);
    double worstOver = 0.0;
    for (const NodeId& k : kids) {
        NodeRange rk; nodeFillHeights(k, R, buf.data(), &rk);
        worstOver = std::max(worstOver, (double)std::max(rp.minM - rk.minM, rk.maxM - rp.maxM));
    }
    std::printf("    padre nivel %u: [%.2f, %.2f] m · los 4 hijos se salen como mucho %.2f m\n",
                parent.level, rp.minM, rp.maxM, worstOver);
    // El padre tiene celda 2x mas gruesa, así que se le escapan picos del hijo. Lo que importa es que
    // el exceso sea PEQUEÑO frente a la amplitud, para que un margen fijo lo cubra.
    const double amp = rp.maxM - rp.minM;
    std::printf("      exceso relativo a la amplitud del padre (%.2f m): %.1f %%\n",
                amp, 100.0 * worstOver / std::max(amp, 1e-6));
    CHECK(worstOver < amp * 0.5, "el rango del hijo cabe en el del padre con margen razonable");

    // ── (2) EL PRESUPUESTO CON LA COTA REAL ─────────────────────────────────────────────────────
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / 1080.0;
    const double cone = nodeFrustumConeHalfAngle(fovY, 1920.0 / 1080.0);
    const glm::dvec3 camDir = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 cam    = pc + camDir * (R + 2.0);
    const glm::dvec3 fwd    = glm::normalize(glm::cross(camDir, glm::dvec3(0, 1, 0)));
    const double kNodeMB = (double)TERRAIN_NODE_TEXELS * TERRAIN_NODE_TEXELS * 4.0 / (1024.0 * 1024.0);

    g_rangeCache.clear();
    std::vector<NodeId> noRange, withRange;
    nodeSelectVisible(R, cam, pc, radPerPx, noRange,   200000, TERRAIN_NODE_ERROR_PX, &fwd, cone, 5000.0);
    nodeSelectVisible(R, cam, pc, radPerPx, withRange, 200000, TERRAIN_NODE_ERROR_PX, &fwd, cone, 5000.0,
                      &realRange, &R);
    std::printf("    sin rango (bound +-5000 m): %5zu nodos (%6.1f MB)\n",
                noRange.size(), noRange.size() * kNodeMB);
    std::printf("    con la cota REAL del nodo:  %5zu nodos (%6.1f MB)  -> x%.1f menos · %zu rangos calculados\n",
                withRange.size(), withRange.size() * kNodeMB,
                withRange.empty() ? 0.0 : (double)noRange.size() / (double)withRange.size(),
                g_rangeCache.size());
    CHECK(withRange.size() < noRange.size(), "la cota real del nodo SI desbloquea el recorte cercano");
    CHECK(withRange.size() * kNodeMB < 128.0, "el conjunto visible cabe holgadamente en VRAM");

    // CONTRAPRUEBA: un proveedor que siempre diga "no lo sé" tiene que dar exactamente el mismo
    // resultado que no pasar proveedor. Sin esto, el test pasaría con un selector que ignorase el
    // rango y recortase por otro motivo.
    struct U { static NodeRange unknown(const NodeId&, void*) { return NodeRange{}; } };
    std::vector<NodeId> dunno;
    nodeSelectVisible(R, cam, pc, radPerPx, dunno, 200000, TERRAIN_NODE_ERROR_PX, &fwd, cone, 5000.0,
                      &U::unknown, nullptr);
    std::printf("    CONTRAPRUEBA: proveedor que siempre dice \"no lo se\" -> %zu nodos (sin rango: %zu)\n",
                dunno.size(), noRange.size());
    CHECK(dunno.size() == noRange.size(),
          "CONTRAPRUEBA: un rango invalido degrada al bound conservador, no recorta por su cuenta");
}

// ================================================================================================
// F2 — EL POOL: residencia, LRU, presupuesto y, sobre todo, REUTILIZACIÓN al moverse
//
// La última es la que justifica el v5 entero. Hoy, al derivar 48 m, el clipmap reutiliza **8 nodos
// de 257 049 (0 %)** porque su marco tangente se re-ancla y todos los nodos se mueven con él
// (memoria `collision-mesh-rebuild-cost`). Los nodos del quadtree están FIJOS AL MUNDO, así que al
// andar los que siguen a la vista son los MISMOS objetos. Esto lo mide.
// ================================================================================================
void test_terrain_node_pool() {
    beginTest("terrain_node_pool");

    // ── (1) CAPACIDAD, LRU Y PRESUPUESTO ────────────────────────────────────────────────────────
    {
        TerrainNodePool pool(8, 3);
        pool.beginFrame();
        // ⚠️ EL CONTRATO CAMBIO (2026-08-24): el presupuesto ya NO se aplica al encolar, sino
        // DESPUES de ordenar por cercania. Antes se cortaba en `request`, asi que los que se
        // generaban eran los que el recorrido visito primero — un orden espacial arbitrario. Al
        // girar 90 grados entran 935 nodos nuevos y solo caben 170 por frame: cuales de esos 170 se
        // eligen es la diferencia entre que se resuelva lo cercano o lo lejano.
        for (uint32_t i = 0; i < 10; ++i) pool.request(NodeId{ PlanetFace::FRONT, 4, i, 0 });
        // ⚠️ EL CONTRATO VOLVIO A CAMBIAR (2026-08-25): se encola tambien LA CADENA DE ANCESTROS.
        // El selector solo pide HOJAS, asi que un nodo interior solo era residente por accidente —de
        // cuando el mismo fue hoja— y la caida por ancestro podia saltarse hasta 4 niveles. El
        // geomorph solo sabe cerrar UNO (apunta al padre), asi que lo demas quedaba como pincho.
        // 10 hojas de nivel 4 (i=0..9) arrastran 5 padres + 3 abuelos + 2 bisabuelos + la raiz.
        std::printf("    10 pedidos -> %zu encolados (las hojas MAS su cadena de ancestros)\n",
                    pool.takePending().size());
        CHECK(pool.takePending().size() == 21, "se encola cada hoja Y los eslabones que le faltan");
        // Y lo que importa no es el numero, es que la cadena llegue ENTERA hasta la raiz.
        {
            std::unordered_set<uint64_t> q;
            for (const NodeId& n : pool.takePending()) q.insert(nodeKey(n));
            size_t orphans = 0;
            for (const NodeId& n : pool.takePending()) {
                if (n.level == 0) continue;
                const NodeId p{ n.face, n.level - 1, n.i / 2, n.j / 2 };
                if (q.count(nodeKey(p)) == 0 && !pool.isResident(p)) ++orphans;
            }
            CHECK(orphans == 0, "ningun nodo encolado se queda sin padre: la cadena llega a la raiz");
        }

        pool.prioritisePending(glm::dvec3(6371000.0, 0, 0), glm::dvec3(0.0), 6371000.0);
        std::printf("    tras ordenar y recortar al tope 3 -> %zu\n", pool.takePending().size());
        CHECK(pool.takePending().size() == 3, "el presupuesto se respeta tras `prioritisePending`");
        // EL ORDEN, que es la mitad del arreglo: generar una hoja cuyo padre no esta residente no
        // cierra ningun salto —el vecino seguira cayendo varios niveles—, mientras que el padre lo
        // cierra para sus cuatro hijos. Asi que el padre va SIEMPRE delante, aunque el hijo este mas
        // cerca de la camara.
        {
            std::unordered_set<uint64_t> seen;
            bool parentFirst = true;
            for (const NodeId& n : pool.takePending()) {
                if (n.level > 0) {
                    const NodeId p{ n.face, n.level - 1, n.i / 2, n.j / 2 };
                    if (seen.count(nodeKey(p)) == 0 && !pool.isResident(p)) parentFirst = false;
                }
                seen.insert(nodeKey(n));
            }
            CHECK(parentFirst, "el padre se genera ANTES que el hijo: nunca sale una hoja huerfana");
        }

        // Llenar el pool por encima de su capacidad.
        for (uint32_t i = 0; i < 20; ++i) {
            pool.beginFrame();
            pool.publish(NodeId{ PlanetFace::FRONT, 4, i, 0 }, NodeRange{ -1.0f, 1.0f });
        }
        std::printf("    capacidad 8 · residentes tras publicar 20: %zu\n", pool.residentCount());
        CHECK(pool.residentCount() <= 8, "nunca hay mas residentes que huecos");

        // LRU: lo que se usó ESTE frame no se desaloja. Se piden 8 (llenan el pool) y en el mismo
        // frame se intenta publicar uno más: debe RECHAZARSE, no tirar un visible.
        TerrainNodePool p2(4, 99);
        p2.beginFrame();
        for (uint32_t i = 0; i < 4; ++i) p2.publish(NodeId{ PlanetFace::TOP, 3, i, 0 }, NodeRange{ 0.f, 1.f });
        for (uint32_t i = 0; i < 4; ++i) p2.request(NodeId{ PlanetFace::TOP, 3, i, 0 });   // los toca
        const int rejected = p2.publish(NodeId{ PlanetFace::TOP, 3, 99, 0 }, NodeRange{ 0.f, 1.f });
        std::printf("    LRU: con los 4 huecos usados ESTE frame, publicar otro devuelve %d\n", rejected);
        CHECK(rejected == -1, "no se desaloja un nodo usado este frame (eso seria parpadeo)");
    }

    // ── (2) FALLBACK POR ANCESTRO: nunca un agujero ─────────────────────────────────────────────
    {
        TerrainNodePool pool(64, 8);
        pool.beginFrame();
        pool.publish(NodeId{ PlanetFace::FRONT, 0, 0, 0 }, NodeRange{ -100.f, 100.f }, /*pin*/ true);
        pool.beginFrame();
        // ⚠️ Índices VÁLIDOS: en el nivel L el rango es [0, 2^L). El primer intento usó j=5678 en el
        // nivel 12 (tope 4095) y al subir hacia la raíz aterrizaba en (0,1), que no existe — el
        // fallback devolvía -1 y parecía un bug del pool. Lo era del test.
        const auto r = pool.request(NodeId{ PlanetFace::FRONT, 12, 1234, 2678 });
        std::printf("    fallback: nodo nivel 12 no residente -> hueco %d, exacto=%s, ancestro nivel %u\n",
                    r.slot, r.exact ? "si" : "no", r.node.level);
        CHECK(r.slot >= 0, "siempre hay algo que dibujar (la raiz esta fijada)");
        CHECK(!r.exact && r.node.level == 0, "cae al ancestro mas profundo residente");

        // CONTRAPRUEBA: sin raíz publicada NO hay ancestro, y el pool tiene que decirlo en vez de
        // devolver un hueco cualquiera. Un -1 honesto es mejor que dibujar el nodo de otro.
        TerrainNodePool empty(8, 4);
        empty.beginFrame();
        const auto r2 = empty.request(NodeId{ PlanetFace::BACK, 5, 3, 3 });
        std::printf("    CONTRAPRUEBA: pool vacio -> hueco %d (debe ser -1)\n", r2.slot);
        CHECK(r2.slot == -1, "CONTRAPRUEBA: sin ancestro el pool devuelve -1, no un hueco ajeno");
    }
}

// ------------------------------------------------------------------ el número que justifica el v5
void test_terrain_node_pool_reuse() {
    beginTest("terrain_node_pool_reuse");
    double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    // Presupuesto de error relajado para que el test sea rápido; la propiedad que se mide (¿los
    // nodos sobreviven al movimiento?) no depende de cuántos haya.
    const double radPerPx = fovY / 1080.0;
    const double cone = nodeFrustumConeHalfAngle(fovY, 1920.0 / 1080.0);
    const double errPx = 8.0;

    TerrainNodePool pool(20000, 100000);   // sin presiones: aquí se mide reutilización, no evicción

    const glm::dvec3 dir0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 east = glm::normalize(glm::cross(dir0, glm::dvec3(0, 1, 0)));

    size_t prevCount = 0, totalSel = 0, totalReused = 0;
    std::vector<NodeId> sel;
    // Doce pasos de 48 m — la MISMA deriva con la que se midió el 0 % del clipmap.
    for (int step = 0; step < 12; ++step) {
        const glm::dvec3 cam = pc + dir0 * (R + 2.0) + east * (48.0 * step);
        pool.beginFrame();
        nodeSelectVisible(R, cam, pc, radPerPx, sel, 200000, errPx, &east, cone, 5000.0,
                          &TerrainNodePool::rangeFnAdapter, &pool);
        size_t reused = 0;
        for (const NodeId& n : sel) if (pool.isResident(n)) ++reused;
        if (step > 0) { totalSel += sel.size(); totalReused += reused; }
        // Publicar todo lo seleccionado (en el motor real esto lo haría el generador, con su tope).
        for (const NodeId& n : sel) pool.publish(n, NodeRange{ -300.0f, 300.0f });
        if (step == 1) prevCount = sel.size();
    }
    const double pct = totalSel ? 100.0 * (double)totalReused / (double)totalSel : 0.0;
    std::printf("    12 pasos de 48 m · %zu nodos por paso (aprox) · residentes al final %zu\n",
                prevCount, pool.residentCount());
    std::printf("    REUTILIZACION al andar: %zu de %zu = %.1f %%\n", totalReused, totalSel, pct);
    std::printf("      (el clipmap, con la MISMA deriva de 48 m, reutiliza 8 de 257 049 = 0,003 %%)\n");
    CHECK(pct > 80.0, "andar 48 m reutiliza la inmensa mayoria de los nodos");

    // CONTRAPRUEBA: un salto ENORME (medio planeta) no debe reutilizar casi nada. Sin esto, el test
    // pasaría con un pool que dijera "residente" a todo.
    const glm::dvec3 far = pc + glm::normalize(glm::dvec3(-1.0, 0.2, -0.4)) * (R + 2.0);
    const glm::dvec3 farFwd = glm::normalize(glm::cross(glm::normalize(far - pc), glm::dvec3(0, 1, 0)));
    pool.beginFrame();
    nodeSelectVisible(R, far, pc, radPerPx, sel, 200000, errPx, &farFwd, cone, 5000.0,
                      &TerrainNodePool::rangeFnAdapter, &pool);
    size_t farReused = 0;
    for (const NodeId& n : sel) if (pool.isResident(n)) ++farReused;
    const double farPct = sel.empty() ? 0.0 : 100.0 * (double)farReused / (double)sel.size();
    std::printf("    CONTRAPRUEBA: teletransporte al otro lado del planeta -> %.1f %% reutilizado\n", farPct);
    CHECK(farPct < 10.0, "CONTRAPRUEBA: saltar medio planeta NO reutiliza (el pool no miente)");
}

/**
 * @brief LA CAÍDA POR ANCESTRO NO PUEDE SALTARSE NIVELES, Y ESO LO DECIDE LA POLÍTICA DEL POOL.
 *
 * El geomorph cierra el escalón contra el vecino grueso apuntando a la altura del **padre**. Eso solo
 * vale si el vecino está a UN nivel: con dos quedan 0,339 m y con cuatro **1,372 m**, que es lo que
 * se ve como pinchos.
 *
 * El árbol que devuelve `nodeSelectVisible` ya es 2:1 (0 saltos de más de uno a cualquier
 * presupuesto). El que se DIBUJA no lo era, y la causa está en el pool: **el selector solo pide
 * HOJAS**, así que un nodo interior solo llegaba a ser residente por accidente —de cuando él mismo
 * fue hoja— y un nodo sin hueco caía al ancestro residente más profundo, que podía estar a cuatro.
 *
 * Desde el 2026-08-25 `request` encola también los eslabones que le faltan a la cadena raíz→nodo y
 * `prioritisePending` los emite de grueso a fino, así que lo residente es un SUBÁRBOL CONEXO desde
 * la raíz y el ancestro más profundo de una hoja pedida es su padre.
 *
 * ⚠️ Se mide el A/B con `setChainAncestors`, no se afirma. Y con el presupuesto REAL del motor: con
 * uno infinito todo es residente y no hay caída que medir — el test pasaría sin comprobar nada.
 *
 * ── ⚠️ LO QUE LA MEDIDA CORRIGIÓ (2026-08-25) ──────────────────────────────────────────────────
 *
 * `TODO.md` daba por hecho que *"el arreglo va en la política del POOL"*. La medida dice otra cosa:
 * **en reposo la política vieja también converge a 0 caídas profundas.** No eran un estado estable,
 * son transitorias — del presupuesto, mientras entran nodos nuevos. La cadena baja la caída peor
 * girando de **11 a 5 niveles** (9 nodos → 4), que es una mejora real del transitorio, pero no es
 * la causa de un pincho que se vea estando quieto. Para eso hay que mirar a otro sitio.
 */
void test_terrain_node_pool_chain() {
    beginTest("terrain_node_pool_chain");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / 1080.0;
    const double cone = nodeFrustumConeHalfAngle(fovY, 1920.0 / 1080.0);

    // `holdFrames` = frames QUIETO tras el giro. Separa las dos preguntas: cuánto se cae MIENTRAS
    // giras (eso es presupuesto: 170 nodos por frame y punto) y cuánto queda cuando el pool ya ha
    // tenido tiempo (eso sí es la política). Sin separarlas, un número malo no dice cuál de las dos.
    auto run = [&](bool chain, int holdFrames,
                   int& worstDrop, size_t& deepNodes, size_t& drawnTotal) {
        TerrainNodePool pool(2048, 170);          // el presupuesto real: 2048 huecos, 170 por frame
        pool.setChainAncestors(chain);
        for (uint32_t f = 0; f < 6; ++f)          // raíces fijadas, como hace el motor
            pool.publish(NodeId{ (PlanetFace)f, 0, 0, 0 }, NodeRange{ -9000.0f, 9000.0f }, true);

        const glm::dvec3 dir0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
        const glm::dvec3 east = glm::normalize(glm::cross(dir0, glm::dvec3(0, 1, 0)));
        const glm::dvec3 cam  = pc + dir0 * (R + 1200.0);
        std::vector<NodeId> sel;
        worstDrop = 0; deepNodes = 0; drawnTotal = 0;
        // Veinte frames GIRANDO, que es cuando entran nodos nuevos de golpe y el pool va por detrás.
        for (int step = 0; step < 20 + holdFrames; ++step) {
            const double ang = 0.06 * (double)std::min(step, 19);   // tras el 19 la camara se para
            const glm::dvec3 fwd = glm::normalize(dir0 * std::cos(ang) + east * std::sin(ang));
            pool.beginFrame();
            nodeSelectVisible(R, cam, pc, radPerPx, sel, 2048, TERRAIN_NODE_ERROR_PX, &fwd, cone,
                              5000.0, &TerrainNodePool::rangeFnAdapter, &pool);
            worstDrop = 0; deepNodes = 0; drawnTotal = 0;   // solo interesa el estado ESTABLE
            for (const NodeId& n : sel) {
                const TerrainNodePool::Resolved r = pool.request(n);
                if (r.slot < 0) continue;
                ++drawnTotal;
                const int d = (int)n.level - (int)r.node.level;
                worstDrop = std::max(worstDrop, d);
                if (d > 1) ++deepNodes;
            }
            pool.prioritisePending(cam, pc, R);
            for (const NodeId& p : pool.takePending())
                pool.publish(p, NodeRange{ -9000.0f, 9000.0f });
        }
    };

    int wOldT = 0, wNewT = 0, wOldS = 0, wNewS = 0;
    size_t dOldT = 0, dNewT = 0, dOldS = 0, dNewS = 0, nOldT = 0, nNewT = 0, nOldS = 0, nNewS = 0;
    run(false, 0,  wOldT, dOldT, nOldT);
    run(true,  0,  wNewT, dNewT, nNewT);
    run(false, 30, wOldS, dOldS, nOldS);
    run(true,  30, wNewS, dNewS, nNewS);

    std::printf("    2048 huecos · 170 nodos/frame · peor caida por ancestro, en NIVELES\n");
    std::printf("                          GIRANDO            QUIETO (30 frames despues)\n");
    std::printf("      SIN cadena     %3d niveles (%zu nodos)   %3d niveles (%zu nodos)\n",
                wOldT, dOldT, wOldS, dOldS);
    std::printf("      CON cadena     %3d niveles (%zu nodos)   %3d niveles (%zu nodos)\n",
                wNewT, dNewT, wNewS, dNewS);
    std::printf("      (dibujados: %zu / %zu · el geomorph apunta al PADRE, asi que a 2 niveles\n"
                "       quedan 0,339 m y a 4, 1,372 m: todo lo que pase de 1 se ve como pincho)\n",
                nOldS, nNewS);

    CHECK(nNewS > 100, "se dibujan nodos de verdad");
    CHECK(wNewS <= 1 && dNewS == 0, "CON cadena, en reposo: ninguna caida de mas de un nivel");

    // ⚠️ Y AQUI EL TEST CORRIGIO LA HIPOTESIS CON LA QUE SE ESCRIBIO. Se esperaba que la politica
    // vieja siguiera cayendo varios niveles aunque se esperase —eso habria hecho de la cadena EL
    // arreglo—. **No es cierto: tambien converge a 0.** O sea que las caidas profundas nunca fueron
    // un estado estable del pool, son TRANSITORIAS: aparecen mientras entran nodos nuevos y el
    // presupuesto va por detras. La cadena no las cura, las REDUCE a la mitad.
    //
    // Consecuencia, y hay que tenerla presente antes de buscar por aqui: si los pinchos se ven
    // ESTANDO QUIETO, la caida por ancestro NO los explica y la causa esta en otra parte.
    CHECK(dOldS == 0, "la politica vieja TAMBIEN converge en reposo: el problema es del transitorio");
    CHECK(wOldT > 1, "CONTRAPRUEBA: girando SI aparecen caidas profundas — ahi es donde vive esto");
    CHECK(wNewT < wOldT, "y la cadena las reduce (11 -> 5 niveles medido): mejora el TRANSITORIO");
}

// ================================================================================================
// EL SUB-RECTANGULO: leer el slot de un ancestro SIN moverse de su sitio
//
// Gemelo de `harukaNodeSample` en `terrain_node.vert`. Cuando la hoja no esta residente, se dibuja
// igualmente en SU huella y lee los datos `k` niveles mas arriba. El texel `t` de la hoja cae, en
// texeles del ancestro, en `(cells*sub + t) / 2^k` con `sub = indice mod 2^k`.
//
// Lo que hay que demostrar es que esa coordenada apunta AL MISMO PUNTO de la cara del cubo que el
// texel de la hoja. Si no, el terreno de relleno saldria desplazado — y un desplazamiento pequeno es
// justo lo que nadie ve mirando y todo el mundo nota andando.
void test_terrain_node_subrect_mapping() {
    beginTest("terrain_node_subrect_mapping");
    const double R = 6371000.0;
    const double cells = (double)TERRAIN_NODE_CELLS;

    // Coordenada local de un texel sobre la cara, en [-1,1]. Es la misma cuenta que hace el .vert.
    auto localOf = [&](uint32_t level, uint32_t idx, double texel) {
        return -1.0 + 2.0 * ((double)idx * cells + texel) / (cells * (double)(1u << level));
    };

    double worst = 0.0, worstOffBy1 = 0.0, worstNoShift = 0.0;
    size_t cases = 0;
    for (uint32_t lvl : { 6u, 11u, 17u })
        for (int kUp = 1; kUp <= 8 && (uint32_t)kUp <= lvl; ++kUp) {
            // Una hoja cualquiera con indices que NO sean multiplos de 2^k: el caso que destapa el
            // fallo de redondeo. Con indices alineados el mapeo sale bien hasta estando mal.
            const uint32_t lim = 1u << lvl;
            const uint32_t li = (lim / 3u) | 1u, lj = (lim / 7u) | 1u;
            const uint32_t sub = (1u << kUp) - 1u;
            const uint32_t ai = li >> kUp, aj = lj >> kUp;
            const uint32_t si = li & sub,  sj = lj & sub;
            const double inv = 1.0 / (double)(1u << kUp);
            for (uint32_t t = 0; t <= TERRAIN_NODE_CELLS; t += 8) {
                const double au = (cells * (double)si + (double)t) * inv;
                const double av = (cells * (double)sj + (double)t) * inv;
                worst = std::max(worst, std::fabs(localOf(lvl, li, (double)t) - localOf(lvl - kUp, ai, au)));
                worst = std::max(worst, std::fabs(localOf(lvl, lj, (double)t) - localOf(lvl - kUp, aj, av)));
                // CONTRAPRUEBAS: los dos errores que este mapeo invita a cometer.
                const double bad1 = (cells * (double)si + (double)t + 1.0) * inv;      // un texel de mas
                const double bad2 = (double)t * inv;                                   // olvidar el sub-indice
                worstOffBy1 = std::max(worstOffBy1,
                    std::fabs(localOf(lvl, li, (double)t) - localOf(lvl - kUp, ai, bad1)));
                worstNoShift = std::max(worstNoShift,
                    std::fabs(localOf(lvl, li, (double)t) - localOf(lvl - kUp, ai, bad2)));
                ++cases;
            }
        }

    // De coordenada local a metros sobre la superficie: la cara mide un cuarto de meridiano de lado.
    const double m_per_local = R * 3.14159265358979 / 2.0 / 2.0;
    std::printf("    %zu casos (nivel 6/11/17 x k=1..8, indices NO alineados)\n", cases);
    std::printf("      mapeo correcto          : %.3e local = %.6f m\n", worst, worst * m_per_local);
    std::printf("      CONTRAPRUEBA +1 texel   : %.3e local = %.3f m\n",
                worstOffBy1, worstOffBy1 * m_per_local);
    std::printf("      CONTRAPRUEBA sin sub-idx: %.3e local = %.0f m\n",
                worstNoShift, worstNoShift * m_per_local);

    CHECK(cases > 100, "se prueban casos de verdad");
    CHECK(worst * m_per_local < 1e-6, "el sub-rectangulo cae EXACTAMENTE sobre el texel de la hoja");
    CHECK(worstOffBy1 * m_per_local > 0.1,
          "CONTRAPRUEBA: un texel de mas ya se sale del milimetro (el test detecta el fallo)");
    CHECK(worstNoShift * m_per_local > 1000.0,
          "CONTRAPRUEBA: olvidar el sub-indice desplaza el relleno kilometros");
}

// ================================================================================================
// LA HUELLA DEL FALLBACK: el ancestro se dibuja ENTERO, y esa es la decision de arquitectura
//
// Hoy la identidad GEOMETRICA de un nodo esta atada a su SLOT DE DATOS: `g.node` y el hueco salen los
// dos de `Resolved`. Cuando una hoja no esta residente, el pool devuelve un ancestro y el renderer
// dibuja **la huella del ancestro**, no la de la hoja que falta. Un ancestro k niveles por encima
// cubre 4^k hojas, asi que **una sola hoja que falta estropea 4^k veces su propia area**.
//
// Lo que mide este test es cuanto vale desacoplarlo: dibujar la huella de LA HOJA con los datos del
// ancestro (un sub-rectangulo de su rejilla de 129x129). El area estropeada pasaria de la del
// ancestro a la de la hoja. Es la cifra que justifica —o no— tocar el shader.
void test_terrain_node_fallback_footprint() {
    beginTest("terrain_node_fallback_footprint");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / 1080.0;
    const double cone = nodeFrustumConeHalfAngle(fovY, 1920.0 / 1080.0);

    // Area de un nodo en unidades de "nodo de nivel 0": 4^-level. Es exacta para comparar huellas.
    auto areaOf = [](const NodeId& n) { return std::pow(0.25, (double)n.level); };

    TerrainNodePool pool(2048, 85);
    for (uint32_t f = 0; f < 6; ++f)
        pool.publish(NodeId{ (PlanetFace)f, 0, 0, 0 }, NodeRange{ -9000.0f, 9000.0f }, true);

    const glm::dvec3 dir0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 east = glm::normalize(glm::cross(dir0, glm::dvec3(0, 1, 0)));
    const glm::dvec3 cam  = pc + dir0 * (R + 1200.0);
    std::vector<NodeId> sel;

    double worstRatio = 0.0, worstAnc = 0.0, worstLeaf = 0.0, worstTotal = 0.0;
    double worstRule = 0.0, worstRuleHole = 0.0, worstAgg = 0.0, worstAggHole = 0.0;
    double ruleArea = 0.0, ruleHoleArea = 0.0, aggArea = 0.0, aggHoleArea = 0.0;
    int    worstUp = 0; size_t worstMisses = 0;
    const int kWarm = 60;

    for (int step = 0; step < kWarm + 30; ++step) {
        const double ang = (step >= kWarm) ? 0.06 * (double)(step - kWarm) : 0.0;
        const glm::dvec3 fwd = glm::normalize(dir0 * std::cos(ang) + east * std::sin(ang));
        pool.beginFrame();
        nodeSelectVisible(R, cam, pc, radPerPx, sel, 2048, TERRAIN_NODE_ERROR_PX, &fwd, cone,
                          5000.0, &TerrainNodePool::rangeFnAdapter, &pool);

        std::vector<std::pair<NodeId, NodeId>> pairs;   // (hoja pedida, nodo resuelto)
        for (const NodeId& n : sel) {
            const TerrainNodePool::Resolved r = pool.request(n);
            if (r.slot >= 0) pairs.emplace_back(n, r.node);
        }
        pool.prioritisePending(cam, pc, R);
        for (const NodeId& p : pool.takePending())
            pool.publish(p, NodeRange{ -9000.0f, 9000.0f });

        if (step < kWarm) continue;

        // HOY: el area rota es la de los ancestros emitidos, contada UNA vez cada uno.
        // CON SUB-RECTANGULO: es la de las hojas que fallan, cada una en su sitio.
        std::unordered_map<uint64_t, double> ancArea;
        double leafArea = 0.0, total = 0.0;
        int up = 0; size_t misses = 0;
        for (const auto& pr : pairs) {
            total += areaOf(pr.first);
            if (nodeKey(pr.first) == nodeKey(pr.second)) continue;   // exacta: no rompe nada
            ancArea[nodeKey(pr.second)] = areaOf(pr.second);
            leafArea += areaOf(pr.first);
            up = std::max(up, (int)pr.first.level - (int)pr.second.level);
            ++misses;
        }
        double aArea = 0.0; for (const auto& kv : ancArea) aArea += kv.second;

        // ── EL DILEMA, MEDIDO: NINGUNA REGLA DE "DIBUJAR O NO" SE SALVA ────────────────────────
        //
        // Solo hay dos formas de decidir sobre un fallback sin tocar el shader, y las dos pierden:
        //   AGRESIVA — quitarlo si tiene ALGUN descendiente dibujado. Quita la sabana entera... y
        //              deja sin dibujar todo lo que ese ancestro tapaba y nadie mas cubre.
        //   SEGURA   — quitarlo solo si le tapan la huella ENTERA. No abre agujeros, pero lejos casi
        //              nunca se cumple, asi que la sabana se queda.
        // Lo unico que escapa del dilema es cambiar la HUELLA: dibujar la hoja con datos del ancestro.
        {
            std::unordered_map<uint64_t, double> cov;      // cuanto le tapan los EXACTOS a cada nodo
            std::unordered_set<uint64_t> hasDesc;
            for (const auto& pr : pairs) {
                if (nodeKey(pr.first) != nodeKey(pr.second)) continue;   // solo los exactos acreditan
                NodeId a = pr.second; double f = 1.0;
                while (a.level > 0) { a.level--; a.i /= 2; a.j /= 2; f *= 0.25;
                                      cov[nodeKey(a)] += f; hasDesc.insert(nodeKey(a)); }
            }
            std::unordered_set<uint64_t> seenA, seenS;
            double aggBad = 0.0, aggHole = 0.0, safeBad = 0.0, safeHole = 0.0;
            for (const auto& pr : pairs) {
                if (nodeKey(pr.first) == nodeKey(pr.second)) continue;   // exacta: no rompe nada
                const uint64_t k = nodeKey(pr.second);
                const bool any  = hasDesc.count(k) != 0;
                const auto  ic  = cov.find(k);
                const bool full = (ic != cov.end() && ic->second >= 1.0 - 1e-9);
                if (any)  aggHole  += areaOf(pr.first);
                else if (seenA.insert(k).second) aggBad += areaOf(pr.second);
                if (full) safeHole += areaOf(pr.first);
                else if (seenS.insert(k).second) safeBad += areaOf(pr.second);
            }
            aggArea = aggBad; aggHoleArea = aggHole; ruleArea = safeBad; ruleHoleArea = safeHole;
        }
        if (total <= 0.0) continue;
        const double ratio = (leafArea > 0.0) ? (aArea / leafArea) : 0.0;
        if (ratio > worstRatio) {
            worstRatio = ratio; worstAnc = aArea; worstLeaf = leafArea;
            worstTotal = total; worstUp = up; worstMisses = misses;
            worstRule = ruleArea; worstRuleHole = ruleHoleArea;
            worstAgg  = aggArea;  worstAggHole  = aggHoleArea;
        }
    }

    std::printf("    el peor frame del giro: %zu hojas sin residencia, caida maxima %d niveles\n",
                worstMisses, worstUp);
    std::printf("      area que se dibuja MAL hoy (huella del ancestro) : %.2f %% de lo visible\n",
                100.0 * worstAnc / worstTotal);
    std::printf("      area con la huella de la HOJA (sub-rectangulo)   : %.2f %% de lo visible\n",
                100.0 * worstLeaf / worstTotal);
    std::printf("      ninguna regla de DIBUJAR-O-NO escapa del dilema:\n");
    std::printf("        quitar si tiene ALGUN descendiente : rompe %.2f %% · AGUJERO %.2f %%\n",
                100.0 * worstAgg / worstTotal, 100.0 * worstAggHole / worstTotal);
    std::printf("        quitar si le tapan la huella ENTERA: rompe %.2f %% · AGUJERO %.2f %%\n",
                100.0 * worstRule / worstTotal, 100.0 * worstRuleHole / worstTotal);
    std::printf("      -> el fallback estropea %.0fx su propia area, y la salida es la HUELLA\n",
                worstRatio);

    CHECK(worstMisses > 0, "hay hojas sin residencia de verdad (si no, el resto no mide nada)");
    CHECK(worstUp > 1, "y la caida pasa de un nivel: es el caso que importa");
    CHECK(worstRatio > 10.0,
          "el ancestro rompe MAS DE DIEZ VECES el area de la hoja que falta: la huella es el problema");
    CHECK(worstLeaf < worstAnc,
          "CONTRAPRUEBA: con la huella de la hoja el area rota es ESTRICTAMENTE menor");
    // ── LAS DOS REGLAS DE "DIBUJAR O NO", Y POR QUE NINGUNA VALE ────────────────────────────────
    //
    // Se probaron LAS DOS en el juego, y las dos las juzgo Andoni mirando:
    //   agresiva (basta un descendiente) -> *"ha mejorado, por lo menos el cercano, el lejano sigue
    //                                        con mal pero mejor"*
    //   segura   (tapado ENTERO)         -> *"con este cambio es peor en general"*
    //
    // ⚠️ Y ESTE BANCO NO REPRODUCE EL CASO QUE DECIDE. Aqui la agresiva abre un agujero de 0,12 %,
    // o sea que parece gratis; en el juego la sonda midio **99,995 % de lo visible**, porque el caso
    // malo es el LEJANO —entra una region entera de golpe, no hay nada fino debajo— y este banco gira
    // sobre terreno ya cargado. Si alguien vuelve por aqui: la cifra del banco NO autoriza a encender
    // la regla. (Y la sonda del juego pesa por area de superficie, no por pixeles, asi que tampoco
    // esa cifra vale para decidir: hay que pesar por pixeles.)
    //
    // La mascara ademas costaba 530 MB de RSS (1 358 contra 827) y esta APAGADA por defecto.
    CHECK(worstAggHole > 0.0 && worstAgg == 0.0,
          "la agresiva quita la sabana entera pero SIEMPRE deja sin dibujar la huella de la hoja");
    CHECK(worstRuleHole == 0.0 && worstRule > worstAnc * 0.5,
          "y la segura no abre agujero pero deja la sabana intacta: el dilema no tiene salida aqui");
}

// =================================================================================================
// LA PARTICION EXTRA BAJO AGUA SOMERA — `nodeShallowWantsSplit` en `nodeSelectVisible`
//
// Los "circulos en la costa": al cruzar la frontera entre dos niveles el texel se dobla y el fondo
// dibujado suelta una octava. Bajo el agua eso se ve como un anillo, porque el agua se dibuja por
// PROFUNDIDAD y es no lineal cerca de cero. Acotar la zancada (renderer) quito las bandas de
// zancada; esto quita las de nivel donde llega (`waterReach`) y se mide:
//   (1) con la regla, ninguna frontera de nivel dentro del alcance salta mas de 0,25 x profundidad;
//   (2) lo que cuesta en nodos, comparado con el selector de siempre;
//   (3) CONTRAPRUEBAS: sin agua el conjunto es IDENTICO; con mar hondo en todas partes la regla
//       no pide nada; y sin la regla los saltos SI pasan del presupuesto (si no, el test no
//       mediria nada).
// =================================================================================================
void test_terrain_node_shallow_split() {
    beginTest("terrain_node_shallow_split");
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / 1080.0;
    const double waterLv = 15.0, frac = 0.25, reach = TERRAIN_NODE_WATER_SPLIT_REACH;

    // Una costa sintetica: la linea `dot(dir, t1) = 0`. A un lado tierra (+5..+50 m); al otro una
    // PLATAFORMA plana a `shelfM` bajo el agua durante `shelfKm`, y luego el talud hasta -2000. La
    // plataforma es el caso que hace anillos: con una playa de pendiente normal (2 m por 100 m) las
    // fronteras de nivel caen ya en agua honda (medido aqui mismo: 17->16 a 692 m con 29 m de
    // fondo, salto 0,04 m: 0,1 % de la profundidad) y la regla no pide nada. Es el bake del juego,
    // que interpola la costa sobre 80 km, el que deja fondos de pocos metros durante kilometros.
    // El rango de un nodo es el de sus 5x5 muestras, como en `terrain_node_demand_with_range`.
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const glm::dvec3 t1  = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    struct Ctx { double R; glm::dvec3 up0, t1; double shelfM, shelfKm; bool deepEverywhere; };
    static const auto floorAt = [](const Ctx& c, const glm::dvec3& d) -> float {
        if (c.deepEverywhere) return -2000.0f;
        const double x = glm::dot(d, c.t1) * c.R;     // metros desde la costa, + hacia el mar
        if (x < 0.0) return 5.0f + (float)std::min(-x * 0.1, 45.0);
        if (x < c.shelfKm * 1000.0) return (float)(15.0 - c.shelfM);   // lamina a 15 m: fondo a -shelfM
        return (float)std::max(15.0 - c.shelfM - (x - c.shelfKm * 1000.0) * 0.05, -2000.0);
    };
    auto rangeFn = [](const NodeId& n, void* user) -> NodeRange {
        const Ctx& c = *static_cast<const Ctx*>(user);
        NodeRange r;
        const uint32_t step = TERRAIN_NODE_CELLS / 4;
        for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += step)
            for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += step) {
                const float h = floorAt(c, nodeTexelDir(n, u, v));
                r.minM = std::min(r.minM, h); r.maxM = std::max(r.maxM, h);
            }
        return r;
    };
    Ctx coast{ R, up0, t1, 3.0, 30.0, false };
    Ctx deep { R, up0, t1, 3.0, 30.0, true  };

    // Camara en la playa, a 2 m sobre el mar, mirando al mar.
    const glm::dvec3 cam = pc + up0 * (R + waterLv + 2.0);
    const glm::dvec3 fwd = glm::normalize(t1 - up0 * 0.05);
    const double cone = nodeFrustumConeHalfAngle(fovY, 1920.0 / 1080.0);

    auto selectR = [&](Ctx& c, double lvl, double fr, double rch, size_t* splits) {
        std::vector<NodeId> v;
        nodeSelectVisible(R, cam, pc, radPerPx, v, 400000, TERRAIN_NODE_ERROR_PX, &fwd, cone,
                          5000.0, rangeFn, &c, nullptr, nullptr, lvl, fr, rch, splits);
        return v;
    };
    auto select = [&](Ctx& c, double lvl, double fr, size_t* splits) { return selectR(c, lvl, fr, reach, splits); };
    size_t splitsOn = 0, splitsDry = 0, splitsDeep = 0;
    const auto base = select(coast, -1e30, 0.0, nullptr);       // el selector de siempre
    const auto on   = select(coast, waterLv, frac, &splitsOn);  // con la regla
    const auto dry  = select(coast, -1e30, frac, &splitsDry);   // regla activa, sin agua
    const auto dp   = select(deep,  waterLv, frac, &splitsDeep);// regla activa, todo hondo
    // The reference is the SAME water level with the rule off (fraction 0), not "no water": since
    // 11-10 the water level also raises a submerged node's height to the sheet (the sea is what is
    // drawn there, not the seabed), so with and without water the deep sea selects differently by
    // design. What this control isolates is the shallow rule: in deep water it must ask nothing.
    const auto dpB  = select(deep,  waterLv, 0.0, nullptr);

    std::printf("    seleccion: sin regla %zu nodos · con regla %zu (+%.0f %%, %zu particiones del agua)\n",
                base.size(), on.size(), 100.0 * ((double)on.size() / (double)base.size() - 1.0), splitsOn);

    // ── (1) EL SALTO EN CADA FRONTERA DE NIVEL, COMO FRACCION DE LA PROFUNDIDAD ─────────────────
    // Se camina mar adentro por la linea de vista y se anota el nivel de la hoja que cubre cada
    // punto. Donde cambia hay una frontera; su salto es lo que el texel grueso esconde y el fino
    // no (`nodeMissingReliefM(t_fino, t_grueso)`), y se compara con la profundidad ALLI.
    auto leafAt = [&](const std::vector<NodeId>& sel, const glm::dvec3& dir) -> int {
        const auto idx = nodeDrawnIndex(sel);
        PlanetFace f; double lx, ly;
        dirToCubeFaceClosed(dir, f, lx, ly);
        for (int lv = (int)nodeMaxLevelFor(R); lv >= 0; --lv) {
            const uint64_t lim = 1ull << lv;
            const int64_t i = (int64_t)((lx + 1.0) * 0.5 * (double)lim);
            const int64_t j = (int64_t)((ly + 1.0) * 0.5 * (double)lim);
            const NodeId n{ f, (uint32_t)lv,
                            (uint32_t)std::min<int64_t>(std::max<int64_t>(i, 0), (int64_t)lim - 1),
                            (uint32_t)std::min<int64_t>(std::max<int64_t>(j, 0), (int64_t)lim - 1) };
            if (idx.find(nodeKey(n)) != idx.end()) return lv;
        }
        return -1;
    };
    // Una frontera esta "al alcance" de la regla si el nodo GRUESO que la forma tiene error en
    // pantalla > errorPx/reach — es la misma puerta que usa el selector, evaluada donde la linea de
    // vista cruza la frontera. Fuera de ella la regla no promete nada (y los anillos siguen, mas
    // lejos y mas pequeños en pantalla).
    struct Jump { double distM, depthM, jumpM; int from, to; bool inReach; };
    auto walk = [&](const std::vector<NodeId>& sel, std::vector<Jump>& out) {
        out.clear();
        int prev = -1;
        for (double x = 5.0; x < 20000.0; x *= 1.02) {
            const glm::dvec3 d = glm::normalize(up0 + t1 * (x / R));
            const int lv = leafAt(sel, d);
            if (prev >= 0 && lv >= 0 && lv != prev) {
                const double tf = nodeTexelM(NodeId{ PlanetFace::FRONT, (uint32_t)std::max(lv, prev), 0, 0 }, R);
                const double tc = nodeTexelM(NodeId{ PlanetFace::FRONT, (uint32_t)std::min(lv, prev), 0, 0 }, R);
                const double depth = waterLv - (double)floorAt(coast, d);
                const double distM = glm::length(pc + d * (R + waterLv - depth) - cam);
                const bool inReach = tc / (distM * radPerPx) * reach > TERRAIN_NODE_ERROR_PX;
                out.push_back(Jump{ x, depth, nodeMissingReliefM(tf, tc), prev, lv, inReach });
            }
            prev = lv;
        }
    };
    std::vector<Jump> jBase, jOn;
    walk(base, jBase);
    walk(on, jOn);
    auto report = [&](const char* tag, const std::vector<Jump>& js, double* worstIn, double* farBad) {
        *worstIn = 0.0; *farBad = 0.0;
        std::printf("    %s: frontera a (m)   nivel   profundidad   salto   salto/profundidad\n", tag);
        for (const Jump& j : js) {
            const double ratio = j.jumpM / std::max(j.depthM, 1.0);
            std::printf("      %14.0f   %2d->%2d   %8.1f m   %6.2f m   %.3f%s\n",
                        j.distM, j.from, j.to, j.depthM, j.jumpM, ratio, j.inReach ? "" : "   (fuera del alcance)");
            if (j.inReach) *worstIn = std::max(*worstIn, ratio);
            if (ratio > frac) *farBad = (*farBad == 0.0) ? j.distM : std::min(*farBad, j.distM);
        }
    };
    double worstBaseIn, worstOnIn, nearBadBase, nearBadOn;
    report("SIN regla", jBase, &worstBaseIn, &nearBadBase);
    report("CON regla", jOn,   &worstOnIn,   &nearBadOn);
    std::printf("    peor salto/profundidad al alcance: sin regla %.3f · con regla %.3f (presupuesto %.2f)\n",
                worstBaseIn, worstOnIn, frac);
    std::printf("    el anillo mas cercano que pasa del presupuesto: sin regla a %.0f m · con regla a %.0f m (x%.2f)\n",
                nearBadBase, nearBadOn, nearBadOn / std::max(nearBadBase, 1.0));
    CHECK(splitsOn > 0, "el agua somera pide particiones que la pantalla no pedia");
    CHECK(worstBaseIn > frac, "CONTRAPRUEBA: sin la regla, al alcance, las fronteras de nivel SI saltan mas de lo que la profundidad tolera");
    CHECK(worstOnIn <= frac + 1e-9, "con la regla, al alcance, ninguna frontera salta mas de 0,25 x profundidad");
    CHECK(nearBadOn >= nearBadBase * 1.5, "el anillo que queda se va al menos 1,5 veces mas lejos (el alcance es 2, medido en la hoja)");
    CHECK(on.size() > base.size(), "y cuesta nodos (los que ahora son finos bajo el agua)");

    // ── (2) EL COSTE, MEDIDO POR ALCANCE ────────────────────────────────────────────────────────
    // El alcance es lo que fija el coste, y crece como el CUADRADO (es un area de nodos finos
    // alrededor de la camara). Se mide para elegirlo: con 4 la regla pedia +251 % (494 -> 1736
    // nodos, mas que todo el presupuesto de la pantalla); la unica frontera que pasaba del
    // presupuesto era la 14->13 a 6 km (0,42), y para llevarla a 12 km basta con 2 (+80 %).
    std::printf("    alcance   nodos   +%%   particiones del agua\n");
    for (double rch : { 1.0, 2.0, 3.0, 4.0 }) {
        size_t sp = 0;
        const auto v = selectR(coast, waterLv, frac, rch, &sp);
        std::printf("    %7.0f   %5zu   %+4.0f %%   %zu%s\n", rch, v.size(),
                    100.0 * ((double)v.size() / (double)base.size() - 1.0), sp,
                    rch == reach ? "   <- el que corre" : "");
    }
    CHECK(on.size() <= base.size() * 2, "con el alcance que corre, la regla no llega a doblar la demanda en la costa");

    // ── (3) CONTRAPRUEBAS ────────────────────────────────────────────────────────────────────────
    std::printf("    CONTRAPRUEBA sin agua: %zu nodos, %zu particiones del agua (debe ser %zu y 0)\n",
                dry.size(), splitsDry, base.size());
    CHECK(dry == base && splitsDry == 0, "sin agua, la regla no cambia NADA: mismo conjunto, mismos nodos");
    std::printf("    CONTRAPRUEBA mar hondo (-2000 m): %zu nodos, %zu particiones del agua (mismo nivel de agua sin regla %zu)\n",
                dp.size(), splitsDeep, dpB.size());
    CHECK(dp == dpB && splitsDeep == 0, "con el fondo a -2000 m el presupuesto es 500 m: la regla no pide nada");
}

// Registro: el grupo sale de la carpeta de este archivo (ver Common/TestCommon.h).
HARUKA_REGISTER_TESTS(
    { "terrain_node_lattice", test_terrain_node_lattice },
    { "terrain_node_scale", test_terrain_node_scale },
    { "terrain_node_content", test_terrain_node_content },
    { "terrain_node_select", test_terrain_node_select },
    { "terrain_node_frustum", test_terrain_node_frustum },
    { "terrain_node_frustum_corners", test_terrain_node_frustum_corners },
    { "terrain_node_face_seam", test_terrain_node_face_seam },
    { "terrain_node_face_seam_gap", test_terrain_node_face_seam_gap },
    { "terrain_node_horizon_cull", test_terrain_node_horizon_cull },
    { "terrain_node_split_uses_elevation", test_terrain_node_split_uses_elevation },
    { "terrain_node_morph_no_pop", test_terrain_node_morph_no_pop },
    { "terrain_node_stride_per_node", test_terrain_node_stride_per_node },
    { "terrain_node_stride_pop", test_terrain_node_stride_pop },
    { "terrain_node_stride_seams", test_terrain_node_stride_seams },
    { "terrain_node_ancestor_disparity", test_terrain_node_ancestor_disparity },
    { "terrain_node_budget_starvation", test_terrain_node_budget_starvation },
    { "terrain_node_walk_shimmer", test_terrain_node_walk_shimmer },
    { "terrain_node_ucenter_jitter", test_terrain_node_ucenter_jitter },
    { "terrain_node_level_balance", test_terrain_node_level_balance },
    { "terrain_prop_anchor_mismatch", test_terrain_prop_anchor_mismatch },
    { "terrain_node_orbit_coverage", test_terrain_node_orbit_coverage },
    { "terrain_node_range_published", test_terrain_node_range_published },
    { "terrain_node_demand_with_range", test_terrain_node_demand_with_range },
    { "terrain_node_inherited_range_flicker", test_terrain_node_inherited_range_flicker },
    { "terrain_node_spike_hunt", test_terrain_node_spike_hunt },
    { "terrain_node_finer_neighbour_step", test_terrain_node_finer_neighbour_step },
    { "terrain_node_edge_audit_all", test_terrain_node_edge_audit_all },
    { "terrain_node_shallow_split", test_terrain_node_shallow_split },
    { "terrain_node_stride_ramp_continuity", test_terrain_node_stride_ramp_continuity },
    { "terrain_node_distance_morph_per_vertex", test_terrain_node_distance_morph_per_vertex },
    { "terrain_node_angle_independence", test_terrain_node_angle_independence },
    { "terrain_backup_bake_cost", test_terrain_backup_bake_cost },
    { "terrain_node_as_heightfield", test_terrain_node_as_heightfield },
    { "terrain_render_vs_collision", test_terrain_render_vs_collision },
    { "terrain_node_range", test_terrain_node_range },
    { "terrain_node_pool", test_terrain_node_pool },
    { "terrain_node_pool_reuse", test_terrain_node_pool_reuse },
    { "terrain_node_pool_chain", test_terrain_node_pool_chain },
    { "terrain_node_subrect_mapping", test_terrain_node_subrect_mapping },
    { "terrain_node_fallback_footprint", test_terrain_node_fallback_footprint },
    { "terrain_node_overlap_on_turn", test_terrain_node_overlap_on_turn },
    { "terrain_node_normal_matches_geometry", test_terrain_node_normal_matches_geometry },
    { "terrain_node_radial_noise_shift", test_terrain_node_radial_noise_shift },
    { "terrain_node_octave_cut_nyquist", test_terrain_node_octave_cut_nyquist },
    { "terrain_node_quad_diagonal_bias", test_terrain_node_quad_diagonal_bias },
    { "terrain_node_float_dir_quantisation", test_terrain_node_float_dir_quantisation },
    { "terrain_collision_walkability", test_terrain_collision_walkability },
    { "terrain_ring_anchor_drift", test_terrain_ring_anchor_drift },
    { "terrain_render_vs_reference_cut", test_terrain_render_vs_reference_cut },
    { "terrain_ring_sample_lateral_shift", test_terrain_ring_sample_lateral_shift },
    { "terrain_node_stride_history_loss", test_terrain_node_stride_history_loss },
    { "terrain_node_winding", test_terrain_node_winding },
    { "terrain_node_stitch", test_terrain_node_stitch },
    { "terrain_node_neighbours", test_terrain_node_neighbours },
    { "terrain_collision_observer_dependence", test_terrain_collision_observer_dependence },
    { "terrain_collision_client_agreement", test_terrain_collision_client_agreement },
    { "terrain_collision_refine_cost", test_terrain_collision_refine_cost },
    { "terrain_ground_query_cost", test_terrain_ground_query_cost });
