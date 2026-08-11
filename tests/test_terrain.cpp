// ================================================================================================
// Tests de TERRENO puros-CPU (sin GL): cubemap inversa, clima, capa granular.
// ================================================================================================
#include "test_common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "core/terrain/cube_sphere.h"
#include "core/weather_system.h"
#include "core/ground_layer.h"
#include "core/planet/terrain_detail.h"
#include "core/planet/terrain_lod.h"

using namespace Haruka;

// ============================ CLIPMAP: paridad del recorte (el hueco 0-2 km) =====================
// El hueco a los pies del jugador ("vr.4: el clipmap no dibuja donde la malla base se recorta") se
// produce cuando el recorte de `terrain.tese` (inClip) y el dibujo del clipmap dejan de cubrir el
// MISMO cuadrado tangente. Las dos piezas viven en shaders distintos; este test las refleja en CPU
// con la MISMA matemática (marco tangente del plano de la cámara) y comprueba que el borde del
// recorte cae EXACTAMENTE en el borde de la rejilla — si uno se desvía, se abre la costura.
//
// Regla material (§9 de TERRENO.md): el semi-lado del clipmap (m_clipCoverM) lo leen TANTO el
// recorte (uClipCover.x) como la rejilla de vértices (NC celdas × 128 m). El recorte corta en
// |proyección en el banda| ≤ cover; la rejilla llega EXACTAMENTE hasta cover; y el marco funcional
// (up, tanU, tanV) que usa el recorte se reconstruye en GLSL con la misma cuenta con la que la CPU
// llena el ClipParams UBO. Cada una de esas tres piezas va en un fichero distinto; si dos de ellas
// divergen, el borde del clipmap pierde la tapa que le pone la malla y el suelo se agujerea.
//
// Se comprueba: (1) cover == NC·PATCH/2 por calidad, (2) el vértice del borde de la rejilla sigue
// DENTRO del recorte (<= cover), (3) la proyección del vértice del borde de la rejilla al marco de
// la cámara devuelve sus coords locales (el borde del recorte = el borde de la rejilla), (4) el
// marco se reconstruye igual desde "cámara relativa al centro" que desde "uCenter = centro − cámara",
// y (5) el caso polar (cámara sobre el eje) tiene fallback y sigue dando un marco ortonormal.

// ⚠️ Aquí VIVÍA una copia del marco tangente, con el comentario "Reproduce EXACTAMENTE
// planet.cpp:2606-2611". Ese es justo el patrón que hace inútil a un test: auditaba su propia copia,
// así que habría seguido en verde con el motor cambiado debajo. Ahora se llama a `terrainClipFrame`
// (core/planet/terrain_lod.h), que es la MISMA función que llena ClipParams y que construye la malla
// de colisión. Si alguien la toca, estos CHECK se enteran.

namespace {
// Punto de la superficie a una distancia tangente (gx, gy) metros del sub-punto: la MISMA
// proyección que clipmap.tese (dir = normalize(up + tanU·gx/R + tanV·gy/R)).
glm::dvec3 clipGridVertex(const glm::dvec3& up, const glm::dvec3& tu, const glm::dvec3& tv,
                          double R, double gx, double gy) {
    const glm::dvec3 dir = glm::normalize(up + tu * (gx / R) + tv * (gy / R));
    return dir * R;
}
}  // namespace

void test_clipmap_parity() {
    beginTest("clipmap_parity");
    const double R     = 6371000.0;
    const double alts[] = { 1.7, 800.0, 5000.0 };
    const int    NCs[]  = { 31, 47, 63, 95 };
    const double PATCH  = Haruka::Planet::TERRAIN_CLIP_PATCH_M;   // del motor, no un literal del test

    double worstErr = 0.0;
    for (int q = 0; q < 4; ++q) {
        const int    NC    = NCs[q];
        const double cover = NC * PATCH * 0.5;
        CHECK(std::fabs(PATCH - 128.0) < 1e-9,
              "el parche del clipmap sigue siendo de 128 m (si cambia, cover y recorte se mueven)");
        for (double alt : alts) {
            const glm::dvec3 cam = glm::dvec3(0.0, 0.0, R + alt);
            glm::dvec3 up, tu, tv;
            Haruka::Planet::terrainClipFrame(cam, glm::dvec3(0.0), up, tu, tv);   // el marco REAL
            // 4 esquinas (±cover, ±cover) + 4 puntos medios de arista: el borde de la rejilla.
            const double edge[8][2] = {
                { cover,  cover}, { cover, -cover}, {-cover,  cover}, {-cover, -cover},
                { cover,  0.0}, {0.0, cover}, {-cover, 0.0}, {0.0, -cover},
            };
            for (const double (&g)[2] : edge) {
                const glm::dvec3 P   = clipGridVertex(up, tu, tv, R, g[0], g[1]);
                const glm::dvec3 rel = P - cam;
                const double wx = glm::dot(rel, tu);
                const double wy = glm::dot(rel, tv);
                worstErr = std::max(worstErr, std::fabs(wx - g[0]));
                worstErr = std::max(worstErr, std::fabs(wy - g[1]));
                CHECK(std::fabs(wx) <= cover + 1e-3 && std::fabs(wy) <= cover + 1e-3,
                      "vértice del borde de la rejilla sigue DENTRO del recorte (<= cover)");
            }
        }
    }
    std::printf("    rejilla ↔ recorte: error de proyección máximo %.4f m\n", (double)worstErr);
    CHECK(worstErr < 1e-2, "el borde de la rejilla y el del recorte CASAN (no hay hueco entre ambos)");

    const glm::dvec3 planetC(0.0, 0.0, 0.0);
    const glm::dvec3 cam = glm::dvec3(123456.0, -654321.0, R + 400.0);
    glm::dvec3 upA, tuA, tvA;
    Haruka::Planet::terrainClipFrame(cam, planetC, upA, tuA, tvA);
    // El shader no recibe la cámara: recibe `uCenter = centro − cámara` y reconstruye el marco desde
    // ahí. Que las dos vías den el MISMO triedro es lo que evita que el recorte y la rejilla estén
    // rotados uno respecto del otro.
    const glm::dvec3 uCenter = planetC - cam;
    glm::dvec3 upB, tuB, tvB;
    Haruka::Planet::terrainClipFrame(planetC - uCenter, planetC, upB, tuB, tvB);
    CHECK(glm::dot(upA, upB) > 1.0 - 1e-12 && glm::length(tuA - tuB) < 1e-9 &&
          glm::length(tvA - tvB) < 1e-9,
          "marco del recorte == marco del ClipParams UBO (misma cuenta en CPU y GLSL)");

    // ── ANCLAJE A LA RETÍCULA DEL MUNDO ────────────────────────────────────────────────────────
    // Sin anclar, `up` se recalcula desde la posición EXACTA de la cámara y la rejilla entera se
    // desliza con ella: la superficie dibujada se re-muestrea cada frame (el terreno "nada") y la
    // colisión no puede casar con un objetivo móvil. Anclado, `up` salta en escalones del tamaño del
    // quad y entre saltos los vértices están QUIETOS en el mundo. Se comprueba justo eso.
    {
        const glm::dvec3 pc(0.0);
        const double alt = 1.7;
        // Dos cámaras separadas 40 cm — un décimo del quad de 4 m: NO debe cambiar la rejilla.
        const glm::dvec3 camA = glm::normalize(glm::dvec3(0.3, 0.5, 0.81)) * (R + alt);
        glm::dvec3 tanU = glm::normalize(glm::cross(glm::dvec3(0,1,0), glm::normalize(camA)));
        const glm::dvec3 camB = camA + tanU * 0.4;

        glm::dvec3 upFree1, tuFree1, tvFree1, upFree2, tuFree2, tvFree2;
        Haruka::Planet::terrainClipFrame(camA, pc, upFree1, tuFree1, tvFree1);           // sin anclar
        Haruka::Planet::terrainClipFrame(camB, pc, upFree2, tuFree2, tvFree2);
        glm::dvec3 upS1, tuS1, tvS1, upS2, tuS2, tvS2;
        Haruka::Planet::terrainClipFrame(camA, pc, upS1, tuS1, tvS1, R);                 // anclado
        Haruka::Planet::terrainClipFrame(camB, pc, upS2, tuS2, tvS2, R);

        // Sin anclar, moverse 40 cm mueve el origen ~40 cm: la rejilla entera se arrastra.
        const double freeShift = glm::length(upFree2 - upFree1) * R;
        const double snapShift = glm::length(upS2 - upS1) * R;
        std::printf("    anclaje: 40 cm de camara mueven la rejilla %.3f m sin anclar · %.3f m anclada\n",
                    freeShift, snapShift);
        CHECK(freeShift > 0.3, "sin anclar la rejilla SÍ se arrastra con la cámara (es el bug)");
        CHECK(snapShift < 1e-6, "anclada, moverse menos de un quad NO mueve la rejilla");

        // Y el anclaje no puede alejar el centro más de un quad: el clipmap sigue centrado en ti.
        const double off = glm::length(upS1 - glm::normalize(camA)) * R;
        CHECK(off <= Haruka::Planet::TERRAIN_CLIP_QUAD_M,
              "el anclaje desplaza el centro menos de un quad (el clipmap sigue centrado)");
        // Anclado sigue siendo un triedro válido: unitario y ortonormal.
        CHECK(std::fabs(glm::length(upS1) - 1.0) < 1e-12 &&
              std::fabs(glm::dot(upS1, tuS1)) < 1e-12 &&
              glm::length(glm::cross(tuS1, tvS1) - upS1) < 1e-12,
              "el marco anclado sigue siendo ortonormal y dextrógiro");
    }

    // Dextrógiro: `cross(tu,tv) == up`. No es cosmético — fija la orientación de las celdas y con
    // ella el winding de la malla de colisión, que comparte este mismo marco.
    CHECK(glm::length(glm::cross(tuA, tvA) - upA) < 1e-12,
          "triedro dextrógiro (cross(tu,tv)==up): fija el winding de la colisión");

    // Caso POLAR: con la cámara sobre el eje Y el producto vectorial auxiliar es CERO. Normalizarlo
    // sin comprobar daría NaN —no un vector corto— y el marco entero se envenenaría en silencio.
    for (int s = 0; s < 2; ++s) {
        const glm::dvec3 polarCam = (s ? glm::dvec3(0, -R - 10.0, 0) : glm::dvec3(0, R + 10.0, 0));
        glm::dvec3 up, tu, tv;
        Haruka::Planet::terrainClipFrame(polarCam, planetC, up, tu, tv);
        CHECK(std::isfinite(tu.x) && std::isfinite(tv.x) && std::isfinite(up.x),
              "fallback polar: marco FINITO (el eje auxiliar se elige antes de normalizar)");
        CHECK(std::fabs(glm::length(tu) - 1.0) < 1e-12 && std::fabs(glm::length(tv) - 1.0) < 1e-12,
              "fallback polar: marco no degenerado");
        CHECK(std::fabs(glm::dot(tu, tv)) < 1e-12 && std::fabs(glm::dot(up, tu)) < 1e-12 &&
              std::fabs(glm::dot(up, tv)) < 1e-12, "marco polar ortonormal");
    }
}

