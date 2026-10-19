// LA COSTA QUE VE EL JUGADOR: si la orilla se mueve, si hay rompiente y si la ola se enrolla.
//
// ⚠️ POR QUE ESTE ARCHIVO Y NO LOS DE TestOceanBreak.cpp. Aquellos miden la FORMA de la ola con el mar
// de referencia (`oceanDefaultState`, Hs 2,80 m) o con 5 m/s como "calma", y pasan. El juego no usa
// ese mar: usa `oceanStateFromWind` con el viento del clima, y en la partida del 16-10 el log decia
//
//     [Mar] viento del mar 3.5 m/s -> Sigma amp 0.17 m (cresta-valle ~0.35 m)
//
// Con ese mar la costa no se movia y no rompia nada, y ningun test lo veia. Estos miden la costa con
// los estados que el juego REALMENTE produce —el viento minimo, el del log y uno moderado— sobre
// playas de pendiente realista, en unidades que se pueden comparar con lo que se ve:
//
//   · cuantos METROS sube y baja la orilla por la arena (no la cota: lo que se ve es el borde moverse)
//   · cuantos metros de franja rompen, contra el quad del agua que la tiene que dibujar
//   · si la cresta llega a PLEGARSE (jacobiano < 0): el labio que pasa por delante de la cara, el
//     principio del "rollo de papel"
//
// Todo con el gemelo de CPU, que es el que usa la fisica y el que los tests de paridad atan al shader.
#include "World/Water/OceanTestCommon.h"
#include <algorithm>
#include <cstdlib>
#include <string>

namespace {

namespace W = Haruka::World::Water;

// Los mares que el juego produce de verdad. El primero es el suelo de `oceanStateFromWind`.
struct SeaCase { const char* name; float windMS; };
constexpr SeaCase kSeas[] = {
    { "viento minimo", W::OCEAN_WIND_MIN },
    { "calma del log (3,5 m/s)", 3.5f },
    { "brisa (8 m/s)", 8.0f },
};
// EL MAR DEL JUEGO: el de viento mas la mar de fondo, igual que `PlanetarySystem::updateOceanState`.
W::OceanState gameSea(float windMS) {
    return W::oceanAddSwell(W::oceanStateFromWind(windMS, 1.0f, 0.0f, 0.0f));
}

// Playas: 1:10 empinada, 1:20 tipica, 1:50 tendida.
constexpr float kSlopes[] = { 0.10f, 0.05f, 0.02f };

// El quad mas fino que el agua puede dibujar hoy (`TerrainNodeRenderer::Water::waveQuadMinM`) y el
// suelo de Nyquist de la ola (`OCEAN_MIN_QUAD_M`). La rompiente tiene que caber en varios.
constexpr float kWaterQuadM = 2.0f;

struct Frame { glm::vec3 up, t1, t2, D0; };
Frame frameAt(const W::OceanState& st) {
    Frame f;
    f.up = upAt(0.3, 0.15);
    f.t1 = glm::normalize(glm::cross(f.up, glm::vec3(0, 1, 0)));
    f.t2 = glm::cross(f.up, f.t1);
    // La ola viaja HACIA la orilla: la pendiente del fondo sube en la direccion del tren dominante.
    f.D0 = glm::normalize(f.t1 * st.wave[0][2] + f.t2 * st.wave[0][3]);
    return f;
}

// Profundidad maxima a la que la ola ya rompe (indice H/d >= 0,78, el clasico). 0 = no rompe en
// ningun fondo de mas de 2 cm.
float breakDepth(const W::OceanState& st) {
    float best = 0.0f;
    for (float d = 0.02f; d < 30.0f; d += 0.01f)
        if (W::oceanBreakIndex(d, st) >= 0.78f) best = d;
    return best;
}

// Recorrido vertical de la trepada (m) en un fondo dado, sobre dos periodos largos.
float swashSwing(const W::OceanState& st, const Frame& f, float depth) {
    const glm::vec3 wp = f.up * (float)kR;
    float lo = 1e9f, hi = -1e9f;
    for (int s = 0; s < 1500; ++s) {
        const float v = W::oceanSwash(depth, wp, f.up, (float)s * 0.02f, st);
        lo = std::min(lo, v); hi = std::max(hi, v);
    }
    return hi - lo;
}

} // namespace

