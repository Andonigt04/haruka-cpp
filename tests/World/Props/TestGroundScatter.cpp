// ================================================================================================
// LA CUBIERTA DENSA, LA PARTE DE CPU (World/Props/PxLod.h, LoadController.h, GroundScatter.h).
//
// Lo que este banco fija
// ----------------------
//  1. LA LEY DE PÍXELES conserva la cobertura: los que quedan × lo que se ensanchan = 1 hasta el
//     fundido, `keep` no crece con la distancia y el alcance sale del umbral (no es un parámetro).
//     La cuenta de hilos que lanza la CPU es una cota de lo que se queda el shader en todo el grupo.
//  2. EL LAZO DE MS llega al presupuesto con un coste lineal en el detalle, se queda quieto dentro de
//     la banda muerta y baja con el TOPE DURO aunque sobren ms.
//  3. EL PLAN está fijo al mundo (mover la cámara medio metro no recoloca ningún grupo), no pasa del
//     alcance, recorta lo que hay detrás de la cámara, cruza aristas del cubo sin huecos ni
//     duplicados y, al llegar al tope de tandas, pierde lo LEJANO.
//  4. LAS ZONAS DESTRUIDAS de la cubierta: una dentro de otra no se guarda, una que contiene a otras
//     se las traga, y guardar/restaurar las conserva.
// ================================================================================================
#include "Common/TestCommon.h"
#include "Haruka/World/Props/GrassSystem.h"
#include "Haruka/World/Props/GroundScatter.h"
#include "Haruka/World/Props/LoadController.h"
#include "Haruka/World/Props/MassGround.h"
#include "Haruka/World/Props/PxLod.h"
#include "Haruka/World/Terrain/CubeSphere.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <tuple>

namespace {
using namespace Haruka::World::Props;

GroundScatterPlanInput planAt(const glm::dvec3& up, double R, float thrPx) {
    GroundScatterPlanInput in;
    in.planetC = glm::dvec3(0.0);
    in.planetR = R;
    in.groundM = 0.0;
    in.camPos  = up * (R + 1.7);
    in.px.focalPx = 935.0f; in.px.widthM = 0.03f; in.px.thrPx = thrPx; in.px.widenMax = 3.5f;
    in.perGroup = 30;
    in.level = groundScatterLevel(R, 1.0);
    return in;
}
using Key = std::tuple<uint32_t, uint32_t, uint32_t>;
std::set<Key> groupsOf(const GroundScatterPlan& p) {
    std::set<Key> s;
    for (const auto& w : p.work) s.insert({ w.faceK & 7u, w.cx, w.cy });
    return s;
}
} // namespace

void test_px_lod_law() {
    beginTest("cover_px_lod_law");
    PxLod L; L.focalPx = 935.0f; L.widthM = 0.03f; L.thrPx = 1.0f; L.widenMax = 3.5f;
    const float full = pxFullM(L), reach = pxReachM(L);
    CHECK(std::abs(full - 0.03f * 935.0f) < 1e-3f, "dentro de pxFullM se dibujan todos (el elemento mide ≥ thrPx)");
    CHECK(std::abs(reach - 3.5f * full) < 1e-3f, "el alcance sale del umbral: widenMax × pxFullM");
    float prev = 2.0f;
    bool monotone = true, conserves = true;
    for (float d = 0.0f; d < reach * 1.1f; d += reach / 400.0f) {
        const float k = pxKeep(L, d);
        if (k > prev + 1e-6f) monotone = false;
        prev = k;
        // Cobertura en pantalla: los que quedan × lo que ensancha cada uno. 1 hasta el fundido.
        if (d < 0.75f * reach && std::abs(k * pxWiden(L, d) - 1.0f) > 1e-4f) conserves = false;
    }
    CHECK(monotone, "keep no crece con la distancia (si creciera, la cuenta de hilos no sería cota)");
    CHECK(conserves, "keep × widen = 1 antes del fundido: el conjunto se hace basto, no se aclara");
    CHECK(pxKeep(L, reach) == 0.0f && pxKeep(L, reach * 2.0f) == 0.0f, "más allá del alcance no queda nada");
    // Bajar el umbral (más detalle) alarga el alcance: un mando, no un radio.
    PxLod fine = L; fine.thrPx = 0.5f;
    CHECK(pxReachM(fine) > reach * 1.99f, "umbral a la mitad → el doble de alcance");
    // La cuenta de la CPU con la distancia más cercana cubre a todos los hilos que el shader quiere.
    bool bound = true;
    for (float dn = 0.0f; dn < reach; dn += 3.7f) {
        const uint32_t n = pxGroupThreads(L, 30, dn);
        for (float d = dn; d < dn + 2.0f; d += 0.1f)
            for (uint32_t k = 0; k < 30; ++k)
                if (pxKeeps(L, 30, k, d) && k >= n) bound = false;
    }
    CHECK(bound, "pxGroupThreads(dCercana) ≥ lo que el shader se queda en el grupo");
    // El detalle del controlador: 1 = umbral mínimo, 0 = máximo, geométrico entre medias.
    CHECK(std::abs(pxThrPx(1.0f, 0.6f, 8.0f) - 0.6f) < 1e-5f && std::abs(pxThrPx(0.0f, 0.6f, 8.0f) - 8.0f) < 1e-5f,
          "pxThrPx: detalle 1 → thrMin, detalle 0 → thrMax");
    CHECK(std::abs(pxThrPx(0.5f, 0.6f, 8.0f) - std::sqrt(0.6f * 8.0f)) < 1e-4f, "pxThrPx: geométrico");
}

