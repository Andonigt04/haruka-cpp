/**
 * @file TestCaves.cpp
 * @brief Las cuevas: siembra determinista, mapas horneados, conectividad GARANTIZADA y campo.
 *
 * El invariante duro es el de §A.3 del plan: **el aire de un sistema es UNA sola pieza**. No se
 * confía a que la intersección de dos mapas conexos salga conexa —no lo es— sino al post-proceso,
 * y aquí se mide cuántas componentes había ANTES para que ese número no sea una suposición.
 *
 * Cada comprobación lleva su contraprueba: un test que sólo mira lo que quiere ver no mide nada.
 */
#include "Common/TestCommon.h"
#include "Haruka/World/Vox/CaveSystem.h"
#include "Haruka/World/Terrain/BakeUtil.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace Haruka;

namespace {

constexpr double kR = 6371000.0;   // radio de la Tierra: la escala en la que se calibró todo

/// Una caja de prueba: la primera celda con cueva que se encuentre barriendo direcciones.
bool findBox(uint32_t seed, World::Vox::CaveBox& out, glm::dvec3* outDir = nullptr) {
    World::Vox::caveSetAutoProbability(0.14f);   // la siembra esta a 0 por defecto (cuevas a mano): el banco la enciende
    for (int i = 0; i < 4000; ++i) {
        const double a = (double)i * 0.017, b = (double)i * 0.0031;
        const glm::dvec3 d = glm::normalize(glm::dvec3(std::cos(a) * std::cos(b), std::sin(b),
                                                       std::sin(a) * std::cos(b)));
        if (World::Vox::caveBoxAt(seed, d, kR, out)) { if (outDir) *outDir = d; return true; }
    }
    return false;
}

/// Componentes conexas (26 vecinos) del aire de un volumen. Recalculado AQUÍ, sin fiarse de la
/// estadística que devuelve el horneado: si el test usara el mismo recuento que el código que
/// prueba, no probaría nada.
int componentesDeAire(const World::Vox::CaveVolume& v) {
    const int n = v.n;
    std::vector<uint8_t> vis((size_t)n * n * n, 0);
    std::vector<size_t> stack;
    int comps = 0;
    auto air = [&](size_t k) { return v.d[k] < 0.0f; };
    for (size_t s = 0; s < v.d.size(); ++s) {
        if (!air(s) || vis[s]) continue;
        ++comps;
        stack.clear(); stack.push_back(s); vis[s] = 1;
        while (!stack.empty()) {
            const size_t c = stack.back(); stack.pop_back();
            const int z = (int)(c / ((size_t)n * n)), y = (int)((c / n) % n), x = (int)(c % n);
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy, nz = z + dz;
                        if (nx < 0 || ny < 0 || nz < 0 || nx >= n || ny >= n || nz >= n) continue;
                        const size_t k = (size_t)nz * n * n + (size_t)ny * n + nx;
                        if (!air(k) || vis[k]) continue;
                        vis[k] = 1; stack.push_back(k);
                    }
        }
    }
    return comps;
}

/// Sizes of the air components (same flood fill as above), largest first.
std::vector<int> airComponentSizes(const World::Vox::CaveVolume& v) {
    const int n = v.n;
    std::vector<uint8_t> vis((size_t)n * n * n, 0);
    std::vector<size_t> stack; std::vector<int> sizes;
    for (size_t s0 = 0; s0 < v.d.size(); ++s0) {
        if (!(v.d[s0] < 0.0f) || vis[s0]) continue;
        int count = 0; stack.assign(1, s0); vis[s0] = 1;
        while (!stack.empty()) {
            const size_t c = stack.back(); stack.pop_back(); ++count;
            const int z = (int)(c / ((size_t)n * n)), y = (int)((c / n) % n), x = (int)(c % n);
            for (int dz = -1; dz <= 1; ++dz) for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy, nz = z + dz;
                if (nx < 0 || ny < 0 || nz < 0 || nx >= n || ny >= n || nz >= n) continue;
                const size_t k = (size_t)nz * n * n + (size_t)ny * n + nx;
                if (!(v.d[k] < 0.0f) || vis[k]) continue;
                vis[k] = 1; stack.push_back(k);
            }
        }
        sizes.push_back(count);
    }
    std::sort(sizes.rbegin(), sizes.rend());
    return sizes;
}

} // namespace

