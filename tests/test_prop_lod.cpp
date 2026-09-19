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
#include "core/planet/prop_collider.h"  // propTreeParams / propColliderParts por estilo

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

// ================================================================================================
// BIOMA como identidad de los props (`when: biome == taiga`).
//
// Hasta aqui una capa solo podia decir "humedad entre A y B, temperatura entre C y D": todos los
// arboles del planeta eran la misma capa, y la taiga y la selva recibian el mismo prototipo. El
// bioma se clasifica de lo que el scatter YA muestrea (clima, cota, pendiente) con los MISMOS
// cortes que los materiales del terreno (0,42 de humedad, 11 °C, -12 °C, 0,55 rad), asi "donde el
// suelo se pinta de taiga" y "donde cae una conifera" son la misma frontera.
// ================================================================================================
namespace {
/// Mitad fria y mitad calida: la temperatura depende del signo de la coordenada tangente `t`.
/// Todo lo demas fijo y humedo, asi la UNICA variable entre las dos mitades es el bioma.
class TwoBiomeField : public Haruka::Planet::IPropSphereField {
public:
    glm::vec3 up{0,1,0}, t{1,0,0};
    Haruka::FieldSample sampleAt(const glm::vec3& dir) const override {
        Haruka::FieldSample fs;
        fs.humidity = 0.6f;                              // humedo: bosque o taiga segun la temperatura
        fs.tempC    = glm::dot(dir, t) < 0.0f ? 4.0f : 22.0f;   // taiga | selva_estacional
        fs.elevKm   = 0.3f;
        return fs;
    }
    float heightAt(const glm::vec3&) const override { return 300.0f; }   // fuera de la banda de playa
};
}  // namespace

void test_prop_biome() {
    beginTest("prop_biome");
    using namespace Haruka::Planet;

    // 1. El clasificador, punto a punto, con los cortes del terreno.
    auto cls = [](float T, float h, float elevM, float slope) {
        return std::string(biomeKey(classifyBiome(BiomeSample{T, h, elevM, slope})));
    };
    CHECK(cls( 5.0f, 0.60f, 300.0f, 0.10f) == "taiga",            "frio y humedo = taiga");
    CHECK(cls(15.0f, 0.60f, 300.0f, 0.10f) == "bosque",           "templado y humedo = bosque");
    CHECK(cls(22.0f, 0.60f, 300.0f, 0.10f) == "selva_estacional", "calido y humedo = selva estacional");
    CHECK(cls(25.0f, 0.90f,  50.0f, 0.10f) == "selva",            "calido y muy humedo = selva");
    CHECK(cls(25.0f, 0.15f, 300.0f, 0.10f) == "desierto",         "calido y arido = desierto");
    CHECK(cls(15.0f, 0.50f,   3.0f, 0.10f) == "playa",            "cota 3 m y llano = playa");
    CHECK(cls(-20.0f,0.50f, 300.0f, 0.10f) == "nieve",            "-20 C = nieve");
    CHECK(cls( 2.0f, 0.30f, 3500.0f,0.10f) == "alpino",           "3500 m y frio = alpino");
    CHECK(cls(15.0f, 0.50f, -10.0f, 0.10f) == "ocean",            "bajo el mar = ocean");
    // Contrapruebas: cada corte cambia la clase al cruzarlo, no antes.
    CHECK(cls(11.0f, 0.60f, 300.0f, 0.10f) == "taiga" && cls(11.01f, 0.60f, 300.0f, 0.10f) == "bosque",
          "11 C es el corte taiga|bosque (el mismo que taiga|green del terreno)");
    CHECK(cls(15.0f, 0.419f, 300.0f, 0.10f) == "pradera" && cls(15.0f, 0.42f, 300.0f, 0.10f) == "bosque",
          "0,42 de humedad es el corte pradera|bosque (el mismo que dry|green del terreno)");
    CHECK(cls(15.0f, 0.50f, 300.0f, 0.56f) == "acantilado" && cls(15.0f, 0.50f, 300.0f, 0.54f) == "bosque",
          "0,55 rad es el corte del acantilado (el mismo que el material rock)");
    CHECK(cls(15.0f, 0.50f,   3.0f, 0.30f) == "bosque", "cota 3 m pero inclinado NO es playa");
    CHECK(biomeFromKey("bosqe") == Biome::COUNT && biomeFromKey("bosque") == Biome::TemperateForest,
          "biomeFromKey: la errata no resuelve; la clave buena si");
    for (int b = 0; b < (int)Biome::COUNT; ++b)
        CHECK(biomeFromKey(biomeKey((Biome)b)) == (Biome)b, "biomeKey <-> biomeFromKey ida y vuelta");

    // 2. El `when` entiende `biome`, y el validador puede recorrer sus hojas.
    {
        std::string err;
        auto c = parsePropCond("biome == taiga || biome == boreal", err);
        CHECK(c != nullptr, "when con biome parsea");
        if (c) {
            CHECK( c->eval(PropCondCtx{"", "", "taiga"}),  "biome == taiga con bioma taiga");
            CHECK(!c->eval(PropCondCtx{"", "", "bosque"}), "biome == taiga con bioma bosque");
            CHECK(!c->eval(PropCondCtx{"", "", ""}),       "sin bioma clasificado, == es falso");
            int leaves = 0;
            c->eachCompare([&](const PropCondCompare& cmp) { if (cmp.var == "biome") ++leaves; });
            CHECK(leaves == 2, "eachCompare recorre las dos hojas");
        }
        auto bad = parsePropCond("clima == taiga", err);
        CHECK(bad == nullptr, "una identidad que no existe no parsea");
    }

    // 3. Sobre el scatter esferico: coniferas solo en la mitad fria, palmeras solo en la calida.
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    TwoBiomeField field;
    const glm::dvec3 cam = pc + glm::dvec3(field.up) * (R + 2.0);
    PropScatterParams params; params.radius = R; params.seed = 11u;
    params.lods = { { 800.0f, 12.0f, 0.30f } };

    auto anyLayer = [](const char* mesh, const char* when) {
        PropLayer L; L.mesh = mesh; L.density = 1.0f; L.when = when;
        L.humMin = -1e3f; L.humMax = 1e3f; L.tempMin = -1e3f; L.tempMax = 1e3f;
        L.slopeMin = 0.0f; L.slopeMax = 1.0f;
        return L;
    };
    auto count = [&](const std::vector<ScatteredProp>& props, const char* mesh, bool coldHalf) {
        int n = 0;
        for (const auto& p : props)
            if (p.mesh == mesh && ((glm::dot(p.dir, field.t) < 0.0f) == coldHalf)) ++n;
        return n;
    };

    PropLayerTable table;
    table.layers.push_back(anyLayer("conifer", "biome == taiga"));
    table.layers.push_back(anyLayer("palm",    "biome == selva_estacional || biome == selva"));
    PropScatterStats stats;
    const auto props = scatterPropsNear(field, cam, pc, params, table, &stats);
    const int cCold = count(props, "conifer", true), cWarm = count(props, "conifer", false);
    const int pCold = count(props, "palm", true),    pWarm = count(props, "palm", false);
    std::printf("    coniferas: fria %d · calida %d   palmeras: fria %d · calida %d   (celdas taiga %d, selva_est %d)\n",
                cCold, cWarm, pCold, pWarm,
                stats.cellsByBiome[(int)Biome::Taiga], stats.cellsByBiome[(int)Biome::TropicalSeasonalForest]);
    CHECK(cCold > 100 && pWarm > 100, "las dos capas colocan (si no, lo de abajo no demuestra nada)");
    CHECK(cWarm == 0, "ninguna conifera en la mitad calida");
    CHECK(pCold == 0, "ninguna palmera en la mitad fria");
    CHECK(stats.placedByBiome[0 * (size_t)Biome::COUNT + (size_t)Biome::Taiga] == cCold,
          "placedByBiome cuenta lo mismo que las instancias");

    // Contraprueba: sin `when`, las dos capas salen en las dos mitades (sorteo entre capas).
    PropLayerTable plain;
    plain.layers.push_back(anyLayer("conifer", ""));
    plain.layers.push_back(anyLayer("palm", ""));
    const auto props2 = scatterPropsNear(field, cam, pc, params, plain);
    CHECK(count(props2, "conifer", false) > 100 && count(props2, "palm", true) > 100,
          "contraprueba: sin biome en el when, la conifera invade la mitad calida y la palmera la fria");
}