void test_load_controller() {
    beginTest("cover_load_controller");
    // Coste lineal en el detalle: 8 ms a detalle 1. Presupuesto 2 ms → equilibrio en 0,25.
    LoadController c; c.budgetMs = 2.0; c.detail = 1.0f;
    double t = 0.0;
    for (int i = 0; i < 6000; ++i, t += 1.0 / 60.0) c.update(8.0 * c.detail, t);
    CHECK(std::abs(c.detail - 0.25f) < 0.03f, "llega al presupuesto (coste lineal → detalle = budget/coste)");
    char msg[128]; std::snprintf(msg, sizeof msg, "detalle final %.3f", c.detail);
    CHECK(c.detail > 0.2f, msg);
    // Dentro de la banda muerta no se mueve.
    LoadController q; q.budgetMs = 2.0; q.detail = 0.5f;
    q.update(2.05, 0.0);   // semilla: detalle → 0,5·2/2,05
    const float seeded = q.detail;
    for (int i = 0; i < 600; ++i) q.update(2.05, 1.0 + i / 60.0);
    CHECK(std::abs(q.detail - seeded) < 1e-6f, "dentro de la banda muerta el detalle no baila");
    // El tope duro baja aunque sobren ms, y también sin medida (GL sin timestamps).
    LoadController o; o.budgetMs = 2.0; o.detail = 0.8f;
    o.update(0.5, 0.0);
    const float before = o.detail;
    for (int i = 0; i < 120; ++i) o.update(0.5, 1.0 + i / 60.0, /*over*/true);
    CHECK(o.detail < before, "con el buffer lleno el detalle baja aunque haya holgura");
    LoadController g; g.detail = 0.8f;
    for (int i = 0; i < 120; ++i) g.update(0.0, i / 60.0, true);
    CHECK(g.detail < 0.8f, "sin medida de GPU solo actúa el tope duro, y actúa");
    LoadController h; h.detail = 0.8f;
    for (int i = 0; i < 120; ++i) h.update(0.0, i / 60.0, false);
    CHECK(h.detail == 0.8f, "sin medida y sin tope, quieto");
}

