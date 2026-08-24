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
#include "test_common.h"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>   // perspective/lookAt: la matriz es la verdad de referencia
                                            // del test de esquinas del frustum
#include <cstring>
#include <vector>
#include <glm/glm.hpp>

#include "core/terrain/terrain_node.h"
#include "core/terrain/terrain_node_pool.h"

using namespace Haruka::Terrain;
using Haruka::PlanetFace;

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
// Reconstruye la dirección de un punto como lo hace HOY el clipmap: proyectándola sobre un marco
// tangente anclado y rehaciéndola en FLOAT (`ringSample` / `clipmap.tese` hacen exactamente esto).
// Es el camino real que produce los 0,9302 m de separación que mide `clipmap_dir_parity`.
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
    // fina de `terrain_detail.h` (λ 4,5 m), y si el nodo no puede representarla, el quadtree no
    // arregla el "se ve basto" — solo mueve el problema.
    const NodeId finest{ PlanetFace::FRONT, TERRAIN_NODE_MAX_LEVEL, 0, 0 };
    const double finestTexel = nodeTexelM(finest, R);
    std::printf("    texel mas fino: %.3f m  (la octava mas fina es lambda 4.5 m -> hacen falta <2.25 m)\n",
                finestTexel);
    CHECK(finestTexel < 2.25, "el nivel mas fino resuelve la octava mas fina del terreno");

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
            const float hD = Haruka::Planet::terrainDetail(d, R, triM);                  // exacta
            const float hF = Haruka::Planet::terrainDetail(glm::vec3(d), (float)R, triM); // por vec3
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
            if (n.level >= TERRAIN_NODE_MAX_LEVEL) ++atMaxLevel; else ++overBudget;
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
        // Se piden 10 nodos con presupuesto de 3: solo 3 deben encolarse.
        for (uint32_t i = 0; i < 10; ++i) pool.request(NodeId{ PlanetFace::FRONT, 4, i, 0 });
        std::printf("    presupuesto: 10 pedidos con tope 3 -> %zu encolados\n", pool.takePending().size());
        CHECK(pool.takePending().size() == 3, "el presupuesto de generacion por frame se respeta");

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
        worstStitched = std::max(worstStitched, glm::length(sew - onLine) * R);
        (void)none;
    }
    std::printf("    vertice impar del borde vs la recta del vecino grueso:\n");
    std::printf("      SIN coser: %.4f m de separacion  <- esto es la grieta\n", worstRaw);
    std::printf("      COSIDO:    %.3e m\n", worstStitched);
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
        const double t = (double)(v - b) / (double)(nx - b);
        const glm::dvec3 onLine = g0 + (g1 - g0) * t;
        worst2 = std::max(worst2, glm::length(nodeStitchedDir(fine, 0, v, coarser2) - onLine) * R);
    }
    std::printf("    salto de DOS niveles (stride 4): separacion peor %.3e m\n", worst2);
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
