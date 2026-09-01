// ================================================================================================
// LOD de malla de los props (tools/procgraph/tree_mesh.h, parámetro `detail`).
//
// Por qué hace falta un LOD si los props YA van instanciados
// ---------------------------------------------------------
// Instanciar ahorra DRAW CALLS, no trabajo de vértices. Medido con RenderDoc sobre un frame real:
// `glDrawElementsInstanced(1536, 6320)` es UN solo draw para los 6320 árboles —por el lado de la CPU
// ya está óptimo— pero la GPU ejecuta el vertex shader de los 512 triángulos POR INSTANCIA. Son
// 3,24 M de triángulos, el **85,8 % del frame**, y eso no lo cambia que vayan en un comando o en 6320.
//
// Lo que este banco fija
// ----------------------
//  1. COMPATIBILIDAD: detalle 1.0 da la malla de siempre, vértice a vértice. Sin eso, activar el LOD
//     habría cambiado el aspecto de todos los árboles ya colocados.
//  2. LA ENVOLVENTE NO CAMBIA. Es LA propiedad de un LOD usable: si la caja del árbol cambiara al
//     conmutar de nivel, el árbol daría un salto de tamaño a media distancia — el artefacto clásico.
//     Y no es teórico: las dos primeras versiones de esto encogían la copa un 33 % de ancho, y la
//     tercera la dejaba como una PESA (3,77 de ancho por 1,86 de fondo) al quedar dos blobs opuestos.
//     Los tres fallos los habría dejado pasar un test que solo contara triángulos.
//  3. La malla BAJA de verdad y de forma monótona: sin eso el LOD existe y no ahorra nada.
//  4. Sigue siendo determinista: misma semilla y mismo detalle ⇒ misma malla.
// ================================================================================================
#include "test_common.h"
#include "tools/procgraph/tree_mesh.h"
#include "core/planet/prop_scatter.h"   // scatterPropsNear: el alcance del scatter

#include <cmath>
#include <cstdio>
#include <unordered_set>
#include <vector>

using namespace Haruka::Tools::ProcGraph;

namespace {

struct Baked {
    size_t tris = 0, verts = 0, colors = 0, uvs = 0, parts = 0;
    glm::vec3 lo{0.0f}, hi{0.0f};
    bool ok = false;
};

Baked bakeAt(int seed, float detail) {
    Graph g;
    const int node = g.emplaceNode<TreeMeshNode>(seed, 6.5f, 0.35f, 1.0f, 8, detail);
    g.compile();
    TreeMeshData m;
    Baked b;
    if (!bakeTreeMesh(g, node, m)) return b;
    b.ok = true;
    b.tris = m.indices.size() / 3;
    b.verts = m.positions.size();
    b.colors = m.colors.size(); b.uvs = m.uvs.size(); b.parts = m.partId.size();
    b.lo = glm::vec3(1e9f); b.hi = glm::vec3(-1e9f);
    for (const glm::vec3& p : m.positions) { b.lo = glm::min(b.lo, p); b.hi = glm::max(b.hi, p); }
    return b;
}

/// Los niveles que el motor usa de verdad (`kLodDetail` en application_render.cpp). Si alguien los
/// cambia allí sin tocarlos aquí, el test deja de auditar lo que se dibuja — de ahí el comentario.
const float kLevels[3] = { 1.0f, 0.60f, 0.45f };


} // namespace

/// ⚠️ CADA VERTICE TIENE QUE TRAER SU COLOR, EN TODOS LOS NIVELES DE LOD.
///
/// `application_render.cpp` sube los vertices asi:
///     pv.color = (vi < tm.colors.size()) ? tm.colors[vi] : glm::vec3(1.0f);
/// El respaldo es **BLANCO**. Un LOD que devuelva menos colores que posiciones no falla, no avisa y
/// no rompe ningun test: simplemente pinta el prop de blanco. Andoni lo reporto mirando —los arboles
/// del fondo salen blancos y el de delante, coloreado— y eso es exactamente la firma de un LOD sin
/// colores. Lo mismo vale para uv y partId: `partId` de mas se lee como "parte 0", que al romper el
/// prop se lleva el objeto entero.
void test_prop_lod_attributes() {
    beginTest("prop_lod_attributes");
    std::printf("    nivel   vertices   colores   uv    partId\n");
    bool todos = true;
    for (int i = 0; i < 3; ++i) {
        const Baked b = bakeAt(1234, kLevels[i]);
        CHECK(b.ok, "el nivel hornea");
        if (!b.ok) { todos = false; continue; }
        std::printf("    %.2f    %8zu   %7zu   %4zu  %6zu%s\n",
                    kLevels[i], b.verts, b.colors, b.uvs, b.parts,
                    (b.colors == b.verts) ? "" : "   <- FALTAN COLORES: se dibuja BLANCO");
        if (b.colors != b.verts) todos = false;
    }
    CHECK(todos, "todos los niveles de LOD traen un color por vertice (si no, el prop sale BLANCO)");
}


