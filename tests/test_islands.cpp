/**
 * @file test_islands.cpp
 * @brief Islas flotantes: mapas en disco, una pieza, y roca AÑADIDA al mismo campo que las cuevas.
 *
 * Lo que se demuestra aquí y no en otro sitio: que el campo del mundo puede PONER roca donde el
 * terreno dice aire. Con `min(terreno, cueva)` eso era imposible por construcción — construir sobre
 * el suelo no hacía nada, y una isla flotante es lo mismo a 200 m de altura. Cada test lleva su
 * contraprueba: lo que el campo viejo habría dicho, calculado con las mismas muestras.
 */
#include "test_common.h"
#include "world/vox/island_system.h"
#include "world/vox/vox_world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace Haruka;

namespace {
constexpr double kR = 6371000.0;

/// Componentes conexas (4 vecinos) de `planta > 0.5`, contadas AQUÍ y no con el código que las
/// garantiza (si contara el mismo código, el test sería una tautología).
int componentesPlanta(const CaveMap& m) {
    std::vector<uint8_t> in((size_t)m.w * m.h);
    for (size_t i = 0; i < in.size(); ++i) in[i] = m.v[i] > 0.5f ? 1 : 0;
    std::vector<uint8_t> seen(in.size(), 0);
    std::vector<int> st;
    int comps = 0;
    for (size_t s = 0; s < in.size(); ++s) {
        if (!in[s] || seen[s]) continue;
        ++comps; st.assign(1, (int)s); seen[s] = 1;
        while (!st.empty()) {
            const int i = st.back(); st.pop_back();
            const int x = i % m.w, y = i / m.w;
            const int nb[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
            for (auto& d : nb) {
                const int nx = x + d[0], ny = y + d[1];
                if (nx < 0 || ny < 0 || nx >= m.w || ny >= m.h) continue;
                const size_t j = (size_t)ny * m.w + nx;
                if (in[j] && !seen[j]) { seen[j] = 1; st.push_back((int)j); }
            }
        }
    }
    return comps;
}
} // namespace

void test_islands_bake() {
    beginTest("islas: horneado determinista, UNA pieza, campo con signo, cache en disco");

    const std::string tmp = (std::filesystem::temp_directory_path() / "haruka_islands").string();
    std::error_code ec; std::filesystem::remove_all(tmp, ec);

    const glm::dvec3 d = glm::normalize(glm::dvec3(0.3, 0.8, 0.5));
    IslandBox box;
    islandBoxForCell(77u, d, kR, box, [](const glm::dvec3&, void*) { return 120.0f; }, nullptr);
    std::printf("    caja: medio lado %.0f m · cima %.0f m · raiz %.0f m · a %.0f m sobre la cota (base R+%.0f m)\n",
                box.radiusM, box.topM, box.rootM, box.altitudeM, box.baseRM - kR);
    CHECK(box.baseRM > kR + 120.0 + 100.0, "la base flota por encima de la cota del terreno en el centro");
    CHECK(box.radiusM >= 200.0f && box.radiusM <= 350.0f, "medio lado en el rango de diseño");

    // (1) Determinismo: dos horneados, los mismos texels.
    CaveMap p1, c1, r1, p2, c2, r2;
    glm::ivec3 st1, st2;
    const auto t0 = std::chrono::high_resolution_clock::now();
    islandBakeMaps(box, kIslandMapRes, p1, c1, r1, &st1);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    islandBakeMaps(box, kIslandMapRes, p2, c2, r2, &st2);
    CHECK(p1.v == p2.v && c1.v == c2.v && r1.v == r2.v, "mismo (seed, celda) -> mismos texels");
    IslandBox otra = box; otra.seed ^= 1u;
    CaveMap p3, c3, r3;
    islandBakeMaps(otra, kIslandMapRes, p3, c3, r3);
    CHECK(p3.v != p1.v, "CONTRAPRUEBA: otra seed, otra silueta");

    // (2) Una pieza: componentes contadas aparte.
    const int comps = componentesPlanta(p1);
    const float frac = (float)st1.y / (float)(p1.w * p1.h);
    std::printf("    horneado en %.1f ms · silueta: %d componentes antes -> %d · %.0f %% de la caja\n",
                ms, st1.x, comps, frac * 100.0f);
    CHECK(comps == 1, "la silueta es UNA componente");
    CHECK(frac > 0.15f && frac < 0.7f, "y ocupa una fraccion razonable de la caja");
    CHECK(ms < 200.0, "hornear tres mapas cuesta milisegundos (va sin hilo)");

    // (3) El campo: roca dentro, aire arriba, abajo y fuera. Y el centinela es de AIRE.
    IslandSystem sys; sys.box = box; sys.planta = p1; sys.cima = c1; sys.raiz = r1;
    // Un punto claramente DENTRO de la silueta: el texel con más distancia al borde.
    size_t best = 0;
    for (size_t i = 0; i < p1.v.size(); ++i) if (p1.v[i] > p1.v[best]) best = i;
    const float bu = (float)(best % p1.w) / (float)(p1.w - 1), bv = (float)(best / p1.w) / (float)(p1.h - 1);
    const glm::vec3 lc((bu - 0.5f) * 2.0f * box.radiusM, (bv - 0.5f) * 2.0f * box.radiusM, 0.0f);
    float topH = 0, botH = 0;
    CHECK(islandColumnAt(sys, glm::normalize(box.toWorld(lc)), topH, botH), "la columna central esta en la silueta");
    std::printf("    columna mas interior: cima %+.1f m · raiz %+.1f m · borde a %.1f m\n", topH, botH, (p1.v[best] - 0.5f) * 2.0f * kIslandRangeM);
    CHECK(topH > 0.0f && botH < 0.0f, "cima por encima de la base, raiz por debajo");
    const float dIn   = islandDensityAt(sys, box.toWorld(glm::vec3(lc.x, lc.y, 0.5f * (topH + botH))));
    const float dUp   = islandDensityAt(sys, box.toWorld(glm::vec3(lc.x, lc.y, topH + 5.0f)));
    const float dDown = islandDensityAt(sys, box.toWorld(glm::vec3(lc.x, lc.y, botH - 5.0f)));
    const float dOut  = islandDensityAt(sys, box.toWorld(glm::vec3(box.radiusM * 3.0f, 0.0f, 0.0f)));
    std::printf("    campo: dentro %+.2f · 5 m sobre la cima %+.2f · 5 m bajo la raiz %+.2f · fuera de la caja %.0e\n", dIn, dUp, dDown, dOut);
    CHECK(dIn > 0.0f, "dentro es roca");
    CHECK(dUp < 0.0f && dDown < 0.0f, "sobre la cima y bajo la raiz es aire");
    CHECK(dOut < -1e8f, "fuera de la caja: centinela de AIRE (la isla se AÑADE, no se quita)");
    // La superficie de la cima esta donde dice la columna: el campo cambia de signo ahi.
    CHECK(islandDensityAt(sys, box.toWorld(glm::vec3(lc.x, lc.y, topH - 0.5f))) > 0.0f &&
          islandDensityAt(sys, box.toWorld(glm::vec3(lc.x, lc.y, topH + 0.5f))) < 0.0f,
          "el cero del campo esta en la cota de la cima (+-0,5 m)");

    // (4) Cache: tres ficheros, y recargar da lo mismo (a la cuantizacion del PNG).
    IslandSystem a, b;
    CHECK(islandLoadOrBake(box, a, tmp), "hornea y guarda");
    const std::string base = tmp + "/island_" + islandCacheKey(box, kIslandMapRes);
    CHECK(std::filesystem::exists(base + "_planta.png") && std::filesystem::exists(base + "_cima.png") &&
          std::filesystem::exists(base + "_raiz.png"), "tres PNG en bakes/");
    CHECK(a.stats.x >= 1, "el primer horneado trae estadistica");
    CHECK(islandLoadOrBake(box, b, tmp), "recarga");
    CHECK(b.stats.x == 0, "la recarga viene del fichero (sin estadistica de horneado)");
    float worst = 0.0f;
    for (size_t i = 0; i < a.planta.v.size(); ++i) worst = std::max(worst, std::fabs(a.planta.v[i] - b.planta.v[i]));
    std::printf("    ida y vuelta por PNG: peor diferencia %.4f (1/255 = %.4f)\n", worst, 1.0f / 255.0f);
    CHECK(worst <= 1.0f / 255.0f + 1e-5f, "recargar del PNG da los mismos texels (a un paso de 8 bits)");
    CHECK(islandCacheKey(box, kIslandMapRes) != islandCacheKey(otra, kIslandMapRes), "CONTRAPRUEBA: otra seed, otra clave");
    std::filesystem::remove_all(tmp, ec);
}

void test_vox_island() {
    beginTest("vox: isla COLOCADA = roca añadida al campo; construir sobre el suelo; sin costura");

    const std::string tmp = (std::filesystem::temp_directory_path() / "haruka_vox_island").string();
    std::error_code ec; std::filesystem::remove_all(tmp, ec);
    caveSetAutoProbability(0.0f);

    VoxWorld w;
    w.configure(7u, kR, tmp + "/bakes", tmp + "/save", [](const glm::dvec3&) { return 500.0f; });
    w.setSynchronousBake(true);
    const glm::dvec3 d = glm::normalize(glm::dvec3(-0.4, 0.7, 0.6));

    // (1) Colocar, guardar, recargar.
    CHECK(w.placeIsland(d), "se coloca un borrador de isla");
    CHECK(!w.placeIsland(d), "CONTRAPRUEBA: la misma celda no dos veces");
    CHECK(w.islandDefs().size() == 1, "y es una DEFINICION con nombre (lo que iria a la escena)");
    const auto boxes = w.placedIslandBoxes();
    CHECK(boxes.size() == 1, "una caja");
    if (boxes.empty()) return;
    const IslandBox& B = boxes[0];
    {
        VoxWorld w2;
        w2.configure(7u, kR, tmp + "/bakes", tmp + "/save", [](const glm::dvec3&) { return 500.0f; });
        w2.setIslands(w.islandDefs());
        CHECK(w2.placedIslandBoxes().size() == 1 && std::fabs(w2.placedIslandBoxes()[0].baseRM - B.baseRM) < 1e-6,
              "la misma definicion en otro mundo da la misma caja");
    }

    // Cargar todos los chunks que la caja cruza (como hara el streaming).
    IslandSystem sys;
    CHECK(islandLoadBox(w.islandDefs()[0], B, sys), "la isla se hornea (borrador)");
    std::vector<VoxKey> keys;
    for (double x = -B.radiusM; x <= B.radiusM; x += 60.0)
        for (double y = -B.radiusM; y <= B.radiusM; y += 60.0)
            for (double h = -B.rootM - 30.0; h <= B.topM + 30.0; h += 60.0) {
                const VoxKey k = w.keyAt(B.toWorld(glm::vec3((float)x, (float)y, (float)h)));
                if (std::find(keys.begin(), keys.end(), k) == keys.end()) { keys.push_back(k); w.ensure(k); }
            }
    int conRoca = 0;
    for (const VoxKey& k : keys) if (w.get(k) && !w.get(k)->a.empty()) ++conRoca;
    std::printf("    isla: %zu chunks la cruzan · %d con canal añadido reservado · base R+%.0f m (suelo R+500) · %.2f MB de voxel\n",
                keys.size(), conRoca, B.baseRM - kR, w.voxelBytes() / 1048576.0);
    CHECK(conRoca > 0 && conRoca <= (int)keys.size(), "los chunks que toca reservan el canal añadido, y solo esos");
    // MEMORIA: sólo pagan los chunks con isla (un canal, 131 KB); la roca maciza de alrededor, nada.
    CHECK(w.voxelBytes() == (size_t)conRoca * kVoxN * kVoxN * kVoxN * sizeof(float),
          "los bytes de voxel son exactamente los de los chunks con isla: la roca maciza no reserva");
    CHECK(w.loadedWithContent() == (size_t)conRoca, "y son los unicos 'con contenido'");

    // (2) El campo: roca en la isla, aire alrededor, y el SUELO NO CAMBIA.
    size_t best = 0;
    for (size_t i = 0; i < sys.planta.v.size(); ++i) if (sys.planta.v[i] > sys.planta.v[best]) best = i;
    const float bu = (float)(best % sys.planta.w) / (float)(sys.planta.w - 1), bv = (float)(best / sys.planta.w) / (float)(sys.planta.h - 1);
    const glm::vec3 lc((bu - 0.5f) * 2.0f * B.radiusM, (bv - 0.5f) * 2.0f * B.radiusM, 0.0f);
    float topH = 0, botH = 0;
    islandColumnAt(sys, glm::normalize(B.toWorld(lc)), topH, botH);
    const glm::dvec3 pIn = B.toWorld(glm::vec3(lc.x, lc.y, 0.5f * (topH + botH)));
    const glm::dvec3 pTop = B.toWorld(glm::vec3(lc.x, lc.y, topH + 3.0f));
    const glm::dvec3 colDir = glm::normalize(pIn);
    std::printf("    campo: dentro %+.2f · 3 m sobre la cima %+.2f · suelo bajo la isla: -1 m %+.2f · +1 m %+.2f\n",
                w.density(pIn), w.density(pTop), w.density(colDir * (kR + 499.0)), w.density(colDir * (kR + 501.0)));
    CHECK(w.density(pIn) > 0.0f, "dentro de la isla el campo del MUNDO es roca");
    CHECK(w.density(pTop) < 0.0f, "sobre su cima, aire");
    // CONTRAPRUEBA del campo viejo: min(terreno, quitado) en el mismo punto dice AIRE (la isla no
    // existiria). Es exactamente lo que pasaba con un solo canal.
    CHECK(std::min(w.caveDensity(pIn), w.terrainDensity(pIn)) < 0.0f, "CONTRAPRUEBA: con min(terreno, quitado) la isla NO existe");
    CHECK(w.density(colDir * (kR + 499.0)) > 0.0f && w.density(colDir * (kR + 501.0)) < 0.0f, "el suelo bajo la isla sigue donde estaba");
    CHECK(w.surfaceCut(colDir) == 0.0f && w.floorDepthM(colDir) == 0.0f, "y no lo recorta ni le hunde el lecho");

    // (3) La malla: la cima tiene caras hacia ARRIBA, y todo cae dentro de la caja.
    {
        CaveMesh m; w.buildMesh(w.keyAt(B.toWorld(glm::vec3(lc.x, lc.y, topH - 1.0f))), m);
        int arriba = 0, fuera = 0;
        for (size_t i = 0; i < m.positions.size(); ++i) {
            const glm::dvec3 pw = m.origin + glm::dvec3(m.positions[i]);
            if (!B.contains(pw)) ++fuera;
            if (glm::dot(glm::dvec3(m.normals[i]), glm::normalize(pw)) > 0.7) ++arriba;
        }
        std::printf("    malla del chunk de la cima: %zu tris · %d normales hacia arriba · %d vertices fuera de la caja\n",
                    m.triangleCount(), arriba, fuera);
        CHECK(m.triangleCount() > 0, "el chunk de la cima tiene malla");
        CHECK(arriba > 0, "con caras hacia arriba (la cima es suelo)");
        CHECK(fuera == 0, "y nada fuera de la caja de la isla");
        // El BOBINADO: consistente en toda la malla y con el signo que Jolt toma como cara
        // delantera para los rigidos — `(b-a)x(c-a)` a favor de la normal de vertice (hacia la
        // roca). Que ESE signo es el bueno lo valida `test_physics_island` (un rigido se posa en la
        // cima; con el contrario atravesaba la cima y paraba en la raiz por dentro). Aqui se guarda
        // que no cambie sin querer.
        int pos = 0, neg = 0;
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            const glm::vec3 a = m.positions[m.indices[t]], b = m.positions[m.indices[t + 1]], c = m.positions[m.indices[t + 2]];
            const glm::vec3 g = glm::cross(b - a, c - a);
            const float s = glm::dot(g, m.normals[m.indices[t]] + m.normals[m.indices[t + 1]] + m.normals[m.indices[t + 2]]);
            if (s > 0.0f) ++pos; else if (s < 0.0f) ++neg;
        }
        std::printf("    bobinado: %d triangulos con el signo de Jolt · %d al reves\n", pos, neg);
        CHECK(neg * 50 < pos, "el bobinado es el que Jolt toma como cara delantera (validado en physics_island)");
        // CONTRAPRUEBA: el chunk del SUELO bajo la isla no tiene malla (el suelo lo dibuja el pase
        // de nodos; si aqui saliera algo, se dibujaria dos veces).
        CaveMesh g; w.buildMesh(w.keyAt(colDir * (kR + 499.0)), g);
        CHECK(g.triangleCount() == 0, "CONTRAPRUEBA: el chunk del suelo bajo la isla no emite nada");
    }

