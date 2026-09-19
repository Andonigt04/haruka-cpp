/**
 * @file test_vox_world.cpp
 * @brief El campo volumétrico del mundo por chunks: una sola forma de tocar el mundo, sin costuras.
 *
 * El invariante que da nombre al test es la COSTURA: un trazo a caballo de dos chunks tiene que
 * dejar un campo continuo a través del plano y dos mallas que casen — ninguna pared invisible.
 */
#include "test_common.h"
#include "core/terrain/vox_world.h"
#include "core/terrain/bake_util.h"
#include "tools/world_edit_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace Haruka;

namespace {
constexpr double kR = 6371000.0;

/// Cuántos triángulos de una malla tienen sus tres vértices pegados a un plano (una "pared" en la
/// costura los tiene a cientos; una superficie que cruza el plano, casi ninguno).
int trisEnPlano(const CaveMesh& m, const glm::dvec3& origin, const glm::dvec3& p0, const glm::dvec3& nrm, double tol) {
    int n = 0;
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        bool todos = true;
        for (int k = 0; k < 3 && todos; ++k) {
            const glm::dvec3 w = origin + glm::dvec3(m.positions[m.indices[t + k]]);
            todos = std::fabs(glm::dot(w - p0, nrm)) < tol;
        }
        if (todos) ++n;
    }
    return n;
}
} // namespace

void test_vox_world() {
    beginTest("vox: chunks bajo demanda, cuevas dentro, y SIN costura");

    const std::string tmp = (std::filesystem::temp_directory_path() / "haruka_vox_test").string();
    std::error_code ec; std::filesystem::remove_all(tmp, ec);

    VoxWorld w;
    caveSetAutoProbability(0.14f);   // este test usa una cueva SEMBRADA; el de abajo, una colocada
    w.configure(1234u, kR, tmp + "/bakes", tmp + "/save", nullptr);
    w.setSynchronousBake(true);      // en el juego va en hilo; aquí se mide el resultado, no la espera

    // ── (1) LA CLAVE: estable, y distinta un chunk más allá ────────────────────────────────────
    const glm::dvec3 d0 = glm::normalize(glm::dvec3(0.3, 0.8, 0.5));
    // ⚠️ BAJO TIERRA. El campo es `min(terreno, cueva)`: una esfera picada en el AIRE (por encima
    // de la cota) no deja pared que dibujar, y eso es correcto. El chunk de prueba es el que está
    // justo debajo de la superficie (k = -1: de R-128 a R), y los trazos van a R-87 m.
    const glm::dvec3 p0 = d0 * (kR - 50.0);
    const VoxKey k0 = w.keyAt(p0);
    CHECK(w.keyAt(p0) == k0, "la clave de un punto es estable");
    VoxChunk& c0 = w.ensure(k0);
    const double N = (double)(kVoxN - 1);
    std::printf("    chunk: %.1f x %.1f m (nominal %.0f) · voxel %.2f x %.2f x %.2f m · %zu chunks cargados\n",
                c0.sideU, c0.sideV, kVoxChunkM, c0.metresPerVoxel().x, c0.metresPerVoxel().y,
                c0.metresPerVoxel().z, w.loadedCount());
    CHECK(std::fabs(c0.sideU - kVoxChunkM) < 0.45 * kVoxChunkM, "el lado real esta cerca del nominal");
    // La rejilla y la clave coinciden POR CONSTRUCCION: el nodo (N,·,·) esta en el borde y un pelo
    // mas alla ya es otro chunk; un pelo mas aca, este.
    CHECK(w.keyAt(c0.worldOfGrid(N - 1e-3, N * 0.5, 4.0)) == k0, "el ultimo nodo de la rejilla es de este chunk");
    CHECK(!(w.keyAt(c0.worldOfGrid(N + 1e-3, N * 0.5, 4.0)) == k0), "CONTRAPRUEBA: un pelo mas alla, otro chunk");
    CHECK(k0.k == -1, "el chunk de prueba es el de justo bajo la superficie");
    CHECK(w.keyAt(c0.worldOfGrid(N * 0.5, N * 0.5, N + 1.0)).k == k0.k + 1, "y hacia arriba sube k");
    // Ida y vuelta mundo→rejilla: exacta hasta el ruido del double.
    {
        const glm::dvec3 g = c0.gridOf(c0.worldOfGrid(7.25, 20.5, 3.75));
        std::printf("    ida y vuelta rejilla: (7.25, 20.5, 3.75) -> (%.6f, %.6f, %.6f)\n", g.x, g.y, g.z);
        CHECK(std::fabs(g.x - 7.25) < 1e-6 && std::fabs(g.y - 20.5) < 1e-6 && std::fabs(g.z - 3.75) < 1e-6,
              "mundo->rejilla->mundo es exacto");
    }

    // ── (2) SIN CUEVA: todo roca (y no cuesta nada) ────────────────────────────────────────────
    CHECK(w.density(p0) > 0.0f, "donde no hay cueva, bajo tierra el campo es roca");
    // Y NO OCUPA: un chunk de roca maciza no reserva vóxel (antes: 131 KB de "+24" repetido). El
    // chunk de prueba tiene cueva sembrada (la de abajo), así que se mide en otro sin nada.
    {
        VoxWorld w0; w0.configure(1234u, kR, tmp + "/bakes0", "", nullptr); w0.setSynchronousBake(true);
        caveSetAutoProbability(0.0f);
        for (int i = 0; i < 20; ++i) { VoxKey k = k0; k.i += 3 * i + 50; w0.ensure(k); }
        caveSetAutoProbability(0.14f);
        std::printf("    20 chunks de roca maciza: %zu bytes de voxel · %zu con contenido\n", w0.voxelBytes(), w0.loadedWithContent());
        CHECK(w0.loadedCount() == 20 && w0.voxelBytes() == 0, "la roca maciza no reserva voxel");
        CaveMesh m; w0.buildMesh(k0, m);
        w0.ensure(k0);
        w0.stroke(w0.get(k0)->worldOfGrid(15, 15, 15), 6.0f, true);
        CHECK(w0.voxelBytes() == (size_t)kVoxN * kVoxN * kVoxN * sizeof(float), "CONTRAPRUEBA: al primer trazo se reserva el canal (131 KB)");
    }
    CHECK(w.density(d0 * (kR + 5.0)) < 0.0f, "CONTRAPRUEBA: por encima de la cota es AIRE (el terreno es parte del campo)");

    // ── (3) UNA CUEVA SEMBRADA APARECE EN SUS CHUNKS ───────────────────────────────────────────
    CaveBox box; bool hay = false;
    for (int i = 0; i < 4000 && !hay; ++i) {
        const double a = i * 0.017, b = i * 0.0031;
        const glm::dvec3 d = glm::normalize(glm::dvec3(std::cos(a) * std::cos(b), std::sin(b), std::sin(a) * std::cos(b)));
        hay = caveBoxAt(1234u, d, kR, box);
    }
    CHECK(hay, "hay una cueva sembrada que probar");
    int aireEnCueva = 0, muestras = 0;
    if (hay) {
        for (int j = 0; j < 12; ++j)
            for (int i = 0; i < 12; ++i) {
                const glm::vec3 l(((float)i / 11.0f - 0.5f) * 1.6f * box.radiusM,
                                  ((float)j / 11.0f - 0.5f) * 1.6f * box.radiusM, box.depthM * 0.5f);
                const glm::dvec3 p = box.toWorld(l);
                w.ensure(w.keyAt(p));
                ++muestras; if (w.density(p) < 0.0f) ++aireEnCueva;
            }
        std::printf("    cueva sembrada: %d de %d puntos a media profundidad son aire · %d cuevas en el ultimo chunk\n",
                    aireEnCueva, muestras, w.lastBakeCaves());
        CHECK(aireEnCueva > 0, "la cueva sembrada esta en los chunks");
        CHECK(aireEnCueva < muestras, "CONTRAPRUEBA: y no es todo aire");
    }

    // ── (4) LA COSTURA: un trazo a caballo de dos chunks ───────────────────────────────────────
    // La costura: el plano u = u1 del chunk, que es donde la rejilla termina (nodo N) y donde la
    // clave cambia. La normal es la direccion de crecimiento de u en ese punto.
    const glm::dvec3 seamP = c0.worldOfGrid(N, N * 0.5 + 0.7, 10.0);
    const glm::dvec3 seamN = glm::normalize(c0.worldOfGrid(N + 0.01, N * 0.5 + 0.7, 10.0)
                                          - c0.worldOfGrid(N - 0.01, N * 0.5 + 0.7, 10.0));
    const VoxKey kA = w.keyAt(seamP - seamN * 1.0), kB = w.keyAt(seamP + seamN * 1.0);
    CHECK(!(kA == kB), "el punto elegido esta de verdad entre dos chunks");
    const int cambiados = w.stroke(seamP, 10.0f, true);
    std::printf("    trazo r=10 m en la costura: %d voxeles a aire · chunks cargados %zu\n", cambiados, w.loadedCount());
    CHECK(cambiados > 0, "el trazo abre hueco");
    CHECK(w.get(kA) && w.get(kA)->edited && w.get(kB) && w.get(kB)->edited,
          "y ha escrito en LOS DOS chunks, no solo en el del centro");

    double peorSalto = 0.0; int fueraDeAire = 0; float prev = 0.0f; bool first = true;
    for (double s = -8.0; s <= 8.0; s += 0.25) {
        const float d = w.density(seamP + seamN * s);
        if (d >= 0.0f) ++fueraDeAire;
        if (!first) peorSalto = std::max(peorSalto, (double)std::fabs(d - prev));
        prev = d; first = false;
    }
    std::printf("    campo a traves de la costura (±8 m, paso 0,25): puntos NO aire %d · peor salto %.3f m\n",
                fueraDeAire, peorSalto);
    CHECK(fueraDeAire == 0, "todo el interior del trazo es aire A AMBOS LADOS del plano");
    CHECK(peorSalto < 0.6, "y el campo es continuo al cruzar (ningun salto mayor que un cuarto de voxel)");

    CaveMesh mA, mB;
    w.buildMesh(kA, mA); w.buildMesh(kB, mB);
    const int paredA = trisEnPlano(mA, mA.origin, seamP, seamN, 0.3);
    const int paredB = trisEnPlano(mB, mB.origin, seamP, seamN, 0.3);
    std::printf("    mallas: %zu + %zu triangulos · pegados al plano de costura: %d + %d\n",
                mA.triangleCount(), mB.triangleCount(), paredA, paredB);
    CHECK(mA.triangleCount() > 0 && mB.triangleCount() > 0, "las dos mitades de la esfera tienen malla");
    CHECK(paredA + paredB == 0, "NINGUNA pared en la costura: la esfera cruza el plano entera");
    // Y LAS NORMALES CASAN: un vértice que los dos chunks reconstruyen (la celda del borde, desde
    // su delantal) tiene que tener la misma normal desde los dos lados, o se ve una línea de
    // sombreado en cada costura. Se buscan pares de vértices coincidentes (< 0,3 m) y se mide el
    // peor ángulo entre sus normales.
    {
        double peorDeg = 0.0; int pares = 0;
        for (size_t i = 0; i < mA.positions.size(); ++i) {
            const glm::dvec3 pa = mA.origin + glm::dvec3(mA.positions[i]);
            if (std::fabs(glm::dot(pa - seamP, seamN)) > 6.0) continue;
            for (size_t j = 0; j < mB.positions.size(); ++j) {
                const glm::dvec3 pb = mB.origin + glm::dvec3(mB.positions[j]);
                if (glm::length(pa - pb) > 0.3) continue;
                const double c = glm::clamp((double)glm::dot(mA.normals[i], mB.normals[j]), -1.0, 1.0);
                peorDeg = std::max(peorDeg, glm::degrees(std::acos(c)));
                ++pares;
            }
        }
        std::printf("    vertices compartidos en la costura: %d · peor desviacion de normal %.2f grados\n", pares, peorDeg);
        CHECK(pares > 0, "hay vertices que los dos chunks reconstruyen");
        CHECK(peorDeg < 2.0, "y sus normales coinciden (< 2 grados): sin linea de sombreado en la costura");
    }
    w.unloadFar(c0.centerDir, -1.0);           // descarga todo (guardando los editados)
    w.ensure(kA);
    CaveMesh mSolo; w.buildMesh(kA, mSolo);
    const int paredSolo = trisEnPlano(mSolo, mSolo.origin, seamP, seamN, 2.5);
    std::printf("    CONTRAPRUEBA (vecino sin cargar): %d triangulos en el plano\n", paredSolo);
    CHECK(paredSolo > 0, "CONTRAPRUEBA: sin el vecino cargado si hay pared (el criterio distingue)");

    // ── (5) LOS TRAZOS SOBREVIVEN A DESCARGAR Y RECARGAR ───────────────────────────────────────
    w.ensure(kB);
    const float dReload = w.density(seamP);
    std::printf("    tras descargar y recargar: campo en el centro del trazo %.2f m\n", dReload);
    CHECK(dReload < 0.0f, "el trazo se reaplica al recargar el chunk (estado de la PARTIDA)");
    int ficheros = 0;
    for (auto& e : std::filesystem::recursive_directory_iterator(tmp + "/save", ec)) if (e.is_regular_file()) ++ficheros;
    CHECK(ficheros >= 2, "y hay un fichero de trazos por chunk editado");

    // ── (6) EL RECORTE DEL TERRENO SALE DEL MISMO CAMPO ────────────────────────────────────────
    const glm::dvec3 dSup = glm::normalize(glm::dvec3(0.7, 0.2, -0.6));
    const float cutAntes = w.surfaceCut(dSup);
    w.stroke(dSup * (kR - 4.0), 9.0f, true);
    const float cutDespues = w.surfaceCut(dSup);
    std::printf("    recorte del terreno: %.2f antes de picar · %.2f despues\n", cutAntes, cutDespues);
    CHECK(cutAntes == 0.0f, "sin picar no hay recorte");
    CHECK(cutDespues > 0.5f, "picar bajo la superficie ABRE el terreno (mismo campo, mismo gesto)");
    const int rell = w.stroke(dSup * (kR - 4.0), 9.5f, false);
    CHECK(rell > 0 && w.surfaceCut(dSup) < 0.05f, "y rellenar lo vuelve a cerrar");

    if (!std::getenv("HARUKA_KEEP_TMP")) std::filesystem::remove_all(tmp, ec);
}