// ⚠️ AQUÍ ESTABA `detail_triM_parity`, y era una TAUTOLOGÍA. Calculaba `meshTriM` y `clipTriM` con
// la misma expresión escrita dos veces, evaluaba `terrainDetail` con ambas y comprobaba que el
// resultado coincidía. Comparaba f(x) con f(x): imprimía 0.0000 m y NO PODÍA FALLAR — seguiría en
// verde con el motor usando otra fórmula, porque el test nunca llamaba al código del motor.
//
// Y sobre todo medía el sitio equivocado. La disparidad NO está entre dos evaluaciones de la función
// continua: está entre las dos TESELACIONES de esa función. El render dibuja quads (lineales por
// celda) y la física pisa una rejilla por anillos (lineal por celda, con celdas de otro tamaño). La
// diferencia entre esas dos superficies poligonales es lo que el jugador nota, y es lo que miden los
// dos tests que lo sustituyen: `terrain_lod_invariants` (las cifras casan) y `terrain_chord_error`
// (cuánto se separan, en metros).

// TEST: las CIFRAS de LOD son coherentes entre sí y con el hardware.
void test_terrain_lod_invariants() {
    beginTest("terrain_lod_invariants");
    using namespace Haruka::Planet;

    // (1) Ningún tope de teselación puede pasar del máximo del hardware. Pedir más NO da error: el
    // driver recorta en silencio y el shader hace algo distinto de lo que dice. Así estuvo el techo
    // de `ringCap` (128 sobre un máximo de 64) sin que nada lo detectara.
    CHECK(TERRAIN_MESH_TESS_CAP <= TERRAIN_MAX_TESS_GEN_LEVEL,
          "el tope de la malla base cabe en GL_MAX_TESS_GEN_LEVEL (si no, el driver lo recorta mudo)");
    CHECK(TERRAIN_CLIP_TESS_CAP <= TERRAIN_MAX_TESS_GEN_LEVEL,
          "el tope del clipmap cabe en GL_MAX_TESS_GEN_LEVEL");
    CHECK(std::fabs(TERRAIN_MAX_TESS_GEN_LEVEL - 64.0) < 1e-9,
          "el máximo del hardware sigue siendo 64 (silicio, igual en GL/Vulkan/D3D)");

    // (2) EL INVARIANTE QUE SE ROMPIÓ: el piso de `triM` tiene que ser el lado REAL del quad. Con el
    // piso en 2.0 y quads de 4 m, la octava de 4,5 m entraba al 12,5 % sobre vértices separados 4 m
    // — sub-Nyquist, el hervido de §3.1. Derivado del quad, no puede volver a desalinearse.
    CHECK(std::fabs(TERRAIN_CLIP_QUAD_M - TERRAIN_CLIP_PATCH_M / TERRAIN_CLIP_TESS_CAP) < 1e-12,
          "el quad del clipmap es parche/tope, no un literal");
    CHECK(std::fabs(TERRAIN_TRIM_FLOOR - TERRAIN_CLIP_QUAD_M) < 1e-12,
          "el piso de triM ES el lado del quad (Nyquist: no se evalúa lo que el triángulo no lleva)");
    std::printf("    parche %.0f m / tope %.0f → quad %.2f m · piso de triM %.2f m\n",
                TERRAIN_CLIP_PATCH_M, TERRAIN_CLIP_TESS_CAP, TERRAIN_CLIP_QUAD_M, TERRAIN_TRIM_FLOOR);

    // (3) La octava más fina que el piso deja pasar tiene que estar POR ENCIMA de Nyquist: una
    // octava λ solo puede dibujarse si el triángulo mide < λ/2. Es la comprobación que convierte el
    // invariante anterior en algo con sentido físico, no en dos números que casan por casualidad.
    CHECK(Haruka::Planet::octaveWeight(4.5f, (float)TERRAIN_TRIM_FLOOR) <= 0.0f,
          "con el piso actual la octava de 4,5 m NO se evalúa (está bajo Nyquist)");
    CHECK(Haruka::Planet::octaveWeight(22.0f, (float)TERRAIN_TRIM_FLOOR) > 0.99f,
          "la octava de 22 m sí entra entera: el relieve que se camina sigue ahí");

    // (4) `terrainTriM` es la ÚNICA definición: crece con la distancia y se apoya en el piso.
    CHECK(std::fabs(terrainTriM(0.0) - TERRAIN_TRIM_FLOOR) < 1e-6, "al pie, triM = piso");
    CHECK(std::fabs(terrainTriM(1000000.0) - 1000000.0 * TERRAIN_TRIM_SLOPE) < 1e-3,
          "lejos, triM = distancia·pendiente");
    CHECK(terrainTriM(5000.0) > terrainTriM(500.0), "triM crece monótonamente con la distancia");
}