void test_caves_bake() {
    beginTest("cuevas: siembra, mapas y conectividad");

    // ── (1) SIEMBRA: determinista, rara, y la misma para dos observadores ───────────────────────
    World::Vox::CaveBox box; glm::dvec3 dirHit(0.0);
    const bool found = findBox(1234u, box, &dirHit);
    CHECK(found, "hay celdas que siembran cueva");
    if (!found) return;

    World::Vox::CaveBox again;
    CHECK(World::Vox::caveBoxAt(1234u, dirHit, kR, again), "la misma direccion vuelve a dar cueva");
    CHECK(again.cell == box.cell && again.radiusM == box.radiusM && again.depthM == box.depthM,
          "y la MISMA caja: funcion pura de (seed, celda)");
    World::Vox::CaveBox otra;
    const bool otraSeed = World::Vox::caveBoxAt(4321u, dirHit, kR, otra);
    CHECK(!otraSeed || otra.radiusM != box.radiusM || otra.depthM != box.depthM,
          "CONTRAPRUEBA: con otra seed la celda no da la misma cueva");

    // Frecuencia: ni todas las celdas ni ninguna. Se mide sobre un barrido real.
    int conCueva = 0, total = 0;
    for (int i = 0; i < 2000; ++i) {
        const double a = (double)i * 0.0137, b = (double)i * 0.0071;
        const glm::dvec3 d = glm::normalize(glm::dvec3(std::cos(a) * std::cos(b), std::sin(b),
                                                       std::sin(a) * std::cos(b)));
        World::Vox::CaveBox t;
        ++total; if (World::Vox::caveBoxAt(1234u, d, kR, t)) ++conCueva;
    }
    std::printf("    siembra: %d de %d direcciones caen en celda con cueva (%.1f %%)\n",
                conCueva, total, 100.0 * conCueva / total);
    CHECK(conCueva > 0, "se siembran cuevas");
    CHECK(conCueva < total, "CONTRAPRUEBA: NO todas las celdas tienen cueva");
    // Y con la siembra a CERO (el defecto: cuevas a mano) no sale ninguna, pero la celda se puede
    // pedir a mano y da la MISMA caja que sembrada.
    World::Vox::caveSetAutoProbability(0.0f);
    World::Vox::CaveBox ninguna;
    CHECK(!World::Vox::caveBoxAt(1234u, dirHit, kR, ninguna), "con la siembra a 0 no hay cuevas automaticas");
    World::Vox::CaveBox aMano;
    World::Vox::caveBoxForCell(1234u, dirHit, kR, aMano);
    CHECK(aMano.cell == box.cell && aMano.radiusM == box.radiusM, "a mano se obtiene la MISMA caja de esa celda");
    World::Vox::caveSetAutoProbability(0.14f);

    // ── (2) LOS MAPAS: existen, tienen contraste y no son constantes ────────────────────────────
    World::Vox::CaveMap planta, perfil;
    World::Vox::caveBakeMaps(box, World::Vox::kCaveMapRes, planta, perfil);
    auto stats = [](const World::Vox::CaveMap& m, float& lo, float& hi, float& mean) {
        lo = 1e9f; hi = -1e9f; double s = 0.0;
        for (float x : m.v) { lo = std::min(lo, x); hi = std::max(hi, x); s += x; }
        mean = (float)(s / std::max<size_t>(m.v.size(), 1));
    };
    float plo, phi, pmean, slo, shi, smean;
    stats(planta, plo, phi, pmean); stats(perfil, slo, shi, smean);
    std::printf("    planta %dx%d: [%.2f, %.2f] media %.2f · perfil %dx%d: [%.2f, %.2f] media %.2f\n",
                planta.w, planta.h, plo, phi, pmean, perfil.w, perfil.h, slo, shi, smean);
    CHECK(phi - plo > 0.3f, "la planta tiene contraste (corredores, no una mancha uniforme)");
    CHECK(shi - slo > 0.3f, "y el perfil tambien");
    CHECK(perfil.at(0.5f, 0.0f) < 0.35f,
          "el perfil deja TECHO de roca justo bajo la superficie (si no, el mundo esta hueco)");

    // Los mapas son deterministas: rehornear da los mismos texels.
    World::Vox::CaveMap planta2, perfil2;
    World::Vox::caveBakeMaps(box, World::Vox::kCaveMapRes, planta2, perfil2);
    CHECK(planta.v == planta2.v && perfil.v == perfil2.v, "rehornear da los MISMOS texels");

    // ── (3) CONECTIVIDAD: el invariante duro, y el numero que justifica el post-proceso ─────────
    World::Vox::CaveVolume vol; glm::ivec3 st(0);
    World::Vox::caveBakeVolume(box, planta, perfil, World::Vox::kCaveVolRes, vol, &st);
    const int comps = componentesDeAire(vol);
    std::printf("    volumen %d^3: aire %d voxeles (%.1f %%) · componentes %d -> %d (recuento propio: %d)\n",
                vol.n, st.z, 100.0 * st.z / std::max(1, vol.n * vol.n * vol.n), st.x, st.y, comps);
    CHECK(st.z > 0, "hay aire (una cueva vacia no es una cueva)");
    CHECK(st.z < vol.n * vol.n * vol.n / 2, "CONTRAPRUEBA: y no es todo aire (queda roca)");
    CHECK(comps <= 1, "EL INVARIANTE: el aire es UNA sola pieza (todo hueco es alcanzable)");
    CHECK(st.x >= 1, "y el recuento previo existe");
    if (st.x <= 1)
        std::printf("    NOTA: el AND de los dos mapas ya salio conexo en esta celda; "
                    "el post-proceso no tuvo que tallar nada\n");

    // ── (4) EL CAMPO: signo coherente con el aire, y gradiente que apunta hacia la roca ─────────
    // Se busca un vóxel de aire y se comprueba que el campo muestreado en su posición del MUNDO
    // (ida y vuelta por el marco local) sigue siendo aire: eso ata `toLocal`/`toWorld` al volumen.
    int idxAir = -1;
    for (size_t k = 0; k < vol.d.size(); ++k) if (vol.d[k] < -3.0f) { idxAir = (int)k; break; }
    CHECK(idxAir >= 0, "hay aire holgado (mas de 3 m de la pared)");
    if (idxAir >= 0) {
        const int n = vol.n;
        const int z = idxAir / (n * n), y = (idxAir / n) % n, x = idxAir % n;
        const glm::vec3 local((((float)x / (n - 1)) - 0.5f) * 2.0f * box.radiusM,
                              (((float)y / (n - 1)) - 0.5f) * 2.0f * box.radiusM,
                              ((float)z / (n - 1)) * box.depthM);
        World::Vox::CaveSystem sys; sys.box = box; sys.planta = planta; sys.perfil = perfil; sys.vol = vol;
        const glm::dvec3 world = box.toWorld(local);
        const float d = World::Vox::caveDensityAt(sys, world);
        std::printf("    campo en un voxel de aire: %.2f m (negativo = aire)\n", d);
        CHECK(d < 0.0f, "el punto que el volumen marca AIRE tambien lo es al muestrear por el mundo");

        // Contraprueba: un punto a 10 km de la caja tiene que ser roca (sin efecto a distancia).
        const glm::dvec3 lejos = glm::normalize(box.centerDir + box.tangentU * 0.002) * box.surfaceRM;
        CHECK(World::Vox::caveDensityAt(sys, lejos) > 0.0f, "CONTRAPRUEBA: fuera de la caja no hay cueva");

        // El gradiente no es nulo cerca de una pared: es lo que empuja al jugador.
        // ⚠️ EL CRITERIO VA EN VÓXELES, NO EN METROS. El campo está cuantizado al lado del vóxel
        // (aquí ~1-3 m), así que "vóxeles con d entre −1 y −2 m" puede ser el conjunto VACÍO — y
        // entonces el test no mide nada y aprueba (o suspende) por accidente. Eso pasó: el primer
        // intento daba |grad| = 0,000 porque no examinó un solo punto.
        const float voxM = std::min(2.0f * box.radiusM, box.depthM) / (float)(n - 1);
        float mejor = 0.0f; int mirados = 0;
        for (size_t k = 0; k < vol.d.size(); ++k) {
            if (vol.d[k] >= 0.0f || vol.d[k] < -2.5f * voxM) continue;   // aire pegado a la pared
            const int zz = (int)(k / ((size_t)n * n)), yy = (int)((k / n) % n), xx = (int)(k % n);
            const glm::vec3 lp((((float)xx / (n - 1)) - 0.5f) * 2.0f * box.radiusM,
                               (((float)yy / (n - 1)) - 0.5f) * 2.0f * box.radiusM,
                               ((float)zz / (n - 1)) * box.depthM);
            mejor = std::max(mejor, glm::length(World::Vox::caveDensityGradient(sys, box.toWorld(lp), voxM)));
            if (++mirados >= 400) break;
        }
        std::printf("    |gradiente| junto a la pared: %.3f (sobre %d puntos, voxel %.2f m)\n",
                    mejor, mirados, voxM);
        CHECK(mirados > 0, "hay aire pegado a la pared que examinar");
        CHECK(mejor > 0.01f, "el campo tiene pendiente en la pared (la colision puede empujar)");
        // ⚠️ Y LA PENDIENTE TIENE QUE VALER ~1. Un campo de distancia con signo cumple |∇d| = 1 por
        // definición; cualquier otra cosa es un error de escala o un centinela colándose en la
        // resta. El primer intento daba 1,98e8 aquí (el "fuera de la caja" restado) y el test lo
        // daba por bueno porque sólo pedía "> 0,01": una cota por abajo no mide un campo.
        CHECK(mejor < 2.0f, "y vale ~1 (es una distancia, no un salto: nada de lanzar al jugador)");
    }

    // ── (5) LA BOCA: la cueva sale a la superficie ──────────────────────────────────────────────
    World::Vox::CaveSystem sys; sys.box = box; sys.planta = planta; sys.perfil = perfil; sys.vol = vol;
    int bocaTexels = 0, sup = 0;
    float maxMask = 0.0f;
    for (int j = 0; j < 64; ++j)
        for (int i = 0; i < 64; ++i) {
            const float u = ((float)i / 63.0f - 0.5f) * 2.0f * box.radiusM;
            const float v = ((float)j / 63.0f - 0.5f) * 2.0f * box.radiusM;
            const glm::dvec3 d = glm::normalize(box.centerDir * box.surfaceRM
                                              + box.tangentU * (double)u + box.tangentV * (double)v);
            const float m = World::Vox::caveEntranceMask(sys, d);
            maxMask = std::max(maxMask, m);
            ++sup; if (m > 0.5f) ++bocaTexels;
        }
    std::printf("    boca: %d de %d puntos de superficie con aire (%.2f %%) · maximo %.2f\n",
                bocaTexels, sup, 100.0 * bocaTexels / sup, maxMask);
    CHECK(maxMask > 0.5f, "la cueva ROMPE la superficie en algun sitio (se puede entrar)");
    CHECK(bocaTexels * 4 < sup, "CONTRAPRUEBA: y la boca es un agujero, no medio planeta abierto");
}