// ================================================================================================
// EL ALCANCE DEL SCATTER ES REDONDO, NO CUADRADO.
//
// ⚠️ ESTE TEST NACE DE UN SINTOMA QUE NADIE PODIA VER: "se ve como hay un LOD cuadrado para
// maximizar el render de props en ese cuadrado". Era literal. Cada banda enumeraba un CUADRADO de
// semilado `radiusM` y cortaba por distancia de CHEBYSHEV, asi que en las diagonales los props
// llegaban `sqrt(2)` = 1,41 veces mas lejos que de frente, y el borde de la ultima banda era un
// cuadrado dibujado en el suelo.
//
// Ninguna prueba lo notaba: la suite entera paso igual antes y despues de arreglarlo. Por eso esto.
// ================================================================================================
namespace {
/// Campo llano y uniforme: aqui lo que se mide es la GEOMETRIA del alcance, no la ecologia.
class FlatField : public Haruka::Planet::IPropSphereField {
public:
    Haruka::FieldSample sampleAt(const glm::vec3&) const override {
        // Todo a cero: llano, sin lago, a nivel del mar. Aqui se mide la GEOMETRIA del alcance.
        return Haruka::FieldSample{};
    }
    float heightAt(const glm::vec3&) const override { return 0.0f; }
};
}  // namespace

void test_prop_scatter_radial() {
    beginTest("prop_scatter_radial");
    using namespace Haruka::Planet;

    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const glm::dvec3 up = glm::normalize(glm::dvec3(0.31, 0.62, 0.72));
    const glm::dvec3 cam = pc + up * (R + 2.0);

    PropScatterParams params;
    params.radius = R;
    params.seed   = 7u;

    PropLayerTable table;
    PropLayer L;
    // Capa que acepta cualquier sitio: si filtrara por ecologia, un hueco en la esquina no se
    // distinguiria de un hueco por el corte, que es justo lo que este test viene a medir.
    L.mesh = "tree"; L.density = 1.0f;
    L.humMin = -1e3f; L.humMax = 1e3f; L.tempMin = -1e3f; L.tempMax = 1e3f;
    L.slopeMin = 0.0f; L.slopeMax = 1.0f;
    table.layers.push_back(L);

    FlatField field;
    const std::vector<ScatteredProp> props = scatterPropsNear(field, cam, pc, params, table);

    // El alcance nominal: el radio de la ultima banda.
    double outer = 0.0;
    for (const PropScatterLod& l : params.lods) outer = std::max(outer, (double)l.radiusM);

    double worst = 0.0;
    for (const ScatteredProp& p : props) {
        // Distancia TANGENTE al jugador, que es en la que estan definidas las bandas.
        const glm::dvec3 d = glm::dvec3(p.dir);
        const glm::dvec3 rel = d - up * glm::dot(d, up);
        worst = std::max(worst, glm::length(rel) * R);
    }
    std::printf("    %zu props · alcance nominal %.0f m · el mas lejano a %.0f m (%.2fx)\n",
                props.size(), outer, worst, outer > 1.0 ? worst / outer : 0.0);

    // ⚠️ SIN ESTO EL TEST NO MIDE NADA: si el scatter no coloca props, todo lo de abajo pasa.
    CHECK(props.size() > 100, "el scatter coloca props (si no, el resto no demuestra nada)");
    // Con corte cuadrado esto daba hasta 1,41x. Se deja holgura de una celda de la banda externa.
    CHECK(worst <= outer * 1.05,
          "ningun prop pasa del radio nominal (con corte cuadrado llegaban a 1,41x en diagonal)");
    // CONTRAPRUEBA: y llegan CERCA del radio. Un scatter que solo sembrara al lado del jugador
    // pasaria la comprobacion de arriba y seria peor que el bug.
    CHECK(worst >= outer * 0.80, "y llegan hasta cerca del radio (no es que no siembre lejos)");
}