// TEST: la rejilla por anillos de la COLISIÓN (§9 Fase 3), la de verdad — `terrainRingGrid`.
void test_terrain_ring_grid() {
    beginTest("terrain_ring_grid");
    using namespace Haruka::Planet;

    for (double halfR : { 200.0, 2000.0, 200000.0 }) {
        const std::vector<double> xs = terrainRingGrid(halfR);
        CHECK(xs.size() >= 3, "la rejilla tiene al menos una celda a cada lado");

        // MONOTONÍA — el peligro que el propio código señala: los índices de triángulo asumen filas
        // y columnas en orden de coordenada, así que una rejilla no creciente invertiría la
        // orientación de las celdas y la colisión quedaría DEL REVÉS en media malla. Nada lo
        // comprobaba.
        bool mono = true, sym = true;
        for (size_t i = 1; i < xs.size(); ++i) if (!(xs[i] > xs[i-1])) mono = false;
        for (size_t i = 0; i < xs.size(); ++i)
            if (std::fabs(xs[i] + xs[xs.size()-1-i]) > 1e-9) sym = false;
        CHECK(mono, "estrictamente creciente (si no, la colisión sale con las normales invertidas)");
        CHECK(sym,  "simétrica respecto al jugador");
        CHECK(std::fabs(xs[xs.size()/2]) < 1e-12, "el centro (el pie del jugador) es exactamente 0");

        // El paso al pie es el fino; el del borde, el grueso. Y el parche llega hasta donde dice.
        const size_t c = xs.size()/2;
        CHECK(std::fabs((xs[c+1] - xs[c]) - TERRAIN_RING_INNER_STEP) < 1e-9,
              "el paso al pie es TERRAIN_RING_INNER_STEP");
        CHECK(xs.back() < halfR && xs.back() > halfR * 0.5,
              "el parche cubre su radio sin pasarse");

        // ── EL INVARIANTE QUE HACE QUE LA DISPARIDAD SEA CERO ───────────────────────────────────
        //
        // Dentro del bloque uniforme todo nodo tiene que caer en un MÚLTIPLO EXACTO del quad del
        // render. No "parecido": exacto. Es lo que hace que las dos teselaciones compartan vértices y
        // por tanto describan la misma superficie, en vez de dos aproximaciones que se separan por la
        // sagita de la celda. Si alguien cambia `TERRAIN_RING_INNER_STEP`, mete crecimiento dentro del
        // bloque o desalinea el origen, esto lo caza aquí y no en el juego.
        {
            int inBlock = 0; bool onLattice = true, uniform = true;
            for (size_t i = c; i + 1 < xs.size(); ++i) {
                if (xs[i] >= TERRAIN_COLLIDE_UNIFORM_M) break;
                ++inBlock;
                const double q = xs[i] / TERRAIN_CLIP_QUAD_M;
                if (std::fabs(q - std::round(q)) > 1e-9) onLattice = false;
                if (std::fabs((xs[i+1] - xs[i]) - TERRAIN_CLIP_QUAD_M) > 1e-9) uniform = false;
            }
            // Solo tiene sentido cuando el parche es más grande que el propio bloque.
            if (halfR > TERRAIN_COLLIDE_UNIFORM_M * 1.5) {
                CHECK(inBlock > 10, "el bloque uniforme tiene nodos de verdad");
                CHECK(onLattice, "cada nodo del bloque cae en un MULTIPLO EXACTO del quad del render");
                CHECK(uniform,   "y el paso NO crece dentro del bloque (si creciera, habria salto al salir)");
            }
        }

        // El bloque tiene que cubrir hasta donde el jugador puede llegar ANTES de que la malla se
        // reconstruya (deriva de 48 m, `physics_engine.cpp`). Si no lo cubriera, el jugador saldría
        // del radio de disparidad cero caminando y el cero no valdría de nada.
        CHECK(TERRAIN_COLLIDE_UNIFORM_M > 48.0 * 3.0,
              "el bloque uniforme cubre la deriva de reconstruccion (48 m) con margen 3x");

        // Coste acotado: es la razón de ser de los anillos. Una rejilla uniforme de 200 km a 4 m
        // serían 2,5·10^9 vértices; por anillos tienen que caber en cientos de miles.
        //
        // ⚠️ La cota SUBIÓ de 120 k a 300 k, y es un presupuesto GASTADO A PROPÓSITO, no un test
        // relajado para que pasara. Antes la celda crecía ×1,12 hasta 25 m —valores como 10, 16, 25 m
        // que no son múltiplos del quad de 4 m del render—, así que a partir de ~256 m las dos
        // retículas no compartían NINGÚN vértice y las superficies se cruzaban. Ahora la celda es
        // siempre `quad·2^k` (`terrainCollideCellAt`), o sea un subconjunto de la del render, y eso
        // obliga a saltar de 16 a 32 en vez de deslizarse. Medido, rejilla completa de 200 km:
        //
        //     antes:  187×187 =  35 k vért · 69 k tris · sagita peor en la caja 1,05 m
        //     ahora:  505×505 = 255 k vért · 508 k tris · sagita peor en la caja 0,68 m
        //
        // El número que justifica el gasto es el último: la separación entre lo que se pisa y lo que
        // se dibuja bajó a la mitad, y dentro del bloque uniforme es 0 EXACTO en vez de pequeña.
        CHECK(xs.size() * xs.size() < 300000, "coste acotado (~cientos de miles de vértices)");
        std::printf("    ±%7.0f m → %3zu×%3zu = %6zu vértices · paso %.1f m → %.0f m\n",
                    halfR, xs.size(), xs.size(), xs.size()*xs.size(),
                    xs[c+1]-xs[c], xs[xs.size()-1] - xs[xs.size()-2]);
    }
}

