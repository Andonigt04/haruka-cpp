// ================================================================================================
// LA HIERBA COMO SISTEMA PROPIO (World/Props/GrassSystem.h + MassBurst.h): hermano de PropSystem
// (los dos heredan de PropSystemBase), un brote por celda que la GPU expande.
//
// Lo que este banco fija
// ----------------------
//  1. DOS SISTEMAS = UN SCATTER: cada sistema re-siembra solo SUS capas (`layerOwned`, reparto por
//     `isMassLayer`). El de PropSystem da los MISMOS árboles —mismas semillas— que el scatter de
//     toda la tabla, y los brotes de GrassSystem representan EXACTAMENTE las matas que antes eran
//     instancias. Si no, separar los sistemas habría cambiado el bosque o raleado el prado.
//  2. LA MEMORIA BAJA DE VERDAD: muchos menos registros que matas (es para lo que existe).
//  3. EL ESTADO: destruir por zona alcanza EXACTAMENTE las matas de la esfera (contra la posición de
//     la función compartida con la GPU), destruir dos veces no cuenta dos, la pisada no resucita lo
//     destruido, se levanta sola, y guardar/restaurar conserva lo destruido y descarta lo pisado.
// ================================================================================================
#include "Common/TestCommon.h"
#include "Haruka/World/Props/GrassSystem.h"
#include "Haruka/World/Props/PropScatter.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace {
class FlatMassField : public Haruka::World::Props::IPropSphereField {
public:
    Haruka::World::Planet::FieldSample sampleAt(const glm::vec3&) const override { return {}; }
    float heightAt(const glm::vec3&) const override { return 3.0f; }
};

Haruka::World::Props::PropLayer openLayer(const char* mesh, float density, float claim) {
    Haruka::World::Props::PropLayer L; L.name = mesh; L.mesh = mesh; L.density = density;
    L.humMin = -1e3f; L.humMax = 1e3f; L.tempMin = -1e3f; L.tempMax = 1e3f; L.slopeMax = 1.0f;
    L.claimRadius = claim;
    return L;
}
}  // namespace

void test_mass_scatter_equivalence() {
    beginTest("grass_scatter_equivalence");
    using namespace Haruka::World::Props;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const glm::dvec3 up = glm::normalize(glm::dvec3(0.2, 0.9, 0.4));
    const glm::dvec3 cam = pc + up * (R + 2.0);

    PropLayerTable table;
    table.layers.push_back(openLayer("tree", 0.4f, 0.5f));          // EXCLUSIVA: sigue siendo prop
    PropLayer g = openLayer("grass", 0.8f, 0.0f);                    // LIBRE: pasa a la capa masiva
    g.perCell = 6; g.maxDistM = 1000.0f; g.scaleMin = 0.8f; g.scaleMax = 1.3f;
    table.layers.push_back(g);

    PropScatterParams params;
    params.radius = R; params.seed = 11u;
    params.lods = { { 600.0f, 12.0f, 0.30f } };
    params.grassNearM = 40.0f; params.grassNearMul = 50.0f; params.grassNearFadeM = 60.0f;
    params.maxProps = 5000000;

    FlatMassField field;
    // El reparto REAL: el mismo predicado que usan los dos sistemas.
    CHECK(!isMassLayer(table.layers[0]) && isMassLayer(table.layers[1]),
          "el arbol (reclama sitio, colisiona) es de PropSystem y la hierba de GrassSystem");
    PropScatterParams pAll = params;                                   // la tabla entera, mata a mata
    PropScatterParams pProps = params, pGrass = params;
    pProps.layerOwned = { 1, 0 };                                      // PropSystem: !isMassLayer
    pGrass.layerOwned = { 0, 1 }; pGrass.massBursts = true;            // GrassSystem: isMassLayer, brotes
    const auto allP   = scatterPropsNear(field, cam, pc, pAll, table);
    const auto propsP = scatterPropsNear(field, cam, pc, pProps, table);
    const auto grassP = scatterPropsNear(field, cam, pc, pGrass, table);

    std::multiset<uint32_t> treesOld, treesNew;
    uint64_t grassOld = 0, grassNew = 0, bursts = 0, grassInstNew = 0, grassInProps = 0, treesInGrass = 0;
    for (const auto& p : allP) {
        if (p.mesh == "tree") treesOld.insert(p.cellSeed);
        else if (p.mesh == "grass") ++grassOld;
    }
    for (const auto& p : propsP) {
        if (p.mesh == "tree") treesNew.insert(p.cellSeed);
        else ++grassInProps;
    }
    for (const auto& p : grassP) {
        if (p.mesh == "grass") {
            if (p.burst > 0) { ++bursts; grassNew += p.burst; }
            else ++grassInstNew;
        } else ++treesInGrass;
    }
    std::printf("    arboles %zu -> %zu · matas de hierba %llu -> %llu en %llu brotes (%.1f matas/registro)\n",
                treesOld.size(), treesNew.size(), (unsigned long long)grassOld, (unsigned long long)grassNew,
                (unsigned long long)bursts, bursts ? (double)grassNew / (double)bursts : 0.0);

    CHECK(grassOld > 1000, "CONTRAPRUEBA: el scatter coloca hierba (si no, la equivalencia no prueba nada)");
    CHECK(!treesOld.empty() && treesOld == treesNew, "el scatter de PropSystem da los MISMOS arboles que el de toda la tabla");
    CHECK(grassInProps == 0 && treesInGrass == 0, "cada sistema coloca solo SUS capas");
    CHECK(grassInstNew == 0, "en GrassSystem ninguna mata va suelta (todas van en brotes)");
    CHECK(grassNew == grassOld, "los brotes representan EXACTAMENTE las matas que antes eran instancias");
    CHECK(bursts * 4 < grassNew, "y con muchos menos registros que matas (para eso existe)");
}