// ================================================================================================
// ¿SOBREVIVEN LOS PROPS AL CAMINAR? La estabilidad del LOD, medida.
//
// ⚠️ LA CABECERA DE `prop_scatter.h` PROMETE UN FUNDIDO QUE NO EXISTE: "el radio se cubre en BANDAS
// de celda creciente con FUNDIDO en el borde de cada banda (encoge antes de cruzar), asi NO se ve un
// anillo cortado". En el codigo no hay nada de eso — ni `fade`, ni `shrink`, ni nada.
//
// Y hay algo peor que el anillo. El tamaño de CELDA sale de la BANDA, y la banda sale de la distancia
// A LA CAMARA. Asi que la rejilla del mundo cambia cuando te mueves: un prop a 1005 m vive en una
// celda de 40 m; caminas diez metros y pasa a la banda de celda 12 -> **otra rejilla, otros hashes,
// otros props**. La promesa de "determinista por celda mundial" solo se cumple DENTRO de una banda.
//
// Esto lo mide: camina en pasos cortos y cuenta cuantos props sobreviven de un paso al siguiente,
// separado por distancia. Los del borde exterior DEBEN entrar y salir; los de dentro, no.
// ================================================================================================
void test_prop_scatter_stability() {
    beginTest("prop_scatter_stability");
    using namespace Haruka::Planet;

    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(0.31, 0.62, 0.72));
    const glm::dvec3 fwd = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));

    PropScatterParams params;
    params.radius = R;
    params.seed   = 7u;
    PropLayerTable table;
    PropLayer L;
    L.mesh = "tree"; L.density = 1.0f;
    L.humMin = -1e3f; L.humMax = 1e3f; L.tempMin = -1e3f; L.tempMax = 1e3f;
    L.slopeMin = 0.0f; L.slopeMax = 1.0f;
    table.layers.push_back(L);
    FlatField field;

    // Cada prop se identifica por su `cellSeed` + su celda: si el mismo sitio del mundo da el mismo
    // prop paso tras paso, el conjunto no deberia cambiar salvo en el borde.
    auto stepAt = [&](double metros) {
        const double a = metros / R;
        const glm::dvec3 up = glm::normalize(up0 * std::cos(a) + fwd * std::sin(a));
        const glm::dvec3 cam = pc + up * (R + 2.0);
        std::vector<ScatteredProp> v = scatterPropsNear(field, cam, pc, params, table);
        return std::make_pair(cam, v);
    };

    // Bandas por distancia: se mira por separado el interior y el borde de cada una.
    const double edges[] = { 0.0, 900.0, 1100.0, 2900.0, 3100.0, 5500.0, 6000.0 };
    const char*  names[] = { "0-900 (dentro b0)", "900-1100 (BORDE b0/b1)", "1100-2900 (dentro b1)",
                             "2900-3100 (BORDE b1/b2)", "3100-5500 (dentro b2)", "5500-6000 (borde ext)" };
    const int    nb = 6;
    long long seen[6] = {0}, lost[6] = {0};

    auto prev = stepAt(0.0);
    for (int step = 1; step <= 8; ++step) {
        auto cur = stepAt(step * 5.0);          // pasos de 5 m: andar, no teletransportarse
        std::unordered_set<uint32_t> now;
        for (const ScatteredProp& p : cur.second) now.insert(p.cellSeed);
        for (const ScatteredProp& p : prev.second) {
            // Distancia del prop a la camara NUEVA: lo que decide en que banda cae ahora.
            const glm::dvec3 d = glm::dvec3(p.dir);
            const glm::dvec3 upN = glm::normalize(cur.first - pc);
            const double dist = glm::length(d - upN * glm::dot(d, upN)) * R;
            int b = -1;
            for (int k = 0; k < nb; ++k) if (dist >= edges[k] && dist < edges[k + 1]) { b = k; break; }
            if (b < 0) continue;
            ++seen[b];
            if (!now.count(p.cellSeed)) ++lost[b];
        }
        prev = std::move(cur);
    }

    std::printf("    tras 8 pasos de 5 m, props que DESAPARECEN por franja:\n");
    double worstInner = 0.0;
    for (int k = 0; k < nb; ++k) {
        const double pct = seen[k] ? 100.0 * (double)lost[k] / (double)seen[k] : 0.0;
        std::printf("      %-26s %6lld de %8lld   %5.1f%%\n", names[k], lost[k], seen[k], pct);
        if (k == 0 || k == 2 || k == 4) worstInner = std::max(worstInner, pct);   // franjas INTERIORES
    }

    // ⚠️ LO QUE SE AFIRMA: dentro de una banda, caminar 5 m NO puede hacer desaparecer props. Son el
    // mismo sitio del mundo, y desde que la celda es `(cara, nivel, i, j)` del planeta la identidad
    // no depende de donde este la camara.
    //
    // El umbral es 0,5% y NO es holgura de sobra: medido da **0,0%** (0 de 255 428 en la banda 0).
    // Estuvo en 5% mientras el cuantizador era una rejilla cubica 3D, y a ese umbral una regresion
    // desde cero no se notaria — que es como no tener test.
    //
    //     rejilla cubica, un punto por celda ....... 15,9%
    //     + sobremuestreo a cell/2 .................  3,2%
    //     + celda de CARA DE CUBO ..................  0,0%
    CHECK(worstInner < 0.5,
          "dentro de una banda los props sobreviven al caminar (si no, la rejilla se mueve contigo)");
    // CONTRAPRUEBA: en el BORDE EXTERIOR si tienen que entrar y salir — es el alcance. Si ahi tampoco
    // cambiara nada, este test no estaria midiendo el paso de la camara.
    // ⚠️ Y la contraprueba se mira en el BORDE DE BANDA, no en el exterior. En el exterior el
    // recambio es minusculo (5 m de avance sobre un anillo de 500 m: 2 de 7092 = 0,03%) y un test
    // que dependa de eso es una moneda al aire. En el borde de banda el prop cambia de nivel y por
    // tanto de celda, asi que ahi TIENE que haber recambio — y si no lo hubiera, seria que las
    // bandas no se estan aplicando y todo lo demas de este test no significa nada.
    const double pctEdge = std::max(seen[1] ? 100.0 * (double)lost[1] / (double)seen[1] : 0.0,
                                    seen[3] ? 100.0 * (double)lost[3] / (double)seen[3] : 0.0);
    std::printf("    recambio en el borde de banda: %.1f%% (tiene que ser > 0: ahi cambia el nivel)\n",
                pctEdge);
    CHECK(pctEdge > 0.1, "en el BORDE de banda si cambian (si no, las bandas no se aplican)");
}