// ================================================================================================
// VARIANTES por capa y ESTILOS de arbol.
//
// Una capa decide el SITIO; la variante decide QUE arbol va (conifera en la taiga, palmera en la
// playa), y cada variante puede tener varias semillas (mallas distintas). El estilo sale del nombre
// del prototipo, y de el salen la malla, el LOD y el collider — la misma fuente para los tres.
// ================================================================================================
namespace {
struct StyleBake {
    size_t tris = 0, parts = 0, branches = 0;
    glm::vec3 lo{1e9f}, hi{-1e9f};
    bool ok = false;
};
StyleBake bakeStyle(const char* name, int seed, float detail) {
    using namespace Haruka::Planet;
    const PropTreeParams tp = propTreeParams(name);
    Graph g;
    const int node = g.emplaceNode<TreeMeshNode>(seed, tp.height, tp.trunkR, tp.canopy, tp.segments, detail, tp.style);
    g.compile();
    TreeMeshData m;
    StyleBake b;
    if (!bakeTreeMesh(g, node, m)) return b;
    b.ok = true;
    b.tris = m.indices.size() / 3;
    for (const glm::vec3& p : m.positions) { b.lo = glm::min(b.lo, p); b.hi = glm::max(b.hi, p); }
    const TreeSkeleton sk = treeSkeleton(seed, tp.height, tp.trunkR, tp.style);
    b.parts = sk.parts.size();
    for (const auto& p : sk.parts) if (p.kind == TreePartKind::Branch) ++b.branches;
    return b;
}
}  // namespace