void test_vox_placed_cave() {
    beginTest("vox: cueva COLOCADA a mano, y el lecho del agua");

    const std::string tmp = (std::filesystem::temp_directory_path() / "haruka_vox_placed").string();
    std::error_code ec; std::filesystem::remove_all(tmp, ec);
    caveSetAutoProbability(0.0f);    // el defecto del juego: nada automatico

    // Un terreno con cota: 500 m en todas partes, para que la boca tenga que subir hasta ahi.
    VoxWorld w;
    w.configure(99u, kR, tmp + "/bakes", tmp + "/save", [](const glm::dvec3&) { return 500.0f; });
    w.setSynchronousBake(true);

    const glm::dvec3 d = glm::normalize(glm::dvec3(0.2, 0.9, -0.4));
    // (1) Sin colocar: nada. Chunk bajo la superficie todo roca.
    w.ensure(w.keyAt(d * (kR + 500.0 - 30.0)));
    CHECK(w.lastBakeCaves() == 0, "sin colocar, el chunk no tiene ninguna cueva");
    CHECK(w.surfaceCut(d) == 0.0f && w.floorDepthM(d) == 0.0f, "y ni boca ni lecho hundido");

    // (2) Se coloca: la celda de `d` pasa a tener cueva, se guarda, y los chunks se rehornean.
    CHECK(w.placeCave(d), "se coloca un borrador de cueva");
    CHECK(!w.placeCave(d), "CONTRAPRUEBA: la misma celda no se coloca dos veces");
    CHECK(w.caveDefs().size() == 1 && !w.caveDefs()[0].name.empty(), "y es una DEFINICION con nombre (lo que iria a la escena)");
    const auto bocas = w.placedCaveMouths();
    CHECK(bocas.size() == 1, "tiene una boca");
    if (bocas.empty()) return;
    const glm::dvec3 m = bocas[0];
    // Cargar la columna de la boca: los chunks desde 2 por debajo hasta la superficie.
    for (int dk = -2; dk <= 0; ++dk) { VoxKey k = w.keyAt(m * (kR + 500.0)); k.k += dk; w.ensure(k); }
    const float cut = w.surfaceCut(m);
    const float depth = w.floorDepthM(m);
    std::printf("    boca colocada: recorte %.2f · lecho a %.0f m bajo la superficie\n", cut, depth);
    CHECK(cut > 0.5f, "la boca ROMPE el suelo donde el terreno esta (cota 500 m)");
    CHECK(depth > 5.0f, "y el lecho del agua esta en el fondo del pozo, no en el heightfield");
    CHECK(w.floorDepthM(glm::normalize(m + glm::dvec3(0.001, 0.0, 0.0))) == 0.0f,
          "CONTRAPRUEBA: a 6 km de la boca el lecho es el suelo");

    // (3) Otro mundo que reciba la MISMA definicion (como hara la escena) tiene la misma boca.
    VoxWorld w2;
    w2.configure(99u, kR, tmp + "/bakes", tmp + "/save", [](const glm::dvec3&) { return 500.0f; });
    w2.setSynchronousBake(true);
    w2.setCaves(w.caveDefs());
    CHECK(w2.placedCaveMouths().size() == 1 && glm::length(w2.placedCaveMouths()[0] - m) < 1e-12,
          "la misma definicion en otro mundo da la misma boca");

    // (2b) EL FALDÓN: la pared del pozo LLEGA al suelo. La malla del chunk de la superficie tiene
    //      vértices a la cota exacta del terreno (500 m) y ninguno por encima; sin el faldón, el
    //      vértice más alto se quedaba media celda (~2 m) por debajo.
    {
        CaveMesh mm; w.buildMesh(w.keyAt(m * (kR + 500.0 - 1.0)), mm);
        double maxH = -1e9; int enSuelo = 0;
        for (const glm::vec3& v : mm.positions) {
            const double h = glm::length(mm.origin + glm::dvec3(v)) - (kR + 500.0);
            maxH = std::max(maxH, h);
            if (std::fabs(h) < 0.05) ++enSuelo;
        }
        std::printf("    faldon: vertice mas alto a %+.3f m del suelo · %d vertices EN el suelo (de %zu)\n", maxH, enSuelo, mm.positions.size());
        CHECK(!mm.positions.empty(), "el chunk de la superficie tiene pared (el pozo)");
        CHECK(enSuelo > 0, "hay vertices exactamente en la cota del terreno: la pared toca el suelo");
        CHECK(maxH < 0.05, "y ninguno por encima del suelo");
    }

    // (3a) EL BORDE DEL STREAMING NO PARTE LA CUEVA. Se busca un chunk con aire de cueva cuyo
    //      vecino en +u también lo tenga; se malla SOLO el primero (el vecino sin cargar) y se cuentan
    //      los triángulos pegados al plano de la costura: con el delantal leyendo "roca" salía una
    //      pared plana (Andoni: "la cueva grande partida en dos"); con el campo horneado, ninguna.
    {
        VoxWorld w4;
        w4.configure(99u, kR, tmp + "/bakes", tmp + "/save", [](const glm::dvec3&) { return 500.0f; });
        w4.setSynchronousBake(true);
        w4.setCaves(w.caveDefs());
        int enPlano = -1, trisTotal = 0;
        const double N = (double)(kVoxN - 1);
        // Columnas alrededor de la boca, un chunk bajo la superficie.
        for (int i = -3; i <= 3 && enPlano < 0; ++i)
            for (int j = -3; j <= 3 && enPlano < 0; ++j) {
                const glm::dvec3 pr = glm::normalize(m + glm::dvec3(0.0, 0.0, 0.0));
                VoxKey k = w4.keyAt(pr * (kR + 500.0 - 70.0));
                k.i += i; k.j += j;
                VoxChunk& c = w4.ensure(k);
                // ¿Aire en la cara +u de este chunk?
                bool aire = false;
                for (int y = 0; y < kVoxN && !aire; y += 3)
                    for (int z = 0; z < kVoxN && !aire; z += 3) aire = c.cutAt(kVoxN - 1, y, z) < 0.0f;
                if (!aire) continue;
                // Vecino +u: NO se carga. Se malla este, y se cuenta lo que hay pegado al plano.
                const glm::dvec3 p0 = c.worldOfGrid(N, N * 0.5, N * 0.5);
                const glm::dvec3 nrm = glm::normalize(c.worldOfGrid(N + 0.01, N * 0.5, N * 0.5) - c.worldOfGrid(N - 0.01, N * 0.5, N * 0.5));
                CaveMesh solo; w4.buildMesh(k, solo);
                const int enPlanoSolo = trisEnPlano(solo, solo.origin, p0, nrm, 2.5);   // el vertice del borde queda a media celda
                // La VERDAD: con el vecino cargado. Lo que haya en el plano ahora es geometria real
                // (el final de una galeria, por ejemplo), no un artefacto del borde.
                w4.ensure(w4.keyAt(c.worldOfGrid(N + 1.0, N * 0.5, N * 0.5)));
                CaveMesh con; w4.buildMesh(k, con);
                const int enPlanoCon = trisEnPlano(con, con.origin, p0, nrm, 2.5);
                enPlano = enPlanoSolo - enPlanoCon;   // lo que el borde AÑADE
                trisTotal = (int)con.triangleCount();
                std::printf("    borde del streaming: en el plano %d sin vecino · %d con vecino (de %d)\n", enPlanoSolo, enPlanoCon, trisTotal);
                w4.unloadFar(pr, -1.0);
            }
        CHECK(enPlano >= 0 || trisTotal > 0, "hay un chunk con aire de cueva en su cara +u");
        // Con el delantal viejo (HARUKA_VOX_APRONROCK=1) el borde añade decenas: la pared entera.
        CHECK(std::abs(enPlano) * 20 < std::max(trisTotal, 1), "el vecino sin cargar NO añade pared en la costura (< 5 % de diferencia)");
    }

    // (3b) EN HILO, como en el juego: el chunk sale provisional (roca) y al recoger el horneado se
    //      rehace con la cueva. Es lo que quita el tirón de 1,1 s del hilo principal.
    {
        VoxWorld w3;
        w3.configure(99u, kR, tmp + "/bakes_async", tmp + "/save", [](const glm::dvec3&) { return 500.0f; });
        w3.setCaves(w.caveDefs());
        const VoxKey km = w3.keyAt(m * (kR + 500.0 - 30.0));
        w3.ensure(km);
        const bool provisional = w3.get(km) && w3.get(km)->pendingCave;
        std::printf("    en hilo: chunk provisional=%d · pendientes=%d\n", provisional ? 1 : 0, w3.cavesPending() ? 1 : 0);
        CHECK(provisional && w3.cavesPending(), "con la cueva en hilo, el chunk sale PROVISIONAL y hay trabajo pendiente");
        CHECK(w3.surfaceCut(m) == 0.0f, "y mientras tanto es roca (no se recorta nada que luego cambie de sitio)");
        w3.waitCaveJobs();
        for (int dk = -2; dk <= 0; ++dk) { VoxKey k = w3.keyAt(m * (kR + 500.0)); k.k += dk; w3.ensure(k); }
        std::printf("    tras recoger: recorte %.2f · pendientes=%d\n", w3.surfaceCut(m), w3.cavesPending() ? 1 : 0);
        CHECK(!w3.cavesPending() && w3.surfaceCut(m) > 0.5f, "al recoger el horneado el chunk se rehace y la boca aparece");
    }

    // (4) Quitarla la quita.
    CHECK(w.removeCave(d), "se quita");
    for (int dk = -2; dk <= 0; ++dk) { VoxKey k = w.keyAt(m * (kR + 500.0)); k.k += dk; w.ensure(k); }
    CHECK(w.surfaceCut(m) == 0.0f, "y el suelo vuelve a estar entero");
    std::filesystem::remove_all(tmp, ec);
}

