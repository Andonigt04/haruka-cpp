/**
 * @file test_caves.cpp
 * @brief Las cuevas: siembra determinista, mapas horneados, conectividad GARANTIZADA y campo.
 *
 * El invariante duro es el de §A.3 del plan: **el aire de un sistema es UNA sola pieza**. No se
 * confía a que la intersección de dos mapas conexos salga conexa —no lo es— sino al post-proceso,
 * y aquí se mide cuántas componentes había ANTES para que ese número no sea una suposición.
 *
 * Cada comprobación lleva su contraprueba: un test que sólo mira lo que quiere ver no mide nada.
 */
#include "test_common.h"
#include "core/terrain/cave_system.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace Haruka;

namespace {

constexpr double kR = 6371000.0;   // radio de la Tierra: la escala en la que se calibró todo

/// Una caja de prueba: la primera celda con cueva que se encuentre barriendo direcciones.
bool findBox(uint32_t seed, CaveBox& out, glm::dvec3* outDir = nullptr) {
    caveSetAutoProbability(0.14f);   // la siembra esta a 0 por defecto (cuevas a mano): el banco la enciende
    for (int i = 0; i < 4000; ++i) {
        const double a = (double)i * 0.017, b = (double)i * 0.0031;
        const glm::dvec3 d = glm::normalize(glm::dvec3(std::cos(a) * std::cos(b), std::sin(b),
                                                       std::sin(a) * std::cos(b)));
        if (caveBoxAt(seed, d, kR, out)) { if (outDir) *outDir = d; return true; }
    }
    return false;
}

/// Componentes conexas (26 vecinos) del aire de un volumen. Recalculado AQUÍ, sin fiarse de la
/// estadística que devuelve el horneado: si el test usara el mismo recuento que el código que
/// prueba, no probaría nada.
int componentesDeAire(const CaveVolume& v) {
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

} // namespace

void test_caves_bake() {
    beginTest("cuevas: siembra, mapas y conectividad");

    // ── (1) SIEMBRA: determinista, rara, y la misma para dos observadores ───────────────────────
    CaveBox box; glm::dvec3 dirHit(0.0);
    const bool found = findBox(1234u, box, &dirHit);
    CHECK(found, "hay celdas que siembran cueva");
    if (!found) return;

    CaveBox again;
    CHECK(caveBoxAt(1234u, dirHit, kR, again), "la misma direccion vuelve a dar cueva");
    CHECK(again.cell == box.cell && again.radiusM == box.radiusM && again.depthM == box.depthM,
          "y la MISMA caja: funcion pura de (seed, celda)");
    CaveBox otra;
    const bool otraSeed = caveBoxAt(4321u, dirHit, kR, otra);
    CHECK(!otraSeed || otra.radiusM != box.radiusM || otra.depthM != box.depthM,
          "CONTRAPRUEBA: con otra seed la celda no da la misma cueva");

    // Frecuencia: ni todas las celdas ni ninguna. Se mide sobre un barrido real.
    int conCueva = 0, total = 0;
    for (int i = 0; i < 2000; ++i) {
        const double a = (double)i * 0.0137, b = (double)i * 0.0071;
        const glm::dvec3 d = glm::normalize(glm::dvec3(std::cos(a) * std::cos(b), std::sin(b),
                                                       std::sin(a) * std::cos(b)));
        CaveBox t;
        ++total; if (caveBoxAt(1234u, d, kR, t)) ++conCueva;
    }
    std::printf("    siembra: %d de %d direcciones caen en celda con cueva (%.1f %%)\n",
                conCueva, total, 100.0 * conCueva / total);
    CHECK(conCueva > 0, "se siembran cuevas");
    CHECK(conCueva < total, "CONTRAPRUEBA: NO todas las celdas tienen cueva");
    // Y con la siembra a CERO (el defecto: cuevas a mano) no sale ninguna, pero la celda se puede
    // pedir a mano y da la MISMA caja que sembrada.
    caveSetAutoProbability(0.0f);
    CaveBox ninguna;
    CHECK(!caveBoxAt(1234u, dirHit, kR, ninguna), "con la siembra a 0 no hay cuevas automaticas");
    CaveBox aMano;
    caveBoxForCell(1234u, dirHit, kR, aMano);
    CHECK(aMano.cell == box.cell && aMano.radiusM == box.radiusM, "a mano se obtiene la MISMA caja de esa celda");
    caveSetAutoProbability(0.14f);

    // ── (2) LOS MAPAS: existen, tienen contraste y no son constantes ────────────────────────────
    CaveMap planta, perfil;
    caveBakeMaps(box, kCaveMapRes, planta, perfil);
    auto stats = [](const CaveMap& m, float& lo, float& hi, float& mean) {
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
    CaveMap planta2, perfil2;
    caveBakeMaps(box, kCaveMapRes, planta2, perfil2);
    CHECK(planta.v == planta2.v && perfil.v == perfil2.v, "rehornear da los MISMOS texels");

    // ── (3) CONECTIVIDAD: el invariante duro, y el numero que justifica el post-proceso ─────────
    CaveVolume vol; glm::ivec3 st(0);
    caveBakeVolume(box, planta, perfil, kCaveVolRes, vol, &st);
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
        CaveSystem sys; sys.box = box; sys.planta = planta; sys.perfil = perfil; sys.vol = vol;
        const glm::dvec3 world = box.toWorld(local);
        const float d = caveDensityAt(sys, world);
        std::printf("    campo en un voxel de aire: %.2f m (negativo = aire)\n", d);
        CHECK(d < 0.0f, "el punto que el volumen marca AIRE tambien lo es al muestrear por el mundo");

        // Contraprueba: un punto a 10 km de la caja tiene que ser roca (sin efecto a distancia).
        const glm::dvec3 lejos = glm::normalize(box.centerDir + box.tangentU * 0.002) * box.surfaceRM;
        CHECK(caveDensityAt(sys, lejos) > 0.0f, "CONTRAPRUEBA: fuera de la caja no hay cueva");

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
            mejor = std::max(mejor, glm::length(caveDensityGradient(sys, box.toWorld(lp), voxM)));
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
    CaveSystem sys; sys.box = box; sys.planta = planta; sys.perfil = perfil; sys.vol = vol;
    int bocaTexels = 0, sup = 0;
    float maxMask = 0.0f;
    for (int j = 0; j < 64; ++j)
        for (int i = 0; i < 64; ++i) {
            const float u = ((float)i / 63.0f - 0.5f) * 2.0f * box.radiusM;
            const float v = ((float)j / 63.0f - 0.5f) * 2.0f * box.radiusM;
            const glm::dvec3 d = glm::normalize(box.centerDir * box.surfaceRM
                                              + box.tangentU * (double)u + box.tangentV * (double)v);
            const float m = caveEntranceMask(sys, d);
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

    CaveBox box;
    if (!findBox(1234u, box)) { CHECK(false, "hay una celda con cueva para probar"); return; }
    CaveSystem sys; sys.box = box;
    caveBakeMaps(box, kCaveMapRes, sys.planta, sys.perfil);
    caveBakeVolume(box, sys.planta, sys.perfil, kCaveVolRes, sys.vol, &sys.connectStats);

    // ── (1) LA MALLA SIGUE AL CAMPO ─────────────────────────────────────────────────────────────
    CaveMesh mesh;
    caveBuildMesh(sys, mesh);
    std::printf("    malla: %zu vertices · %zu triangulos\n", mesh.positions.size(), mesh.triangleCount());
    CHECK(!mesh.empty(), "la cueva tiene paredes");
    CHECK(mesh.positions.size() == mesh.normals.size(), "una normal por vertice");

    // El invariante que de verdad ata malla y campo: TODO vertice esta en la superficie del campo,
    // o sea |d| pequeño. Si la malla se separa del campo, se ve una pared donde no la hay (y se
    // atraviesa la que sí hay) — que es justo el bug que la colision analitica viene a evitar.
    double peor = 0.0, media = 0.0; size_t fuera = 0;
    const float voxM = std::min(2.0f * box.radiusM, box.depthM) / (float)(kCaveVolRes - 1);
    for (const glm::vec3& v : mesh.positions) {
        const float d = std::fabs(caveDensityAt(sys, box.toWorld(v)));
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
        CHECK(caveDensityAt(sys, wp) > 0.0f, "antes de picar: es roca");

        const int abiertos = caveApplyStrokeWorld(sys, wp, 8.0f, true);
        std::printf("    picar r=8 m: %d voxeles pasan a aire · campo en el centro %.2f m\n",
                    abiertos, caveDensityAt(sys, wp));
        CHECK(abiertos > 0, "picar ABRE hueco");
        CHECK(caveDensityAt(sys, wp) < 0.0f, "y el punto picado es aire");

        // Y rellenar es el mismo gesto al reves: vuelve a ser roca.
        const int cerrados = caveApplyStrokeWorld(sys, wp, 8.0f, false);
        std::printf("    rellenar r=8 m: %d voxeles vuelven a roca · campo %.2f m\n",
                    cerrados, caveDensityAt(sys, wp));
        CHECK(cerrados > 0 && caveDensityAt(sys, wp) > 0.0f,
              "rellenar es el MISMO mecanismo con el signo cambiado");

        // Fuera de la caja el trazo no aplica: un sistema no tiene efecto a distancia.
        const glm::dvec3 lejosW = glm::normalize(box.centerDir + box.tangentU * 0.01) * box.surfaceRM;
        CHECK(caveApplyStrokeWorld(sys, lejosW, 8.0f, true) < 0,
              "CONTRAPRUEBA: un trazo fuera de la caja no toca este sistema");

        // La malla nota el cambio: picar una sala nueva anade superficie.
        const size_t trisAntes = mesh.triangleCount();
        caveApplyStrokeWorld(sys, wp, 12.0f, true);
        CaveMesh m2; caveBuildMesh(sys, m2);
        std::printf("    triangulos: %zu antes · %zu tras picar\n", trisAntes, m2.triangleCount());
        CHECK(m2.triangleCount() > trisAntes, "la malla sigue al campo tras el trazo");
    }

    // ── (3) LOS TRAZOS SOBREVIVEN EN LA PARTIDA (no en la cache) ────────────────────────────────
    std::vector<CaveStroke> tr = { {glm::vec3(10.0f, -4.0f, 30.0f), 6.0f, true},
                                   {glm::vec3(0.0f, 0.0f, 50.0f), 3.5f, false} };
    const std::string path = (std::filesystem::temp_directory_path() / "haruka_cave_test" / "trazos.txt").string();
    CHECK(caveSaveStrokes(path, tr), "los trazos se guardan");
    std::vector<CaveStroke> leidos;
    CHECK(caveLoadStrokes(path, leidos), "y se leen");
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

    CaveBox box;
    if (!findBox(1234u, box)) { CHECK(false, "hay una celda con cueva para probar"); return; }
    CaveSystem sys; sys.box = box;
    caveBakeMaps(box, kCaveMapRes, sys.planta, sys.perfil);
    caveBakeVolume(box, sys.planta, sys.perfil, kCaveVolRes, sys.vol, &sys.connectStats);
    caveBakeEntrance(box, sys.vol, kCaveMapRes, sys.boca);

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
            const float cut = caveSurfaceCut(sys, dir);
            // ¿Hay aire en los primeros metros bajo ESTE punto?
            const float d = caveDensityAt(sys, glm::normalize(dir) * (box.surfaceRM - 3.0));
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
            if (caveSurfaceCut(sys, dir) < 0.01f &&
                caveDensityAt(sys, glm::normalize(dir) * (box.surfaceRM - 6.0)) > 5.0f)
                idx = j * 96 + i;
        }
    CHECK(idx >= 0, "hay un punto con suelo macizo donde picar");
    if (idx >= 0) {
        const float lx = ((float)(idx % 96) / 95.0f - 0.5f) * 2.0f * box.radiusM * 0.6f;
        const float ly = ((float)(idx / 96) / 95.0f - 0.5f) * 2.0f * box.radiusM * 0.6f;
        const glm::dvec3 dir = glm::normalize(box.toWorld(glm::vec3(lx, ly, 0.0f)));
        const glm::dvec3 wp = glm::normalize(dir) * (box.surfaceRM - 5.0);
        CHECK(caveSurfaceCut(sys, dir) < 0.01f, "antes de picar: aqui hay suelo");
        const int n = caveApplyStrokeWorld(sys, wp, 10.0f, true);
        caveBakeEntrance(box, sys.vol, kCaveMapRes, sys.boca);   // el mapa de boca es DERIVADO
        const float cut = caveSurfaceCut(sys, dir);
        std::printf("    picar bajo el suelo (r=10 m, %d voxeles): recorte %.2f -> %.2f\n", n, 0.0, cut);
        CHECK(cut > 0.5f, "picar ABRE boca: el terreno deja de dibujarse ahi");
    }
}

void test_caves_cache() {
    beginTest("cuevas: el fichero ES la definicion");

    CaveBox box;
    if (!findBox(777u, box)) { CHECK(false, "hay una celda con cueva para probar el cache"); return; }

    // Carpeta temporal propia: el test no debe ensuciar (ni leer) los horneados del proyecto.
    const std::string dir = (std::filesystem::temp_directory_path() / "haruka_cave_test").string();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    CaveSystem a;
    const bool ok1 = caveLoadOrBake(box, a, dir, 128, 48);
    CHECK(ok1 && a.valid(), "primera vez: hornea");

    int ficheros = 0;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) { (void)e; ++ficheros; }
    std::printf("    ficheros escritos: %d (planta, perfil, volumen, boca)\n", ficheros);
    CHECK(ficheros == 4, "y deja los CUATRO mapas en disco");

    CaveSystem b;
    const bool ok2 = caveLoadOrBake(box, b, dir, 128, 48);
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
    CaveBox box2 = box; box2.depthM += 37.0f;
    CHECK(caveCacheKey(box, 128, 48) != caveCacheKey(box2, 128, 48),
          "CONTRAPRUEBA: cambiar la caja cambia la clave (no se reutiliza un bake ajeno)");

    std::filesystem::remove_all(dir, ec);
}