void test_ground_scatter_plan() {
    beginTest("cover_plan");
    const double R = 6371000.0;
    const glm::dvec3 up = glm::normalize(glm::dvec3(0.31, 0.83, 0.47));
    GroundScatterPlanInput in = planAt(up, R, 1.5f);
    const GroundScatterPlan p = planGroundScatter(in);
    CHECK(p.groups > 100 && !p.work.empty(), "hay grupos alrededor de la cámara");
    CHECK(!p.clipped, "sin tope en un plan normal");

    // Todos dentro del alcance, con los hilos de la ley, y en tandas de 64 bien cortadas.
    const glm::dvec3 rel = in.camPos - in.planetC;
    const int cellsFace = 1 << in.level;
    bool inReach = true, threadsOk = true, batchesOk = true;
    uint64_t sum = 0;
    std::set<Key> seen;
    for (const auto& w : p.work) {
        const uint32_t face = w.faceK & 7u, k0 = w.faceK >> 3;
        if (k0 % 64u != 0u || k0 >= w.threads) batchesOk = false;
        if (k0 == 0u) sum += w.threads;
        const glm::dvec3 c = Haruka::World::Terrain::cubeFaceToDir((Haruka::Tools::PlanetFace)face,
                               -1.0 + (w.cx + 0.5) * 2.0 / cellsFace, -1.0 + (w.cy + 0.5) * 2.0 / cellsFace) * R;
        const glm::dvec3 d = c - rel;
        const double dh = glm::length(d - up * glm::dot(d, up));
        if (dh > p.reachM + 2.0) inReach = false;
        if (w.threads > in.perGroup) threadsOk = false;
        seen.insert({ face, w.cx, w.cy });
    }
    CHECK(inReach, "ningún grupo más allá del alcance (+ media diagonal)");
    CHECK(threadsOk, "ningún grupo pide más hilos que elementos tiene");
    CHECK(batchesOk, "las tandas empiezan en múltiplos de 64 y dentro del grupo");
    CHECK(sum == p.threads && seen.size() == p.groups, "las cifras del plan cuadran con su lista");

    // Fijo al mundo: medio metro de paso no recoloca ningún grupo (la identidad es la celda).
    GroundScatterPlanInput in2 = in;
    const glm::dvec3 east = glm::normalize(glm::cross(up, glm::dvec3(0, 1, 0)));
    in2.camPos += east * 0.5;
    const auto a = groupsOf(p), b = groupsOf(planGroundScatter(in2));
    size_t common = 0;
    for (const auto& k : a) common += b.count(k);
    CHECK(common > a.size() * 95 / 100, "medio metro de paso conserva casi todos los grupos (fijos al mundo)");

    // El frustum: mirando a +E, lo que queda detrás no se planifica.
    GroundScatterPlanInput fr = in;
    const glm::vec3 fwd = glm::vec3(east), upv = glm::vec3(up);
    const glm::vec3 right = glm::normalize(glm::cross(fwd, upv));
    const float tv = 0.577f, th = tv * 1.78f;
    fr.planes[0] = glm::normalize(fwd * th + right); fr.planes[1] = glm::normalize(fwd * th - right);
    fr.planes[2] = glm::normalize(fwd * tv + upv);   fr.planes[3] = glm::normalize(fwd * tv - upv);
    const GroundScatterPlan pf = planGroundScatter(fr);
    CHECK(pf.groups < p.groups / 2, "con frustum se planifica menos de la mitad del disco");
    bool behind = false;
    for (const auto& w : pf.work) {
        const glm::dvec3 c = Haruka::World::Terrain::cubeFaceToDir((Haruka::Tools::PlanetFace)(w.faceK & 7u),
                               -1.0 + (w.cx + 0.5) * 2.0 / cellsFace, -1.0 + (w.cy + 0.5) * 2.0 / cellsFace) * R;
        if (glm::dot(c - rel, east) < -10.0) behind = true;
    }
    CHECK(!behind, "nada a más de 10 m por detrás de la cámara");

    // Tope de tandas: se pierde lo lejano, no lo cercano.
    GroundScatterPlanInput cl = in; cl.maxWork = 200;
    const GroundScatterPlan pc = planGroundScatter(cl);
    CHECK(pc.clipped && pc.work.size() <= 200, "al tope, se marca y se respeta");
    const auto near = groupsOf(pc);
    bool nearKept = true;
    for (const auto& w : p.work) {
        const glm::dvec3 c = Haruka::World::Terrain::cubeFaceToDir((Haruka::Tools::PlanetFace)(w.faceK & 7u),
                               -1.0 + (w.cx + 0.5) * 2.0 / cellsFace, -1.0 + (w.cy + 0.5) * 2.0 / cellsFace) * R;
        const glm::dvec3 d = c - rel;
        if (glm::length(d - up * glm::dot(d, up)) < 3.0 && !near.count({ w.faceK & 7u, w.cx, w.cy })) nearKept = false;
    }
    CHECK(nearKept, "al tope, los grupos bajo los pies siguen");
}