    // (3b) TODA la cima tiene malla, en TODOS los chunks. Se recorren columnas de la silueta y para
    //      cada una se busca el vértice más cercano a su cota de cima en la unión de las mallas de
    //      todos los chunks: si hay un chunk que no emite su parte del techo, salen columnas huérfanas
    //      (en el juego: un chunk con la cima y el vecino enseñando la raíz por dentro).
    {
        std::vector<glm::dvec3> verts;
        for (const VoxKey& k : keys) {
            CaveMesh m; w.buildMesh(k, m);
            for (const glm::vec3& v : m.positions) verts.push_back(m.origin + glm::dvec3(v));
        }
        int columnas = 0, huerfanas = 0; double peor = 0.0;
        for (double x = -B.radiusM; x <= B.radiusM; x += 25.0)
            for (double y = -B.radiusM; y <= B.radiusM; y += 25.0) {
                float th, bh;
                const glm::dvec3 dir = glm::normalize(B.toWorld(glm::vec3((float)x, (float)y, 0.0f)));
                if (!islandColumnAt(sys, dir, th, bh)) continue;
                if (sys.planta.at((float)(x / (2.0 * B.radiusM)) + 0.5f, (float)(y / (2.0 * B.radiusM)) + 0.5f) < 0.6f) continue;   // a >5 m del borde
                ++columnas;
                const glm::dvec3 pt = B.toWorld(glm::vec3((float)x, (float)y, th));
                double best = 1e9;
                for (const glm::dvec3& v : verts) best = std::min(best, glm::length(v - pt));
                peor = std::max(peor, best);
                if (best > 5.0) ++huerfanas;
            }
        std::printf("    cima: %d columnas · sin vertice a <5 m: %d · la mas lejana a %.1f m (%zu vertices en %zu chunks)\n",
                    columnas, huerfanas, peor, verts.size(), keys.size());
        CHECK(columnas > 20, "hay columnas de cima que medir");
        CHECK(huerfanas == 0, "TODA la cima tiene malla (ningun chunk se salta su parte del techo)");
    }