void test_vox_draped_cave() {
    beginTest("vox: la cueva sigue al relieve (drapeada), no a un techo plano");

    const std::string tmp = (std::filesystem::temp_directory_path() / "haruka_vox_drape").string();
    std::error_code ec; std::filesystem::remove_all(tmp, ec);
    caveSetAutoProbability(0.0f);

    // Terreno en PENDIENTE: cota 500 m en el centro de la caja y 15 cm menos por metro hacia +e1.
    // A 300 m, el suelo esta 45 m mas bajo que el techo plano de la caja.
    static glm::dvec3 s_c, s_e1;
    const glm::dvec3 d = glm::normalize(glm::dvec3(-0.3, 0.85, 0.4));
    s_c = d;
    s_e1 = glm::normalize(glm::cross(glm::dvec3(0, 1, 0), d));
    auto elev = [](const glm::dvec3& dir) -> float {
        const double along = glm::dot(dir - s_c, s_e1) * kR;
        return 500.0f - 0.15f * (float)along;
    };
    VoxWorld w;
    w.configure(7u, kR, tmp + "/bakes", tmp + "/save", elev);
    w.setSynchronousBake(true);
    CHECK(w.placeCave(d), "cueva colocada sobre la pendiente");
    const auto boxes = w.placedCaveBoxes();
    if (boxes.empty()) return;
    const CaveBox& b = boxes[0];
    const glm::dvec3 mouth = caveEntranceDir(b);

    // Se muestrea la superficie de la caja (fuera de la boca) por el lado BAJO de la pendiente:
    // ¿cuantos puntos tienen la cueva rompiendo el suelo (boquete) y cuantos tienen galeria debajo?
    int boquetes = 0, conGaleria = 0, muestras = 0;
    for (int j = -6; j <= 6; ++j)
        for (int i = 2; i <= 8; ++i) {          // i > 0: lado bajo
            const glm::vec3 l((float)i / 8.0f * b.radiusM * 0.9f, (float)j / 6.0f * b.radiusM * 0.9f, 0.0f);
            const glm::dvec3 dir = glm::normalize(b.toWorld(l));
            if (std::acos(std::clamp(glm::dot(dir, mouth), -1.0, 1.0)) * kR < caveEntranceRadiusM(b) + 12.0) continue;
            const double top = kR + (double)elev(dir);
            for (int dk = -3; dk <= 0; ++dk) { VoxKey k = w.keyAt(dir * top); k.k += dk; w.ensure(k); }
            ++muestras;
            if (w.surfaceCut(dir) > 0.5f) ++boquetes;
            bool galeria = false;
            for (double z = 8.0; z < (double)b.depthM && !galeria; z += 4.0) galeria = w.caveDensity(dir * (top - z)) < 0.0f;
            if (galeria) ++conGaleria;
        }
    std::printf("    lado bajo de la pendiente: %d puntos · boquetes en el suelo %d · con galeria debajo %d\n",
                muestras, boquetes, conGaleria);
    CHECK(muestras > 20, "hay muestras");
    CHECK(boquetes == 0, "NINGUN boquete: la cueva no asoma por encima del suelo donde el terreno baja");
    CHECK(conGaleria > 0, "y las galerias SIGUEN bajo el suelo bajo (la cueva se drapea sobre el relieve)");
    std::filesystem::remove_all(tmp, ec);
}