// ------------------------------------------------------- LA ORILLA SE MUEVE (con el mar del juego)
void test_coast_surf_moves() {
    beginTest("coast_surf_moves");
    for (const SeaCase& sc : kSeas) {
        const W::OceanState st = gameSea(sc.windMS);
        const Frame f = frameAt(st);
        // (a) El BORDE del agua por la arena: recorrido vertical de la trepada justo en la orilla,
        //     pasado a metros horizontales con la pendiente. Es lo que el ojo ve moverse.
        const float swing = swashSwing(st, f, 0.1f);
        // (b) La superficie a 1,5 m de fondo, donde el jugador mira el agua a sus pies.
        const glm::vec3 wp = f.up * (float)kR;
        float lo = 1e9f, hi = -1e9f;
        for (int s = 0; s < 1500; ++s) {
            const float t = (float)s * 0.02f;
            const float h = W::oceanWaveHeight(wp, f.up, t, 1.5f, 1.0f, st, W::WATER_FETCH_UNLIMITED,
                                               f.D0, kWaterQuadM)
                          + W::oceanSwash(1.5f, wp, f.up, t, st);
            lo = std::min(lo, h); hi = std::max(hi, h);
        }
        std::printf("    %-24s Hs %.2f m · orilla sube/baja %.2f m · a 1,5 m de fondo %.2f m\n",
                    sc.name, W::oceanSignificantHeight(st), swing, hi - lo);
        for (float slope : kSlopes)
            std::printf("        playa 1:%-3.0f -> el borde recorre %.1f m de arena\n",
                        1.0f / slope, swing / slope);
        // Umbrales de "se ve": la orilla tiene que barrer al menos 2 m de arena en una playa tipica
        // (1:20) y el agua a los pies subir y bajar al menos 30 cm. Por debajo, desde la altura de
        // los ojos, la costa se lee congelada — que es lo que se reporto.
        CHECK(swing / 0.05f >= 2.0f, "la orilla barre >= 2 m de arena en una playa 1:20");
        CHECK(hi - lo >= 0.30f, "a 1,5 m de fondo el agua sube y baja >= 30 cm");
    }
}

// ------------------------------------------------------------ HAY ROMPIENTE, Y CABE EN LA MALLA
void test_coast_surf_breaks() {
    beginTest("coast_surf_breaks");
    for (const SeaCase& sc : kSeas) {
        const W::OceanState st = gameSea(sc.windMS);
        const float db = breakDepth(st);
        // Altura de la ola al romper: `H = 2·Σamp·green` sin recortar, en el fondo donde rompe.
        float sum = 0.0f;
        for (int i = 0; i < W::OCEAN_WAVES; ++i) sum += st.wave[i][1];
        const float hb = (db > 0.0f) ? 2.0f * sum * W::oceanGreenGain(db) : 0.0f;
        std::printf("    %-24s rompe desde %.2f m de fondo con ola de %.2f m\n", sc.name, db, hb);
        for (float slope : kSlopes)
            std::printf("        playa 1:%-3.0f -> franja de rompiente %.1f m = %.1f quads de agua\n",
                        1.0f / slope, db / slope, db / slope / kWaterQuadM);
        // Una rompiente se VE si la ola que rompe tiene medio metro y la franja la dibujan al menos
        // 4 quads en una playa tipica: con menos, la forma de la ola rota cae entre dos vertices.
        CHECK(hb >= 0.5f, "la ola que rompe mide >= 0,5 m");
        CHECK(db / 0.05f >= 4.0f * kWaterQuadM, "en una playa 1:20 la franja de rompiente ocupa >= 4 quads");
    }
}

