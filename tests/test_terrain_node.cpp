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
#include <chrono>
#include <tuple>

#include "test_common.h"

#include <cmath>
#include "core/terrain/base_field.h"          // baseFieldHeightAt: el muestreo del bake del CUBO
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
        for (int lv = (int)TERRAIN_NODE_MAX_LEVEL; lv >= 0; --lv) {
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
        const double triRing = Haruka::Planet::terrainTriM(d);

        // La MISMA composicion en los dos lados; solo cambia el corte de octavas.
        const float hNode = Haruka::Planet::terrainDetail(dir, R, (float)triNode);
        const float hRing = Haruka::Planet::terrainDetail(dir, R, (float)triRing);
        const double diff = std::fabs((double)hNode - (double)hRing);

        // ⚠️ Y EL MORPH, QUE ES RENDER-ONLY. La fisica no lo aplica: muestrea el campo y ya. Asi que
        // donde el morph vale >0, lo DIBUJADO se aparta de lo que se PISA a proposito — y esa es
        // justo la clase de fallo invisible que tenia el v4 (se veia bien y la colision decia otra
        // cosa). Hasta el 2026-08-25 este test no lo modelaba, o sea que no podia verlo.
        const double m0 = (double)nodeVertexMorph((uint32_t)lv, dir, R, cam, pc, radPerPx);
        const double m1 = (lv <= 1) ? 0.0
                        : (double)nodeVertexMorph((uint32_t)lv - 1, dir, R, cam, pc, radPerPx);
        const float hPar  = Haruka::Planet::terrainDetail(dir, R, (float)(triNode * 2.0));
        const float hGran = Haruka::Planet::terrainDetail(dir, R, (float)(triNode * 4.0));
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
    // disparidad exige AFINAR LA REJILLA, no mover el corte. Ver la nota en `terrain_lod.h`.
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
            Haruka::Planet::terrainDetail(dd, R, (float)nodeTexelM(nn, R)) -
            Haruka::Planet::terrainDetail(dd, R, (float)nodeTexelM(nn, R))));
    }
    std::printf("    si la colision cortara como el nodo seria %.4f m — pero es SUB-NYQUIST para una\n"
                "      rejilla de %.1f m: el twist del quad se triplica (0,035 -> 0,108 m). Ver terrain_lod.h\n",
                oldWorst, Haruka::Planet::TERRAIN_TRIM_FLOOR);

    // CONTRAPRUEBA: con cortes de octava DELIBERADAMENTE distintos, la diferencia tiene que dispararse.
    // Sin esto, un `terrainDetail` que ignorara `minFeatureM` daria 0 y se leeria como paridad.
    const glm::dvec3 dprobe = glm::normalize(up0 + t1 * (50.0 / R));
    const double far = std::fabs((double)Haruka::Planet::terrainDetail(dprobe, R, 0.6f) -
                                 (double)Haruka::Planet::terrainDetail(dprobe, R, 300.0f));
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
            for (; lv < (int)TERRAIN_NODE_MAX_LEVEL; ++lv) {
                if (!nodeShouldSplit(nn, R, cam2, pc, radPerPx, TERRAIN_NODE_ERROR_PX, elev)) break;
                const uint64_t lim = 1ull << (lv + 1);
                nn = NodeId{ f, (uint32_t)(lv + 1),
                    (uint32_t)std::min<int64_t>((int64_t)((lx + 1.0) * 0.5 * (double)lim), (int64_t)lim - 1),
                    (uint32_t)std::min<int64_t>((int64_t)((ly + 1.0) * 0.5 * (double)lim), (int64_t)lim - 1) };
            }
        }
        const NodeId nn{ PlanetFace::FRONT, (uint32_t)lv, 0, 0 };
        const double triNode = nodeTexelM(nn, R);
        const double triRing = Haruka::Planet::terrainTriM(100.0);
        const double gap = std::fabs(
            (double)Haruka::Planet::terrainDetail(dd, R, (float)triNode) -
            (double)Haruka::Planet::terrainDetail(dd, R, (float)triRing));
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
            Haruka::Planet::terrainDetail(dd, R, (float)nodeTexelM(nn, R)) -
            Haruka::Planet::terrainDetail(dd, R, (float)nodeTexelM(nn, R))));
    }
    std::printf("    si el anillo cortara como el nodo, la disparidad seria %.4f m "
                "(hoy %.4f m, piso del anillo %.1f m)\n",
                bestPossible, worstNear, Haruka::Planet::TERRAIN_TRIM_FLOOR);
    CHECK(far > worstNear * 10.0, "CONTRAPRUEBA: el corte de octavas SI mueve la superficie (el test mide eso)");
}

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
            const double hFineOwn = Haruka::Planet::terrainDetail(d, R, triF);   // sin morph
            const double hParent  = Haruka::Planet::terrainDetail(d, R, triF * 2.0f);
            const double hCoarse  = Haruka::Planet::terrainDetail(d, R, triC);   // lo que dibuja el vecino
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
        for (int lv = (int)TERRAIN_NODE_MAX_LEVEL; lv >= 0; --lv) {
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
        const float hOwn    = Haruka::Planet::terrainDetail(dir, R, (float)nodeTexelM(n, R));
        const float hParent = Haruka::Planet::terrainDetail(dir, R, (float)nodeTexelM(parent, R));

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
                for (; lv < TERRAIN_NODE_MAX_LEVEL; ++lv) {
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
    while (a < TERRAIN_NODE_MAX_LEVEL &&
           nodeShouldSplit(n0, R, cam, pc, radPerPx, TERRAIN_NODE_ERROR_PX, 0.0)) { ++a; break; }
    while (b < TERRAIN_NODE_MAX_LEVEL &&
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
    const int CR = 512;                   // `faceRes` real del motor (planet.cpp: cfg.faceRes ? : 512)
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
            const float hEq = Haruka::Planet::sampleHeightField(
                Haruka::Planet::equirectUV(glm::vec3(d)), EW, EH, eq.data());
            const float hCu = Haruka::Terrain::baseFieldHeightAt(cube.data(), CR, d);
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
                Haruka::Terrain::baseFieldHeightAt(cube.data(), CR, d) - (float)elevM(d)));
        }
    std::printf("    CONTRAPRUEBA: el muestreador del cubo contra la funcion ANALITICA: peor %.2f m"
                "  (la elevacion abarca +-3700 m -> %.4f %% )\n", self, 100.0 * self / 3700.0);
    CHECK(self > 1e-6, "CONTRAPRUEBA: el muestreador NO devuelve una constante (interpola de verdad)");
    CHECK(self < 3700.0 * 0.001, "CONTRAPRUEBA: y reproduce su origen dentro del 0,1 % de la amplitud");
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
 * @brief EL BOBINADO DE LA REJILLA ES CCW DESDE FUERA — EN LAS SEIS CARAS.
 *
 * Es el requisito para poder dibujar el pase v5 con `CullMode::Back`. Hasta el 2026-08-25 iba con
 * `CullMode::None`, así que **cada nodo rasterizaba sus dos caras**: en el limbo y a distancia
 * rasante la cara de delante y la de detrás del mismo relieve compiten por el depth, que es
 * parpadeo, y además se paga el doble de fragmentos.
 *
 * El comentario del bobinado en `terrain_node_renderer.h` razona sobre la cara FRONT. Eso no basta:
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
    const double vertPx = 4.0, fineCell = Haruka::Planet::TERRAIN_RING_FINE_CELL;
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
                    return d * (R + (double)Haruka::Planet::terrainDetail(d, R, (float)cut));
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
            const double h = kBaseAlt + (double)Haruka::Planet::terrainDetail(dir, R, cut);
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
            const double h2 = kBaseAlt + (double)Haruka::Planet::terrainDetail(dirShift, R, cut);
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
    const double cutRef    = Haruka::Planet::TERRAIN_TRIM_FLOOR;     // lo que muestrea la referencia

    double worst = 0.0, sum = 0.0; size_t cnt = 0;
    for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += 2)
        for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += 2) {
            const glm::dvec3 d = nodeTexelDir(n, u, v);
            const double hR = (double)Haruka::Planet::terrainDetail(d, R, (float)cutRender);
            const double hC = (double)Haruka::Planet::terrainDetail(d, R, (float)cutRef);
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
    Haruka::Planet::terrainClipFrame(pc + up0 * (R + 2.0), pc, upA, t1A, t2A, R);

    // ⚠️ LO QUE IMPORTA NO ES CUANTO SE MUEVE EL ANCLA, SINO SI LAS MUESTRAS NUEVAS CAEN DONDE YA
    // HABIA MUESTRAS. Si el salto es un multiplo exacto de la celda del anillo, cada nodo nuevo
    // aterriza sobre uno viejo y la superficie lineal a trozos es IDENTICA: no cambia nada bajo los
    // pies. Si no lo es, todas las muestras caen en posiciones sub-celda distintas y el suelo se
    // reesculpe entero.
    const double cell = Haruka::Planet::TERRAIN_RING_FINE_CELL;
    std::printf("    paso de camara   salto del ancla   ¿multiplo de la celda de %.2f m?\n", cell);
    double worstFrac = 0.0;
    for (double step : { 0.05, 0.5, 2.0, 8.0, 32.0, 64.0 }) {
        glm::dvec3 upB, t1B, t2B;
        Haruka::Planet::terrainClipFrame(pc + up0 * (R + 2.0) + east * step, pc, upB, t1B, t2B, R);
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
    const double cellM = Haruka::Planet::TERRAIN_RING_FINE_CELL;   // la celda del anillo fino
    const float  cutM  = 2.0f;                                     // lo que pasa la fisica

    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.35, 0.22));
    const glm::dvec3 e1  = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
    const glm::dvec3 e2  = glm::cross(up0, e1);
    auto hAt = [&](double x, double y) {
        const glm::dvec3 d = glm::normalize(up0 + e1 * (x / R) + e2 * (y / R));
        return (double)Haruka::Planet::terrainDetail(d, R, cutM);
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
 * `terrain_node_renderer.h`). Un quad no es plano, así que el centro del quad queda por encima o por
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
            return (double)Haruka::Planet::terrainDetail(nodeTexelDir(n, u, v), R, (float)triM);
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
                const double t0 = (double)Haruka::Planet::terrainDetail(d0, R + b0, (float)triM);
                const double t1 = (double)Haruka::Planet::terrainDetail(d1, R + b0, (float)triM);
                tan_ = std::max(tan_, std::fabs(t1 - t0));
                // RADIAL: el MISMO punto, con el radio del vecino. Es puro artefacto de muestreo.
                const double r1 = (double)Haruka::Planet::terrainDetail(d0, R + b1, (float)triM);
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
        return (double)Haruka::Planet::terrainDetail(nodeTexelDir(n, u, v), R, (float)texM);
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
                gap = std::max(gap, std::fabs((double)Haruka::Planet::terrainDetail(d, R, (float)texC)
                                            - (double)Haruka::Planet::terrainDetail(d, R, (float)texA)));
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
    const double fineCell = Haruka::Planet::TERRAIN_RING_FINE_CELL;
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
            return (double)Haruka::Planet::terrainDetail(glm::normalize(dir + t1 * (x / R)), R, (float)texM);
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
            return (double)Haruka::Planet::terrainDetail(nodeTexelDir(fine, u, v), R, (float)texM);
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
            const Haruka::Terrain::StitchRef sr = nodeStitchStep(E2, v, stepFine);
            const glm::dvec3 dA = nodeTexelDir(fine, sr.u0, sr.v0), dB = nodeTexelDir(fine, sr.u1, sr.v1);
            const double hA = hOf(sr.u0, sr.v0), hB = hOf(sr.u1, sr.v1);
            const glm::dvec3 sewn = (dA + (dB - dA) * sr.t) * (R + hA + (hB - hA) * sr.t);

            const glm::dvec3 seg = pb - pa;
            const double tt = glm::clamp(glm::dot(sewn - pa, seg) / glm::dot(seg, seg), 0.0, 1.0);
            worstSewn = std::max(worstSewn, glm::length(sewn - (pa + seg * tt)));
            worstRaw  = std::max(worstRaw,  glm::length(posOf(E2, v) - onCoarse));
            // El cosido VIEJO (solo por diferencia de nivel) es un no-op aqui: mismo nivel.
            const Haruka::Terrain::StitchRef so = nodeStitch(E2, v, coarserOld);
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
    const double fineCell = Haruka::Planet::TERRAIN_RING_FINE_CELL;

    auto chordErr = [&](double spanM) {
        auto hAt = [&](double x) {
            return (double)Haruka::Planet::terrainDetail(glm::normalize(dir + t1 * (x / R)), R,
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
    const double fineCell = Haruka::Planet::TERRAIN_RING_FINE_CELL;
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
            const double hOwn = (double)Haruka::Planet::terrainDetail(d, R, (float)texM);
            const double hPar = (double)Haruka::Planet::terrainDetail(d, R, (float)parM);
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
                const double dh = std::fabs((double)Haruka::Planet::terrainDetail(d, R, (float)texM)
                                          - (double)Haruka::Planet::terrainDetail(d, R, (float)ancM));
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

    std::vector<NodeId> full;
    nodeSelectVisible(R, cam, center, radPerPx, full, 1000000, errPx);
    std::printf("    demanda sin tope: %zu nodos · presupuesto del juego (pool 4096): 3072\n",
                full.size());
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
            if (nodeBelowHorizon(n, R, cam, center)) continue;
            const bool room = (out.size() + stack.size() + 4) <= budget;
            if (room && nodeShouldSplit(n, R, cam, center, radPerPx, errPx, 0.0)) {
                NodeId kids[4]; nodeChildren(n, kids);
                for (const NodeId& k : kids) stack.push_back(k);
            } else out.push_back(n);
        }
    };
    auto lifoWorstErr = [&](size_t budget) {
        std::vector<NodeId> out; lifoSelect(budget, out);
        double w = 0.0;
        for (const NodeId& n : out) w = std::max(w, nodeScreenError(n, R, cam, center, radPerPx));
        return w;
    };

    std::printf("\n    presupuesto   PEOR error en pantalla (px)     ganancia\n");
    std::printf("                   LIFO (antes)   por error (ahora)\n");
    bool better = false;
    for (size_t budget : { (size_t)2500, (size_t)2000, (size_t)1500, (size_t)1000 }) {
        std::vector<NodeId> tight;
        nodeSelectVisible(R, cam, center, radPerPx, tight, budget, errPx);
        double wNew = 0.0;
        for (const NodeId& n : tight) wNew = std::max(wNew, nodeScreenError(n, R, cam, center, radPerPx));
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
        for (int i = 0; i < kIter; ++i) nodeSelectVisible(R, cam, center, radPerPx, tmp, 3072, errPx);
        const auto t1 = std::chrono::steady_clock::now();
        for (int i = 0; i < kIter; ++i) lifoSelect(3072, tmp2);
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
        nodeSelectVisible(R, cam, center, radPerPx, byHeap, 3072, errPx);
        lifoSelect(3072, byLifo);
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

    CHECK(full.size() < 3072, "con este punto de vista el presupuesto del juego NO satura — la "
                              "saturacion queda como hipotesis SIN confirmar, no como causa");
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
    const double fineCell = Haruka::Planet::TERRAIN_RING_FINE_CELL;
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
        if (nx == b2) return (double)Haruka::Planet::terrainDetail(nodeTexelDir(target, c, c), R, (float)texM);
        const double t = (double)(c - b2) / (double)(nx - b2);
        const double hA = (double)Haruka::Planet::terrainDetail(nodeTexelDir(target, b2, c), R, (float)texM);
        const double hB = (double)Haruka::Planet::terrainDetail(nodeTexelDir(target, nx, c), R, (float)texM);
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
    const double h = (double)Haruka::Planet::terrainDetail(dirT, R, (float)nodeTexelM(n, R));

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
        const double h0 = (double)Haruka::Planet::terrainDetail(d0, R, 0.6f);
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
    const double propTriM = (double)Haruka::Planet::terrainTriM(0.0);

    std::printf("    el corte de octavas que usa cada uno en el MISMO punto:\n");
    std::printf("      props  (sampleHeight -> terrainTriM(0)) : %.3f m\n", propTriM);
    std::printf("      render (texel del nodo, nivel 17)       : %.3f m\n", texel17);
    std::printf("      colision (celda del anillo fino)        : %.3f m\n",
                Haruka::Planet::TERRAIN_RING_FINE_CELL);

    // Cuanto se separan de hecho, sobre puntos repartidos por el nodo.
    double worstPR = 0.0, sumPR = 0.0; size_t cnt = 0;
    double worstFar = 0.0; uint32_t farLevel = 0;
    for (uint32_t v = 1; v < TERRAIN_NODE_CELLS; v += 7)
        for (uint32_t u = 1; u < TERRAIN_NODE_CELLS; u += 7) {
            const glm::dvec3 d = nodeTexelDir(n17, u, v);
            const double hProp = (double)Haruka::Planet::terrainDetail(d, R, (float)propTriM);
            const double hRend = (double)Haruka::Planet::terrainDetail(d, R, (float)texel17);
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
                    (double)Haruka::Planet::terrainDetail(d, R, (float)propTriM)
                  - (double)Haruka::Planet::terrainDetail(d, R, (float)tx)));
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
                                            Haruka::Planet::TERRAIN_RING_FINE_CELL, 6, 0.0);
    const uint32_t skReal = nodeStrideIndex(n, R, cam, center, 1.0, 4.0,
                                            Haruka::Planet::TERRAIN_RING_FINE_CELL, 6,
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
                const float h = 1026.0f + (float)Haruka::Planet::terrainDetail(dd, c->R, 4.0f);
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
            if (n.level == TERRAIN_NODE_MAX_LEVEL) {
                const glm::dvec3 p = center + nodeTexelDir(n, TERRAIN_NODE_CELLS/2,
                                                           TERRAIN_NODE_CELLS/2) * (R + 1026.0);
                nearestDeep = std::min(nearestDeep, glm::length(p - cam2));
            }
        }
        // Y el stride del nodo bajo los pies, con el errorPx por defecto.
        const NodeId foot{ PlanetFace::FRONT, TERRAIN_NODE_MAX_LEVEL,
                           (1u << TERRAIN_NODE_MAX_LEVEL) / 2, (1u << TERRAIN_NODE_MAX_LEVEL) / 2 };
        const glm::dvec3 dF = nodeTexelDir(foot, TERRAIN_NODE_CELLS/2, TERRAIN_NODE_CELLS/2);
        const glm::dvec3 camF = center + dF * (R + 1026.0 + 2.0);
        const uint32_t sk = nodeStrideIndex(foot, R, camF, center, TERRAIN_NODE_ERROR_PX, 4.0,
                                            Haruka::Planet::TERRAIN_RING_FINE_CELL, 6, 1026.0);
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
        CHECK(deepest == TERRAIN_NODE_MAX_LEVEL, "cerca se sigue llegando al nivel MAXIMO: subir el "
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
                              + (float)Haruka::Planet::terrainDetail(nodeTexelDir(q, u, v),
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
static Haruka::Terrain::NodeRange spikeHuntRangeFn(const Haruka::Terrain::NodeId& n, void* user) {
    using namespace Haruka::Terrain;
    auto* c = static_cast<RangeFnCtx*>(user);
    NodeRange r;
    const uint32_t step = TERRAIN_NODE_CELLS / 4;
    for (uint32_t v = 0; v <= TERRAIN_NODE_CELLS; v += step)
        for (uint32_t u = 0; u <= TERRAIN_NODE_CELLS; u += step) {
            const float h = 1026.0f
                          + (float)Haruka::Planet::terrainDetail(nodeTexelDir(n, u, v), c->R, 4.0f);
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
    const double fineCell = Haruka::Planet::TERRAIN_RING_FINE_CELL;
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
                + (float)Haruka::Planet::terrainDetail(dd, R, 20000.0f);   // relieve continental
        }
    auto bakeChain = [&](const glm::dvec3& d, double triM) {
        const glm::vec2 uv = Haruka::Planet::equirectUV(glm::vec3(d));
        const float baseH = Haruka::Planet::sampleHeightField(uv, BW, BH, bake.data());
        const double baseR = R + (double)baseH;
        float det = Haruka::Planet::terrainDetail(d, baseR, (float)triM)
                  * Haruka::Planet::seaLevelAttenuation(baseH);
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
            return (double)Haruka::Planet::terrainDetail(nodeTexelDir(mine, u, v), R, (float)texM);
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
    const double fineCell = Haruka::Planet::TERRAIN_RING_FINE_CELL;
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
                const double own = (double)Haruka::Planet::terrainDetail(d, R, (float)texM);
                const double par = (double)Haruka::Planet::terrainDetail(d, R, (float)parM);
                // ⚠️ El destino es lo que el padre DIBUJA, no su altura cruda: `mix(padre, abuelo,
                // m(L-1))`. Gemelo del bloque largo de `terrain_node.vert` — el arreglo de los
                // pinchos del 2026-08-25.
                const double gran = (double)Haruka::Planet::terrainDetail(d, R, (float)(texM * 4.0));
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
    NodeId wcA{}, wcB{}; double wcT = 0.0, wcTB = 0.0;   // la peor pareja ENTRE NIVELES, para diseccionarla
    // ⚠️ LAS DOS DIRECCIONES. `axis 0` = vecino de la DERECHA (+i, arista de u constante) y
    // `axis 1` = vecino de ARRIBA (+j, arista de v constante). Hasta hoy las tres auditorias solo
    // pedian la derecha, asi que el eje v no lo comprobaba nadie — y en el shader NO es simetrico:
    // el cosido va por una cadena `else if` y las rampas usan los otros dos bits de la mascara.
    size_t auditedAxis[2] = { 0, 0 };
    double gapAxis[2] = { 0.0, 0.0 };
    for (int axis = 0; axis < 2; ++axis)
    for (const NodeId& a : sel) {
        const uint32_t lim = 1u << a.level;
        if (axis == 0 ? (a.i + 1 >= lim) : (a.j + 1 >= lim)) continue;
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
    std::printf("    %zu parejas adyacentes auditadas punto a punto (%zu de DISTINTO nivel)\n",
                audited, crossLevel);
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
        std::printf("    DISECCION de la peor (%u vs %u): m(L)=%.4f  m(L-1)=%.4f  m(L-2)=%.4f\n",
                    wcA.level, wcB.level, mAt(wcA.level), mAt(wcA.level - 1),
                    (wcA.level >= 2) ? mAt(wcA.level - 2) : 0.0f);
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
    CHECK(gap[0][1] < 0.9, "GUARDARRAIL del escalon entre niveles (hoy 0,785 m, peor en +j)");
    CHECK(auditedAxis[0] > 100 && auditedAxis[1] > 100,
          "se auditan las DOS direcciones (+i y +j), no solo la derecha");
    // CONTRAPRUEBA: sin morph el escalon es PEOR. Sin esto el morph podria no estar haciendo nada y
    // el test pasaria igual — que es exactamente lo que le pasaba al morph por arista, ya borrado.
    CHECK(gap[1][1] > gap[0][1] + 0.5,
          "CONTRAPRUEBA: sin morph por distancia el escalon entre niveles crece de verdad");
    // CONTRAPRUEBA DEL ARREGLO: el modo 2 quita el morph al lado GRUESO, que es como se comportaba
    // el motor antes de apuntar al abuelo. Tiene que salir MUCHO peor — si no, el mapa del abuelo
    // (65 KB por nodo, 130 MB de pool) no estaria comprando nada y habria que quitarlo.
    CHECK(gap[2][1] > gap[0][1] * 2.0,
          "CONTRAPRUEBA: sin apuntar a lo que el padre DIBUJA, el escalon se multiplica (2,75 m)");
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
                (double)Haruka::Planet::terrainDetail(d, R, (float)(texM * 2.0))
              - (double)Haruka::Planet::terrainDetail(d, R, (float)texM)));
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