// TEST: LA DISPARIDAD REAL, en metros — lo que se DIBUJA contra lo que se PISA.
//
// Las dos son superficies POLIGONALES de la misma función continua, pero con celdas distintas: el
// render usa quads del clipmap (TERRAIN_CLIP_QUAD_M) y la colisión la rejilla por anillos, cuyo paso
// crece ×TERRAIN_RING_GROWTH con la distancia. Dentro de cada celda ambas son planos, así que se
// separan de la función —y entre sí— por la SAGITA de la celda. Ninguna comparación de la función
// consigo misma puede ver esto: hay que teselar.
void test_terrain_chord_error() {
    beginTest("terrain_chord_error");
    using namespace Haruka::Planet;
    const double R = 6371000.0;

    // Altura de la función continua a un desplazamiento tangente (gx,gz) del punto base.
    auto fCont = [&](const glm::dvec3& up, const glm::dvec3& t1, const glm::dvec3& t2,
                     double gx, double gz, float triM) {
        const glm::dvec3 d = glm::normalize(up + t1 * (gx / R) + t2 * (gz / R));
        return (double)Haruka::Planet::terrainDetail(glm::vec3(d), (float)R, triM);
    };
    // Altura de la superficie POLIGONAL: bilineal de las 4 esquinas de la celda que contiene (gx,gz).
    // Es lo que el rasterizador dibuja dentro del triángulo y lo que Jolt colisiona dentro de la
    // celda — no la función.
    auto fPoly = [&](const glm::dvec3& up, const glm::dvec3& t1, const glm::dvec3& t2,
                     double gx, double gz, double step, float triM) {
        const double x0 = std::floor(gx / step) * step, z0 = std::floor(gz / step) * step;
        const double u = (gx - x0) / step, v = (gz - z0) / step;
        const double h00 = fCont(up,t1,t2, x0,      z0,      triM);
        const double h10 = fCont(up,t1,t2, x0+step, z0,      triM);
        const double h01 = fCont(up,t1,t2, x0,      z0+step, triM);
        const double h11 = fCont(up,t1,t2, x0+step, z0+step, triM);
        const double a = h00 + (h10 - h00) * u, b = h01 + (h11 - h01) * u;
        return a + (b - a) * v;
    };

    // ── (1) EN EL PIE: el suelo que se camina. Aquí el listón del motor es 1 cm — y ahora es 0.
    //
    // ⚠️ CÓMO SE MIDE, que aquí está el peligro. Desde que la colisión comparte la retícula del render
    // (`TERRAIN_RING_INNER_STEP == TERRAIN_CLIP_QUAD_M`), comparar `fPoly(QUAD)` con
    // `fPoly(INNER_STEP)` sería escribir la misma expresión dos veces: la tautología exacta que este
    // fichero denuncia en `detail_triM_parity`. Un cero así no probaría nada y seguiría en verde con la
    // retícula de colisión cambiada debajo.
    //
    // Así que la celda de la COLISIÓN se saca de `terrainRingGrid` —la rejilla que el motor construye
    // de verdad— buscando el intervalo que contiene el punto. Si la rejilla dejara de estar alineada
    // con el quad del render, este cero se rompería, que es justo lo que tiene que pasar.
    const std::vector<double> collideGrid = terrainRingGrid(200000.0);
    // Altura poligonal sobre la celda REAL de la rejilla de colisión que contiene (gx,gz).
    auto fPolyGrid = [&](const glm::dvec3& up, const glm::dvec3& t1, const glm::dvec3& t2,
                         double gx, double gz, float triM) {
        auto cellLo = [&](double v) {
            // Último nodo <= v. `upper_bound` da el primero >, así que el anterior es el buscado.
            auto it = std::upper_bound(collideGrid.begin(), collideGrid.end(), v);
            const size_t k = (size_t)(it - collideGrid.begin());
            return (k == 0) ? 0 : k - 1;
        };
        const size_t ix = cellLo(gx), iz = cellLo(gz);
        if (ix + 1 >= collideGrid.size() || iz + 1 >= collideGrid.size()) return 0.0;
        const double x0 = collideGrid[ix], x1 = collideGrid[ix+1];
        const double z0 = collideGrid[iz], z1 = collideGrid[iz+1];
        const double u = (gx - x0) / (x1 - x0), v = (gz - z0) / (z1 - z0);
        const double h00 = fCont(up,t1,t2, x0, z0, triM), h10 = fCont(up,t1,t2, x1, z0, triM);
        const double h01 = fCont(up,t1,t2, x0, z1, triM), h11 = fCont(up,t1,t2, x1, z1, triM);
        const double a = h00 + (h10 - h00) * u, b = h01 + (h11 - h01) * u;
        return a + (b - a) * v;
    };

    double wRender = 0.0, wPhys = 0.0, wCross = 0.0, sumCross = 0.0; long n = 0;
    for (int s = 0; s < 120; ++s) {
        const double ph = std::acos(1.0 - 2.0 * (s + 0.5) / 120.0), th = 2.39996 * s;
        const glm::dvec3 up(std::cos(th)*std::sin(ph), std::sin(th)*std::sin(ph), std::cos(ph));
        glm::dvec3 t1 = glm::normalize(glm::cross(up, std::fabs(up.z) < 0.9 ? glm::dvec3(0,0,1)
                                                                            : glm::dvec3(1,0,0)));
        const glm::dvec3 t2 = glm::cross(up, t1);
        for (int k = 0; k < 40; ++k) {
            const double gx = ((k*7919)%1000)/1000.0 * 12.0, gz = ((k*104729)%1000)/1000.0 * 12.0;
            const float triM = terrainTriM(0.0);
            const double cont = fCont(up,t1,t2, gx,gz, triM);
            const double rend = fPoly(up,t1,t2, gx,gz, TERRAIN_CLIP_QUAD_M, triM);  // quad del clipmap
            const double phys = fPolyGrid(up,t1,t2, gx,gz, triM);   // celda REAL de la rejilla
            wRender = std::max(wRender, std::fabs(rend - cont));
            wPhys   = std::max(wPhys,   std::fabs(phys - cont));
            wCross  = std::max(wCross,  std::fabs(rend - phys));
            sumCross += std::fabs(rend - phys); ++n;
        }
    }
    std::printf("    en el pie · render(quad %.0f m) vs función: %.4f m · colisión(rejilla real) vs función: %.4f m\n",
                TERRAIN_CLIP_QUAD_M, wRender, wPhys);
    std::printf("    en el pie · RENDER vs COLISIÓN: peor %.6f m · media %.6f m\n", wCross, sumCross/n);
    // EL LISTÓN ES CERO, no "poco". Al compartir la retícula las dos superficies tienen los mismos
    // vértices y la misma bilineal: no son dos aproximaciones parecidas, son la misma superficie. El
    // umbral es de redondeo de double (la celda se localiza con búsqueda binaria sobre la rejilla, así
    // que las coordenadas no salen del mismo cálculo que el quad).
    CHECK(wCross < 1e-9, "a pie, lo que se dibuja y lo que se pisa es la MISMA superficie (0 m)");
    CHECK(sumCross/n < 1e-12, "y la separación típica es exactamente 0");
    CHECK(wRender < 0.25, "la cuerda del quad del render está acotada");
    // Y las dos se separan de la FUNCIÓN por lo mismo, que es la sagita del quad: si una de las dos
    // teselara distinto, este par dejaría de coincidir.
    CHECK(std::fabs(wRender - wPhys) < 1e-9,
          "render y colision se separan de la funcion EXACTAMENTE igual (misma teselacion)");

    // ── (2) POR ANILLO: cómo crece la sagita al alejarse. Es el precio de `TERRAIN_RING_GROWTH`, y
    // hasta ahora no estaba medido en ninguna parte: dentro de la caja del clipmap el render dibuja
    // geometría fina mientras la colisión ya va por celdas de cientos de metros.
    const glm::dvec3 up(0,0,1), t1(1,0,0), t2(0,1,0);
    const std::vector<double> xs = terrainRingGrid(200000.0);
    const size_t c = xs.size() / 2;
    double worstInsideClip = 0.0;
    std::printf("    sagita de la colisión por anillo:\n");
    for (size_t i = c + 1; i + 1 < xs.size(); ++i) {
        const double r = xs[i], step = xs[i+1] - xs[i];
        const float triM = terrainTriM(r);
        double w = 0.0;
        for (int k = 0; k < 24; ++k) {
            const double gx = r + ((k*7919)%1000)/1000.0 * step;
            const double gz = ((k*104729)%1000)/1000.0 * step;
            w = std::max(w, std::fabs(fPoly(up,t1,t2, gx,gz, step, triM)
                                    - fCont(up,t1,t2, gx,gz, triM)));
        }
        if (r <= 1984.0) worstInsideClip = std::max(worstInsideClip, w);   // dentro de la caja (NC=31)
        if ((i - c) % 12 == 1)
            std::printf("      dist %8.0f m · paso %8.1f m · triM %7.1f · sagita %8.2f m\n",
                        r, step, (double)triM, w);
    }
    std::printf("    peor sagita DENTRO de la caja del clipmap (±1984 m): %.2f m\n", worstInsideClip);
    // Dentro de la caja el render dibuja geometría fina, así que aquí la colisión NO puede irse a
    // metros. Llegó a 10,18 m porque el paso crecía desde el pie sin tope; con
    // `TERRAIN_RING_MAX_STEP_IN_BOX` se queda en ~1,2 m. La cota vigila ESO, no el statu quo:
    // subir el tope o el crecimiento la rompe, que es justo lo que se quiere que pase.
    CHECK(worstInsideClip < 1.5,
          "dentro de la caja del clipmap, lo que se pisa no se separa >1,5 m de lo que se dibuja");
}