void test_caves_mesh_and_strokes() {
    beginTest("cuevas: malla y trazos (una sola forma de tocar el mundo)");

    World::Vox::CaveBox box;
    if (!findBox(1234u, box)) { CHECK(false, "hay una celda con cueva para probar"); return; }
    World::Vox::CaveSystem sys; sys.box = box;
    World::Vox::caveBakeMaps(box, World::Vox::kCaveMapRes, sys.planta, sys.perfil);
    World::Vox::caveBakeVolume(box, sys.planta, sys.perfil, World::Vox::kCaveVolRes, sys.vol, &sys.connectStats);

    // ── (1) LA MALLA SIGUE AL CAMPO ─────────────────────────────────────────────────────────────
    World::Vox::CaveMesh mesh;
    World::Vox::caveBuildMesh(sys, mesh);
    std::printf("    malla: %zu vertices · %zu triangulos\n", mesh.positions.size(), mesh.triangleCount());
    CHECK(!mesh.empty(), "la cueva tiene paredes");
    CHECK(mesh.positions.size() == mesh.normals.size(), "una normal por vertice");

    // El invariante que de verdad ata malla y campo: TODO vertice esta en la superficie del campo,
    // o sea |d| pequeño. Si la malla se separa del campo, se ve una pared donde no la hay (y se
    // atraviesa la que sí hay) — que es justo el bug que la colision analitica viene a evitar.
    double peor = 0.0, media = 0.0; size_t fuera = 0;
    const float voxM = std::min(2.0f * box.radiusM, box.depthM) / (float)(World::Vox::kCaveVolRes - 1);
    for (const glm::vec3& v : mesh.positions) {
        const float d = std::fabs(World::Vox::caveDensityAt(sys, box.toWorld(v)));
        peor = std::max(peor, (double)d); media += d;
        if (d > 1.5f * voxM) ++fuera;
    }
    media /= std::max<size_t>(mesh.positions.size(), 1);
    std::printf("    |campo| en los vertices de la malla: media %.3f m · peor %.3f m · fuera de 1,5 voxeles: %zu (voxel %.2f m)\n",
                media, peor, fuera, voxM);
    CHECK(fuera * 100 < mesh.positions.size(), "los vertices estan SOBRE la superficie del campo (<1 % fuera)");

    // Contraprueba: puntos al azar del volumen NO cumplen eso (si no, el criterio seria trivial).
    size_t lejos = 0;
    for (size_t k = 0; k < sys.vol.d.size(); k += 977)
        if (std::fabs(sys.vol.d[k]) > 1.5f * voxM) ++lejos;
    CHECK(lejos > 0, "CONTRAPRUEBA: el campo NO es pequeno en todas partes (el criterio distingue)");

    // ── (2) LOS TRAZOS: quitar y poner son el MISMO mecanismo ───────────────────────────────────
    // Un punto de roca maciza, lejos de toda pared: se pica y tiene que quedar aire.
    int idxRoca = -1;
    for (size_t k = 0; k < sys.vol.d.size(); ++k) if (sys.vol.d[k] > 10.0f) { idxRoca = (int)k; break; }
    CHECK(idxRoca >= 0, "hay roca maciza donde picar");
    if (idxRoca >= 0) {
        const int n = sys.vol.n;
        const int z = idxRoca / (n * n), y = (idxRoca / n) % n, x = idxRoca % n;
        const glm::vec3 lp((((float)x / (n - 1)) - 0.5f) * 2.0f * box.radiusM,
                           (((float)y / (n - 1)) - 0.5f) * 2.0f * box.radiusM,
                           ((float)z / (n - 1)) * box.depthM);
        const glm::dvec3 wp = box.toWorld(lp);
        CHECK(World::Vox::caveDensityAt(sys, wp) > 0.0f, "antes de picar: es roca");

        const int abiertos = World::Vox::caveApplyStrokeWorld(sys, wp, 8.0f, true);
        std::printf("    picar r=8 m: %d voxeles pasan a aire · campo en el centro %.2f m\n",
                    abiertos, World::Vox::caveDensityAt(sys, wp));
        CHECK(abiertos > 0, "picar ABRE hueco");
        CHECK(World::Vox::caveDensityAt(sys, wp) < 0.0f, "y el punto picado es aire");

        // Y rellenar es el mismo gesto al reves: vuelve a ser roca.
        const int cerrados = World::Vox::caveApplyStrokeWorld(sys, wp, 8.0f, false);
        std::printf("    rellenar r=8 m: %d voxeles vuelven a roca · campo %.2f m\n",
                    cerrados, World::Vox::caveDensityAt(sys, wp));
        CHECK(cerrados > 0 && World::Vox::caveDensityAt(sys, wp) > 0.0f,
              "rellenar es el MISMO mecanismo con el signo cambiado");

        // Fuera de la caja el trazo no aplica: un sistema no tiene efecto a distancia.
        const glm::dvec3 lejosW = glm::normalize(box.centerDir + box.tangentU * 0.01) * box.surfaceRM;
        CHECK(World::Vox::caveApplyStrokeWorld(sys, lejosW, 8.0f, true) < 0,
              "CONTRAPRUEBA: un trazo fuera de la caja no toca este sistema");

        // La malla nota el cambio: picar una sala nueva anade superficie.
        const size_t trisAntes = mesh.triangleCount();
        World::Vox::caveApplyStrokeWorld(sys, wp, 12.0f, true);
        World::Vox::CaveMesh m2; World::Vox::caveBuildMesh(sys, m2);
        std::printf("    triangulos: %zu antes · %zu tras picar\n", trisAntes, m2.triangleCount());
        CHECK(m2.triangleCount() > trisAntes, "la malla sigue al campo tras el trazo");
    }

    // ── (3) LOS TRAZOS SOBREVIVEN EN LA PARTIDA (no en la cache) ────────────────────────────────
    std::vector<World::Vox::CaveStroke> tr = { {glm::vec3(10.0f, -4.0f, 30.0f), 6.0f, true},
                                   {glm::vec3(0.0f, 0.0f, 50.0f), 3.5f, false} };
    const std::string path = (std::filesystem::temp_directory_path() / "haruka_cave_test" / "trazos.txt").string();
    CHECK(World::Vox::caveSaveStrokes(path, tr), "los trazos se guardan");
    std::vector<World::Vox::CaveStroke> leidos;
    CHECK(World::Vox::caveLoadStrokes(path, leidos), "y se leen");
    CHECK(leidos.size() == tr.size(), "todos");
    bool iguales = leidos.size() == tr.size();
    for (size_t i = 0; i < leidos.size() && iguales; ++i)
        iguales = std::fabs(leidos[i].centerLocal.z - tr[i].centerLocal.z) < 1e-3f &&
                  std::fabs(leidos[i].radiusM - tr[i].radiusM) < 1e-3f &&
                  leidos[i].remove == tr[i].remove;
    CHECK(iguales, "con su sitio, su radio y su signo");
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(path).parent_path(), ec);
}

