// ================================================================================================
// PEÑONES ENTERRADOS de la roca (`bakeRockMesh`, cullFrac).
//
// Qué se afirma
// -------------
// Los peñones que caen ENTEROS dentro del blob principal ya no se generan. La afirmación fuerte no
// es "ahorra triángulos" —eso es contarlos— sino **que no se ve la diferencia desde ningún ángulo**.
//
// Cómo se comprueba, y por qué así
// --------------------------------
// Comparar los vértices no sirve: la malla CAMBIA a propósito. Lo que tiene que ser idéntico es lo
// que la superficie ENSEÑA, así que el banco lanza rayos ortográficos desde muchas direcciones
// —como si la mirara una cámara desde cada una— y compara la PRIMERA intersección. Si el primer
// impacto coincide rayo a rayo, la silueta y todo lo visible coinciden: lo que se quitó estaba
// tapado por definición.
//
// ⚠️ CONTRAPRUEBA OBLIGATORIA. Un test de igualdad pasa solo si el recorte no hiciera NADA (quitar
// cero peñones también da rayos idénticos). Por eso el banco comprueba las dos mitades:
//   · que el recorte de verdad quita peñones (si no, no hay nada que verificar), y
//   · que el test SABE FALLAR: con `cullFrac` absurdo (2.0) se recortan peñones VISIBLES y los rayos
//     TIENEN que diferir. Sin esa mitad, "todos los rayos coinciden" no demostraría nada.
// ================================================================================================
#include "test_common.h"
#include "tools/procgraph/prop_mesh.h"

#include <cmath>
#include <cstdio>
#include <vector>

using Haruka::Tools::ProcGraph::TreeMeshData;
using Haruka::Tools::ProcGraph::bakeRockMesh;

namespace {

/// Möller–Trumbore. Devuelve t > 0 del impacto, o -1.
float rayTri(const glm::vec3& o, const glm::vec3& d,
             const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 e1 = b - a, e2 = c - a;
    const glm::vec3 p  = glm::cross(d, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-12f) return -1.0f;          // rayo paralelo al plano
    const float inv = 1.0f / det;
    const glm::vec3 tv = o - a;
    const float u = glm::dot(tv, p) * inv;
    if (u < -1e-6f || u > 1.0f + 1e-6f) return -1.0f;
    const glm::vec3 q = glm::cross(tv, e1);
    const float v = glm::dot(d, q) * inv;
    if (v < -1e-6f || u + v > 1.0f + 1e-6f) return -1.0f;
    const float t = glm::dot(e2, q) * inv;
    return t > 1e-6f ? t : -1.0f;
}

/// Primer impacto del rayo contra la malla entera (-1 = no toca).
float firstHit(const TreeMeshData& m, const glm::vec3& o, const glm::vec3& d) {
    float best = -1.0f;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const float t = rayTri(o, d, m.positions[m.indices[i]],
                                     m.positions[m.indices[i + 1]],
                                     m.positions[m.indices[i + 2]]);
        if (t > 0.0f && (best < 0.0f || t < best)) best = t;
    }
    return best;
}

/// Rayos ortográficos desde `nDir` direcciones sobre una cuadrícula `grid`×`grid`.
/// Devuelve el nº de rayos cuyo PRIMER impacto difiere entre las dos mallas.
int compareViews(const TreeMeshData& a, const TreeMeshData& b, int nDir, int grid, float radius) {
    int diff = 0;
    for (int k = 0; k < nDir; ++k) {
        // Espiral de Fibonacci: direcciones repartidas, no un anillo que se perdería los polos.
        const float z   = 1.0f - 2.0f * ((float)k + 0.5f) / (float)nDir;
        const float rr  = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float phi = 2.39996323f * (float)k;
        const glm::vec3 dir(rr * std::cos(phi), z, rr * std::sin(phi));
        // Base del plano de imagen.
        glm::vec3 up = std::abs(dir.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        const glm::vec3 ex = glm::normalize(glm::cross(up, dir));
        const glm::vec3 ey = glm::cross(dir, ex);
        for (int i = 0; i < grid; ++i)
            for (int j = 0; j < grid; ++j) {
                const float u = ((float)i + 0.5f) / (float)grid * 2.0f - 1.0f;
                const float v = ((float)j + 0.5f) / (float)grid * 2.0f - 1.0f;
                const glm::vec3 o = (ex * u + ey * v) * radius - dir * (radius * 3.0f);
                const float ta = firstHit(a, o, dir);
                const float tb = firstHit(b, o, dir);
                const bool ha = ta > 0.0f, hb = tb > 0.0f;
                if (ha != hb || (ha && std::abs(ta - tb) > 1e-4f)) ++diff;
            }
    }
    return diff;
}

} // namespace