// TEST: el gradiente ANALÍTICO del detalle coincide con las diferencias finitas (§8).
//
// El render sacaba la normal evaluando `terrainDetail` en dos puntos desplazados: 3 evaluaciones
// (15 ruidos) por vértice para un dato que la función ya conoce. `terrainDetailGrad` lo devuelve en
// una (5 ruidos). Este test es la red: si el gradiente y la función se separan, la iluminación
// dejaría de describir la geometría — y eso no lo caza ningún test de altura, porque la ALTURA
// seguiría siendo correcta.
void test_terrain_detail_gradient() {
    beginTest("terrain_detail_gradient");
    const float R = 6371000.0f;

    // (1) El VALOR no cambia: `terrainDetailGrad` tiene que devolver exactamente lo mismo que
    // `terrainDetail`, o la paridad CPU↔GPU de §5 se rompería por la puerta de atrás.
    double worstVal = 0.0;
    for (int i = 0; i < 20000; ++i) {
        const double ph = std::acos(1.0 - 2.0 * (i + 0.5) / 20000.0), th = 2.39996 * i;
        const glm::vec3 dir((float)(std::cos(th)*std::sin(ph)), (float)(std::sin(th)*std::sin(ph)),
                            (float)std::cos(ph));
        for (float triM : { 4.0f, 30.0f, 200.0f }) {
            glm::vec3 g;
            const float ha = Haruka::Planet::terrainDetail(dir, R, triM);
            const float hb = Haruka::Planet::terrainDetailGrad(dir, R, triM, g);
            worstVal = std::max(worstVal, (double)std::fabs(ha - hb));
        }
    }
    std::printf("    valor: |grad − detail| peor = %.3e m (tiene que ser 0)\n", worstVal);
    CHECK(worstVal == 0.0, "terrainDetailGrad devuelve EXACTAMENTE la misma altura que terrainDetail");

    // (2) El GRADIENTE coincide con la diferencia finita centrada. Se compara la derivada TANGENCIAL
    // —que es la que el render usa— con un paso pequeño frente a la octava más fina activa.
    // ⚠️ Dónde se mide importa tanto como qué se mide. Dos trampas, las dos pagadas aquí:
    //
    //  · A ESCALA PLANETARIA no se puede: `dir` es un `vec3` y 1 ulp de float cerca de 1.0 son
    //    1,2e-7 → **0,76 m sobre la Tierra** (la mina de §5). Con un paso de centímetros la
    //    "referencia" es puro ruido de redondeo, y el test acusa al gradiente de SU propio error.
    //  · EL RUIDO SOLO ES C¹: los pesos son `smoothstep`, así que la segunda derivada SALTA en las
    //    fronteras de celda. Una diferencia centrada que cruce una frontera no mide la pendiente
    //    del centro, y no hay gradiente analítico que pueda coincidir con ella.
    //
    // Así que se audita la matemática donde es limpia: `detailNoiseGrad` en su propio espacio, con
    // coordenadas modestas (double de sobra) y lejos de las fronteras de celda.
    double worstRel = 0.0, sumRel = 0.0; int n = 0;
    for (int i = 0; i < 6000; ++i) {
        // Puntos con la parte fraccionaria en [0.25, 0.75]: dentro de la celda, sin rozar el salto
        // de la segunda derivada, y con la centrada (eps=1e-3) entera dentro de la misma celda.
        auto frac = [&](int s) { return 0.25 + 0.5 * (double)((s * 2654435761u) >> 8) / 16777216.0; };
        const glm::dvec3 x(std::floor(i * 0.37) + frac(i + 1),
                           std::floor(i * 0.11) + frac(i + 7),
                           std::floor(i * 0.53) + frac(i + 13));
        glm::vec3 g;
        Haruka::Planet::detailNoiseGrad(x, g);
        const double eps = 1e-3;
        for (int a = 0; a < 3; ++a) {
            glm::dvec3 dp = x, dm = x;
            dp[a] += eps; dm[a] -= eps;
            const double fd = ((double)Haruka::Planet::detailNoise(dp)
                             - (double)Haruka::Planet::detailNoise(dm)) / (2.0 * eps);
            // Error RELATIVO a la escala de la pendiente local: la absoluta no dice nada, porque
            // donde la pendiente es ~0 cualquier residuo parece enorme y da exactamente igual.
            const double scale = std::max(std::fabs(fd), 1e-3);
            const double rel = std::fabs(fd - (double)g[a]) / scale;
            worstRel = std::max(worstRel, rel); sumRel += rel; ++n;
        }
    }
    std::printf("    gradiente vs diferencia finita: error relativo peor %.5f · medio %.6f\n",
                worstRel, sumRel / n);
    // La MEDIA es la que prueba que la matemática está bien (0,016 % medido). El peor caso no puede
    // apretarse más: `detailNoise` calcula `f` en float, así que la propia referencia trae ~6e-5 de
    // ruido relativo, y donde la pendiente pasa por cero eso se amplifica al dividir por su escala.
    CHECK(sumRel / n < 0.002, "el gradiente analítico ES la pendiente del ruido (error medio <0,2%)");
    CHECK(worstRel < 0.05, "ningún punto con el gradiente apuntando a otro sitio");

    // ── A ESCALA PLANETARIA ─────────────────────────────────────────────────────────────────────
    //
    // Esta comprobación daba 98,5 % y el 1,5 % que faltaba NO era del código: era del criterio y del
    // paso. Los dos defectos, medidos:
    //
    //  1. EL PASO estaba en 200 m y hay una ventana. Con eps grande manda el error de truncamiento de
    //     la centrada (∝ eps²); con eps pequeño manda el redondeo del `dir` float (1 ulp = 0,76 m de
    //     posición sobre la Tierra). Barriendo eps sale una curva en U con óptimo claro:
    //         200 m → 98,50 %   ·   50 m → 99,90 %   ·   5 m → 98,65 %   ·   0,5 m → 85,00 %
    //     50 m es 66× el redondeo de `dir` y 0,018 λ: los dos errores a la vez en su mínimo.
    //
    //  2. EL CRITERIO era relativo a la pendiente LOCAL, con un suelo de 1e-4 puesto a dedo. Eso está
    //     mal planteado en los extremos de la octava, donde la pendiente analítica es EXACTAMENTE 0 y
    //     la secante de 100 m no lo es: dividir por ~0 hace que cualquier residuo parezca infinito, y
    //     no hay gradiente que pueda coincidir ahí. Los 2 puntos que seguían fallando con eps=50 m
    //     tenían pendiente al 0,00 % y al 0,82 % del pico — o sea, eran justo eso.
    //
    // El criterio correcto es ABSOLUTO contra la pendiente PICO de la octava: un listón físico fijo en
    // vez de un cociente que explota. Con él pasa el 100 % de los puntos, y es un listón MÁS ESTRICTO
    // que el anterior en todo el rango donde la pendiente sí existe — no se ha relajado nada, se ha
    // dejado de medir con una regla que se rompía en los extremos.
    const double eps = 50.0;
    std::vector<double> fds, ans;
    fds.reserve(2000); ans.reserve(2000);
    double peakSlope = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const double ph = std::acos(1.0 - 2.0 * (i + 0.5) / 2000.0), th = 2.39996 * i;
        const glm::dvec3 d(std::cos(th)*std::sin(ph), std::sin(th)*std::sin(ph), std::cos(ph));
        const float triM = 1000.0f;               // solo la octava de 2857 m
        glm::vec3 g;
        Haruka::Planet::terrainDetailGrad(glm::vec3(d), R, triM, g);
        const glm::dvec3 t = glm::normalize(glm::cross(d, std::fabs(d.z) < 0.9 ? glm::dvec3(0,0,1)
                                                                              : glm::dvec3(1,0,0)));
        const double fd = ((double)Haruka::Planet::terrainDetail(glm::vec3(glm::normalize(d*(double)R + t*eps)), R, triM)
                         - (double)Haruka::Planet::terrainDetail(glm::vec3(glm::normalize(d*(double)R - t*eps)), R, triM))
                        / (2.0 * eps);
        const double an = glm::dot(g, glm::vec3(t));
        fds.push_back(fd); ans.push_back(an);
        peakSlope = std::max(peakSlope, std::fabs(an));
    }
    // Magnitud: TODOS los puntos, contra el pico. Y signo: solo donde la pendiente es real (>5 % del
    // pico) — exigir el signo de una pendiente nula no es exigente, es incoherente.
    int okMag = 0, okSign = 0, signTotal = 0;
    double worstAbs = 0.0;
    for (size_t i = 0; i < fds.size(); ++i) {
        const double dif = std::fabs(fds[i] - ans[i]);
        worstAbs = std::max(worstAbs, dif);
        if (dif < 0.02 * peakSlope) ++okMag;
        if (std::fabs(ans[i]) > 0.05 * peakSlope) {
            ++signTotal;
            if (fds[i] * ans[i] > 0.0) ++okSign;
        }
    }
    std::printf("    a escala planetaria (paso %.0f m): magnitud %d/%zu · signo %d/%d · "
                "peor desvío %.4f %% de la pendiente pico\n",
                eps, okMag, fds.size(), okSign, signTotal, 100.0 * worstAbs / peakSlope);
    CHECK(okMag == (int)fds.size(),
          "a escala planetaria el gradiente coincide con la centrada en TODOS los puntos (2% del pico)");
    CHECK(okSign == signTotal,
          "y el signo coincide en todos los puntos con pendiente real");
    CHECK(worstAbs < 0.02 * peakSlope, "el peor desvío absoluto se queda por debajo del 2% del pico");

    // (3) Donde no hay octavas activas, el gradiente es cero (no basura).
    glm::vec3 g0(1.0f);
    const float hFar = Haruka::Planet::terrainDetailGrad(glm::vec3(0,0,1), R, 5000.0f, g0);
    CHECK(hFar == 0.0f && g0 == glm::vec3(0.0f), "sin octavas activas: altura 0 y gradiente 0");
}

// TEST: la inversa cube-sphere (dir ↔ cara+UV) es precisa y determinista
void test_cube_sphere_inverse() {
    beginTest("cube_sphere_inverse");
    uint32_t st = 4242u;
    auto rnd = [&]{ st = st * 1664525u + 1013904223u; return double(st >> 8) * (1.0 / 16777216.0); };

    double maxErrDir = 0.0, maxErrLx = 0.0;
    int wrongFace = 0;
    for (int i = 0; i < 20000; ++i) {
        const PlanetFace face = (PlanetFace)(int)(rnd() * 5.999);
        const double lx = rnd() * 2.0 - 1.0, ly = rnd() * 2.0 - 1.0;
        const glm::dvec3 dir = cubeFaceToDir(face, lx, ly);

        PlanetFace f2; double lx2, ly2;
        dirToCubeFace(dir, f2, lx2, ly2);

        const bool onEdge = (std::abs(lx) > 0.999 || std::abs(ly) > 0.999);
        if (f2 != face && !onEdge) ++wrongFace;

        const glm::dvec3 d2 = cubeFaceToDir(f2, lx2, ly2);
        maxErrDir = std::max(maxErrDir, glm::length(d2 - dir));

        if (f2 == face) {
            maxErrLx = std::max(maxErrLx, std::abs(lx2 - lx));
        }
    }
    std::printf("    caras mal=%d  ·  error max en lx=%.2e  ·  error angular max=%.2e (%.3f m en la Tierra)\n",
                wrongFace, maxErrLx, maxErrDir, maxErrDir * 6.371e6);
    CHECK(wrongFace == 0, "la CARA se identifica bien (salvo en la arista, donde hay dos válidas)");
    CHECK(maxErrDir * 6.371e6 < 0.5, "la inversa cae en el mismo punto (< 0.5 m en la Tierra)");

    // La forma CERRADA sola, sin Newton. Es la que se porta al shader del clipmap
    // (assets/shaders/lib/cube_face.glsl), y el shader no puede permitirse 12 evaluaciones del
    // spherify por vértice teselado. Esto es lo que ata las dos implementaciones: si alguien
    // "simplifica" la fórmula de una, esta comprobación cae.
    //
    // Importa porque el fallo anterior fue justo este y era MUDO: el clipmap invertía con la
    // proyección gnómica a secas (y con el signo de v cambiado), leía la altura de otro punto del
    // planeta —normalmente fondo oceánico—, se hundía kilómetros y no pintaba un solo píxel. Ni
    // error de GL, ni shader que no compila: terreno sin relieve y ninguna pista.
    double maxErrClosed = 0.0;
    int    wrongFaceClosed = 0;
    st = 4242u;
    for (int i = 0; i < 20000; ++i) {
        const PlanetFace face = (PlanetFace)(int)(rnd() * 5.999);
        const double lx = rnd() * 2.0 - 1.0, ly = rnd() * 2.0 - 1.0;
        const glm::dvec3 dir = cubeFaceToDir(face, lx, ly);

        PlanetFace f2; double lx2, ly2;
        dirToCubeFaceClosed(dir, f2, lx2, ly2);

        const bool onEdge = (std::abs(lx) > 0.999 || std::abs(ly) > 0.999);
        if (f2 != face) { if (!onEdge) ++wrongFaceClosed; continue; }
        maxErrClosed = std::max(maxErrClosed, std::max(std::abs(lx2 - lx), std::abs(ly2 - ly)));
    }
    // El umbral está en téxeles de la retícula, que es lo que de verdad importa: un error de 1e-6
    // en (lx,ly) sobre una cara de 256 celdas es 1e-4 téxeles, o sea nada de altura.
    std::printf("    forma cerrada: caras mal=%d  ·  error max en lx=%.2e (%.1e téxeles con res=256)\n",
                wrongFaceClosed, maxErrClosed, maxErrClosed * 128.0);
    CHECK(wrongFaceClosed == 0, "la forma cerrada identifica la misma CARA");
    CHECK(maxErrClosed * 128.0 < 1e-3, "la forma cerrada cae en el mismo téxel que el Newton");
}