void test_vox_columns() {
    beginTest("vox: el streaming no se salta columnas (celdas de ~50 m en las esquinas del cubo)");
    VoxWorld w;
    w.configure(1u, kR, "", "", nullptr);
    // Dos sitios: el centro de una cara (celdas de ~128 m) y donde Andoni juega (31°N 131°E: cerca
    // de una esquina del cubo, celdas mucho más pequeñas). En cada uno: las columnas que devuelve
    // `columnsNear` contra la enumeración POR ÍNDICE de la caja (i,j) que las contiene.
    struct Sitio { const char* nombre; double lat, lon; };
    const Sitio sitios[] = { { "centro de cara", 0.0, 0.0 }, { "31N 131E", 31.19765, 131.62482 }, { "esquina", 35.0, 45.0 } };
    for (const Sitio& st : sitios) {
        const double la = glm::radians(st.lat), lo = glm::radians(st.lon);
        const glm::dvec3 d(std::cos(la) * std::cos(lo), std::sin(la), std::cos(la) * std::sin(lo));
        const double radio = 500.0;
        const std::vector<VoxKey> cols = w.columnsNear(d, radio);
        int imin = 1 << 30, imax = -1, jmin = 1 << 30, jmax = -1;
        for (const VoxKey& k : cols) { imin = std::min(imin, k.i); imax = std::max(imax, k.i); jmin = std::min(jmin, k.j); jmax = std::max(jmax, k.j); }
        // Por índice: toda celda de la caja cuyo CENTRO cae a menos de `radio − diagonal/2` tiene
        // que estar (su centro está dentro seguro); ninguna a más de `radio + diagonal` puede estar.
        const double lado = w.chunkSideAt(d);
        int deben = 0, faltan = 0, sobran = 0;
        const int face = cols.empty() ? 0 : cols[0].face;
        for (int j = jmin - 1; j <= jmax + 1; ++j)
            for (int i = imin - 1; i <= imax + 1; ++i) {
                if (i < 0 || j < 0 || i >= w.chunksPerFace() || j >= w.chunksPerFace()) continue;   // mas alla de la arista: otra cara
                VoxKey k; k.face = face; k.i = i; k.j = j; k.k = 0;
                const double dist = std::acos(std::clamp(glm::dot(w.chunkCenterDir(k), d), -1.0, 1.0)) * kR;
                const bool esta = std::find(cols.begin(), cols.end(), k) != cols.end();
                if (dist < radio - lado) { ++deben; if (!esta) ++faltan; }
                if (dist > radio + 1.5 * lado && esta) ++sobran;
            }
        std::printf("    %-14s celda %.0f m · %zu columnas en %.0f m · por indice deben estar %d: faltan %d · sobran %d\n",
                    st.nombre, lado, cols.size(), radio, deben, faltan, sobran);
        CHECK(deben > 0 && faltan == 0, "ninguna columna del disco se salta");
        CHECK(sobran == 0, "y ninguna de fuera se cuela");
        // CONTRAPRUEBA: el barrido viejo, a 128·0,8 m, en el sitio de celdas pequeñas SÍ se salta.
        if (lado < 90.0) {
            const glm::dvec3 ref = (std::fabs(d.y) < 0.9) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
            const glm::dvec3 e1 = glm::normalize(glm::cross(ref, d)), e2 = glm::cross(d, e1);
            std::vector<VoxKey> viejo;
            const double step = kVoxChunkM * 0.8; const int n = (int)std::ceil(radio / step);
            for (int j = -n; j <= n; ++j) for (int i = -n; i <= n; ++i) {
                if ((double)(i * i + j * j) * step * step > radio * radio) continue;
                VoxKey k = w.keyAt(glm::normalize(d + (e1 * (i * step) + e2 * (j * step)) / kR) * kR); k.k = 0;
                if (std::find(viejo.begin(), viejo.end(), k) == viejo.end()) viejo.push_back(k);
            }
            std::printf("    CONTRAPRUEBA barrido a %.0f m: %zu columnas (de %zu)\n", step, viejo.size(), cols.size());
            CHECK(viejo.size() < cols.size(), "el barrido a pasos de 100 m se saltaba columnas aqui");
        }
    }
}

// ---------------------------------------------- TEST: una cueva DE LA ESCENA, sin semilla
// Lo que Andoni pidió: "son parte del json de la escena y debería funcionar sin semillas". Una
// `CaveDef` con sus mapas PINTADOS (un corredor recto en planta, una banda de profundidad en
// perfil) tiene que dar aire exactamente por ese corredor, y el MISMO campo con dos semillas de
// mundo distintas. Contraprueba: un borrador (sin mapas) SÍ cambia con la semilla.
void test_vox_scene_cave() {
    beginTest("vox: cueva de la ESCENA con mapas pintados, sin semilla");
    const std::string tmp = (std::filesystem::temp_directory_path() / "haruka_vox_scene").string();
    std::error_code ec; std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp + "/assets/vox", ec);
    caveSetAutoProbability(0.0f);

    // Los mapas pintados: planta = corredor de 12 texels de ancho a lo largo de u (v = 0,5);
    // perfil = hueco sólo entre el 40 % y el 60 % de la profundidad.
    const int res = 256;
    std::vector<float> planta((size_t)res * res, 0.0f), perfil((size_t)res * (res / 2), 0.0f);
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x)
            if (std::abs(y - res / 2) <= 6 && x >= res / 5 && x <= res * 4 / 5) planta[(size_t)y * res + x] = 1.0f;
    for (int y = 0; y < res / 2; ++y)
        for (int x = 0; x < res; ++x)
            if (y >= (res / 2) * 2 / 5 && y <= (res / 2) * 3 / 5) perfil[(size_t)y * res + x] = 1.0f;
    CHECK(BakeUtil::savePNGGray(tmp + "/assets/vox/mina_planta.png", res, res, planta), "planta pintada a PNG");
    CHECK(BakeUtil::savePNGGray(tmp + "/assets/vox/mina_perfil.png", res, res / 2, perfil), "perfil pintado a PNG");

    CaveDef def;
    def.name = "mina"; def.centerDir = glm::normalize(glm::dvec3(0.1, 0.9, 0.3));
    def.radiusM = 300.0f; def.depthM = 200.0f; def.mouth = glm::vec3(0.5f, 0.5f, 0.05f);
    def.planta = tmp + "/assets/vox/mina_planta.png"; def.perfil = tmp + "/assets/vox/mina_perfil.png";

    auto campo = [&](uint32_t seed, std::vector<float>& out, CaveBox& box) {
        VoxWorld w;
        w.configure(seed, kR, tmp + "/bakes", "", [](const glm::dvec3&) { return 500.0f; });
        w.setSynchronousBake(true);
        CHECK(w.placeCave(def), "la definicion se coloca");
        box = w.placedCaveBoxes()[0];
        // Chunks de toda la caja, y el campo en una rejilla de 20 m de la caja.
        for (double x = -box.radiusM; x <= box.radiusM; x += 60.0)
            for (double y = -box.radiusM; y <= box.radiusM; y += 60.0)
                for (double z = 0.0; z <= box.depthM + 10.0; z += 60.0)
                    w.ensure(w.keyAt(box.toWorld(glm::vec3((float)x, (float)y, (float)z))));
        out.clear();
        for (double x = -box.radiusM; x <= box.radiusM; x += 20.0)
            for (double y = -box.radiusM; y <= box.radiusM; y += 20.0)
                for (double z = 5.0; z <= box.depthM; z += 20.0)
                    out.push_back(w.density(box.toWorld(glm::vec3((float)x, (float)y, (float)z))));
    };
    std::vector<float> f0, f1; CaveBox b0, b1;
    campo(0u, f0, b0);
    campo(0u, f0, b0);   // dos veces: la primera hornea el volumen a PNG (8 bits), la segunda lo lee, como hara la de seed 1234
    campo(1234u, f1, b1);
    CHECK(f0.size() == f1.size() && f0 == f1, "seed 0 y seed 1234 dan el MISMO campo (la cueva no depende de la semilla)");

    // Aire exactamente por el corredor: a media profundidad, sobre la linea v = 0,5, aire; a 40 m
    // de lado, roca; y en el corredor pero al 15 % de profundidad (fuera de la banda), roca.
    int aireCorredor = 0, rocaLado = 0, rocaArriba = 0, n = 0;
    for (double x = -0.5 * b0.radiusM; x <= 0.5 * b0.radiusM; x += 25.0) {
        if (std::fabs(x) < 45.0) continue;   // el pozo de la boca (u = 0,5, radio 30 m) tambien es aire por encima de la banda
        ++n;
        VoxWorld w; w.configure(0u, kR, tmp + "/bakes", "", [](const glm::dvec3&) { return 500.0f; });
        w.setSynchronousBake(true); w.placeCave(def);
        const glm::dvec3 pc = b0.toWorld(glm::vec3((float)x, 0.0f, 0.5f * b0.depthM));
        const glm::dvec3 pl = b0.toWorld(glm::vec3((float)x, 40.0f, 0.5f * b0.depthM));
        const glm::dvec3 pa = b0.toWorld(glm::vec3((float)x, 0.0f, 0.15f * b0.depthM));
        for (const glm::dvec3& p : { pc, pl, pa }) w.ensure(w.keyAt(p));
        if (w.density(pc) < 0.0f) ++aireCorredor;
        if (w.density(pl) > 0.0f) ++rocaLado;
        if (w.density(pa) > 0.0f) ++rocaArriba;
    }
    std::printf("    corredor pintado: %d/%d puntos de aire en el corredor · %d/%d roca a 40 m de lado · %d/%d roca por encima de la banda\n",
                aireCorredor, n, rocaLado, n, rocaArriba, n);
    CHECK(aireCorredor == n, "el corredor pintado es aire");
    CHECK(rocaLado == n && rocaArriba == n, "y fuera del corredor (de lado y por encima) es roca");

    // CONTRAPRUEBA: un BORRADOR (sin mapas) si depende de la semilla.
    CaveDef borrador = def; borrador.name = "borrador"; borrador.planta.clear(); borrador.perfil.clear();
    std::vector<float> g0, g1; CaveBox c0, c1;
    {
        auto campoB = [&](uint32_t seed, std::vector<float>& out, CaveBox& box) {
            VoxWorld w; w.configure(seed, kR, tmp + "/bakes", "", [](const glm::dvec3&) { return 500.0f; });
            w.setSynchronousBake(true);
            CaveDef b2 = borrador; b2.draftSeed = seed;
            w.placeCave(b2); box = w.placedCaveBoxes()[0];
            for (double x = -box.radiusM; x <= box.radiusM; x += 60.0)
                for (double y = -box.radiusM; y <= box.radiusM; y += 60.0)
                    for (double z = 0.0; z <= box.depthM + 10.0; z += 60.0)
                        w.ensure(w.keyAt(box.toWorld(glm::vec3((float)x, (float)y, (float)z))));
            out.clear();
            for (double x = -box.radiusM; x <= box.radiusM; x += 20.0)
                for (double y = -box.radiusM; y <= box.radiusM; y += 20.0)
                    for (double z = 5.0; z <= box.depthM; z += 20.0)
                        out.push_back(w.density(box.toWorld(glm::vec3((float)x, (float)y, (float)z))));
        };
        campoB(0u, g0, c0); campoB(1234u, g1, c1);
    }
    CHECK(g0 != g1, "CONTRAPRUEBA: el borrador sin mapas SI cambia con la semilla");
    std::filesystem::remove_all(tmp, ec);
}