void test_rock_interior() {
    beginTest("rock_interior");

    const int   kSeeds = 24;
    const float kScale = 1.0f, kSquash = 0.72f;

    // --- 1. El recorte QUITA algo (si no, el resto del banco no verificaría nada) ----------------
    size_t triRef = 0, triCull = 0;
    int    seedsChanged = 0;
    for (int s = 0; s < kSeeds; ++s) {
        const auto ref  = bakeRockMesh(s, kScale, kSquash, 0.0f);    // sin recortar
        const auto cull = bakeRockMesh(s, kScale, kSquash);          // producción
        triRef  += ref.indices.size()  / 3;
        triCull += cull.indices.size() / 3;
        if (cull.indices.size() != ref.indices.size()) ++seedsChanged;
    }
    printf("    %d semillas: %zu -> %zu triangulos (%.1f%% menos), %d semillas afectadas\n",
           kSeeds, triRef, triCull,
           100.0 * (double)(triRef - triCull) / (double)triRef, seedsChanged);
    CHECK(triCull < triRef, "el recorte quita triangulos (si no, no hay nada que verificar)");

    // --- 2. Y NO SE VE: mismo primer impacto en todos los rayos ---------------------------------
    int totalDiff = 0, totalRays = 0;
    const int nDir = 26, grid = 11;
    for (int s = 0; s < kSeeds; ++s) {
        const auto ref  = bakeRockMesh(s, kScale, kSquash, 0.0f);
        const auto cull = bakeRockMesh(s, kScale, kSquash);
        totalDiff += compareViews(ref, cull, nDir, grid, 1.1f * kScale * 1.6f);
        totalRays += nDir * grid * grid;
    }
    printf("    %d rayos desde %d direcciones: %d con impacto distinto\n",
           totalRays, nDir, totalDiff);
    CHECK(totalDiff == 0, "lo que se quito estaba TAPADO: la superficie visible es identica");

    // --- 3. CONTRAPRUEBA: el test sabe fallar ---------------------------------------------------
    //
    // Con `cullFrac = 2.0` el criterio recorta peñones que SÍ asoman. Si los rayos siguieran
    // coincidiendo, el punto 2 estaría midiendo el vacío.
    int badDiff = 0;
    for (int s = 0; s < kSeeds; ++s) {
        const auto ref = bakeRockMesh(s, kScale, kSquash, 0.0f);
        const auto bad = bakeRockMesh(s, kScale, kSquash, 2.0f);
        badDiff += compareViews(ref, bad, nDir, grid, 1.1f * kScale * 1.6f);
    }
    printf("    CONTRAPRUEBA (cullFrac=2.0, recorta lo visible): %d rayos distinos\n", badDiff);
    CHECK(badDiff > 0, "CONTRAPRUEBA: recortar penones visibles SI cambia lo que se ve");

    // --- 4. El collider no se entera ------------------------------------------------------------
    //
    // `rockExtent` es la envolvente que usa la física y sale del blob PRINCIPAL, que no se toca.
    // Si el recorte la moviera, la roca se vería en un sitio y se chocaría en otro.
    const auto e = Haruka::Tools::ProcGraph::rockExtent(kScale, kSquash);
    CHECK(std::abs(e.half.x - 1.1f * kScale) < 1e-6f,
          "la envolvente del collider no cambia con el recorte");
}