void test_mass_state() {
    beginTest("grass_state");
    using namespace Haruka::World::Props;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const glm::vec3 dir = glm::normalize(glm::vec3(0.3f, 0.8f, 0.5f));

    GrassSystem m;
    MassBurst b;
    b.proto = 0; b.dir = dir; b.heightM = 2.0f; b.cellM = 12.0f; b.seed = 0xC0FFEEu; b.count = 400;
    b.scaleMin = 0.8f; b.scaleMax = 1.2f;
    MassBurst b2 = b; b2.seed = 0xBEEFu; b2.dir = glm::normalize(dir + glm::vec3(0.0f, 0.0f, 1e-5f));   // ~64 m al lado
    m.setBursts({ b, b2 }, pc, R, pc + glm::dvec3(dir) * R);

    // Las matas caen dentro de su celda (90 % del lado) y la función es determinista.
    double worstOff = 0.0;
    for (uint32_t k = 0; k < b.count; ++k) {
        const MassElement e = massElement(b.seed, k, b.cellM, b.scaleMin, b.scaleMax);
        const MassElement e2 = massElement(b.seed, k, b.cellM, b.scaleMin, b.scaleMax);
        worstOff = std::max(worstOff, (double)std::max(std::abs(e.offset.x), std::abs(e.offset.y)));
        if (e.offset != e2.offset || e.scale != e2.scale) { CHECK(false, "massElement es determinista"); return; }
    }
    CHECK(worstOff <= 0.45 * b.cellM + 1e-4, "las matas caen dentro del 90 % de su celda");

    // ── Destruir por zona: EXACTAMENTE las de la esfera ──
    const glm::dvec3 c0 = m.elementWorld(b, 0);
    const double r = 3.0;
    int expected = 0;
    for (uint32_t k = 0; k < b.count; ++k) if (glm::length(m.elementWorld(b, k) - c0) <= r) ++expected;
    for (uint32_t k = 0; k < b2.count; ++k) if (glm::length(m.elementWorld(b2, k) - c0) <= r) ++expected;
    const int got = m.destroyAt(c0, r);
    std::printf("    zona de %.1f m: %d matas destruidas (esperadas %d)\n", r, got, expected);
    CHECK(expected > 3, "CONTRAPRUEBA: la esfera contiene matas");
    CHECK(got == expected, "destruir por zona alcanza EXACTAMENTE las matas de la esfera");
    CHECK(m.destroyAt(c0, r) == 0, "destruir otra vez la misma zona no cuenta dos veces");

    // ── Por mata ──
    const GrassSystem::Hit h = m.nearest(c0, 10.0);
    CHECK(h.hit && glm::length(h.pos - c0) > r - 1e-6, "la mas cercana viva queda FUERA de la zona destruida");
    CHECK(m.destroyElement(h.seed, h.element), "destruir una mata concreta");
    CHECK(!m.destroyElement(h.seed, h.element), "destruirla otra vez devuelve false");

    // ── Pisada: no resucita lo destruido y se levanta sola ──
    const int flat = m.flattenAt(c0, 6.0, /*until*/10.0);
    CHECK(flat > 0, "la pisada tumba matas vivas");
    CHECK(m.destroyAt(c0, r) == 0, "la pisada no resucita lo destruido");
    m.sweepFlat(5.0);
    CHECK(m.flattenAt(c0, 6.0, 10.0) == 0, "antes de vencer el reloj siguen tumbadas");
    m.sweepFlat(11.0);
    CHECK(m.flattenAt(c0, 6.0, 20.0) == flat, "al vencer se levantan todas (y se pueden volver a pisar)");

    // ── Guardar / restaurar: solo lo destruido ──
    const auto saved = m.serialize();
    uint64_t destroyedBits = 0;
    for (const auto& d : saved) for (uint32_t w : d.words) {
        CHECK((w & 0x55555555u) == 0u, "lo guardado no lleva pisadas (bit bajo de cada par)");
        destroyedBits += (uint64_t)__builtin_popcount(w);
    }
    CHECK(destroyedBits == (uint64_t)expected + 1, "lo guardado es exactamente lo destruido");
    GrassSystem m2;
    m2.setBursts({ b, b2 }, pc, R, pc + glm::dvec3(dir) * R);
    m2.restore(saved);
    CHECK(m2.destroyAt(c0, r) == 0, "restaurado: la zona sigue destruida");
    const GrassSystem::Hit h2 = m2.nearest(c0, 10.0);
    CHECK(h2.hit && !(h2.seed == h.seed && h2.element == h.element), "restaurado: la mata cortada sigue cortada");
}

HARUKA_REGISTER_TESTS(
    { "grass_scatter_equivalence", test_mass_scatter_equivalence },
    { "grass_state", test_mass_state });