void test_caves_coupling() {
    beginTest("cuevas: el acoplamiento con el terreno (la boca)");

    World::Vox::CaveBox box;
    if (!findBox(1234u, box)) { CHECK(false, "hay una celda con cueva para probar"); return; }
    World::Vox::CaveSystem sys; sys.box = box;
    World::Vox::caveBakeMaps(box, World::Vox::kCaveMapRes, sys.planta, sys.perfil);
    World::Vox::caveBakeVolume(box, sys.planta, sys.perfil, World::Vox::kCaveVolRes, sys.vol, &sys.connectStats);
    World::Vox::caveBakeEntrance(box, sys.vol, World::Vox::kCaveMapRes, sys.boca);

    // ── (1) HAY BOCA, Y ES UN AGUJERO ───────────────────────────────────────────────────────────
    size_t abierto = 0, borde = 0;
    for (float x : sys.boca.v) { if (x > 0.5f) ++abierto; else if (x > 0.05f) ++borde; }
    std::printf("    mapa de boca %dx%d: %.2f %% recortado · %.2f %% de borde suave\n",
                sys.boca.w, sys.boca.h, 100.0 * abierto / sys.boca.v.size(),
                100.0 * borde / sys.boca.v.size());
    CHECK(abierto > 0, "el terreno se recorta en alguna parte (si no, la cueva es inaccesible)");
    CHECK(abierto * 4 < sys.boca.v.size(), "CONTRAPRUEBA: y NO se recorta medio parche (sigue habiendo suelo)");
    CHECK(borde > 0, "el borde del recorte es SUAVE (un escalon duro deja filo dentado por texel)");

    // ── (2) EL RECORTE COINCIDE CON EL AIRE ─────────────────────────────────────────────────────
    // Es LA condicion del acoplamiento: si el terreno se recorta donde hay roca, se ve un agujero
    // al vacio; si NO se recorta donde hay aire, la cueva queda tapada por una tapa de terreno.
    int cortaYaire = 0, cortaYroca = 0, noCortaYaire = 0, muestras = 0;
    for (int j = 0; j < 96; ++j)
        for (int i = 0; i < 96; ++i) {
            const float lx = ((float)i / 95.0f - 0.5f) * 2.0f * box.radiusM * 0.95f;
            const float ly = ((float)j / 95.0f - 0.5f) * 2.0f * box.radiusM * 0.95f;
            const glm::dvec3 dir = glm::normalize(box.toWorld(glm::vec3(lx, ly, 0.0f)));
            const float cut = World::Vox::caveSurfaceCut(sys, dir);
            // ¿Hay aire en los primeros metros bajo ESTE punto?
            const float d = World::Vox::caveDensityAt(sys, glm::normalize(dir) * (box.surfaceRM - 3.0));
            const bool aire = (d < 0.0f);
            ++muestras;
            if (cut > 0.5f && aire) ++cortaYaire;
            if (cut > 0.5f && !aire) ++cortaYroca;
            if (cut < 0.05f && aire) ++noCortaYaire;
        }
    std::printf("    de %d puntos: recorta-y-hay-aire %d · recorta-sobre-ROCA %d · no-recorta-y-hay-AIRE %d\n",
                muestras, cortaYaire, cortaYroca, noCortaYaire);
    CHECK(cortaYaire > 0, "donde hay aire bajo el suelo, el terreno se recorta");
    CHECK(cortaYroca == 0, "y NUNCA se recorta sobre roca (seria un agujero al vacio)");
    CHECK(noCortaYaire == 0, "ni se deja tapa de terreno sobre el aire (la cueva quedaria sellada)");

    // ── (3) UN TRAZO ABRE BOCA: picar desde dentro saca a la superficie ─────────────────────────
    // Es la prueba de que cavar y "que haya cuevas" son el MISMO mecanismo: se pica bajo un punto
    // con suelo y ese punto pasa a estar recortado.
    int idx = -1;
    for (int j = 0; j < 96 && idx < 0; ++j)
        for (int i = 0; i < 96 && idx < 0; ++i) {
            const float lx = ((float)i / 95.0f - 0.5f) * 2.0f * box.radiusM * 0.6f;
            const float ly = ((float)j / 95.0f - 0.5f) * 2.0f * box.radiusM * 0.6f;
            const glm::dvec3 dir = glm::normalize(box.toWorld(glm::vec3(lx, ly, 0.0f)));
            if (World::Vox::caveSurfaceCut(sys, dir) < 0.01f &&
                World::Vox::caveDensityAt(sys, glm::normalize(dir) * (box.surfaceRM - 6.0)) > 5.0f)
                idx = j * 96 + i;
        }
    CHECK(idx >= 0, "hay un punto con suelo macizo donde picar");
    if (idx >= 0) {
        const float lx = ((float)(idx % 96) / 95.0f - 0.5f) * 2.0f * box.radiusM * 0.6f;
        const float ly = ((float)(idx / 96) / 95.0f - 0.5f) * 2.0f * box.radiusM * 0.6f;
        const glm::dvec3 dir = glm::normalize(box.toWorld(glm::vec3(lx, ly, 0.0f)));
        const glm::dvec3 wp = glm::normalize(dir) * (box.surfaceRM - 5.0);
        CHECK(World::Vox::caveSurfaceCut(sys, dir) < 0.01f, "antes de picar: aqui hay suelo");
        const int n = World::Vox::caveApplyStrokeWorld(sys, wp, 10.0f, true);
        World::Vox::caveBakeEntrance(box, sys.vol, World::Vox::kCaveMapRes, sys.boca);   // el mapa de boca es DERIVADO
        const float cut = World::Vox::caveSurfaceCut(sys, dir);
        std::printf("    picar bajo el suelo (r=10 m, %d voxeles): recorte %.2f -> %.2f\n", n, 0.0, cut);
        CHECK(cut > 0.5f, "picar ABRE boca: el terreno deja de dibujarse ahi");
    }
}