// ---------------------------------------------- TEST: trazos del MUNDO (editor) y de la PARTIDA
// Dos listas por chunk: las del diseñador (carpeta `voxEdits` del planeta en la escena) y las del
// jugador (partida), aplicadas en ese orden. El juego nunca escribe en la del mundo.
void test_vox_designer_strokes() {
    beginTest("vox: trazos del mundo (editor) + trazos de la partida, en ese orden");
    const std::string tmp = (std::filesystem::temp_directory_path() / "haruka_vox_designer").string();
    std::error_code ec; std::filesystem::remove_all(tmp, ec);
    caveSetAutoProbability(0.0f);
    const glm::dvec3 d = glm::normalize(glm::dvec3(0.5, 0.7, -0.2));
    const glm::dvec3 pMundo = d * (kR + 500.0 - 40.0), pPartida = d * (kR + 500.0 - 70.0);

    // (1) El EDITOR: en modo diseñador, pica y guarda -> va a la carpeta del mundo.
    {
        VoxWorld ed; ed.configure(3u, kR, tmp + "/bakes", "", [](const glm::dvec3&) { return 500.0f; });
        ed.setSynchronousBake(true);
        ed.setDesignerDir(tmp + "/assets/vox/earth");
        ed.setDesignerMode(true);
        CHECK(ed.stroke(pMundo, 8.0f, true) > 0, "el editor pica");
        CHECK(ed.saveEdits(), "y guarda");
        CHECK(std::filesystem::exists(tmp + "/assets/vox/earth/vox") , "en la carpeta del MUNDO");
    }
    // (2) El JUEGO: carga el mundo + su partida; ve el trazo del editor; pica lo suyo y guarda.
    {
        VoxWorld g; g.configure(3u, kR, tmp + "/bakes", tmp + "/save", [](const glm::dvec3&) { return 500.0f; });
        g.setSynchronousBake(true);
        g.setDesignerDir(tmp + "/assets/vox/earth");
        g.ensure(g.keyAt(pMundo)); g.ensure(g.keyAt(pPartida));
        CHECK(g.density(pMundo) < 0.0f, "el juego ve el trazo del editor");
        CHECK(g.stroke(pPartida, 8.0f, true) > 0, "el jugador pica");
        CHECK(g.saveEdits(), "y guarda");
        CHECK(std::filesystem::exists(tmp + "/save/vox"), "en la PARTIDA");
    }
    // (3) El fichero del mundo no ha cambiado (el juego no escribe ahí); otra partida limpia ve el
    //     trazo del mundo pero NO el del jugador anterior.
    {
        size_t enMundo = 0;
        for (auto& e : std::filesystem::directory_iterator(tmp + "/assets/vox/earth/vox")) { (void)e; ++enMundo; }
        VoxWorld g2; g2.configure(3u, kR, tmp + "/bakes", tmp + "/save2", [](const glm::dvec3&) { return 500.0f; });
        g2.setSynchronousBake(true);
        g2.setDesignerDir(tmp + "/assets/vox/earth");
        g2.ensure(g2.keyAt(pMundo)); g2.ensure(g2.keyAt(pPartida));
        std::printf("    ficheros de trazos del mundo: %zu · campo en el trazo del mundo %+.2f · en el del jugador %+.2f\n",
                    enMundo, g2.density(pMundo), g2.density(pPartida));
        CHECK(enMundo == 1, "el juego NO escribio en la carpeta del mundo");
        CHECK(g2.density(pMundo) < 0.0f && g2.density(pPartida) > 0.0f,
              "otra partida ve lo del mundo y no lo del otro jugador (CONTRAPRUEBA)");
    }
    std::filesystem::remove_all(tmp, ec);
}