void test_prop_variants() {
    beginTest("prop_variants");
    using namespace Haruka::Planet;

    // 1. El estilo sale del nombre; `#k` no lo estropea; lo desconocido es la frondosa.
    CHECK(propTreeStyle("conifer#3") == TreeStyle::Conifer, "conifer#3 -> Conifer");
    CHECK(propTreeStyle("palmera")   == TreeStyle::Palm,    "palmera -> Palm");
    CHECK(propTreeStyle("hierba#0")  == TreeStyle::Grass,   "hierba#0 -> Grass");
    CHECK(propTreeStyle("tree")      == TreeStyle::Broadleaf, "tree -> Broadleaf");
    CHECK(propShapeKind("rock#2")    == PropShapeKind::Rock, "rock#2 sigue siendo roca");

    // 2. La frondosa NO cambia: mismo esqueleto con y sin estilo (compatibilidad con lo ya colocado).
    {
        const TreeSkeleton a = treeSkeleton(4242, 6.5f, 0.35f);
        const TreeSkeleton b = treeSkeleton(4242, 6.5f, 0.35f, TreeStyle::Broadleaf);
        bool same = a.parts.size() == b.parts.size();
        for (size_t i = 0; same && i < a.parts.size(); ++i)
            same = a.parts[i].a == b.parts[i].a && a.parts[i].b == b.parts[i].b &&
                   a.parts[i].radiusA == b.parts[i].radiusA;
        CHECK(same, "Broadleaf por defecto = el esqueleto de siempre, bit a bit");
        const Baked old = bakeAt(4242, 1.0f);
        const StyleBake now = bakeStyle("tree", 4242, 1.0f);
        CHECK(old.ok && now.ok && old.tris == now.tris && old.hi == now.hi,
              "la malla 'tree' es la de siempre (mismos triangulos y misma caja)");
    }

    // 3. Cada estilo hornea, es determinista, y tiene la silueta que dice ser.
    const char* names[] = { "conifer", "palm", "shrub", "dead", "cactus", "grass" };
    std::printf("    estilo      tris   partes ramas   alto    ancho\n");
    for (const char* n : names) {
        auto M = [&](const char* what) { static std::string s; s = std::string(n) + ": " + what; return s.c_str(); };
        const StyleBake a = bakeStyle(n, 77, 1.0f), b = bakeStyle(n, 77, 1.0f), c = bakeStyle(n, 78, 1.0f);
        const float alto = a.hi.y - a.lo.y, ancho = std::max(a.hi.x - a.lo.x, a.hi.z - a.lo.z);
        std::printf("    %-10s %5zu   %2zu    %2zu    %5.2f   %5.2f\n", n, a.tris, a.parts, a.branches, alto, ancho);
        CHECK(a.ok && a.tris > 0, M("hornea"));
        CHECK(a.tris == b.tris && a.hi == b.hi, M("determinista"));
        CHECK(a.tris != c.tris || a.hi != c.hi, M("otra semilla, otra malla (contraprueba)"));
        const PropTreeParams tp = propTreeParams(n);
        CHECK(alto > tp.height * 0.8f && alto < tp.height * 1.6f, M("la altura es la nominal"));
        // LOD: la envolvente no salta (misma regla que la frondosa).
        const StyleBake lo = bakeStyle(n, 77, 0.45f);
        const float dAlto = std::abs((lo.hi.y - lo.lo.y) - alto) / alto;
        CHECK(lo.ok && lo.tris < a.tris, M("el LOD bajo tiene menos triangulos"));
        CHECK(dAlto < 0.15f, M("el LOD bajo conserva la altura (<15%)"));
    }
    {
        const StyleBake con = bakeStyle("conifer", 77, 1.0f), bro = bakeStyle("tree", 77, 1.0f);
        const float wCon = (con.hi.x - con.lo.x) / (con.hi.y - con.lo.y);
        const float wBro = (bro.hi.x - bro.lo.x) / (bro.hi.y - bro.lo.y);
        CHECK(wCon < wBro, "la conifera es mas esbelta (ancho/alto) que la frondosa");
        CHECK(bakeStyle("palm", 77, 1.0f).branches == 0, "la palmera no tiene ramas");
        CHECK(bakeStyle("cactus", 77, 1.0f).branches == 2, "el cactus tiene dos brazos");
        CHECK(bakeStyle("grass", 77, 1.0f).parts == 0, "la hierba no tiene esqueleto");
        CHECK(propColliderParts("grass#1", 77).empty(), "la hierba no colisiona");
        CHECK(!propColliderParts("shrub", 77).empty(), "el arbusto si (contraprueba)");
        // Collider y malla del mismo estilo: la altura del fuste del collider es la del esqueleto
        // de ESE estilo, no la de la frondosa.
        const auto pc = propColliderParts("palm", 77);
        const TreeSkeleton sk = treeSkeleton(77, propTreeParams("palm").height, propTreeParams("palm").trunkR, TreeStyle::Palm);
        CHECK(pc.size() == 1 && std::abs(pc[0].length - sk.parts[0].length()) < 1e-4f,
              "el collider de la palmera es su fuste entero, no el 62 % de una frondosa");
    }

    // 4. Variantes en el scatter: la capa 'tree' pone coniferas (3 semillas) en la taiga y palmeras
    //    en la selva estacional; la mitad sin variante elegible queda para la siguiente capa.
    {
        const double R = 6371000.0;
        const glm::dvec3 pc(0.0);
        TwoBiomeField field;
        const glm::dvec3 cam = pc + glm::dvec3(field.up) * (R + 2.0);
        PropScatterParams params; params.radius = R; params.seed = 5u;
        params.lods = { { 800.0f, 12.0f, 0.30f } };

        PropLayer tree; tree.name = "tree"; tree.mesh = "tree"; tree.density = 1.0f; tree.claimRadius = 0.3f;
        tree.humMin = -1e3f; tree.humMax = 1e3f; tree.tempMin = -1e3f; tree.tempMax = 1e3f; tree.slopeMax = 1.0f;
        PropVariant con; con.mesh = "conifer"; con.when = "biome == taiga"; con.seeds = 3; con.weight = 1.0f;
        PropVariant plm; plm.mesh = "palm"; plm.when = "biome == selva_estacional"; plm.seeds = 1;
        tree.variants = { con, plm };
        PropLayer rock = tree; rock.name = "rock"; rock.mesh = "rock"; rock.variants.clear(); rock.seeds = 2;
        rock.when = "biome == selva_estacional";   // la roca solo en la mitad calida, para la contraprueba
        PropLayerTable table; table.layers = { tree, rock };

        const auto names = tree.prototypeNames();
        CHECK(names.size() == 4 && names[0] == "conifer#0" && names[2] == "conifer#2" && names[3] == "palm",
              "prototypeNames: conifer#0..2 y palm");

        PropScatterStats stats;
        const auto props = scatterPropsNear(field, cam, pc, params, table, &stats);
        int cCold[3] = {0,0,0}, cWarm = 0, pCold = 0, pWarm = 0, rockN = 0;
        for (const auto& p : props) {
            const bool cold = glm::dot(p.dir, field.t) < 0.0f;
            if (p.mesh.rfind("conifer#", 0) == 0) { if (cold) ++cCold[p.mesh.back() - '0']; else ++cWarm; }
            else if (p.mesh == "palm") { if (cold) ++pCold; else ++pWarm; }
            else if (p.mesh.rfind("rock", 0) == 0) ++rockN;
        }
        std::printf("    coniferas frias #0/#1/#2 = %d/%d/%d · calidas %d · palmeras frias %d calidas %d · rocas %d · sin_variante %d\n",
                    cCold[0], cCold[1], cCold[2], cWarm, pCold, pWarm, rockN, stats.noVariant[0]);
        CHECK(cCold[0] > 100 && cCold[1] > 100 && cCold[2] > 100, "las tres semillas de conifera salen en la taiga");
        CHECK(cWarm == 0 && pCold == 0, "ninguna conifera en la selva ni palmera en la taiga");
        CHECK(pWarm > 100, "palmeras en la selva estacional");
        CHECK(stats.noVariant[0] == 0, "con las dos variantes, toda celda de la capa tiene especie");
        // La roca (when selva_estacional, mismo peso) se SORTEA con la palmera en la mitad calida:
        // ni se la come el arbol por ir detras, ni se lo come ella por ir delante.
        int rockCold = 0;
        for (const auto& p : props) if (p.mesh.rfind("rock", 0) == 0 && glm::dot(p.dir, field.t) < 0.0f) ++rockCold;
        CHECK(rockN > 100 && rockCold == 0, "la roca sale sorteada en la mitad calida y solo ahi");
        CHECK(std::abs((float)rockN / (float)std::max(1, pWarm) - 1.0f) < 0.25f,
              "a igual peso, roca y palmera se reparten la mitad calida ~50/50");

        // Contraprueba: quitar la palmera deja la mitad calida SIN variante -> la roca entra ahi.
        PropLayerTable t2 = table; t2.layers[0].variants = { con };
        PropScatterStats s2;
        const auto props2 = scatterPropsNear(field, cam, pc, params, t2, &s2);
        int rock2 = 0, palm2 = 0;
        for (const auto& p : props2) { if (p.mesh.rfind("rock", 0) == 0) ++rock2; if (p.mesh == "palm") ++palm2; }
        CHECK(s2.noVariant[0] > 100 && rock2 > rockN * 1.5f && palm2 == 0,
              "contraprueba: sin variante elegible la celda queda para la otra capa (la roca dobla)");
    }

    // 5. JSON de ida y vuelta con variantes.
    {
        PropLayerTable t;
        PropLayer L; L.name = "tree"; L.mesh = "tree"; L.seeds = 4;
        PropVariant v; v.mesh = "conifer"; v.when = "biome == taiga"; v.weight = 2.0f; v.seeds = 3;
        L.variants = { v };
        t.layers = { L };
        nlohmann::json raw; raw["propLayers"] = t.toJSON();
        const PropLayerTable back = PropLayerTable::fromJSON(raw);
        CHECK(back.layers.size() == 1 && back.layers[0].seeds == 4 && back.layers[0].variants.size() == 1 &&
              back.layers[0].variants[0].mesh == "conifer" && back.layers[0].variants[0].seeds == 3 &&
              back.layers[0].variants[0].weight == 2.0f && back.layers[0].variants[0].when == "biome == taiga",
              "toJSON/fromJSON conserva seeds y variantes");
    }
}