void test_ground_scatter_cross_face() {
    beginTest("cover_plan_cross_face");
    // Cámara a 20 m de la arista entre dos caras (lx ≈ 1 en su cara): el plan tiene que tomar
    // grupos de las dos, sin duplicados, y cubrir cualquier punto dentro del alcance.
    const double R = 6371000.0;
    using Haruka::Tools::PlanetFace;
    const double edgeLx = 1.0 - 20.0 / (R * 0.785);
    const glm::dvec3 up = Haruka::World::Terrain::cubeFaceToDir(PlanetFace::FRONT, edgeLx, 0.1);
    GroundScatterPlanInput in = planAt(up, R, 3.0f);
    const GroundScatterPlan p = planGroundScatter(in);
    std::set<uint32_t> faces;
    std::set<Key> groups;
    size_t firstBatches = 0;
    for (const auto& w : p.work) {
        faces.insert(w.faceK & 7u);
        if ((w.faceK >> 3) == 0u) { groups.insert({ w.faceK & 7u, w.cx, w.cy }); ++firstBatches; }
    }
    CHECK(faces.size() == 2, "junto a la arista se siembra en las dos caras");
    CHECK(groups.size() == firstBatches, "ningún grupo duplicado al cruzar la arista");
    // Sin huecos: puntos al azar dentro de 0,8 del alcance caen en un grupo planificado.
    const int cellsFace = 1 << in.level;
    const glm::dvec3 e = glm::normalize(glm::cross(up, glm::dvec3(0, 1, 0)));
    const glm::dvec3 n = glm::cross(up, e);
    int holes = 0, probes = 0;
    for (int i = 0; i < 400; ++i) {
        const double ang = i * 2.399963, rr = p.reachM * 0.8 * std::sqrt((i + 0.5) / 400.0);
        const glm::dvec3 q = glm::normalize(up * R + (e * std::cos(ang) + n * std::sin(ang)) * rr);
        PlanetFace f; double lx, ly;
        Haruka::World::Terrain::dirToCubeFaceClosed(q, f, lx, ly);
        const uint32_t cx = (uint32_t)std::floor((lx + 1.0) * 0.5 * cellsFace);
        const uint32_t cy = (uint32_t)std::floor((ly + 1.0) * 0.5 * cellsFace);
        ++probes;
        if (!groups.count({ (uint32_t)f, cx, cy })) ++holes;
    }
    char msg[128]; std::snprintf(msg, sizeof msg, "sin huecos al cruzar la arista (%d de %d puntos sin grupo)", holes, probes);
    CHECK(holes == 0, msg);
}

void test_cover_zones() {
    beginTest("cover_zones");
    GrassSystem g;
    const glm::dvec3 c(100.0, 0.0, 0.0);
    g.destroyAt(c, 5.0);
    g.destroyAt(c + glm::dvec3(1.0, 0.0, 0.0), 2.0);   // dentro de la primera
    CHECK(g.zones().size() == 1, "una zona dentro de otra no se guarda");
    g.destroyAt(c + glm::dvec3(30.0, 0.0, 0.0), 3.0);
    g.destroyAt(c + glm::dvec3(30.0, 1.0, 0.0), 2.0);
    CHECK(g.zones().size() == 2, "zonas separadas se guardan aparte");
    g.destroyAt(c + glm::dvec3(15.0, 0.0, 0.0), 40.0);  // contiene a las dos
    CHECK(g.zones().size() == 1 && g.zones()[0].radius == 40.0, "la que contiene a otras se las traga");
    GrassSystem h;
    h.restoreZones(g.serializeZones());
    CHECK(h.zones().size() == 1 && h.zones()[0].center == g.zones()[0].center, "guardar/restaurar conserva las zonas");
}