void test_prop_lod_mesh() {
    beginTest("prop_lod_mesh");

    // ── (1) COMPATIBILIDAD: detalle 1.0 == la malla histórica ────────────────────────────────────
    //
    // La malla de referencia con estos parámetros (seed 1234, altura 6,5, tronco 0,35, copa 1,
    // segments 8) tenía 276 vértices y 464 triángulos ANTES de que existiera `detail`. Es un número
    // duro a propósito: si el generador cambia, esto salta y hay que decidir a conciencia.
    {
        const Baked full = bakeAt(1234, 1.0f);
        CHECK(full.ok, "detalle 1.0 hornea");
        CHECK(full.verts == 276 && full.tris == 464,
              "detalle 1.0 da la malla HISTORICA (276 vertices, 464 triangulos)");
    }

    // ── (2) LA ENVOLVENTE, en varias semillas ────────────────────────────────────────────────────
    //
    // Tres medidas, y cada una caza un fallo distinto (las tres verificadas por mutación):
    //   ALTURA    — la dimensión que más se nota; un árbol que crece o se hunde al alejarse. Exacta.
    //   ANCHO     — media de ancho y fondo. Tolerancia del 12 %: medido sale 4 %.
    //   PROPORCIÓN— ancho/fondo. Es la única que ve la "pesa": con dos blobs opuestos la MEDIA queda
    //               dentro de tolerancia mientras la silueta es una losa plana.
    double worstH = 0.0, worstW = 0.0, worstAsp = 0.0;
    for (int seed : { 1234, 77, 9001, 424242, 5 }) {
        const Baked ref = bakeAt(seed, kLevels[0]);
        CHECK(ref.ok, "nivel 0 hornea");
        if (!ref.ok) continue;
        const double refH = ref.hi.y - ref.lo.y;
        const double refW = 0.5 * ((ref.hi.x - ref.lo.x) + (ref.hi.z - ref.lo.z));
        CHECK(refH > 1.0 && refW > 0.5, "la referencia tiene tamaño razonable");

        for (int l = 1; l < 3; ++l) {
            const Baked b = bakeAt(seed, kLevels[l]);
            CHECK(b.ok, "el nivel reducido hornea");
            if (!b.ok) continue;
            const double h = b.hi.y - b.lo.y;
            const double w = 0.5 * ((b.hi.x - b.lo.x) + (b.hi.z - b.lo.z));
            worstH = std::max(worstH, std::abs(h - refH) / refH);
            worstW = std::max(worstW, std::abs(w - refW) / refW);
            // ⚠️ LA PROPORCIÓN ancho/fondo, no solo su media. Sin esto el test NO detecta la "pesa":
            // dos blobs en ángulos opuestos dan 3,77 de ancho por 1,86 de fondo, y la MEDIA de los dos
            // sale dentro de tolerancia mientras la silueta es una losa plana. Verificado por mutación:
            // con la media sola, reintroducir la pesa pasaba el banco en verde.
            const double refAsp = (ref.hi.x - ref.lo.x) / std::max(1e-6, (double)(ref.hi.z - ref.lo.z));
            const double asp    = (b.hi.x - b.lo.x)     / std::max(1e-6, (double)(b.hi.z - b.lo.z));
            worstAsp = std::max(worstAsp, std::abs(asp - refAsp) / refAsp);
            // La base sigue en y≈0: si un nivel flotara o se hundiera, el árbol se despegaría del suelo.
            CHECK(std::abs(b.lo.y - ref.lo.y) < 0.02, "la base no se mueve entre niveles");
        }
    }
    std::printf("    envolvente entre niveles: ALTURA %.2f %% · ANCHO %.1f %% · PROPORCION %.1f %%\n",
                100.0 * worstH, 100.0 * worstW, 100.0 * worstAsp);
    CHECK(worstH < 0.005, "la ALTURA es la misma en todos los niveles (<0,5%)");
    CHECK(worstW < 0.12,  "el ANCHO se mantiene dentro del 12%");
    CHECK(worstAsp < 0.35, "y la PROPORCION ancho/fondo no se deforma (nada de siluetas en losa)");

    // ── (3) EL LOD AHORRA DE VERDAD, y de forma monótona ─────────────────────────────────────────
    {
        std::vector<size_t> tris;
        for (float d : kLevels) tris.push_back(bakeAt(1234, d).tris);
        std::printf("    triangulos por nivel: %zu -> %zu -> %zu  (%.1fx menos en el ultimo)\n",
                    tris[0], tris[1], tris[2], (double)tris[0] / (double)std::max<size_t>(tris[2], 1));
        CHECK(tris[1] < tris[0] && tris[2] < tris[1], "cada nivel tiene MENOS triangulos que el anterior");
        CHECK(tris[0] >= tris[2] * 8, "el ultimo nivel ahorra al menos 8x");
        CHECK(tris[2] >= 6, "y no se queda en nada: sigue habiendo un solido");
    }

    // ── (4) DETERMINISMO ────────────────────────────────────────────────────────────────────────
    //
    // Los props se re-siembran cuando la cámara cruza un tramo, así que la malla se re-hornea: si no
    // fuera determinista, un árbol cambiaría de forma al caminar.
    {
        const Baked a = bakeAt(4242, 0.60f);
        const Baked b = bakeAt(4242, 0.60f);
        CHECK(a.tris == b.tris && a.verts == b.verts, "misma semilla y detalle -> misma malla");
        CHECK(a.lo == b.lo && a.hi == b.hi, "y la misma envolvente, bit a bit");
        const Baked c = bakeAt(4243, 0.60f);
        CHECK(c.tris != a.tris || c.lo != a.lo, "semilla distinta -> arbol distinto");
    }

    // ── (5) LÍMITES del parámetro ───────────────────────────────────────────────────────────────
    //
    // El detalle llega por un `float` desde el llamador: un 0 o un valor negativo no pueden producir
    // una malla vacía (un árbol invisible) ni un bucle degenerado.
    for (float d : { 0.0f, -1.0f, 0.01f, 2.0f }) {
        const Baked b = bakeAt(1234, d);
        CHECK(b.ok && b.tris > 0, "un detalle fuera de rango sigue dando una malla valida");
        CHECK(b.hi.y - b.lo.y > 1.0, "y con altura de arbol, no degenerada");
    }
}