// ================================================================================================
// DENSIDAD que no es ruido blanco: manchas (fbm anclado al mundo), banda de cota y sorteo entre
// capas. La medida es el INDICE DE DISPERSION (varianza/media de conteos por cuadrado de 100 m):
// un Poisson da ~1; manchas dan >1 (cuadrados vacios y cuadrados llenos). Es una cifra, no "se ve
// mas natural".
// ================================================================================================
namespace {
/// Cota que sube con la coordenada tangente: 0 m en t=-1, 3000 m en t=+1. Todo lo demas fijo.
class RampField : public Haruka::Planet::IPropSphereField {
public:
    glm::vec3 up{0,1,0}, t{1,0,0};
    float heightM(const glm::vec3& dir) const { return 3000.0f * (glm::dot(dir, t) * 1e4f + 0.5f); } // ±0.05e-3 -> 0..3000
    Haruka::FieldSample sampleAt(const glm::vec3& dir) const override {
        Haruka::FieldSample fs; fs.humidity = 0.6f; fs.tempC = 15.0f;
        fs.elevKm = heightM(dir) * 0.001f;
        return fs;
    }
    float heightAt(const glm::vec3& dir) const override { return heightM(dir); }
};
}  // namespace

namespace {
/// Clima seco (matorral) en todas partes; el mapa de bosque pintado solo en la mitad t>0.
class DryMappedField : public Haruka::Planet::IPropSphereField {
public:
    glm::vec3 t{1,0,0};
    Haruka::FieldSample sampleAt(const glm::vec3&) const override {
        Haruka::FieldSample fs; fs.humidity = 0.12f; fs.tempC = 8.0f; fs.elevKm = 0.3f; return fs;
    }
    float heightAt(const glm::vec3&) const override { return 300.0f; }
    float mapDensityAt(const glm::vec3& dir, const std::string&) const override {
        return glm::dot(dir, t) > 0.0f ? 1.0f : 0.0f;
    }
};
}  // namespace