// --------------------------------------------- LA CRESTA SE PLIEGA: el principio del rollo de papel
void test_coast_surf_curls() {
    beginTest("coast_surf_curls");
    for (const SeaCase& sc : kSeas) {
        const W::OceanState st = gameSea(sc.windMS);
        const Frame f = frameAt(st);
        const float db = std::max(breakDepth(st), 0.3f);
        // Barrido de la franja de rompiente (de su borde exterior a la orilla) y de dos periodos del
        // tren dominante, con el quad que el agua dibuja de verdad.
        float jmin = 1e9f, dAt = 0.0f;
        for (float d = db; d >= 0.1f; d -= std::max(db / 24.0f, 0.02f))
            for (int s = 0; s < 600; ++s) {
                const float t = (float)s * 0.03f;
                for (int xi = 0; xi < 8; ++xi) {
                    const glm::vec3 wp = f.up * (float)kR + f.D0 * ((float)xi * 3.0f);
                    const float j = W::oceanJacobian(wp, f.up, t, d, 1.0f, st, W::WATER_FETCH_UNLIMITED,
                                                     f.D0, nullptr, nullptr, kWaterQuadM);
                    if (j < jmin) { jmin = j; dAt = d; }
                }
            }
        std::printf("    %-24s jacobiano minimo en la rompiente %+.3f (en %.2f m de fondo)%s\n",
                    sc.name, jmin, dAt, jmin < 0.0f ? "  <- PLIEGA" : "");
        // Jacobiano < 0 = la superficie se dobla sobre si misma: el labio adelanta a la cara. Es la
        // condicion NECESARIA para el rollo; sin ella no hay tubo que dibujar, solo una cresta.
        CHECK(jmin < 0.0f, "la cresta se pliega en la rompiente (jacobiano < 0)");
    }
}

// ----------------------------------------------- LA MAR DE FONDO NO ENTRA EN UN LAGO NI EN UN CHARCO
//
// El swell viene de un temporal a miles de km: un lago solo tiene la ola que su viento y su fetch
// levantan. Si la mar de fondo se colara, un lago de montaña respiraria como la costa.
void test_coast_swell_not_in_lakes() {
    beginTest("coast_swell_not_in_lakes");
    const glm::vec3 up = upAt(0.3, 0.15);
    const glm::vec3 wp = up * (float)kR;
    for (const SeaCase& sc : kSeas) {
        const W::OceanState wind = W::oceanStateFromWind(sc.windMS, 1.0f, 0.0f, 0.0f);
        const W::OceanState game = gameSea(sc.windMS);
        for (float fetch : { 30.0f, 2000.0f }) {
            float dh = 0.0f, ds = 0.0f;
            for (int s = 0; s < 400; ++s) {
                const float t = (float)s * 0.05f;
                dh = std::max(dh, std::abs(W::oceanWaveHeight(wp, up, t, 2.0f, 1.0f, game, fetch)
                                         - W::oceanWaveHeight(wp, up, t, 2.0f, 1.0f, wind, fetch)));
                ds = std::max(ds, std::abs(W::oceanSwash(0.3f, wp, up, t, game, fetch)
                                         - W::oceanSwash(0.3f, wp, up, t, wind, fetch)));
            }
            std::printf("    %-24s fetch %6.0f m: con y sin mar de fondo difieren %.5f m (ola) · %.5f m (orilla)\n",
                        sc.name, fetch, dh, ds);
            CHECK(dh < 1e-4f && ds < 1e-4f, "en agua cerrada la mar de fondo no cambia nada");
        }
        // CONTRAPRUEBA: en mar abierto SI la cambia (si no, el test de arriba pasaria sin swell).
        float dOpen = 0.0f;
        for (int s = 0; s < 400; ++s)
            dOpen = std::max(dOpen, std::abs(W::oceanWaveHeight(wp, up, (float)s * 0.05f, 2.0f, 1.0f, game)
                                           - W::oceanWaveHeight(wp, up, (float)s * 0.05f, 2.0f, 1.0f, wind)));
        CHECK(sc.windMS > 10.0f || dOpen > 0.05f, "CONTRAPRUEBA: en mar abierto la mar de fondo se nota");
    }
}