// ── LAS MATAS DE LOS BROTES, SOBRE EL SUELO DIBUJADO: el plan de CPU (MassGround.h) ───────────────
//
// El compute coloca la mata k en `l0 + J·offset` (cara del cubo) en vez de invertir la proyección en
// float (~1 m a radio terrestre). Aquí se mide ese atajo contra la inversa EXACTA en doble, para
// matas en toda la celda y en brotes repartidos por la esfera: el error tiene que ser de milímetros.
// Y el tope: los brotes MÁS CERCANOS entran primero, y ninguno pasa del alcance.
void test_mass_ground_plan() {
    beginTest("cover_mass_ground_plan");
    namespace T = Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const glm::dvec3 camDir = glm::normalize(glm::dvec3(0.3, 0.8, -0.5));
    const glm::dvec3 cam = camDir * (R + 1.7);
    std::vector<MassBurst> bursts;
    const glm::dvec3 e1 = glm::normalize(glm::cross(camDir, glm::dvec3(0, 0, 1))), e2 = glm::cross(camDir, e1);
    for (int i = 0; i < 400; ++i) {   // brotes en un disco de ~300 m alrededor de la cámara
        const double a = i * 2.39996, r = 15.0 * std::sqrt((double)i);
        MassBurst b;
        b.dir = glm::vec3(glm::normalize(camDir * R + e1 * (r * std::cos(a)) + e2 * (r * std::sin(a))));
        b.heightM = (float)(0.02 * r); b.cellM = 12.0f; b.seed = 1234u + (uint32_t)i; b.count = 64;
        b.scaleMin = 0.8f; b.scaleMax = 1.2f;
        bursts.push_back(b);
    }
    MassGroundPlanInput in;
    in.bursts = &bursts; in.planetC = pc; in.planetR = R; in.camPos = cam; in.reachM = 250.0; in.budget = 64u * 150u;
    uint32_t total = 0;
    const std::vector<MassGroundJob> jobs = planMassGround(in, total);

    double worstM = 0.0, lastD = -1e300, maxD = 0.0;   // la distancia al BORDE de la celda puede ser negativa
    bool ordered = true, basesOk = true;
    uint32_t base = 0;
    for (const MassGroundJob& j : jobs) {
        const MassBurst& b = bursts[j.a[0]];
        const glm::dvec3 c = glm::normalize(glm::dvec3(b.dir)) * (R + (double)b.heightM);
        const double d = glm::length(c - cam) - 0.65 * b.cellM;
        ordered = ordered && d >= lastD - 1e-9; lastD = d; maxD = std::max(maxD, d);
        basesOk = basesOk && j.a[1] == base; base += j.a[2];
        glm::vec3 u, v; massTangents(b.dir, u, v);
        for (uint32_t k = 0; k < j.a[2]; ++k) {
            const MassElement e = massElement(b.seed, k, b.cellM, b.scaleMin, b.scaleMax);
            // La verdad: la raíz en el plano del brote, invertida EXACTA en doble.
            const glm::dvec3 root = c + glm::dvec3(u) * (double)e.offset.x + glm::dvec3(v) * (double)e.offset.y;
            Haruka::Tools::PlanetFace f; double lx, ly;
            T::dirToCubeFaceClosed(glm::normalize(root), f, lx, ly);
            // El atajo del compute.
            const double ax = j.l[0] + (double)(j.j[0] * e.offset.x + j.j[1] * e.offset.y);
            const double ay = j.l[1] + (double)(j.j[2] * e.offset.x + j.j[3] * e.offset.y);
            const glm::dvec3 got = T::cubeFaceToDir(f, ax, ay) * R, want = T::cubeFaceToDir(f, lx, ly) * R;
            worstM = std::max(worstM, glm::length(got - want));
        }
    }
    std::printf("    %zu brotes, %u matas (tope %u) · más lejano %.1f m · peor error de posición %.4f m\n",
                jobs.size(), total, in.budget, maxD, worstM);
    CHECK(!jobs.empty() && total <= in.budget, "entra lo que cabe en el tope");
    CHECK(total + 64u > in.budget, "y el tope se llena (hay más brotes de los que caben)");
    CHECK(ordered, "los más cercanos primero");
    CHECK(basesOk, "con las bases contiguas");
    CHECK(maxD <= in.reachM, "ninguno pasa del alcance");
    CHECK(worstM < 0.005, "l0 + J·offset cae a menos de 5 mm de la inversa exacta en doble");
}

HARUKA_REGISTER_TESTS(
    { "cover_px_lod_law", test_px_lod_law },
    { "cover_load_controller", test_load_controller },
    { "cover_plan", test_ground_scatter_plan },
    { "cover_plan_cross_face", test_ground_scatter_cross_face },
    { "cover_zones", test_cover_zones },
    { "cover_mass_ground_plan", test_mass_ground_plan });