// TEST: el CLIMA es del mundo, y la lluvia CAE DE LAS NUBES
void test_weather_fronts() {
    beginTest("weather_fronts");
    Haruka::WeatherSystem W;
    W.configure(4242u);

    int  total = 0, wet = 0, snowy = 0, clear = 0;
    float maxCover = 0.0f;
    bool  rainWithoutCloud = false;
    for (int ti = 0; ti < 24; ++ti) {
        W.setTime(ti * 137.0);
        int clearThisTick = 0;
        for (int i = 0; i < 400; ++i) {
            const float z  = 1.0f - 2.0f * (i + 0.5f) / 400.0f;
            const float r  = std::sqrt(std::max(0.0f, 1.0f - z * z));
            const float th = 2.39996323f * i;
            const glm::dvec3 d(r * std::cos(th), r * std::sin(th), z);
            const float humid = 0.15f + 0.8f * ((i * 37) % 100) / 100.0f;
            const float tempC = -12.0f + 40.0f * ((i * 53) % 100) / 100.0f;

            const Haruka::WeatherSample s = W.sampleAt(d, tempC, humid);
            ++total;
            maxCover = std::max(maxCover, s.cloudCover);
            if (s.cloudCover < 0.15f) { ++clear; ++clearThisTick; }
            if (s.precip > 0.0f) {
                ++wet;
                if (s.cloudCover < Haruka::WeatherSystem::kPrecipCover) rainWithoutCloud = true;
                if (s.type == Haruka::Precip::Snow) {
                    ++snowy;
                    CHECK(tempC <= Haruka::WeatherSystem::kSnowTempC, "solo nieva bajo cero");
                } else {
                    CHECK(tempC > Haruka::WeatherSystem::kSnowTempC, "por encima de cero, llueve");
                }
            } else {
                CHECK(s.type == Haruka::Precip::None, "sin precipitación no hay tipo");
            }
            CHECK(s.cloudCover >= 0.0f && s.cloudCover <= 1.0f, "cobertura acotada a [0,1]");
        }
        CHECK(clearThisTick > 0, "a cualquier hora queda cielo DESPEJADO en alguna parte del planeta");
    }
    std::printf("    clima: %d muestras · %d con precipitacian (%.1f%%, %d de nieve) · %d despejadas · cobertura max %.2f\n",
                total, wet, 100.0f * wet / total, snowy, clear, maxCover);
    CHECK(!rainWithoutCloud, "NUNCA llueve sin nube encima (precip>0 ⇒ cobertura ≥ kPrecipCover)");
    CHECK(wet > 0, "en algïn sitio y momento SA precipita (el test no pasa por vacuidad)");
    CHECK(maxCover > 0.8f, "los frentes llegan a cerrar el cielo de verdad");

    Haruka::WeatherSystem W2; W2.configure(4242u); W2.setTime(1000.0);
    W.setTime(1000.0);
    const glm::dvec3 probe = glm::normalize(glm::dvec3(0.3, 0.7, 0.5));
    const Haruka::WeatherSample a = W.sampleAt(probe, 12.0f, 0.7f);
    const Haruka::WeatherSample b = W2.sampleAt(probe, 12.0f, 0.7f);
    CHECK(a.cloudCover == b.cloudCover && a.precip == b.precip, "determinista: misma seed+tiempo = mismo clima");

    Haruka::WeatherSystem W3; W3.configure(99u); W3.setTime(1000.0);
    float diff = 0.0f;
    for (int i = 0; i < 200; ++i) {
        const float z  = 1.0f - 2.0f * (i + 0.5f) / 200.0f;
        const float r  = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float th = 2.39996323f * i;
        const glm::dvec3 d(r * std::cos(th), r * std::sin(th), z);
        diff += std::abs(W.cloudCoverAt(d, 0.6f) - W3.cloudCoverAt(d, 0.6f));
    }
    CHECK(diff / 200.0f > 0.05f, "otra seed = otro clima (frentes en otro sitio)");

    float minC = 1e9f, maxC = -1e9f;
    for (int ti = 0; ti < 120; ++ti) {
        W.setTime(ti * 30.0);
        const float c2 = W.cloudCoverAt(probe, 0.6f);
        minC = std::min(minC, c2); maxC = std::max(maxC, c2);
    }
    std::printf("    sobre un punto fijo en 1 h: cobertura %.2f → %.2f\n", minC, maxC);
    CHECK(maxC - minC > 0.25f, "los frentes PASAN: sobre un punto fijo el cielo cambia con el tiempo");
}

// TEST: la CAPA GRANULAR y sus HUELLAS
void test_ground_layer() {
    beginTest("ground_layer");
    using GL = Haruka::GroundLayer;
    using M  = Haruka::GroundMaterial;

    CHECK(GL::lifetimeFor(M::Snow) > GL::lifetimeFor(M::Sand) * 3.0f,
          "la huella en NIEVE dura mucho más que en arena");
    CHECK(GL::sinkDepthFor(M::Snow) > GL::sinkDepthFor(M::Sand),
          "en nieve te hundes más que en arena");
    CHECK(GL::speedFactorFor(M::Snow) < GL::speedFactorFor(M::Sand),
          "la nieve frena más al andar que la arena");
    CHECK(GL::speedFactorFor(M::None) == 1.0f, "sin capa NO se frena (suelo firme)");

    GL::Stamp snow; snow.material = M::Snow; snow.bornAt = 0.0; snow.depth = 1.0f;
    GL::Stamp sand; sand.material = M::Sand; sand.bornAt = 0.0; sand.depth = 1.0f;
    const float fSnow = GL::fade(snow, GL::lifetimeFor(M::Snow) * 0.5);
    const float fSand = GL::fade(sand, GL::lifetimeFor(M::Sand) * 0.5);
    std::printf("    a media vida: nieve %.2f · arena %.2f\n", fSnow, fSand);
    CHECK(fSnow > 0.8f, "a media vida la huella en nieve sigue casi entera");
    CHECK(fSand < 0.35f, "a media vida la huella en arena ya se ha desmoronado");
    CHECK(GL::fade(snow, GL::lifetimeFor(M::Snow) + 1.0) == 0.0f, "pasada su vida, la huella no existe");

    Haruka::GroundLayer L;
    const glm::dvec3 base(6371000.0, 0.0, 0.0);
    for (int i = 0; i < 5000; ++i) {
        GL::Stamp s; s.pos = base + glm::dvec3(0, i * 0.85, 0); s.bornAt = 0.0; s.material = M::Snow;
        L.stamp(s);
    }
    CHECK(L.size() == GL::kMaxStamps, "la lista de huellas está ACOTADA (es un rastro, no un historial)");

    Haruka::GroundLayer P;
    GL::Stamp p; p.pos = base; p.radius = 0.25f; p.depth = 1.0f; p.bornAt = 0.0; p.material = M::Snow;
    P.stamp(p);
    const float atCenter = P.trampledAt(base, 1.0);
    const float atEdge   = P.trampledAt(base + glm::dvec3(0.0, 0.24, 0.0), 1.0);
    const float outside  = P.trampledAt(base + glm::dvec3(0.0, 0.60, 0.0), 1.0);
    std::printf("    pisada: centro %.2f · borde %.2f · fuera %.2f\n", atCenter, atEdge, outside);
    CHECK(atCenter > 0.9f, "el centro de la pisada está hundido del todo");
    CHECK(atEdge < atCenter, "el borde de la pisada hunde menos que el centro");
    CHECK(outside == 0.0f, "fuera del radio la capa queda INTACTA (la huella no es global)");

    P.update(GL::lifetimeFor(M::Snow) + 1.0);
    CHECK(P.size() == 0, "update() retira las huellas caducadas");

    Haruka::GroundLayer N;
    GL::Stamp none; none.material = M::None; none.pos = base; none.bornAt = 0.0;
    N.stamp(none);
    CHECK(N.size() == 0, "sin capa que pisar no se registra huella");

    Haruka::GroundLayer B;
    GL::Stamp sn; sn.pos = base;                          sn.bornAt = 0.0; sn.material = M::Snow; sn.radius = 0.25f;
    GL::Stamp sa; sa.pos = base + glm::dvec3(0, 5.0, 0);  sa.bornAt = 0.0; sa.material = M::Sand; sa.radius = 0.25f;
    B.stamp(sn); B.stamp(sa);
    B.update(60.0);
    CHECK(B.size() == 1, "a los 60 s solo sobrevive la huella de NIEVE (la de arena ya se desmoronó)");
    CHECK(B.trampledAt(base, 60.0) > 0.5f, "la huella de nieve sigue marcada a los 60 s");
    CHECK(B.trampledAt(base + glm::dvec3(0, 5.0, 0), 60.0) == 0.0f, "la de arena ya no marca");
}