void test_caves_cache() {
    beginTest("cuevas: el fichero ES la definicion");

    World::Vox::CaveBox box;
    if (!findBox(777u, box)) { CHECK(false, "hay una celda con cueva para probar el cache"); return; }

    // Carpeta temporal propia: el test no debe ensuciar (ni leer) los horneados del proyecto.
    const std::string dir = (std::filesystem::temp_directory_path() / "haruka_cave_test").string();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    World::Vox::CaveSystem a;
    const bool ok1 = World::Vox::caveLoadOrBake(box, a, dir, 128, 48);
    CHECK(ok1 && a.valid(), "primera vez: hornea");

    int ficheros = 0;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) { (void)e; ++ficheros; }
    std::printf("    ficheros escritos: %d (planta, perfil, volumen, boca)\n", ficheros);
    CHECK(ficheros == 4, "y deja los CUATRO mapas en disco");

    World::Vox::CaveSystem b;
    const bool ok2 = World::Vox::caveLoadOrBake(box, b, dir, 128, 48);
    CHECK(ok2 && b.valid(), "segunda vez: carga");

    // Lo que se lee es lo que se horneó. El PNG es de 8 bits, así que el campo se cuantiza: el
    // criterio es el paso de cuantización (48 m de rango / 256), no la igualdad exacta.
    // El fichero satura a ±24 m (ver `kVolRangeM`): lejos de la pared sólo se guarda el signo. Así
    // que se compara donde el campo SE USA —los primeros metros— y aparte se exige que el signo
    // sobreviva en todas partes, que es lo que decide si un punto es aire o roca.
    double peor = 0.0; size_t signoRoto = 0;
    for (size_t k = 0; k < a.vol.d.size(); ++k) {
        if ((a.vol.d[k] < 0.0f) != (b.vol.d[k] < 0.0f)) ++signoRoto;
        if (std::fabs(a.vol.d[k]) > 23.0f) continue;
        peor = std::max(peor, (double)std::fabs(a.vol.d[k] - b.vol.d[k]));
    }
    std::printf("    peor diferencia horneado vs cargado (|d| < 23 m): %.3f m (paso del byte: %.3f m) · "
                "signos distintos: %zu\n", peor, 48.0 / 256.0, signoRoto);
    CHECK(peor <= 48.0 / 256.0 + 1e-3, "cargar del disco da el MISMO campo que hornear, junto a la pared");
    CHECK(signoRoto == 0, "y en TODO el volumen coincide aire/roca");
    double peorMapa = 0.0;
    for (size_t k = 0; k < a.planta.v.size(); ++k)
        peorMapa = std::max(peorMapa, (double)std::fabs(a.planta.v[k] - b.planta.v[k]));
    std::printf("    peor diferencia en la planta: %.5f (paso del byte: %.5f)\n", peorMapa, 1.0 / 255.0);
    CHECK(peorMapa <= 1.0 / 255.0 + 1e-6, "y los mismos mapas (dentro del paso del byte del PNG)");

    // CONTRAPRUEBA del cache: otra caja tiene otra clave y no reutiliza el fichero.
    World::Vox::CaveBox box2 = box; box2.depthM += 37.0f;
    CHECK(World::Vox::caveCacheKey(box, 128, 48) != World::Vox::caveCacheKey(box2, 128, 48),
          "CONTRAPRUEBA: cambiar la caja cambia la clave (no se reutiliza un bake ajeno)");

    std::filesystem::remove_all(dir, ec);
}