// ------------------------------------------------------------- EN MAR ABIERTO SE VEN OLAS
//
// "No se ven olas" (16-10), con Hs ~1 m de mar de fondo ya puesto: es que ESA ola mide 89-137 m de
// largo, y una ola de 137 m con 25 cm de amplitud tiene una pendiente del 1 % — un plano a la vista.
// Lo que el ojo llama ola es la PENDIENTE, no la altura. Esto mide la pendiente RMS de la superficie
// por diferencias finitas, con el quad que el agua dibuja cerca (2 m) y a media distancia (8 m).
void test_coast_open_sea_waves_visible() {
    beginTest("coast_open_sea_waves_visible");
    for (const SeaCase& sc : kSeas) {
        const W::OceanState st = gameSea(sc.windMS);
        const Frame f = frameAt(st);
        const glm::vec3 side = glm::normalize(glm::cross(f.up, f.D0));
        for (float quad : { kWaterQuadM, 8.0f }) {
            double s2 = 0.0; int n = 0;
            const float e = 0.5f;
            for (int ix = 0; ix < 40; ++ix)
                for (int iy = 0; iy < 6; ++iy)
                    for (int it = 0; it < 10; ++it) {
                        const glm::vec3 wp = f.up * (float)kR + f.D0 * ((float)ix * 5.3f) + side * ((float)iy * 7.1f);
                        const float t = (float)it * 1.7f;
                        auto h = [&](const glm::vec3& p) {
                            return W::oceanWaveHeight(p, f.up, t, 100.0f, 1.0f, st, W::WATER_FETCH_UNLIMITED,
                                                      glm::vec3(0.0f), quad);
                        };
                        const float gx = (h(wp + f.D0 * e) - h(wp - f.D0 * e)) / (2.0f * e);
                        const float gy = (h(wp + side * e) - h(wp - side * e)) / (2.0f * e);
                        s2 += (double)(gx * gx + gy * gy); ++n;
                    }
            const float rms = (float)std::sqrt(s2 / std::max(n, 1));
            std::printf("    %-24s quad %4.1f m: pendiente RMS %.3f (%.1f grados)\n",
                        sc.name, quad, rms, std::atan(rms) * 57.2958f);
            // 0,06 de pendiente RMS (~3,4 grados): por debajo, con la luz de cielo y sol que lleva el agua,
            // la superficie se lee como un plano con destellos. Es el umbral de "se ve que hay oleaje".
            // ⚠️ SOLO CON VIENTO (18-10): el escarpado mínimo que hacía visible el mar en calma era un mar
            // INVENTADO, y se quitó ("el clima decide"). En calma el mar puede ser casi un plano: lo mueve
            // el rizo por píxel. Con brisa (8 m/s) el viento ya da pendiente de sobra.
            if (sc.windMS >= 8.0f)
                CHECK(rms >= 0.06f, "con brisa, en mar abierto la superficie tiene pendiente visible (RMS >= 0,06)");
        }
    }
}

// ------------------------------------------------- EL PERFIL DE LA OLA, CORTADO EN SU RUMBO
//
// Corte vertical a lo largo del tren que rompe (`D0`), una longitud de onda local, en un instante:
// (X, Z) = posicion DIBUJADA de cada punto de la lamina (con el vaiven horizontal y el labio).
struct Profile { std::vector<float> X, Z; };
Profile cutProfile(const W::OceanState& st, const Frame& f, float depth, float t, float fetch = W::WATER_FETCH_UNLIMITED) {
    const float k = W::oceanWaveNumber(6.2831853f / st.wave[0][0], depth);
    const float L = 6.2831853f / k;
    const int N = 1500;
    Profile p; p.X.resize(N + 1); p.Z.resize(N + 1);
    for (int i = 0; i <= N; ++i) {
        const float x = (float)i * L / (float)N;
        const glm::vec3 wp = f.D0 * x;          // relativo al ancla: numeros pequenos
        const glm::vec3 d = W::oceanDisplacement(wp, f.up, t, depth, 1.0f, st, fetch);
        p.X[i] = x + glm::dot(d, f.D0);
        p.Z[i] = glm::dot(d, f.up);
    }
    return p;
}
// EL TUBO: una vertical que corta la lamina 3 veces o mas tiene AGUA-AIRE-AGUA: el labio encima, el
// hueco debajo y la cara mas abajo. `pocket` = el hueco mas alto (entre el corte de arriba y el
// siguiente), `over` = cuantos metros de X tienen esa forma (el vuelo del labio).
void tubeOf(const Profile& p, float& pocket, float& over) {
    pocket = over = 0.0f;
    const float x0 = *std::min_element(p.X.begin(), p.X.end()), x1 = *std::max_element(p.X.begin(), p.X.end());
    for (float xv = x0; xv < x1; xv += 0.02f) {
        float zs[16]; int n = 0;
        for (size_t i = 0; i + 1 < p.X.size() && n < 16; ++i)
            if ((p.X[i] - xv) * (p.X[i + 1] - xv) < 0.0f) {
                const float a = (xv - p.X[i]) / (p.X[i + 1] - p.X[i]);
                zs[n++] = p.Z[i] + a * (p.Z[i + 1] - p.Z[i]);
            }
        if (n < 3) continue;
        std::sort(zs, zs + n, [](float a, float b) { return a > b; });
        pocket = std::max(pocket, zs[0] - zs[1]);
        over += 0.02f;
    }
}

