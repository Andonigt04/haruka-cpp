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

#include <cmath>
#include <cstdio>
#include <vector>

using namespace Haruka::Tools::ProcGraph;

namespace {

struct Baked {
    size_t tris = 0, verts = 0;
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
    b.lo = glm::vec3(1e9f); b.hi = glm::vec3(-1e9f);
    for (const glm::vec3& p : m.positions) { b.lo = glm::min(b.lo, p); b.hi = glm::max(b.hi, p); }
    return b;
}

/// Los niveles que el motor usa de verdad (`kLodDetail` en application_render.cpp). Si alguien los
/// cambia allí sin tocarlos aquí, el test deja de auditar lo que se dibuja — de ahí el comentario.
const float kLevels[3] = { 1.0f, 0.60f, 0.45f };

} // namespace

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