    // (3c) LO MISMO SOBRE UN MONTE: terreno en pendiente fuerte (1,5 m/m) que por un lado sube por
    //      encima de la base de la isla. Las columnas de cima que quedan sobre el terreno tienen que
    //      tener malla igual; las que el monte se traga, no (las dibuja el terreno).
    {
        static glm::dvec3 s_c, s_e1;
        s_c = B.centerDir; s_e1 = glm::normalize(glm::cross(glm::dvec3(0, 1, 0), B.centerDir));
        VoxWorld ws;
        // Un embudo con la isla en el fondo: el monte sube 2,5 m/m en todas direcciones.
        ws.configure(7u, kR, tmp + "/bakes", tmp + "/save_slope", [](const glm::dvec3& dir) -> float {
            return 500.0f + 2.5f * (float)(std::acos(std::clamp(glm::dot(dir, s_c), -1.0, 1.0)) * kR);
        });
        ws.setSynchronousBake(true);
        CHECK(ws.placeIsland(d), "isla sobre el monte");
        const IslandBox Bs = ws.placedIslandBoxes()[0];
        IslandSystem ss; islandLoadBox(ws.islandDefs()[0], Bs, ss);
        std::vector<VoxKey> ks;
        for (double x = -Bs.radiusM; x <= Bs.radiusM; x += 60.0)
            for (double y = -Bs.radiusM; y <= Bs.radiusM; y += 60.0)
                for (double h = -Bs.rootM - 30.0; h <= Bs.topM + 30.0; h += 60.0) {
                    const VoxKey k = ws.keyAt(Bs.toWorld(glm::vec3((float)x, (float)y, (float)h)));
                    if (std::find(ks.begin(), ks.end(), k) == ks.end()) { ks.push_back(k); ws.ensure(k); }
                }
        std::vector<glm::dvec3> verts;
        for (const VoxKey& k : ks) { CaveMesh m; ws.buildMesh(k, m); for (const glm::vec3& v : m.positions) verts.push_back(m.origin + glm::dvec3(v)); }
        int columnas = 0, huerfanas = 0, tragadas = 0; double peor = 0.0;
        for (double x = -Bs.radiusM; x <= Bs.radiusM; x += 25.0)
            for (double y = -Bs.radiusM; y <= Bs.radiusM; y += 25.0) {
                float th, bh;
                const glm::dvec3 dir = glm::normalize(Bs.toWorld(glm::vec3((float)x, (float)y, 0.0f)));
                if (!islandColumnAt(ss, dir, th, bh)) continue;
                if (ss.planta.at((float)(x / (2.0 * Bs.radiusM)) + 0.5f, (float)(y / (2.0 * Bs.radiusM)) + 0.5f) < 0.6f) continue;
                const glm::dvec3 pt = Bs.toWorld(glm::vec3((float)x, (float)y, th));
                if (ws.terrainDensity(pt) > -2.0f) { ++tragadas; continue; }   // el monte esta por encima
                ++columnas;
                double best = 1e9;
                for (const glm::dvec3& v : verts) best = std::min(best, glm::length(v - pt));
                peor = std::max(peor, best);
                if (best > 5.0) ++huerfanas;
            }
        std::printf("    sobre el monte: %d columnas de cima al aire · %d tragadas por el monte · sin vertice a <5 m: %d · la mas lejana a %.1f m\n",
                    columnas, tragadas, huerfanas, peor);
        CHECK(columnas > 5 && tragadas > 0, "la pendiente parte la isla: parte al aire, parte dentro del monte");
        CHECK(huerfanas == 0, "y toda la cima al aire tiene malla");
    }