// ----------------------------------------------- LA CARA EMPINADA ES LA DE DELANTE (17-10)
//
// La ola viaja hacia +D0 (`φ = k·x − ω·t`). El adelanto de cresta estaba en `max(0, cos φ)` —la mitad
// de ATRAS— y la rompiente salia al reves: la cara trasera casi vertical y la delantera tendida.
// Se mide, en la franja que rompe, la PENDIENTE MAXIMA de cada cara en su mitad alta (donde actua
// el adelanto; mas abajo manda la mar de fondo larga y las dos caras miden lo mismo, ~10 m). Donde la
// cara vuelca (`dX <= 0`) la pendiente cuenta como vertical (tope 10).
void test_coast_lip_front_steep() {
    beginTest("coast_lip_front_steep");
    for (const SeaCase& sc : kSeas) {
        const W::OceanState st = gameSea(sc.windMS);
        const Frame f = frameAt(st);
        // En la franja que ROMPE (indice 0,78-1,70). La cara delantera la empina tambien el labio, asi
        // que lo que delata el signo del adelanto es la ESPALDA: con `max(0, cos φ)` media 0,67 (34°)
        // y una rompiente de verdad la tiene tendida. Antes de romper (0,60-0,78) el adelanto es aun
        // pequeno y los dos signos dan lo mismo (medido: 0,18/0,25 con los dos).
        auto measure = [&](float gLo, float gHi, double& front, double& back) {
            front = back = 0.0; int n = 0;
            for (float depth = 30.0f; depth >= 0.3f; depth -= 0.05f) {
                const float g = W::oceanBreakIndex(depth, st);
                if (g < gLo || g >= gHi) continue;
                for (int it = 0; it < 24; ++it) {
                    const Profile p = cutProfile(st, f, depth, (float)it * 0.37f);
                    const size_t ic = (size_t)(std::max_element(p.Z.begin(), p.Z.end()) - p.Z.begin());
                    const float zTop = p.Z[ic], zMin = *std::min_element(p.Z.begin(), p.Z.end());
                    const float zHalf = zTop - 0.5f * (zTop - zMin);
                    auto steep = [&](size_t i0, size_t i1) {   // |dZ/dX| del tramo, vertical si vuelca
                        const float dx = std::abs(p.X[i1] - p.X[i0]), dz = std::abs(p.Z[i1] - p.Z[i0]);
                        const bool turns = (i1 > i0) ? (p.X[i1] <= p.X[i0]) : (p.X[i0] <= p.X[i1]);
                        return turns ? 10.0f : std::min(dz / std::max(dx, 1e-4f), 10.0f);
                    };
                    float sf = 0.0f, sb = 0.0f;
                    size_t b = ic, a = ic;
                    for (; b + 1 < p.Z.size() && p.Z[b + 1] > zHalf; ++b) sf = std::max(sf, steep(b, b + 1));
                    // La ESPALDA se mide en el 1/8 de onda detrás de la cresta, donde actúa el adelanto.
                    // ⚠️ Con solo mar de fondo (sin los trenes medios que antes se inventaban, 18-10) la
                    // "mitad alta" de una ola en la rompiente mide un tercio de onda y metía dentro el
                    // pliegue DELANTERO de la cresta anterior: lo contaba como espalda (10, vuelco).
                    const size_t backSpan = p.Z.size() / 8;
                    for (; a > 0 && p.Z[a - 1] > zHalf; --a)
                        if (ic - (a - 1) <= backSpan) sb = std::max(sb, steep(a, a - 1));
                    if (b + 1 >= p.Z.size() || a == 0) continue;   // la cresta cae en el borde del corte
                    front += sf; back += sb; ++n;
                }
            }
            front /= std::max(n, 1); back /= std::max(n, 1);
            return n;
        };
        double fB, bB;
        const int nB = measure(0.78f, 1.70f, fB, bB);
        std::printf("    %-24s pendiente maxima de la mitad alta al romper: cara delantera %.2f · espalda %.2f (%d cortes)\n",
                    sc.name, fB, bB, nB);
        CHECK(nB > 20, "hay cortes con la cresta dentro (si no, la media no significa nada)");
        CHECK(bB < 0.5, "la ESPALDA de la ola que rompe queda tendida (< 0,5, unos 27 grados)");
        CHECK(fB > 2.5 * bB, "la cara empinada es la DELANTERA, la que mira a la orilla");
    }
}