void test_prop_map_biome() {
    beginTest("prop_map_biome");
    using namespace Haruka::Planet;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    DryMappedField field;
    const glm::dvec3 cam = pc + glm::dvec3(0,1,0) * (R + 2.0);
    PropScatterParams params; params.radius = R; params.seed = 9u;
    params.lods = { { 800.0f, 12.0f, 0.30f } };

    PropLayer L; L.name = "tree"; L.mesh = "tree"; L.density = 1.0f; L.densityMap = "bosque.png";
    L.humMin = -1e3f; L.humMax = 1e3f; L.tempMin = -1e3f; L.tempMax = 1e3f; L.slopeMax = 1.0f;
    PropVariant con; con.mesh = "conifer"; con.when = "biome == taiga";
    L.variants = { con };
    auto run = [&](bool force) {
        PropLayer M = L; M.mapForcesBiome = force;
        PropLayerTable table; table.layers = { M };
        PropScatterStats st;
        const auto props = scatterPropsNear(field, cam, pc, params, table, &st);
        int mapped = 0, unmapped = 0;
        for (const auto& p : props) (glm::dot(p.dir, field.t) > 0.0f ? mapped : unmapped)++;
        std::printf("    mapForcesBiome=%d: %d coniferas en la mitad pintada, %d fuera · sin_variante %d · celdas matorral %d\n",
                    (int)force, mapped, unmapped, st.noVariant[0], st.cellsByBiome[(int)Biome::Shrubland]);
        return std::make_pair(mapped, unmapped);
    };
    const auto with = run(true), without = run(false);
    CHECK(with.first > 100 && with.second == 0, "con el mapa mandando, coniferas SOLO donde esta pintado el bosque");
    CHECK(without.first == 0 && without.second == 0, "contraprueba: sin mapForcesBiome el clima (matorral) no deja ninguna");
    // El mapa no convierte el mar ni el acantilado.
    CHECK(L.biomeFor("acantilado", 1.0f, 8.0f) == "acantilado" && L.biomeFor("ocean", 1.0f, 8.0f) == "ocean",
          "el mapa no fuerza bosque en acantilado ni en el mar");
    PropLayer F = L; F.mapForcesBiome = true;
    CHECK(F.biomeFor("matorral", 1.0f, 8.0f) == "taiga" && F.biomeFor("matorral", 1.0f, 15.0f) == "bosque" &&
          F.biomeFor("matorral", 1.0f, 25.0f) == "selva_estacional" && F.biomeFor("matorral", 0.2f, 15.0f) == "matorral",
          "el bosque forzado sigue la temperatura, y con el mapa < 0,5 no se fuerza");
}