// ---------------------------------------------- TEST: pincel — formas, deshacer bit a bit, rayo
void test_vox_brush() {
    beginTest("vox: pincel con formas, deshacer/rehacer bit a bit, rayo contra el campo");
    const std::string tmp = (std::filesystem::temp_directory_path() / "haruka_vox_brush").string();
    std::error_code ec; std::filesystem::remove_all(tmp, ec);
    caveSetAutoProbability(0.0f);
    VoxWorld w; w.configure(11u, kR, tmp + "/bakes", tmp + "/save", [](const glm::dvec3&) { return 500.0f; });
    w.setSynchronousBake(true);
    const glm::dvec3 d = glm::normalize(glm::dvec3(0.3, 0.6, 0.7));
    const glm::dvec3 up = d;
    const glm::dvec3 ref = (std::fabs(up.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 e1 = glm::normalize(glm::cross(ref, up)), e2 = glm::cross(up, e1);
    const glm::dvec3 c = d * (kR + 500.0 - 60.0);   // bajo tierra

    // ── (1) FORMAS: el mismo centro y radio con esfera, cilindro y caja dan huecos distintos ──
    auto campo = [&](const glm::dvec3& p) { w.ensure(w.keyAt(p)); return w.density(p); };
    // Radios grandes frente al vóxel de 4 m: un margen de 1 m no lo resuelve la trilineal (medido:
    // la esquina de una caja de 6 a 0,5 m del borde salía +0,39). Aquí los márgenes son de 2,5-3 m.
    VoxWorld::Brush cyl{ VoxWorld::Brush::Cylinder, 14.0f, 20.0f };
    w.stroke(c, cyl, true);
    const float cAxis = campo(c + up * 17.0), cLat = campo(c + e1 * 16.5), cCorner = campo(c + e1 * 11.0 + e2 * 11.0);
    std::printf("    cilindro r=14 h=+-20: en el eje a +17 m %+.2f · a 16,5 m de lado %+.2f · esquina (11;11) %+.2f\n", cAxis, cLat, cCorner);
    CHECK(cAxis < 0.0f, "el cilindro abre a lo largo de su eje (17 m: fuera de una esfera de 14)");
    CHECK(cLat > 0.0f && cCorner > 0.0f, "y no de lado (la esquina a 15,6 m del eje es roca)");
    CHECK(w.undo(), "deshacer");
    VoxWorld::Brush box{ VoxWorld::Brush::Box, 14.0f, 20.0f };
    w.stroke(c, box, true);
    const float bCorner = campo(c + e1 * 11.0 + e2 * 11.0);
    std::printf("    caja 14x14x+-20: esquina (11;11) %+.2f\n", bCorner);
    CHECK(bCorner < 0.0f, "CONTRAPRUEBA: la caja SI abre la esquina que el cilindro deja en roca");
    CHECK(w.undo(), "deshacer");
    w.stroke(c, 14.0f, true);
    CHECK(campo(c + up * 17.0) > 0.0f, "CONTRAPRUEBA: la esfera de 14 no llega a +17 m");
    CHECK(w.undo(), "deshacer");

    // ── (2) DESHACER BIT A BIT: el campo de la caja de chunks vuelve exactamente ────────────
    auto muestra = [&](std::vector<float>& out) {
        out.clear();
        for (double x = -40; x <= 40; x += 4) for (double y = -40; y <= 40; y += 4) for (double h = -40; h <= 40; h += 4)
            out.push_back(w.density(c + e1 * x + e2 * y + up * h));
    };
    // Una cueva de verdad debajo y un trazo previo cargado de FICHERO (no deshacible): el deshacer
    // tiene que respetar lo que había.
    std::vector<float> antes, despues, tras;
    for (double h = -60; h <= 60; h += 30) for (double x = -60; x <= 60; x += 30) w.ensure(w.keyAt(c + e1 * x + up * h));
    w.stroke(c + e1 * 20.0, 5.0f, true);
    w.saveEdits();
    muestra(antes);
    const size_t nAntes = w.undoCount();
    w.stroke(c, VoxWorld::Brush{ VoxWorld::Brush::Cylinder, 7.0f, 30.0f }, true);
    w.stroke(c + up * 25.0, 9.0f, false);
    muestra(despues);
    CHECK(despues != antes, "los dos trazos cambian el campo");
    CHECK(w.undo() && w.undo(), "se deshacen los dos");
    muestra(tras);
    int distintos = 0; for (size_t i = 0; i < antes.size(); ++i) if (antes[i] != tras[i]) ++distintos;
    std::printf("    deshacer: %zu muestras · %d distintas de antes (bit a bit)\n", antes.size(), distintos);
    CHECK(distintos == 0, "tras deshacer, el campo es BIT A BIT el de antes");
    CHECK(w.undoCount() == nAntes && w.redoCount() == 2, "y quedan dos para rehacer");
    CHECK(w.redo() && w.redo(), "rehacer los dos");
    muestra(tras);
    CHECK(tras == despues, "rehacer devuelve exactamente el campo de despues");
    CHECK(!w.redo(), "y ya no hay mas que rehacer");
    // Deshacer sobrevive a descargar/recargar (el chunk vuelve de fichero + sesion).
    w.undo(); w.undo();
    w.unloadFar(d, -1.0);
    for (double h = -60; h <= 60; h += 30) for (double x = -60; x <= 60; x += 30) w.ensure(w.keyAt(c + e1 * x + up * h));
    muestra(tras);
    distintos = 0; for (size_t i = 0; i < antes.size(); ++i) if (antes[i] != tras[i]) ++distintos;
    CHECK(distintos == 0, "descargar y recargar tras deshacer: sigue siendo el campo de antes (el fichero se reescribio)");
    CHECK(w.density(c + e1 * 20.0) < 0.0f, "y el trazo previo (de fichero) sigue ahi");

    // ── (3) RAYO contra el campo ──────────────────────────────────────────────────────────────
    const glm::dvec3 o = d * (kR + 500.0 + 30.0);
    glm::dvec3 hit;
    CHECK(w.raycast(o, -d, 100.0, hit), "un rayo hacia abajo pega");
    std::printf("    rayo al suelo: pega a %.3f m de la cota del terreno\n", glm::length(hit) - (kR + 500.0));
    CHECK(std::fabs(glm::length(hit) - (kR + 500.0)) < 0.05, "en la superficie del terreno (a 5 cm)");
    w.stroke(d * (kR + 500.0), 10.0f, true);   // un foso de 10 m en la superficie
    CHECK(w.raycast(o, -d, 100.0, hit), "con foso, pega");
    const double prof = (kR + 500.0) - glm::length(hit);
    std::printf("    rayo al foso: pega %.2f m bajo la cota (foso de 10 m)\n", prof);
    CHECK(prof > 7.0 && prof < 11.0, "en el fondo del foso, no en el heightfield");
    CHECK(!w.raycast(o, -d, 20.0, hit), "CONTRAPRUEBA: con alcance 20 m no llega al fondo (a 40 m)");
    CHECK(!w.raycast(d * (kR + 400.0), -d, 50.0, hit), "CONTRAPRUEBA: desde dentro de la roca no hay impacto");
    std::filesystem::remove_all(tmp, ec);
}

// ---------------------------------------------- TEST: el panel del pincel, sin ratón ni ImGui
// Lo que el editor hace al pintar un mapa y pulsar "Rehornear", llamado por programa: pintar un
// corredor en la planta de una cueva de la escena, guardar sus PNG y volver a colocarla. El campo
// tiene que abrir aire por el corredor pintado (y el PNG en disco es el que se pintó).
void test_world_edit_panel() {
    beginTest("pincel del mundo (panel): pintar la planta de una cueva y rehornear");
    const std::string tmp = (std::filesystem::temp_directory_path() / "haruka_world_panel").string();
    std::error_code ec; std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp + "/assets/vox", ec);
    caveSetAutoProbability(0.0f);
    VoxWorld w; w.configure(0u, kR, tmp + "/bakes", "", [](const glm::dvec3&) { return 500.0f; });
    w.setSynchronousBake(true);
    CaveDef def;
    def.name = "mina"; def.centerDir = glm::normalize(glm::dvec3(0.2, 0.9, -0.3));
    def.radiusM = 300.0f; def.depthM = 200.0f; def.mouth = glm::vec3(0.5f, 0.5f, 0.05f);
    def.planta = tmp + "/assets/vox/mina_planta.png"; def.perfil = tmp + "/assets/vox/mina_perfil.png";
    // Los PNG existen ya, TODO ROCA (planta y perfil a 0): sin pintar, la cueva es sólo su pozo.
    {
        std::vector<float> z((size_t)256 * 256, 0.0f), zp((size_t)256 * 128, 0.0f);
        BakeUtil::savePNGGray(def.planta, 256, 256, z);
        BakeUtil::savePNGGray(def.perfil, 256, 128, zp);
    }
    CHECK(w.placeCave(def), "cueva de la escena colocada");
    const CaveBox B = w.placedCaveBoxes()[0];
    // Un punto a media profundidad, a 90 m del centro a lo largo de +x: roca antes de pintar.
    const glm::dvec3 pc = B.toWorld(glm::vec3(90.0f, 0.0f, 0.5f * B.depthM));
    w.ensure(w.keyAt(pc));
    const float antes = w.density(pc);
    CHECK(antes > 0.0f, "antes de pintar, roca");

    WorldEditPanel panel;
    panel.setWorld(&w, glm::dvec3(0.0), kR);
    panel.setAssetDir(tmp + "/assets/vox/");
    panel.selectCave(0);
    panel.setPaint(1.0f, 6.0f, 1.0f);
    // Corredor en la planta: a lo largo de u por v = 0,5, del 20 % al 80 %.
    for (int k = 0; k <= 60; ++k) panel.paintMap(0, 0.2f + 0.6f * (float)k / 60.0f, 0.5f);
    // Perfil: la banda del 40-60 % de profundidad, en todo u.
    for (int k = 0; k <= 80; ++k) for (int j = 0; j <= 4; ++j) panel.paintMap(1, (float)k / 80.0f, 0.4f + 0.05f * (float)j);
    CHECK(panel.rebakeSelected(), "rehornear guarda los PNG y vuelve a colocar la cueva");
    const glm::ivec3 st = panel.lastRebakeStats();
    std::printf("    rehorneado: %d componentes -> %d · %d voxeles de aire\n", st.x, st.y, st.z);
    CHECK(st.z > 0 && st.y == 1, "el volumen nuevo tiene aire y es UNA pieza");
    // El PNG en disco es lo pintado: el texel del centro del corredor vale 1, uno lejos vale 0.
    {
        int pw = 0, ph = 0; std::vector<float> px;
        CHECK(BakeUtil::loadPNGGray(def.planta, pw, ph, px) && pw == 256, "la planta se guardo en su ruta");
        CHECK(px[(size_t)128 * 256 + 128] > 0.9f && px[(size_t)30 * 256 + 128] < 0.1f, "y contiene el corredor pintado (1 dentro, 0 fuera)");
    }
    w.ensure(w.keyAt(pc));
    const float despues = w.density(pc);
    std::printf("    campo en el corredor: %+.2f antes -> %+.2f despues\n", antes, despues);
    CHECK(despues < 0.0f, "tras rehornear, el corredor pintado es aire en el mundo");
    CHECK(w.density(B.toWorld(glm::vec3(90.0f, 60.0f, 0.5f * B.depthM))) > 0.0f, "CONTRAPRUEBA: a 60 m del corredor sigue siendo roca");
    std::filesystem::remove_all(tmp, ec);
}

// ---------------------------------------------- TEST: rayo rasante sobre una cresta
// El pincel del editor lanza el rayo desde donde esté la cámara: a menudo casi paralelo al suelo.
// Un paso fijo de 2 m se saltaba una cresta con menos de 2 m de roca a lo largo del rayo y pegaba
// en la ladera de detrás ("no prioriza la parte más cercana"). El paso adaptativo baja a 25 cm
// junto a la superficie. Contraprueba: el marchador de paso fijo, reproducido aquí, la pierde.
void test_vox_raycast_grazing() {
    beginTest("vox: rayo rasante pega en la cresta mas cercana, no en la ladera de detras");
    static glm::dvec3 s_c, s_e1;
    const glm::dvec3 d = glm::normalize(glm::dvec3(0.4, 0.8, 0.2));
    s_c = d; s_e1 = glm::normalize(glm::cross(glm::dvec3(0, 1, 0), d));
    // Una loma de 30 m de alto y ~40 m de ancho a lo largo de e1, sobre un llano a 500 m.
    VoxWorld w; w.configure(1u, kR, "", "", [](const glm::dvec3& dir) -> float {
        const double along = glm::dot(dir - s_c, s_e1) * kR;
        return 500.0f + 30.0f * (float)std::exp(-(along * along) / (40.0 * 40.0));
    });
    const glm::dvec3 up = d, e1 = s_e1;
    // Rayo horizontal (tangente en el centro) a 1 cm por debajo de la cresta: roca durante ~1,5 m.
    const glm::dvec3 o = up * (kR + 529.99) - e1 * 300.9;   // 300,9: que las muestras de 2 m NO caigan justo en el eje
    glm::dvec3 hit;
    const bool ok = w.raycast(o, e1, 1000.0, hit);
    const double along = ok ? glm::dot(hit - up * kR, e1) : 1e9;
    std::printf("    cresta: impacto %s en along = %+.2f m (la cresta esta en 0, la ladera de detras en +)\n", ok ? "si" : "no", along);
    CHECK(ok, "el rayo rasante pega");
    CHECK(std::fabs(along) < 1.5, "y pega en la CRESTA (a menos de 1,5 m del eje), no detras");
    // CONTRAPRUEBA: paso fijo de 2 m.
    double tHit = -1.0;
    for (double t = 2.0; t <= 1000.0; t += 2.0) if (w.density(o + e1 * t) >= 0.0f) { tHit = t; break; }
    const double alongFijo = tHit > 0 ? tHit - 300.9 : 1e9;
    std::printf("    CONTRAPRUEBA paso fijo 2 m: %s (along %+.2f)\n", tHit > 0 ? "pega" : "no pega", alongFijo);
    CHECK(tHit < 0 || std::fabs(alongFijo) > 1.5, "el paso fijo se salta la cresta o pega fuera de ella");
}

// ---------------------------------------------- TEST: los TIPOS de pincel
// Allanar, subir, bajar, túnel (cápsula) y suavizar, cada uno con su contraprueba y con la costura
// medida después (el suavizado no toca los nodos del borde del chunk para que dos vecinos sigan
// diciendo lo mismo en su cara común).
void test_vox_brush_ops() {
    beginTest("vox: tipos de pincel (allanar, subir, bajar, tunel, suavizar)");
    const std::string tmp = (std::filesystem::temp_directory_path() / "haruka_vox_ops").string();
    std::error_code ec; std::filesystem::remove_all(tmp, ec);
    caveSetAutoProbability(0.0f);
    static glm::dvec3 s_c, s_e1;
    const glm::dvec3 d = glm::normalize(glm::dvec3(0.6, 0.5, 0.6));
    s_c = d; s_e1 = glm::normalize(glm::cross(glm::dvec3(0, 1, 0), d));
    // Ladera: 30 cm por metro a lo largo de e1.
    VoxWorld w; w.configure(2u, kR, tmp + "/bakes", "", [](const glm::dvec3& dir) -> float {
        return 500.0f + 0.3f * (float)(glm::dot(dir - s_c, s_e1) * kR);
    });
    w.setSynchronousBake(true);
    const glm::dvec3 up = d, e1 = s_e1, e2 = glm::cross(up, e1);
    auto at = [&](double along, double across, double h) { return up * (kR + h) + e1 * along + e2 * across; };
    auto dens = [&](const glm::dvec3& p) { w.ensure(w.keyAt(p)); return w.density(p); };

    // (1) ALLANAR en el centro (cota 500) con radio 20 y borde duro.
    {
        VoxWorld::Brush b; b.shape = VoxWorld::Brush::Cylinder; b.radiusM = 20.0f; b.halfM = 20.0f; b.op = VoxWorld::Brush::Flatten; b.hardness = 1.0f;
        const float antesAlto = dens(at(15, 0, 502.0)), antesBajo = dens(at(-15, 0, 498.0));
        w.stroke(at(0, 0, 500.0), b);
        const float alto = dens(at(15, 0, 502.0)), bajo = dens(at(-15, 0, 498.0)), fuera = dens(at(35, 0, 508.0));
        std::printf("    allanar: ladera alta (15 m, cota 502): %+.2f -> %+.2f · ladera baja (-15 m, 498): %+.2f -> %+.2f · fuera (35 m, 508) %+.2f\n",
                    antesAlto, alto, antesBajo, bajo, fuera);
        CHECK(antesAlto > 0.0f && alto < 0.0f, "por encima del plano la ladera alta pasa a AIRE");
        CHECK(antesBajo < 0.0f && bajo > 0.0f, "por debajo del plano la ladera baja pasa a ROCA");
        CHECK(fuera > 0.0f, "CONTRAPRUEBA: fuera de la huella la ladera sigue (roca bajo su cota)");
        w.undo();
    }
    // (2) SUBIR 8 m y BAJAR 6 m, en el llano (along = 0 → cota 500).
    {
        VoxWorld::Brush b; b.shape = VoxWorld::Brush::Cylinder; b.radiusM = 20.0f; b.halfM = 20.0f; b.op = VoxWorld::Brush::Raise; b.amountM = 8.0f; b.hardness = 1.0f;
        w.stroke(at(0, 0, 500.0), b);
        const float dentro = dens(at(0, 5, 504.0)), encima = dens(at(0, 5, 512.0)), lado = dens(at(0, 30, 504.0));
        std::printf("    subir 8 m: a +4 m %+.2f · a +12 m %+.2f · fuera de la huella a +4 m %+.2f\n", dentro, encima, lado);
        CHECK(dentro > 0.0f && encima < 0.0f, "subir pone roca hasta +8 m y no mas");
        CHECK(lado < 0.0f, "CONTRAPRUEBA: fuera de la huella sigue el aire");
        w.undo();
        b.op = VoxWorld::Brush::Lower; b.amountM = 6.0f;
        w.stroke(at(0, 0, 500.0), b);
        const float hueco = dens(at(0, 5, 497.0)), suelo = dens(at(0, 5, 491.0)), ladoB = dens(at(0, 30, 497.0));
        std::printf("    bajar 6 m: a -3 m %+.2f · a -9 m %+.2f · fuera de la huella a -3 m %+.2f\n", hueco, suelo, ladoB);
        CHECK(hueco < 0.0f && suelo > 0.0f, "bajar abre hasta -6 m y no mas");
        CHECK(ladoB > 0.0f, "CONTRAPRUEBA: fuera de la huella sigue la roca");
        w.undo();
    }
    // (3) TÚNEL: cápsula entre dos puntos a 30 m bajo el suelo, 60 m de largo, radio 5.
    {
        const glm::dvec3 A = at(-30, 0, 470.0), B = at(30, 0, 470.0);
        VoxWorld::Brush b; b.shape = VoxWorld::Brush::Capsule; b.radiusM = 5.0f; b.op = VoxWorld::Brush::Remove; b.capsuleEnd = B;
        w.stroke(A, b);
        const float medio = dens(at(0, 0, 470.0)), lado = dens(at(0, 12, 470.0)), extremo = dens(at(28, 0, 470.0));
        std::printf("    tunel A->B: en medio %+.2f · a 12 m de lado %+.2f · junto a B %+.2f\n", medio, lado, extremo);
        CHECK(medio < 0.0f && extremo < 0.0f, "la capsula abre todo el segmento");
        CHECK(lado > 0.0f, "y no de lado");
        w.undo();
        w.stroke(A, 5.0f, true);   // CONTRAPRUEBA: una esfera en A no llega al medio
        CHECK(dens(at(0, 0, 470.0)) > 0.0f, "CONTRAPRUEBA: una esfera en A deja el medio en roca");
        w.undo();
    }
    // (4) SUAVIZAR: un agujero de caja (aristas duras) a caballo de dos chunks; la rugosidad (suma de
    //     |laplaciano| del canal) baja, y la costura entre los dos chunks sigue exacta.
    {
        // Centro justo en una cara de chunk: el nodo N de un chunk en +u.
        w.ensure(w.keyAt(at(0, 0, 470.0)));
        const VoxChunk* c0 = w.get(w.keyAt(at(0, 0, 470.0)));
        const glm::dvec3 seam = c0->worldOfGrid((double)(kVoxN - 1), 15.0, 15.0);
        VoxWorld::Brush box; box.shape = VoxWorld::Brush::Box; box.radiusM = 12.0f; box.halfM = 12.0f; box.op = VoxWorld::Brush::Remove;
        w.stroke(seam, box);
        auto rugosidad = [&]() {
            double sum = 0.0; int n = 0;
            const glm::dvec3 su = glm::normalize(seam);
            const glm::dvec3 rf = (std::fabs(su.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
            const glm::dvec3 t1 = glm::normalize(glm::cross(rf, su)), t2 = glm::cross(su, t1);
            for (double x = -24; x <= 24; x += 4) for (double y = -24; y <= 24; y += 4) for (double h = -24; h <= 24; h += 4) {
                const glm::dvec3 p = seam + t1 * x + t2 * y + su * h;
                const float c = w.density(p);
                const float l = w.density(p + t1 * 4.0) + w.density(p - t1 * 4.0) + w.density(p + t2 * 4.0) + w.density(p - t2 * 4.0)
                              + w.density(p + su * 4.0) + w.density(p - su * 4.0) - 6.0f * c;
                sum += std::fabs(l); ++n;
            }
            return sum / n;
        };
        const double antes = rugosidad();
        VoxWorld::Brush sm; sm.shape = VoxWorld::Brush::Sphere; sm.radiusM = 30.0f; sm.op = VoxWorld::Brush::Smooth; sm.amountM = 1.0f; sm.hardness = 1.0f;
        for (int i = 0; i < 3; ++i) w.stroke(seam, sm);
        const double despues = rugosidad();
        int faces = 0; const float costura = w.seamMismatch(&faces);
        std::printf("    suavizar: rugosidad %.3f -> %.3f (3 pasadas) · costura peor %.4f m en %d caras\n", antes, despues, costura, faces);
        CHECK(despues < antes * 0.8, "suavizar baja la rugosidad al menos un 20 %");
        CHECK(faces > 0 && costura < 1e-4f, "y la costura entre los dos chunks sigue exacta");
        for (int i = 0; i < 4; ++i) w.undo();
    }
    std::filesystem::remove_all(tmp, ec);
}

// ---------------------------------------------- TEST: esculpir (pinceles de Blender)
// Desplazamiento ± de la superficie con curva de caída, huella hexagonal y patrón de olas.
void test_vox_sculpt() {
    beginTest("vox: esculpir — olas +/-, huella hexagonal, curva de caida");
    const std::string tmp = (std::filesystem::temp_directory_path() / "haruka_vox_sculpt").string();
    std::error_code ec; std::filesystem::remove_all(tmp, ec);
    caveSetAutoProbability(0.0f);
    VoxWorld w; w.configure(3u, kR, tmp + "/bakes", "", [](const glm::dvec3&) { return 500.0f; });
    w.setSynchronousBake(true);
    const glm::dvec3 up = glm::normalize(glm::dvec3(0.2, 0.7, 0.6));
    const glm::dvec3 ref = (std::fabs(up.y) < 0.99) ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 e1 = glm::normalize(glm::cross(ref, up)), e2 = glm::cross(up, e1);
    auto at = [&](double x, double y, double h) { return up * (kR + h) + e1 * x + e2 * y; };
    auto dens = [&](const glm::dvec3& p) { w.ensure(w.keyAt(p)); return w.density(p); };
    const glm::dvec3 c = at(0, 0, 500.0);

    // (1) OLAS ±: patrón de paso 16 m, amplitud 5 m, caída constante, radio 40.
    {
        VoxWorld::Brush b; b.op = VoxWorld::Brush::Sculpt; b.radiusM = 40.0f; b.amountM = 5.0f;
        b.falloff = VoxWorld::Brush::FConstant; b.pattern = VoxWorld::Brush::PWaves; b.patternM = 16.0f; b.hardness = 1.0f;
        w.stroke(c, b);
        const float cresta = dens(at(4, 0, 502.5)), valle = dens(at(12, 0, 497.5)), crestaAlta = dens(at(4, 0, 507.0)), valleBajo = dens(at(12, 0, 493.0));
        std::printf("    olas: cresta (x=4) a +2,5 m %+.2f · a +7 m %+.2f · valle (x=12) a -2,5 m %+.2f · a -7 m %+.2f\n", cresta, crestaAlta, valle, valleBajo);
        CHECK(cresta > 0.0f && crestaAlta < 0.0f, "en la cresta el suelo sube ~5 m (roca a +2,5, aire a +7)");
        CHECK(valle < 0.0f && valleBajo > 0.0f, "en el valle baja ~5 m (aire a -2,5, roca a -7)");
        CHECK(dens(at(60, 0, 502.5)) < 0.0f, "CONTRAPRUEBA: fuera del radio, sin olas");
        w.undo();
        CHECK(dens(at(4, 0, 502.5)) < 0.0f, "deshacer devuelve el llano");
    }
    // (2) HEXÁGONO: radio 60 (apotema 52). A 57 m en dirección de una ARISTA queda fuera; a 50 m
    //     hacia un VÉRTICE, dentro. (El círculo coge los dos.) Radio grande y +12 m para que la
    //     trilineal de 4 m no mezcle un vóxel de fuera con el punto medido (medido con r=30: a 2 m
    //     del borde el vecino de fuera dominaba y salía −2,76).
    {
        VoxWorld::Brush b; b.op = VoxWorld::Brush::Sculpt; b.radiusM = 60.0f; b.amountM = 12.0f;
        b.falloff = VoxWorld::Brush::FConstant; b.sides = 6; b.hardness = 1.0f;
        w.stroke(c, b);
        const double a30 = glm::radians(30.0);
        const float arista = dens(at(57, 0, 503.0)), vertice = dens(at(50 * std::cos(a30), 50 * std::sin(a30), 503.0)), centro = dens(at(0, 0, 503.0));
        std::printf("    hexagono r=60: centro %+.2f · a 57 m hacia una arista %+.2f · a 50 m hacia un vertice %+.2f\n", centro, arista, vertice);
        CHECK(centro > 0.0f && vertice > 0.0f, "dentro del hexagono el suelo ha subido");
        CHECK(arista < 0.0f, "y fuera de la arista (pero dentro del circulo) no: la huella ES hexagonal");
        w.undo();
        b.sides = 0; w.stroke(c, b);
        const float circulo = dens(at(57, 0, 503.0));
        std::printf("    circulo r=60: a 57 m %+.2f\n", circulo);
        CHECK(circulo > 0.0f, "CONTRAPRUEBA: con huella circular ese punto si sube");
        w.undo();
    }
    // (3) CAÍDA: con caída lineal y +8 m, el centro sube ~8 y a medio radio ~4.
    {
        VoxWorld::Brush b; b.op = VoxWorld::Brush::Sculpt; b.radiusM = 40.0f; b.amountM = 8.0f; b.falloff = VoxWorld::Brush::FLinear; b.hardness = 1.0f;
        w.stroke(c, b);
        const float c6 = dens(at(0, 0, 506.0)), c10 = dens(at(0, 0, 510.0)), m2 = dens(at(20, 0, 502.0)), m6 = dens(at(20, 0, 506.0));
        std::printf("    caida lineal +8 m: centro a +6 %+.2f, a +10 %+.2f · medio radio a +2 %+.2f, a +6 %+.2f\n", c6, c10, m2, m6);
        CHECK(c6 > 0.0f && c10 < 0.0f, "en el centro sube ~8 m");
        CHECK(m2 > 0.0f && m6 < 0.0f, "a medio radio, ~4 m");
        w.undo();
    }
    std::filesystem::remove_all(tmp, ec);
}