// ----------------------------------------------------------- EL ROLLO DE PAPEL: HAY TUBO
//
// "Las olas tienen que romper como un rollo de papel" (16-10). Plegar (`coast_surf_curls`) es
// necesario pero no basta: un pliegue puede ser un bucle de Gerstner en la cresta, sin hueco. Esto
// pide lo que se ve como tubo: en algun instante de la franja que rompe, el labio cuelga sobre un
// HUECO DE AIRE de al menos un cuarto de la ola y vuela al menos medio metro. CONTRAPRUEBA: la misma
// mar en mar abierto (100 m de fondo) y en un lago (fetch 300 m) no hace tubo.
void test_coast_lip_barrel() {
    beginTest("coast_lip_barrel");
    for (const SeaCase& sc : kSeas) {
        const W::OceanState st = gameSea(sc.windMS);
        const Frame f = frameAt(st);
        const float db = breakDepth(st);
        float bestPocket = 0.0f, bestOver = 0.0f, waveAt = 0.0f, dAt = 0.0f;
        for (float depth = db; depth >= 0.4f * db; depth -= 0.05f * db)
            for (int it = 0; it < 60; ++it) {
                const Profile p = cutProfile(st, f, depth, (float)it * 0.21f);
                float pocket, over; tubeOf(p, pocket, over);
                if (pocket > bestPocket) {
                    bestPocket = pocket; bestOver = over; dAt = depth;
                    waveAt = *std::max_element(p.Z.begin(), p.Z.end()) - *std::min_element(p.Z.begin(), p.Z.end());
                }
            }
        float openPocket = 0.0f, lakePocket = 0.0f;
        for (int it = 0; it < 60; ++it) {
            float pk, ov;
            tubeOf(cutProfile(st, f, 100.0f, (float)it * 0.21f), pk, ov);   openPocket = std::max(openPocket, pk);
            tubeOf(cutProfile(st, f, 0.5f * db, (float)it * 0.21f, 300.0f), pk, ov); lakePocket = std::max(lakePocket, pk);
        }
        std::printf("    %-24s tubo: hueco %.2f m bajo el labio, vuelo %.2f m, ola %.2f m (a %.2f m de fondo) · mar abierto %.2f · lago %.2f\n",
                    sc.name, bestPocket, bestOver, waveAt, dAt, openPocket, lakePocket);
        CHECK(bestPocket >= 0.25f * waveAt && waveAt > 0.5f, "el labio cuelga sobre un hueco de >= 1/4 de la ola");
        CHECK(bestOver >= 0.5f, "el labio vuela >= 0,5 m por delante de su cara");
        CHECK(openPocket == 0.0f && lakePocket == 0.0f, "CONTRAPRUEBA: en mar abierto y en un lago no hay tubo");
    }
}