void test_prop_density() {
    beginTest("prop_density");
    using namespace Haruka::Planet;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    PropScatterParams params; params.radius = R; params.seed = 3u;
    params.lods = { { 800.0f, 12.0f, 0.30f } };
    auto openLayer = [](const char* mesh, float density) {
        PropLayer L; L.name = mesh; L.mesh = mesh; L.density = density;
        L.humMin = -1e3f; L.humMax = 1e3f; L.tempMin = -1e3f; L.tempMax = 1e3f; L.slopeMax = 1.0f;
        L.claimRadius = 0.5f;   // EXCLUSIVA: entra en el sorteo (0 = libre, sotobosque)
        return L;
    };
    // Coordenadas tangentes (m) de un prop respecto al jugador.
    auto tangent = [&](const glm::vec3& dir, const glm::vec3& up, const glm::vec3& t) {
        const glm::vec3 b = glm::cross(up, t);
        return glm::vec2(glm::dot(dir, t), glm::dot(dir, b)) * (float)R;
    };

    // 1. Manchas: indice de dispersion en cuadrados de 100 m dentro del disco de 800 m.
    {
        FlatField field;
        const glm::vec3 up(0,1,0), t(1,0,0);
        const glm::dvec3 cam = pc + glm::dvec3(up) * (R + 2.0);
        auto dispersion = [&](float clumpM, size_t& n) {
            PropLayerTable table; PropLayer L = openLayer("tree", 0.5f); L.clumpM = clumpM; table.layers = { L };
            const auto props = scatterPropsNear(field, cam, pc, params, table);
            n = props.size();
            std::vector<int> q(16 * 16, 0);
            int used = 0;
            for (const auto& p : props) {
                const glm::vec2 xy = tangent(p.dir, up, t);
                if (std::abs(xy.x) >= 500.0f || std::abs(xy.y) >= 500.0f) continue;   // cuadrado interior 1 km
                const int qx = (int)((xy.x + 500.0f) / 62.5f), qy = (int)((xy.y + 500.0f) / 62.5f);
                ++q[qy * 16 + qx]; ++used;
            }
            double mean = 0.0; for (int c : q) mean += c; mean /= (double)q.size();
            double var = 0.0; for (int c : q) var += (c - mean) * (c - mean); var /= (double)q.size();
            return mean > 0.0 ? var / mean : 0.0;
        };
        size_t nWhite = 0, nClump = 0;
        const double dWhite = dispersion(0.0f, nWhite), dClump = dispersion(150.0f, nClump);
        std::printf("    ruido blanco: %zu props, var/media %.2f · manchas 150 m: %zu props, var/media %.2f\n",
                    nWhite, dWhite, nClump, dClump);
        CHECK(nWhite > 1000 && nClump > 500, "los dos scatters colocan");
        CHECK(dWhite < 1.4, "sin manchas la distribucion es ~Poisson (var/media < 1,4)");
        CHECK(dClump > 2.0, "con manchas hay claros y espesuras (var/media > 2)");
        CHECK(nClump > nWhite * 0.3 && nClump < nWhite * 0.8,
              "la mancha (media 0,5) rebaja la densidad efectiva: entre el 30 y el 80 % del blanco");
        // Determinismo: el mismo scatter dos veces es el mismo conjunto (la mancha va anclada al mundo).
        size_t n2 = 0; const double d2 = dispersion(150.0f, n2);
        CHECK(n2 == nClump && d2 == dClump, "las manchas son deterministas");
    }

    // 2. Banda de cota: la capa con elevMaxM=1500 no sube de ahi (mas el degradado).
    {
        RampField field;
        const glm::dvec3 cam = pc + glm::dvec3(field.up) * (R + 2.0);
        PropLayer L = openLayer("tree", 1.0f); L.elevMaxM = 1500.0f; L.elevFeatherM = 60.0f;
        PropLayerTable table; table.layers = { L };
        const auto props = scatterPropsNear(field, cam, pc, params, table);
        float hi = -1e9f; int below = 0;
        for (const auto& p : props) { hi = std::max(hi, p.heightM); if (p.heightM < 1400.0f) ++below; }
        std::printf("    banda de cota: %zu props, el mas alto a %.0f m\n", props.size(), hi);
        CHECK(below > 500, "hay props bajo la linea");
        CHECK(hi <= 1560.0f, "ninguno por encima de 1500 m + degradado");
        PropLayerTable open; open.layers = { openLayer("tree", 1.0f) };
        float hi2 = -1e9f;
        for (const auto& p : scatterPropsNear(field, cam, pc, params, open)) hi2 = std::max(hi2, p.heightM);
        CHECK(hi2 > 2500.0f, "contraprueba: sin banda, llegan arriba");
    }

    // 4. perCell: cuatro matas por celda, todas dentro de la celda (a menos de cell*sqrt(2) de la primera).
    {
        FlatField field;
        const glm::vec3 up(0,1,0), t(1,0,0);
        const glm::dvec3 cam = pc + glm::dvec3(up) * (R + 2.0);
        PropLayerTable one; one.layers = { openLayer("grass", 0.5f) };
        PropLayerTable four = one; four.layers[0].perCell = 4;
        const auto p1 = scatterPropsNear(field, cam, pc, params, one);
        const auto p4 = scatterPropsNear(field, cam, pc, params, four);
        std::printf("    perCell: 1 -> %zu props · 4 -> %zu props\n", p1.size(), p4.size());
        CHECK(p4.size() == p1.size() * 4, "perCell=4 da exactamente 4x instancias");
        float worst = 0.0f; size_t distinctSeeds = 0;
        std::unordered_set<uint32_t> seeds;
        for (size_t i = 0; i + 3 < p4.size(); i += 4) {
            const glm::vec2 a = tangent(p4[i].dir, up, t);
            for (int k = 1; k < 4; ++k) {
                worst = std::max(worst, glm::length(tangent(p4[i + k].dir, up, t) - a));
                seeds.insert(p4[i + k].cellSeed);
            }
        }
        distinctSeeds = seeds.size();
        std::printf("    hermanas: la mas lejana a %.1f m de la primera (celda de 12 m) · %zu semillas distintas\n", worst, distinctSeeds);
        CHECK(worst < 12.0f * 1.5f, "las hermanas quedan dentro de la celda");
        CHECK(distinctSeeds > p1.size() * 2, "cada hermana tiene su semilla (escala/giro propios)");
    }

    // 5. maxDistM: la capa no pasa de la banda que cabe en su alcance.
    {
        FlatField field;
        const glm::vec3 up(0,1,0), t(1,0,0);
        const glm::dvec3 cam = pc + glm::dvec3(up) * (R + 2.0);
        PropScatterParams far = params;
        far.lods = { { 500.0f, 12.0f, 0.30f }, { 1500.0f, 40.0f, 0.30f } };
        PropLayerTable tbl; tbl.layers = { openLayer("grass", 1.0f) }; tbl.layers[0].maxDistM = 600.0f;
        const auto props = scatterPropsNear(field, cam, pc, far, tbl);
        float worst = 0.0f;
        for (const auto& p : props) worst = std::max(worst, glm::length(tangent(p.dir, up, t)));
        std::printf("    maxDistM 600 con bandas 500/1500: %zu props, el mas lejano a %.0f m\n", props.size(), worst);
        CHECK(props.size() > 1000 && worst < 520.0f, "con maxDistM=600 solo la banda de 500 m instala (jitter de celda aparte)");
        tbl.layers[0].maxDistM = 0.0f;
        float worst2 = 0.0f;
        for (const auto& p : scatterPropsNear(field, cam, pc, far, tbl)) worst2 = std::max(worst2, glm::length(tangent(p.dir, up, t)));
        CHECK(worst2 > 1000.0f, "contraprueba: sin alcance llega a la banda de 1500 m");
    }

    // 3. Sorteo entre capas: a igual peso ~50/50; con 3:1, ~3:1. Antes la primera se lo quedaba todo.
    {
        FlatField field;
        const glm::dvec3 cam = pc + glm::dvec3(0,1,0) * (R + 2.0);
        auto ratio = [&](float dA, float dB, size_t& total) {
            PropLayerTable table; table.layers = { openLayer("a", dA), openLayer("b", dB) };
            const auto props = scatterPropsNear(field, cam, pc, params, table);
            size_t a = 0, b = 0;
            for (const auto& p : props) (p.mesh == "a" ? a : b)++;
            total = props.size();
            return b > 0 ? (double)a / (double)b : 1e9;
        };
        size_t n1 = 0, n3 = 0;
        const double r1 = ratio(1.0f, 1.0f, n1), r3 = ratio(0.9f, 0.3f, n3);
        std::printf("    sorteo: 1:1 -> a/b %.2f (%zu props) · 0,9:0,3 -> a/b %.2f (%zu props)\n", r1, n1, r3, n3);
        CHECK(std::abs(r1 - 1.0) < 0.15, "a igual peso, mitad y mitad");
        CHECK(std::abs(r3 - 3.0) < 0.5, "con 0,9 y 0,3 la primera triplica a la segunda (no se la come entera)");
        CHECK(n3 < n1 && n3 > n1 * 0.8, "la ocupacion la fija el peso mayor (0,9): baja poco respecto a 1,0");

        // Capa LIBRE (claimRadius 0): no entra en el sorteo, asi que anadir hierba no quita arboles.
        PropLayerTable solo; solo.layers = { openLayer("tree", 0.5f) };
        PropLayerTable conHierba = solo; PropLayer g = openLayer("grass", 0.8f); g.claimRadius = 0.0f; conHierba.layers.push_back(g);
        PropLayerTable conHierbaExcl = solo; conHierbaExcl.layers.push_back(openLayer("grass", 0.8f));
        auto trees = [&](const PropLayerTable& t) { size_t n = 0; for (const auto& p : scatterPropsNear(field, cam, pc, params, t)) if (p.mesh == "tree") ++n; return n; };
        const size_t tSolo = trees(solo), tLibre = trees(conHierba), tExcl = trees(conHierbaExcl);
        std::printf("    arboles: solos %zu · con hierba libre %zu · con hierba exclusiva %zu\n", tSolo, tLibre, tExcl);
        CHECK(tLibre == tSolo, "la hierba libre no quita ni un arbol");
        CHECK(tExcl < tSolo * 0.75, "contraprueba: la misma hierba como exclusiva quita mas de un cuarto de los arboles");
    }
}