    // (4) Sin costura, en los dos canales.
    int faces = 0;
    const float seam = w.seamMismatch(&faces);
    std::printf("    costura: peor |delta| %.4f m en %d caras\n", seam, faces);
    CHECK(faces > 0 && seam < 1e-4f, "el campo es continuo entre chunks (canal quitado Y añadido)");

    // (5) EL MISMO GESTO: picar quita isla; construir pone roca EN EL AIRE y SOBRE EL SUELO.
    const int quitados = w.stroke(pIn, 10.0f, true);
    std::printf("    picar en la isla: %d voxeles cambian · campo ahi %+.2f\n", quitados, w.density(pIn));
    CHECK(quitados > 0 && w.density(pIn) < 0.0f, "picar en la isla abre aire");
    const glm::dvec3 pAir = B.toWorld(glm::vec3(lc.x, lc.y, topH + 30.0f));
    CHECK(w.density(pAir) < 0.0f, "30 m sobre la cima es aire");
    const int puestos = w.stroke(pAir, 8.0f, false);
    std::printf("    construir en el aire: %d voxeles cambian · campo ahi %+.2f\n", puestos, w.density(pAir));
    CHECK(puestos > 0 && w.density(pAir) > 0.0f, "construir en el aire pone roca");
    // Sobre el SUELO, lejos de la isla: un parapeto de 6 m de radio a 3 m sobre la cota.
    const glm::dvec3 d2 = glm::normalize(glm::dvec3(0.5, 0.6, -0.7));
    const glm::dvec3 pGround = d2 * (kR + 503.0);
    CHECK(w.density(pGround) < 0.0f, "3 m sobre el suelo es aire");
    const int parapeto = w.stroke(pGround, 6.0f, false);
    CHECK(parapeto > 0 && w.density(pGround) > 0.0f, "construir sobre el suelo pone roca");
    CHECK(std::min(w.caveDensity(pGround), w.terrainDensity(pGround)) < 0.0f, "CONTRAPRUEBA: con min(terreno, quitado) el parapeto NO existe");
    {
        CaveMesh m; w.buildMesh(w.keyAt(pGround), m);
        int sobreSuelo = 0;
        for (const glm::vec3& v : m.positions) if (glm::length(m.origin + glm::dvec3(v)) > kR + 500.5) ++sobreSuelo;
        std::printf("    parapeto: %zu tris · %d vertices por encima del suelo\n", m.triangleCount(), sobreSuelo);
        CHECK(sobreSuelo > 0, "el parapeto tiene malla POR ENCIMA del suelo (el faldon no lo aplasta)");
    }
    // Y picarlo lo quita (los dos canales bajan).
    w.stroke(pGround, 7.0f, true);
    CHECK(w.density(pGround) < 0.0f, "picar el parapeto lo quita");

    // (6) La partida: descargar y volver a cargar reproduce los tres trazos.
    w.unloadFar(d, -1.0);
    CHECK(w.loadedCount() == 0, "todo descargado");
    for (const VoxKey& k : keys) w.ensure(k);
    w.ensure(w.keyAt(pGround));
    CHECK(w.density(pIn) < 0.0f && w.density(pAir) > 0.0f && w.density(pGround) < 0.0f,
          "al recargar, lo picado sigue picado y lo construido construido");

    // (7) Quitar la isla la quita.
    CHECK(w.removeIsland(d), "se quita");
    for (const VoxKey& k : keys) w.ensure(k);
    CHECK(w.density(B.toWorld(glm::vec3(lc.x + 40.0f, lc.y, 0.5f * (topH + botH)))) < 0.0f, "y donde estaba, aire");
    std::filesystem::remove_all(tmp, ec);
}