// =================================================================================================
// LA RETÍCULA DEL CLIPMAP: ¿cae cada nodo de colisión en un vértice que el render dibuja de verdad?
//
// Por qué hacía falta ESTE test y no bastaba `terrain_chord_error`
// ---------------------------------------------------------------
// Aquel compara la rejilla de colisión REAL contra un MODELO del render: «una retícula uniforme de
// quads de 4 m». Mientras el teselador usó `fractional_odd_spacing` ese modelo era sencillamente
// falso —el nivel era continuo con la distancia, los tramos de los extremos medían la mitad y los
// vértices se DESLIZABAN al caminar— así que su `0.000000 m` no medía la paridad: la suponía. Habría
// seguido en verde con el render poniendo los vértices en cualquier otro sitio.
//
// Lo que hace posible auditarlo es el cambio a `equal_spacing` con el nivel en potencias de dos: esa
// regla es determinista y se puede reimplementar exactamente, cosa que la fraccionaria no permite
// (el spec no fija su aritmética, así que una réplica en CPU diferiría por ulps y por fabricante).
//
// La propiedad que se comprueba, y su dirección
// ---------------------------------------------
// NO es «todo vértice del render es un nodo de colisión»: la colisión es más gruesa lejos (16 m
// contra 4 m del render), así que esa dirección es falsa por diseño. La que importa es la contraria:
//
//     **todo nodo de la malla de COLISIÓN es también un vértice que el render dibuja**
//
// Si se cumple, las dos superficies se TOCAN en cada nodo de colisión —ahí las dos evalúan la misma
// función en el mismo punto— y entre nodos solo queda la cuerda. Si no se cumple, las superficies se
// CRUZAN y no hay afinado que las junte, que es exactamente lo que pasaba con la progresión ×1,12
// (celdas de 10, 16, 25 m contra quads de 4 m: ningún vértice compartido a partir de ~256 m).
//
// ⚠️ Sigue siendo un GEMELO, no una observación del driver: reimplementa `clipmap.tesc`. Un test en
// CPU no puede ver lo que hace el silicio. Lo que sí hace es dejar la suposición ESCRITA y romperse
// si alguien cambia el reparto del nivel, el tamaño del parche o la rejilla de colisión.
// =================================================================================================
void test_clipmap_vertex_lattice() {
    using namespace Haruka::Planet;
    beginTest("clipmap_vertex_lattice");

    // Gemelo de `edgeFactor` en clipmap.tesc, incluido el redondeo a potencia de dos.
    auto edgeFactor = [](double mx, double mz, double arc) {
        const double d = std::max(std::sqrt(mx*mx + mz*mz), 1.0);
        double lvl = std::min(std::max(arc / (d * 0.004), 1.0), TERRAIN_CLIP_TESS_CAP);
        return std::min(std::max(std::exp2(std::floor(std::log2(lvl))), 1.0), TERRAIN_CLIP_TESS_CAP);
    };
    const double P = TERRAIN_CLIP_PATCH_M;

    // Paso que el render dibuja EN EL EJE X dentro del parche que contiene (x,z). El nivel interior
    // de esa dirección es `max` de las dos aristas opuestas, igual que en el control shader.
    auto renderStepX = [&](double x, double z) {
        const double px = std::floor(x / P) * P, pz = std::floor(z / P) * P;
        // aristas z=pz y z=pz+P (las que corren en X): sus puntos medios
        const double lo = edgeFactor(px + P*0.5, pz,       P);
        const double hi = edgeFactor(px + P*0.5, pz + P,   P);
        return P / std::max(lo, hi);
    };

    // Radio en el que el render está al TOPE de teselación, o sea dibujando el quad de 4 m: es donde la
    // propiedad puede exigirse, y no por casualidad es el terreno que se pisa y donde se construye.
    const double fullR = TERRAIN_CLIP_PATCH_M / (TERRAIN_CLIP_TESS_CAP * 0.004);   // 1000 m

    const std::vector<double> grid = terrainRingGrid(200000.0);
    long nearChecked = 0, nearOff = 0, farChecked = 0, farOff = 0;
    double worstOff = 0.0, worstX = 0.0, worstZ = 0.0;
    for (double x : grid) {
        if (std::fabs(x) > TERRAIN_CLIP_COVER_MIN_M) continue;   // solo dentro de la caja del clipmap
        for (double z : grid) {
            if (std::fabs(z) > TERRAIN_CLIP_COVER_MIN_M) continue;
            const double step = renderStepX(x, z);
            // ¿es `x` un múltiplo exacto del paso del render, medido desde el origen del parche?
            const double px  = std::floor(x / P) * P;
            const double q   = (x - px) / step;
            const double off = std::fabs(q - std::round(q)) * step;   // en METROS fuera de la retícula
            const bool   near = std::sqrt(x*x + z*z) <= fullR;
            if (near) { ++nearChecked; if (off > 1e-9) ++nearOff; }
            else      { ++farChecked;  if (off > 1e-9) ++farOff;  }
            if (off > worstOff) { worstOff = off; worstX = x; worstZ = z; }
        }
    }
    std::printf("    dentro de %.0f m (render al tope, quad 4 m): %ld nodos · %ld fuera de la reticula\n",
                fullR, nearChecked, nearOff);
    std::printf("    fuera de %.0f m: %ld nodos · %ld fuera (%.1f %%) · peor desvio %.1f m en (%.0f,%.0f)\n",
                fullR, farChecked, farOff, 100.0 * (double)farOff / (double)std::max(1L, farChecked),
                worstOff, worstX, worstZ);

    CHECK(nearChecked > 10000, "el barrido cubre de verdad la zona de teselacion maxima");
    CHECK(nearOff == 0, "donde el render esta al tope, TODO nodo de colision es un vertice suyo");

    // ⚠️ FUERA DE ESE RADIO LA PROPIEDAD NO SE CUMPLE, Y NO ES UN AJUSTE PENDIENTE: es geométrico. La
    // rejilla de colisión es SEPARABLE (el producto de dos listas 1D, porque así se indexa una
    // `MeshShape`) y el LOD del render es RADIAL. En (x=-252, z=1900) el render dibuja a paso 8 por su
    // distancia 2D mientras la colisión va a paso 4 por su |x|: ninguna rejilla separable puede seguir
    // a una radial en todo el plano. Se acota en vez de exigirse cero, y se deja escrito el número.
    CHECK(farOff < farChecked / 2, "fuera del tope, el desajuste esta acotado (menos de la mitad)");

    // Y la contraprueba, que es lo que impide que el CHECK de arriba sea otra tautología: con la
    // progresión ×1,12 de antes (10, 16, 25 m — ningún múltiplo del quad) esto TIENE que fallar. Si no
    // fallara, el test no estaría mirando lo que dice mirar.
    long badOff = 0;
    for (double r = 300.0; r < 1900.0; r += 11.0) {
        double s = 4.0, acc = 0.0;
        while (acc < r) { acc += std::min(s, 25.0); s *= 1.12; }   // la rejilla vieja
        const double step = renderStepX(acc, 0.0), px = std::floor(acc / P) * P;
        const double q = (acc - px) / step;
        if (std::fabs(q - std::round(q)) * step > 1e-9) ++badOff;
    }
    std::printf("    contraprueba (rejilla vieja x1,12): %ld nodos de %d caen FUERA de la reticula\n",
                badOff, (int)((1900.0 - 300.0) / 11.0) + 1);
    CHECK(badOff > 100, "la rejilla ANTERIOR falla este mismo test (si no, no esta midiendo nada)");
}