// ----------------------------------------------- EL LABIO CAE DELANTE, NO ATRAVIESA LA CARA (18-10)
//
// Un tubo de verdad es una curva que se enrolla SIN cortarse: el labio vuela por encima del hueco y cae
// en el seno, delante de la cara. Si el desplazamiento del labio es mas largo que el hueco que deja, la
// punta se mete por la cara y el perfil se CRUZA consigo mismo (un lazo): agua que atraviesa agua.
// `coast_lip_barrel` no lo ve (cuenta cortes de una vertical, y un lazo tambien da 3). Aqui se cuentan
// los cruces del perfil consigo mismo (segmentos no vecinos que se cortan) en toda la franja que rompe.
int profileSelfCrossings(const Profile& p, int stride = 3) {
    std::vector<float> X, Z;
    for (size_t i = 0; i < p.X.size(); i += stride) { X.push_back(p.X[i]); Z.push_back(p.Z[i]); }
    int n = 0;
    const size_t m = X.size();
    for (size_t i = 0; i + 1 < m; ++i)
        for (size_t j = i + 2; j + 1 < m; ++j) {
            const float ax = X[i], az = Z[i], bx = X[i + 1], bz = Z[i + 1];
            const float cx = X[j], cz = Z[j], dx = X[j + 1], dz = Z[j + 1];
            if (std::max(ax, bx) < std::min(cx, dx) || std::max(cx, dx) < std::min(ax, bx)) continue;
            const float d1 = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
            const float d2 = (bx - ax) * (dz - az) - (bz - az) * (dx - ax);
            const float d3 = (dx - cx) * (az - cz) - (dz - cz) * (ax - cx);
            const float d4 = (dx - cx) * (bz - cz) - (dz - cz) * (bx - cx);
            if (d1 * d2 < 0.0f && d3 * d4 < 0.0f) ++n;
        }
    return n;
}
void test_coast_lip_no_loop() {
    beginTest("coast_lip_no_loop");
    for (const SeaCase& sc : kSeas) {
        const W::OceanState st = gameSea(sc.windMS);
        const Frame f = frameAt(st);
        const float db = breakDepth(st);
        int cuts = 0, looped = 0, tubes = 0; float worstD = 0.0f;
        for (float depth = db; depth >= 0.4f * db; depth -= 0.05f * db)
            for (int it = 0; it < 30; ++it) {
                const Profile p = cutProfile(st, f, depth, (float)it * 0.41f);
                float pocket, over; tubeOf(p, pocket, over);
                ++cuts;
                if (pocket > 0.0f) ++tubes;
                if (profileSelfCrossings(p) > 0) {
                    ++looped; worstD = depth;
                    // Volcado para mirarlo (HARUKA_DUMP_LOOP=fichero.csv): el primer lazo de cada mar.
                    if (const char* dump = std::getenv("HARUKA_DUMP_LOOP"); dump && looped == 1) {
                        if (FILE* fo = std::fopen((std::string(dump) + "." + std::to_string((int)(sc.windMS * 10)) + ".csv").c_str(), "w")) {
                            for (size_t i = 0; i < p.X.size(); ++i) std::fprintf(fo, "%f,%f\n", p.X[i], p.Z[i]);
                            std::fclose(fo);
                        }
                    }
                }
            }
        std::printf("    %-24s %d cortes de la franja · %d con tubo · %d con LAZO (la punta atraviesa la cara)%s\n",
                    sc.name, cuts, tubes, looped, looped ? "" : "");
        if (looped) std::printf("      ultimo lazo a %.2f m de fondo\n", worstD);
        CHECK(tubes > 0, "hay tubos en la franja (si no, no hay nada que medir)");
        CHECK(looped == 0, "el labio cae delante de la cara: el perfil no se cruza consigo mismo");
    }
}

HARUKA_REGISTER_TESTS(
    { "coast_lip_front_steep", test_coast_lip_front_steep },
    { "coast_lip_barrel", test_coast_lip_barrel },
    { "coast_lip_no_loop", test_coast_lip_no_loop },
    { "coast_open_sea_waves_visible", test_coast_open_sea_waves_visible },
    { "coast_swell_not_in_lakes", test_coast_swell_not_in_lakes },
    { "coast_surf_moves",  test_coast_surf_moves },
    { "coast_surf_breaks", test_coast_surf_breaks },
    { "coast_surf_curls",  test_coast_surf_curls });