// ================================================================================================
// COSTE del scatter con la tabla del JUEGO (7 capas, variantes, manchas, hierba x6 por celda).
// Andoni: "gasta demasiada RAM". Aqui va en numeros lo que cuesta cada instancia en CPU (tres
// copias: ScatteredProp del scatter, InstancedObject del registro, InstanceDataFloat del bucket) y
// cuanto tarda el scatter. Lo que no se mide aqui es la GPU: eso esta en el banco RHI
// (`instancing: anillo`). Los presupuestos son los del juego: 200 000 instancias de tope.
// ================================================================================================
#include "game/instanced_object.h"
#include "renderer/gpu_instancing.h"
#include <chrono>

void test_prop_scatter_cost() {
    beginTest("prop_scatter_cost");
    using namespace Haruka::Planet;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const glm::dvec3 cam = pc + glm::dvec3(0,1,0) * (R + 2.0);
    PropScatterParams params; params.radius = R; params.seed = 3u;   // bandas por defecto: 1/3/6 km

    // Campo con clima de matorral (el del spawn) y un "mapa de bosque" a 1 en todas partes.
    class SpawnField : public IPropSphereField {
    public:
        Haruka::FieldSample sampleAt(const glm::vec3&) const override {
            Haruka::FieldSample fs; fs.humidity = 0.12f; fs.tempC = 14.5f; fs.elevKm = 1.0f; return fs;
        }
        float heightAt(const glm::vec3&) const override { return 1000.0f; }
        float mapDensityAt(const glm::vec3&, const std::string&) const override { return 1.0f; }
    } field;

    // La tabla de Survival (scenes/main.scene), transcrita: si alli cambia, esto mide otra cosa.
    auto L = [](const char* name, const char* mesh, float density, float claim, const char* when) {
        PropLayer l; l.name = name; l.mesh = mesh; l.density = density; l.claimRadius = claim; l.when = when;
        return l;
    };
    PropLayerTable t;
    { PropLayer r = L("rock", "rock", 0.4f, 0.4f, "!(biome == bosque || biome == taiga || biome == selva || biome == selva_estacional)");
      r.slopeMin = 0.2f; r.seeds = 3; r.clumpM = 120.0f; r.clumpAmount = 0.9f; t.layers.push_back(r); }
    { PropLayer a = L("tree", "tree", 0.9f, 0.3f, ""); a.humMin = 0.15f; a.tempMin = -2.0f; a.tempMax = 32.0f; a.slopeMax = 0.6f;
      a.seeds = 3; a.densityMap = "bosque.png"; a.mapForcesBiome = true; a.clumpM = 220.0f; a.clumpAmount = 0.85f; a.elevMaxM = 2400.0f;
      PropVariant c; c.mesh = "conifer"; c.when = "biome == taiga || biome == boreal"; c.seeds = 3;
      PropVariant f; f.mesh = "tree"; f.when = "biome == bosque || biome == mediterraneo"; f.seeds = 3;
      PropVariant p; p.mesh = "palm"; p.when = "biome == selva || biome == playa"; p.seeds = 2;
      a.variants = { c, f, p }; t.layers.push_back(a); }
    { PropLayer s = L("shrub", "shrub", 0.25f, 0.0f, "biome == matorral || biome == pradera"); s.seeds = 2; s.clumpM = 150.0f; s.clumpAmount = 0.8f; t.layers.push_back(s); }
    { PropLayer d = L("dead", "dead", 0.03f, 0.3f, "biome == matorral"); d.seeds = 2; t.layers.push_back(d); }
    PropLayer g = L("grass", "grass", 0.7f, 0.0f, "biome == matorral || biome == bosque"); g.seeds = 2; g.perCell = 6; g.clumpM = 80.0f; g.clumpAmount = 0.5f; g.maxDistM = 1000.0f;
    t.layers.push_back(g);

    auto run = [&](const PropLayerTable& table, double& ms, size_t& bytesScatter) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto props = scatterPropsNear(field, cam, pc, params, table);
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        bytesScatter = props.capacity() * sizeof(ScatteredProp);
        for (const auto& p : props) if (p.mesh.size() > 15) bytesScatter += p.mesh.capacity() + 1;   // fuera del SSO
        return props.size();
    };
    double msFull = 0, msNoGrass = 0, msPer1 = 0; size_t bFull = 0, bNoGrass = 0, bPer1 = 0;
    const size_t nFull = run(t, msFull, bFull);
    PropLayerTable noGrass = t; noGrass.layers.pop_back();
    const size_t nNoGrass = run(noGrass, msNoGrass, bNoGrass);
    PropLayerTable per1 = t; per1.layers.back().perCell = 1;
    const size_t nPer1 = run(per1, msPer1, bPer1);

    const size_t perInst = sizeof(ScatteredProp) + sizeof(Haruka::InstancedObject) + sizeof(Haruka::InstanceDataFloat);
    std::printf("    tabla del juego: %zu instancias en %.0f ms · sin hierba %zu en %.0f ms · hierba x1 %zu en %.0f ms\n",
                nFull, msFull, nNoGrass, msNoGrass, nPer1, msPer1);
    std::printf("    por instancia: ScatteredProp %zu B + InstancedObject %zu B + InstanceDataFloat %zu B = %zu B "
                "-> %.1f MB en CPU para %zu (el scatter solo: %.1f MB)\n",
                sizeof(ScatteredProp), sizeof(Haruka::InstancedObject), sizeof(Haruka::InstanceDataFloat), perInst,
                (double)(perInst * nFull) / 1048576.0, nFull, (double)bFull / 1048576.0);
    CHECK(nFull > 20000, "el scatter coloca (si no, no mide nada)");
    CHECK(nFull < 200000, "no se llega al tope de 200 000 instancias (a partir de ahi el scatter se corta y faltan props lejos)");
    CHECK(perInst * nFull < 64u * 1048576u, "las tres copias de las instancias caben en 64 MB");
    // perCell NO multiplica el tiempo: las celdas son lo caro (muestrear el campo), no emitir.
    CHECK(msFull < msPer1 * 1.5 + 5.0, "hierba x6 no cuesta 1,5x mas de scatter que x1 (el coste es por celda)");
    CHECK(nFull > nPer1 + 4 * (nFull - nNoGrass) / 6, "contraprueba: x6 si multiplica las INSTANCIAS de hierba");
    CHECK(msFull < 400.0, "el scatter de la tabla del juego baja de 400 ms (era ~121 ms con 2 capas)");
}