// =================================================================================================
// ANILLOS DE HEIGHTFIELD: que TESELEN el plano — ni hueco ni solape
// =================================================================================================
//
// Es la propiedad de la que cuelga todo el diseño y la que más fácil se rompe al tocar un número.
// Un HUECO entre dos niveles es suelo que no existe: el jugador se cae. Un SOLAPE es peor de
// diagnosticar: Jolt genera contactos DOBLES sobre el mismo suelo y el personaje rebota o se engancha
// justo en la frontera (es el fallo que `terrainHeightFieldCovers` existe para evitar en la malla).
//
// Se comprueba sobre la geometría REAL —`terrainRingHole`, `terrainRingNode`, `terrainRingExtent`,
// las mismas que construyen los anillos— y no sobre una reimplementación de la regla, que no
// auditaría nada.
void test_terrain_ring_tiling() {
    beginTest("terrain_ring_tiling");
    using namespace Haruka::Planet;

    const std::vector<TerrainRingSpec> L = terrainRingLayout(200000.0);
    CHECK(L.size() == 11, "11 anillos alcanzan los 200 km pedidos");
    CHECK(std::fabs(L[0].extent - TERRAIN_COLLIDE_UNIFORM_M) < 1e-9,
          "el anillo 0 es exactamente el bloque cercano de siempre (+-256 m)");
    CHECK(L.back().extent >= 200000.0, "el ultimo anillo cubre el radio pedido");
    for (const auto& r : L) std::printf("    anillo: celda %6.0f m  half %4d  alcance +-%8.0f m  "
                                       "hueco +-%8.0f m  %ux%u muestras\n",
                                       r.cell, r.half, r.extent, r.hole, r.samples, r.samples);

    // ── EL TOPE DENTRO DE LA CAJA DEL CLIPMAP ──────────────────────────────────────────────────
    // Sin el, la celda dobla libre y a 2 km vale 32 m contra los 16 m a los que la malla la topa: el
    // escalon en la costura de 2 km, MEDIDO en el juego, era 2,9 m contra la sagita de 0,68 m de la
    // malla. O sea que los anillos empeoraban justo donde importa.
    bool capped = true;
    for (const auto& r : L)
        if (r.hole < TERRAIN_CLIP_COVER_MIN_M && r.cell > TERRAIN_COLLIDE_CELL_CAP + 1e-9) capped = false;
    CHECK(capped, "ningun anillo que empiece dentro de la caja pasa de TERRAIN_COLLIDE_CELL_CAP");

    // ── LA COSTURA CAE EN UN NODO ──────────────────────────────────────────────────────────────
    // El borde del hueco tiene que ser un nodo EXACTO del anillo de fuera. Si no, queda media celda
    // sin cubrir entre lo que llega el de dentro y el primer quad que el de fuera puede conservar.
    bool seamOnNode = true, chained = true;
    for (size_t k = 1; k < L.size(); ++k) {
        const double q = L[k].hole / L[k].cell;
        if (std::fabs(q - std::round(q)) > 1e-9) seamOnNode = false;
        if (std::fabs(L[k].hole - L[k-1].extent) > 1e-9) chained = false;
    }
    CHECK(seamOnNode, "el borde del hueco es un nodo exacto del anillo de fuera");
    CHECK(chained,    "el hueco de cada anillo es justo el alcance del de dentro");

    // ── LA RETICULA ────────────────────────────────────────────────────────────────────────────
    // Todo nodo util de todo anillo tiene que caer en un multiplo EXACTO del quad del render: es lo
    // que hace que la reticula gruesa sea un SUBCONJUNTO de la fina y que las dos superficies se
    // toquen en vez de cruzarse.
    bool onLattice = true;
    for (const auto& r : L)
        for (uint32_t i = 1; i < r.samples; ++i) {
            const double q = terrainRingNode(i, r) / TERRAIN_CLIP_QUAD_M;
            if (std::fabs(q - std::round(q)) > 1e-9) onLattice = false;
        }
    CHECK(onLattice, "todo nodo util cae en la reticula del render (multiplo del quad)");

    // ── COBERTURA: cada punto en EXACTAMENTE un anillo ──────────────────────────────────────────
    // Un anillo cubre un punto si la celda que lo contiene tiene sus 4 nodos con superficie (Jolt
    // tira el quad entero si un solo nodo esta marcado sin colision) y cae dentro de su alcance.
    auto coveredBy = [&](const TerrainRingSpec& r, double x, double z) {
        if (std::fabs(x) >= r.extent || std::fabs(z) >= r.extent) return false;
        const double x0 = std::floor(x / r.cell) * r.cell, z0 = std::floor(z / r.cell) * r.cell;
        for (int dx = 0; dx <= 1; ++dx) for (int dz = 0; dz <= 1; ++dz)
            if (terrainRingHole(r, x0 + dx * r.cell, z0 + dz * r.cell)) return false;
        return true;
    };
    int gaps = 0, overlaps = 0, probed = 0;
    for (double d = 0.5; d < L.back().extent; d *= 1.0009) {
        for (double ang : { 0.0, 0.7, 1.3, 2.9 }) {
            const double x = d * std::cos(ang), z = d * std::sin(ang);
            int n = 0;
            for (const auto& r : L) if (coveredBy(r, x, z)) ++n;
            ++probed;
            if (n == 0) ++gaps; else if (n > 1) ++overlaps;
        }
    }
    std::printf("    %d puntos sondeados: %d sin cubrir, %d cubiertos por mas de un anillo\n",
                probed, gaps, overlaps);
    CHECK(gaps == 0,     "ningun punto queda sin suelo (un hueco = el jugador se cae)");
    CHECK(overlaps == 0, "ningun punto tiene dos anillos (un solape = contactos dobles de Jolt)");

    // ── CONTRAPRUEBA ───────────────────────────────────────────────────────────────────────────
    // Sin ella este test no puede fallar y no demuestra nada. Se repite con la disposicion que sale
    // de una rejilla de 128 muestras, que es la que tiene HOY el bloque cercano: cobertura
    // ASIMETRICA `[-64c, +63c]` (ver TERRAIN_HF_LO). El hueco del anillo de fuera tendria entonces
    // que llegar a `+63·c(dentro)`, que NO es un nodo suyo — asi que entre lo que cubre el de dentro
    // y el primer quad que el de fuera conserva queda media celda gruesa de NADA.
    auto coveredByAsym = [&](size_t k, double x, double z) {
        const double c = L[k].cell;
        const double lo = -(double)L[k].half * c, hi = (double)(L[k].half - 1) * c;
        if (x < lo || x >= hi || z < lo || z >= hi) return false;
        if (k == 0) return true;
        const double cin = L[k-1].cell;
        const double hlo = -(double)L[k-1].half * cin, hhi = (double)(L[k-1].half - 1) * cin;
        const double x0 = std::floor(x / c) * c, z0 = std::floor(z / c) * c;
        for (int dx = 0; dx <= 1; ++dx) for (int dz = 0; dz <= 1; ++dz) {
            const double nx = x0 + dx * c, nz = z0 + dz * c;
            if (nx > hlo + 1e-9 && nx < hhi - 1e-9 && nz > hlo + 1e-9 && nz < hhi - 1e-9) return false;
        }
        return true;
    };
    int badGaps = 0, badOverlaps = 0;
    for (double d = 0.5; d < L.back().extent; d *= 1.0009) {
        for (double ang : { 0.0, 0.7, 1.3, 2.9 }) {
            const double x = d * std::cos(ang), z = d * std::sin(ang);
            int n = 0;
            for (size_t k = 0; k < L.size(); ++k) if (coveredByAsym(k, x, z)) ++n;
            if (n == 0) ++badGaps; else if (n > 1) ++badOverlaps;
        }
    }
    std::printf("    contraprueba (cobertura asimetrica de una reja de 128): "
                "%d sin cubrir, %d con solape\n", badGaps, badOverlaps);
    CHECK(badGaps + badOverlaps > 100,
          "la disposicion ASIMETRICA falla este mismo test (si no, no esta midiendo nada)");
}