// ── LAYERS: the cave as heightfields ────────────────────────────────────────────────────────────
// A layer is a ceiling map and a floor map (metres, as a fraction of depthM), like the planet's
// elevation map. Measured here:
//   (1) converting the legacy volume to layers keeps the air (control: another cave's layers don't);
//   (2) layers express what footprint x profile could not: a gallery whose depth changes ALONG v;
//   (3) 16-bit round trip; (4) ceiling = 0 is the mouth; (5) the field is a distance on a slope.
void test_cave_layers() {
    beginTest("caves: layers (ceiling/floor heightfields) - conversion, expressiveness, mouth, distance");
    using namespace World::Vox;
    using clk = std::chrono::high_resolution_clock;
    auto ms = [](clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };

    CaveBox box;
    if (!findBox(1234u, box)) { CHECK(false, "there is a cell with a cave to test"); return; }

    // (1a) CONVERSION of a legacy cave PAINTED the old way: a curved corridor in the footprint and
    // TWO depth bands in the profile = two galleries per column. Compared voxel by voxel against
    // the volume it came from; the control is another cave's layers in the same box.
    auto compareAir = [&](const CaveSystem& vol, const CaveSystem& other, int& air, int& wrong, int& wrongFar) {
        air = wrong = wrongFar = 0;
        const int n = vol.vol.n;
        for (int z = 0; z < n; ++z)
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x) {
                    const float dv = vol.vol.d[(size_t)z * n * n + (size_t)y * n + x];
                    const glm::vec3 l(((float)x / (float)(n - 1) - 0.5f) * 2.0f * vol.box.radiusM,
                                      ((float)y / (float)(n - 1) - 0.5f) * 2.0f * vol.box.radiusM, (float)z / (float)(n - 1) * vol.box.depthM);
                    const float dl = caveDensityLocal(other, l);
                    if (dv < 0.0f) ++air;
                    if ((dv < 0.0f) == (dl < 0.0f)) continue;
                    ++wrong;
                    // AT THE WALL = some neighbour (26) has the other sign; outside the box counts as
                    // rock. Not the volume's distance: its chamfer uses the SMALLER voxel side, so
                    // "5 m" can be less than one 7.5 m lateral voxel.
                    bool wall = false;
                    for (int dz = -1; dz <= 1 && !wall; ++dz)
                        for (int dy = -1; dy <= 1 && !wall; ++dy)
                            for (int dx = -1; dx <= 1 && !wall; ++dx) {
                                const int nx = x + dx, ny = y + dy, nz = z + dz;
                                const bool nAir = (nx >= 0 && ny >= 0 && nz >= 0 && nx < n && ny < n && nz < n)
                                                  && vol.vol.d[(size_t)nz * n * n + (size_t)ny * n + nx] < 0.0f;
                                wall = nAir != (dv < 0.0f);
                            }
                    if (!wall) ++wrongFar;
                }
    };
    CaveSystem painted; painted.box = box;
    {
        const int res = kCaveMapRes;
        painted.planta.w = painted.planta.h = res; painted.planta.v.assign((size_t)res * res, 0.0f);
        painted.perfil.w = res; painted.perfil.h = res / 2; painted.perfil.v.assign((size_t)res * (res / 2), 0.0f);
        for (int y = 0; y < res; ++y)
            for (int x = 0; x < res; ++x) {
                const float u = (float)x / (float)(res - 1), v = (float)y / (float)(res - 1);
                const float centre = 0.5f + 0.15f * std::sin(u * 6.0f);   // a curved corridor
                if (u > 0.15f && u < 0.85f && std::fabs(v - centre) < 0.04f) painted.planta.v[(size_t)y * res + x] = 1.0f;
            }
        for (int y = 0; y < res / 2; ++y)
            for (int x = 0; x < res; ++x) {
                const float t = (float)y / (float)(res / 2 - 1);
                if ((t > 0.25f && t < 0.35f) || (t > 0.6f && t < 0.72f)) painted.perfil.v[(size_t)y * res + x] = 1.0f;
            }
        caveBakeVolume(box, painted.planta, painted.perfil, kCaveVolRes, painted.vol, &painted.connectStats);
    }
    CaveSystem paintedL; paintedL.box = box;
    auto t0 = clk::now();
    const int droppedP = caveLayersFromVolume(painted, kCaveMapRes, paintedL.layers);
    const double extractMs = ms(t0, clk::now());
    caveLayersPrepare(paintedL.layers, box);
    int air = 0, wrong = 0, wrongFar = 0;
    compareAir(painted, paintedL, air, wrong, wrongFar);
    std::printf("    painted legacy cave -> %zu layers in %.0f ms · %d spans dropped · air voxels %d · sign differs at %d (%.2f %%), %d away from the wall\n",
                paintedL.layers.size(), extractMs, droppedP, air, wrong, 100.0 * wrong / std::max(air, 1), wrongFar);
    std::printf("    connectivity: volume %d -> layers %d\n", painted.connectStats.y, caveLayerComponents(paintedL.layers));
    CHECK(air > 1000, "the painted legacy cave has air to compare");
    CHECK(droppedP == 0 && wrongFar == 0, "conversion keeps the air: differences only at the wall itself");
    CHECK(caveLayerComponents(paintedL.layers) == painted.connectStats.y, "and the same number of air pieces");

    // CONTROL: the layers of ANOTHER cave do not match this volume.
    CaveSystem seeded; seeded.box = box;
    caveBakeMaps(box, kCaveMapRes, seeded.planta, seeded.perfil);
    caveBakeVolume(box, seeded.planta, seeded.perfil, kCaveVolRes, seeded.vol, &seeded.connectStats);
    CaveSystem seededL; seededL.box = box;
    const int droppedS = caveLayersFromVolume(seeded, kCaveMapRes, seededL.layers);
    caveLayersPrepare(seededL.layers, box);
    int airB = 0, wrongB = 0, wrongFarB = 0;
    compareAir(painted, seededL, airB, wrongB, wrongFarB);
    std::printf("    CONTROL another cave's layers: sign differs at %d (%d far from the wall)\n", wrongB, wrongFarB);
    CHECK(wrongFarB > 100, "CONTROL: the comparison tells two different caves apart");

    // (1c) INFORMATION, not a requirement: the OLD seeded generator is a sponge of strata (the
    // profile's ridged noise), more than 4 spans in some columns. Seeded caves are test-only
    // (automatic seeding is 0); drafts are now written directly as layers (1b).
    {
        int a2 = 0, w2 = 0, f2 = 0;
        compareAir(seeded, seededL, a2, w2, f2);
        std::printf("    (info) old seeded sponge -> layers: %d spans dropped · sign differs at %.2f %% of the air (%d far) · components %d -> %d\n",
                    droppedS, 100.0 * w2 / std::max(a2, 1), f2, seeded.connectStats.y, caveLayerComponents(seededL.layers));
    }

    // (1d) THE LOADER decides: a legacy cave that converts exactly is loaded as layers; one that
    // would lose air (the sponge) keeps its volume, so a cave that worked does not get worse.
    {
        const std::string ldir = (std::filesystem::temp_directory_path() / "haruka_cave_legacy_load").string();
        std::error_code lec; std::filesystem::remove_all(ldir, lec);
        auto loadLegacy = [&](const CaveSystem& src, const char* name, CaveSystem& out) {
            CaveDef def; def.name = name; def.radiusM = box.radiusM; def.depthM = box.depthM; def.mouth = box.mouth; def.draftSeed = box.seed;
            def.planta = ldir + "/" + name + "_planta.png"; def.perfil = ldir + "/" + name + "_perfil.png";
            World::Terrain::BakeUtil::savePNGGray(def.planta, src.planta.w, src.planta.h, src.planta.v);
            World::Terrain::BakeUtil::savePNGGray(def.perfil, src.perfil.w, src.perfil.h, src.perfil.v);
            CaveBox b = caveBoxFromDef(def, kR); b.seed = box.seed; b.cell = box.cell;   // same tau as the source volume
            return caveLoadBox(def, b, out, ldir + "/bakes");
        };
        CaveSystem exact, lossy;
        const bool okE = loadLegacy(painted, "exact", exact), okL = loadLegacy(seeded, "sponge", lossy);
        std::printf("    loader: painted legacy -> %zu layer(s) · sponge legacy -> %zu layer(s), volume %s\n",
                    exact.layers.size(), lossy.layers.size(), lossy.vol.empty() ? "EMPTY" : "kept");
        CHECK(okE && !exact.layers.empty(), "an exact legacy conversion is loaded as layers");
        CHECK(okL && lossy.layers.empty() && !lossy.vol.empty(), "a lossy one keeps its volume (nothing gets worse)");
        std::filesystem::remove_all(ldir, lec);
    }

    // (1b) NATIVE DRAFTS: written as layers, connected by construction, open at the mouth.
    {
        int ok = 0, total = 0, multi = 0;
        float worstMouth = 1.0f, worstShare = 1.0f;
        for (int i = 0; i < 12; ++i) {
            CaveBox b2 = box; b2.seed = 1000u + (uint32_t)i * 7919u; b2.cell = box.cell + (uint32_t)i;
            CaveSystem draft; draft.box = b2;
            glm::ivec3 st;
            caveBakeLayers(b2, kCaveMapRes, draft.layers, &st);
            CaveMap mouthMap; caveBakeEntrance(draft, kCaveMapRes, mouthMap);
            const float m = mouthMap.at(b2.mouth.x, b2.mouth.y);
            worstMouth = std::min(worstMouth, m);
            // INDEPENDENT count: the field sampled on a 3D grid and flood-filled with this file's own
            // counter (26 neighbours), not the 2D counter the generator uses.
            CaveVolume grid; grid.n = 128; grid.d.resize((size_t)128 * 128 * 128);
            for (int z = 0; z < 128; ++z)
                for (int y = 0; y < 128; ++y)
                    for (int x = 0; x < 128; ++x)
                        grid.d[(size_t)z * 128 * 128 + (size_t)y * 128 + x] = caveDensityLocal(draft, glm::vec3(
                            ((float)x / 127.0f - 0.5f) * 2.0f * b2.radiusM, ((float)y / 127.0f - 0.5f) * 2.0f * b2.radiusM, (float)z / 127.0f * b2.depthM));
            const int comps3d = componentesDeAire(grid);
            const std::vector<int> sizes = airComponentSizes(grid);
            int airAll = 0; for (int q : sizes) airAll += q;
            const float mainShare = sizes.empty() ? 0.0f : (float)sizes[0] / (float)std::max(airAll, 1);
            worstShare = std::min(worstShare, mainShare);
            ++total; if (st.y == 1 && mainShare > 0.999f) ++ok; if (draft.layers.size() > 1) ++multi;
            std::printf("      draft %2d: %zu level(s) · 2D pieces %2d -> %d · 3D: %d piece(s), the main one holds %.4f of the air (others:",
                        i, draft.layers.size(), st.x, st.y, comps3d, mainShare);
            for (size_t q = 1; q < sizes.size() && q < 6; ++q) std::printf(" %d", sizes[q]);
            std::printf(" voxels)\n");
        }
        std::printf("    native drafts: %d/%d connected · %d with 2-3 levels · worst main-piece share %.4f · worst mouth %.2f\n", ok, total, multi, worstShare, worstMouth);
        CHECK(ok == total, "every native draft is one piece in 2D, and in an independent 3D flood fill the main piece holds > 99.9 % of the air");
        CHECK(multi > 0, "and some have several levels (the shafts were really tested)");
        CHECK(worstMouth > 0.9f, "and every one opens the ground at its mouth");
    }

    // Sampling cost: prepared layers vs trilinear volume.
    {
        const int n = painted.vol.n;
        auto local = [&](int i) { return glm::vec3(((float)(i % n) / (float)(n - 1) - 0.5f) * 2.0f * box.radiusM,
                                                   ((float)((i / n) % n) / (float)(n - 1) - 0.5f) * 2.0f * box.radiusM,
                                                   (float)((i / 7) % n) / (float)(n - 1) * box.depthM); };
        volatile float sink = 0.0f; const int samples = 200000;
        auto t1 = clk::now();
        for (int i = 0; i < samples; ++i) sink = sink + caveDensityLocal(paintedL, local(i));
        auto t2 = clk::now();
        for (int i = 0; i < samples; ++i) sink = sink + caveDensityLocal(painted, local(i));
        auto t3 = clk::now();
        std::printf("    one sample: %zu prepared layers %.0f ns · volume %.0f ns\n", paintedL.layers.size(), ms(t1, t2) * 1e6 / samples, ms(t2, t3) * 1e6 / samples);
    }

    // (2) EXPRESSIVENESS: two hand-painted layers. Layer 0: a gallery along u at 30-40 m. Layer 1: a
    // gallery along v whose depth RAMPS with v (ceiling 60 + 40 v m). The old profile only depended
    // on (u, depth): a depth that changes along v did not exist in it.
    CaveSystem art; art.box = box; art.box.radiusM = 200.0f; art.box.depthM = 120.0f;
    const float D = art.box.depthM; const int res = 128;
    auto F = [&](float metres) { return caveLayerFromM(art.box, metres); };
    auto blank = [&](CaveMap& m, float metres) { m.w = m.h = res; m.v.assign((size_t)res * res, F(metres)); };
    art.layers.resize(2);
    for (CaveLayer& L : art.layers) { blank(L.ceiling, D); blank(L.floor, 0.0f); }   // closed everywhere
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x) {
            const float u = (float)x / (float)(res - 1), v = (float)y / (float)(res - 1);
            const size_t i = (size_t)y * res + x;
            if (std::fabs(v - 0.5f) < 0.05f && u > 0.2f && u < 0.8f) { art.layers[0].ceiling.v[i] = F(30.0f); art.layers[0].floor.v[i] = F(40.0f); }
            if (std::fabs(u - 0.5f) < 0.05f && v > 0.1f && v < 0.9f) { art.layers[1].ceiling.v[i] = F(60.0f + 40.0f * v); art.layers[1].floor.v[i] = F(70.0f + 40.0f * v); }
        }
    auto at = [&](const CaveSystem& sys, float u, float v, float z) {
        return caveDensityAtDraped(sys, sys.box.toWorld(glm::vec3((u - 0.5f) * 2.0f * sys.box.radiusM, (v - 0.5f) * 2.0f * sys.box.radiusM, z)), sys.box.surfaceRM);
    };
    const bool g0 = at(art, 0.3f, 0.5f, 35.0f) < 0.0f && at(art, 0.3f, 0.5f, 50.0f) > 0.0f && at(art, 0.3f, 0.5f, 25.0f) > 0.0f;
    const bool rampLo = at(art, 0.5f, 0.2f, 73.0f) < 0.0f && at(art, 0.5f, 0.2f, 97.0f) > 0.0f;
    const bool rampHi = at(art, 0.5f, 0.8f, 97.0f) < 0.0f && at(art, 0.5f, 0.8f, 73.0f) > 0.0f;
    const bool cross = at(art, 0.5f, 0.5f, 35.0f) < 0.0f && at(art, 0.5f, 0.5f, 85.0f) < 0.0f && at(art, 0.5f, 0.5f, 55.0f) > 0.0f;
    std::printf("    painted: layer-0 gallery %s · ramp at v=0.2 %s · at v=0.8 %s · crossing two levels %s · components %d\n",
                g0 ? "ok" : "NO", rampLo ? "ok" : "NO", rampHi ? "ok" : "NO", cross ? "ok" : "NO", caveLayerComponents(art.layers));
    CHECK(g0, "layer 0: air in its gallery, rock above and below");
    CHECK(rampLo && rampHi, "layer 1 follows its depth ALONG v (73 m at v=0.2, 97 m at v=0.8)");
    CHECK(cross, "two galleries stacked in one column, rock between them");
    CHECK(caveLayerComponents(art.layers) == 2, "two separate galleries are counted as two components");
    CaveSystem art0 = art; art0.layers.resize(1);
    CHECK(at(art0, 0.5f, 0.2f, 73.0f) > 0.0f, "CONTROL: without layer 1 that point is rock");

    // (3) 16-bit round trip.
    const std::string dir = (std::filesystem::temp_directory_path() / "haruka_cave_layers").string();
    std::error_code ec; std::filesystem::remove_all(dir, ec);
    const auto paths = caveLayerPathsFor(dir + "/art", 2);
    float worstRt = 0.0f;
    for (int L = 0; L < 2; ++L) {
        CHECK(caveSaveMap16(paths[L].ceiling, art.layers[L].ceiling) && caveSaveMap16(paths[L].floor, art.layers[L].floor), "layer maps saved");
        CaveMap c, f;
        CHECK(caveLoadMap16(paths[L].ceiling, c) && caveLoadMap16(paths[L].floor, f), "layer maps loaded");
        for (size_t i = 0; i < c.v.size(); ++i)
            worstRt = std::max({ worstRt, std::fabs(c.v[i] - art.layers[L].ceiling.v[i]), std::fabs(f.v[i] - art.layers[L].floor.v[i]) });
    }
    std::printf("    16-bit round trip: worst %.6f of the range = %.2f mm\n", worstRt, worstRt * (D + kCaveOpenM) * 1000.0f);
    CHECK(worstRt <= 0.5f / 65535.0f + 1e-7f, "the PNG keeps the value to half a 16-bit step");

    // (4) THE MOUTH: ceiling 0 in a disc of layer 0 = the gallery reaches the ground.
    CaveSystem mouth = art;
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x)
            if ((x - res * 3 / 10) * (x - res * 3 / 10) + (y - res / 2) * (y - res / 2) <= 16)
                mouth.layers[0].ceiling.v[(size_t)y * res + x] = 0.0f;   // the top of the range: open to the sky
    CaveMap boca, bocaArt;
    caveBakeEntrance(mouth, res, boca); caveBakeEntrance(art, res, bocaArt);
    const float mouthHere = boca.at(0.3f, 0.5f), mouthArt = bocaArt.at(0.3f, 0.5f);
    std::printf("    mouth map at the disc: %.2f (control, ceiling 30 m: %.2f)\n", mouthHere, mouthArt);
    CHECK(mouthHere > 0.9f, "ceiling = 0 opens the ground (the mouth, no special case)");
    CHECK(mouthArt < 0.01f, "CONTROL: with a 30 m ceiling there is no mouth");

    // (5) DISTANCE on a 45° ceiling: |grad| ~ 1 a couple of metres below it. Raw `ceiling - z`
    // would give sqrt(2).
    CaveSystem slope = art; slope.layers.resize(1);
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x) {
            const float xm = ((float)x / (float)(res - 1) - 0.5f) * 2.0f * slope.box.radiusM;   // ceiling rises 1 m per metre
            slope.layers[0].ceiling.v[(size_t)y * res + x] = F(50.0f + xm);
            slope.layers[0].floor.v[(size_t)y * res + x] = 1.0f;
        }
    float gradWorst = 0.0f, gradBest = 9.0f;
    for (float xm = -30.0f; xm <= 30.0f; xm += 10.0f) {
        const float z = 50.0f + xm - 3.0f;   // 3 m above the ceiling surface, in rock
        const float h = 0.25f;
        auto f = [&](float x, float zz) { return caveDensityLocal(slope, glm::vec3(x, 0.0f, zz)); };
        const float gx = (f(xm + h, z) - f(xm - h, z)) / (2 * h), gz = (f(xm, z + h) - f(xm, z - h)) / (2 * h);
        const float g = std::sqrt(gx * gx + gz * gz);
        gradWorst = std::max(gradWorst, g); gradBest = std::min(gradBest, g);
    }
    std::printf("    45 degree ceiling: |grad| between %.3f and %.3f (raw ceiling - z would be 1.414)\n", gradBest, gradWorst);
    CHECK(gradBest > 0.9f && gradWorst < 1.1f, "the layer field is a distance on a slope, not 1.41x it");
    std::filesystem::remove_all(dir, ec);
}

// Registro: el grupo sale de la carpeta de este archivo (ver Common/TestCommon.h).
HARUKA_REGISTER_TESTS(
    { "caves_bake", test_caves_bake },
    { "caves_cache", test_caves_cache },
    { "caves_mesh_and_strokes", test_caves_mesh_and_strokes },
    { "caves_coupling", test_caves_coupling },
    { "cave_layers", test_cave_layers });
