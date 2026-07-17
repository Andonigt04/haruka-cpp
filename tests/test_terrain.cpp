// ================================================================================================
// Tests de TERRENO puros-CPU (sin GL): el sampler analítico y su determinismo, la inversa cube-sphere,
// y los campos del planeta (tectónica, clima orográfico, erosión+hidrología, teselas MESO). Se dirigen
// con entrada sintética y se verifican invariantes — rápidos y deterministas, sin contexto gráfico.
// ================================================================================================
#include "test_common.h"

#include <cmath>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "core/terrain/terrain_sampler_v2.h"
#include "core/terrain/planet_geology.h"    // tectónica de placas
#include "core/terrain/planet_fields.h"     // erosión + hidrología + clima
#include "core/terrain/planet_meso.h"       // teselas erosionadas on-demand
#include "core/terrain/gpu_heightfield.h"
#include "core/terrain/terrain_generator.h"

using namespace Haruka;

// ------------------------------------------------------------------ TEST: sampler determinista (real)
void test_sampler_determinism() {
    beginTest("sampler_determinism");
    const double R = 6.371e6;
    WorldGenParams W = deriveWorldParams(1234u, R);
    W.profile = 0;
    glm::vec3 d = glm::normalize(glm::vec3(0.3f, 0.7f, 0.5f));
    float e1 = sampleTerrainV2(d, W, R).elevKm;
    float e2 = sampleTerrainV2(d, W, R).elevKm;
    CHECK(e1 == e2, "sampleTerrainV2 determinista");
    int land = 0, sea = 0; uint32_t s = 777u;
    auto rnd = [&]{ s = s * 1664525u + 1013904223u; return (float)(s >> 8) * (1.0f / 16777216.0f); };
    for (int i = 0; i < 512; ++i) {
        glm::vec3 dir = glm::normalize(glm::vec3(rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1));
        float e = sampleTerrainV2(dir, W, R).elevKm;
        if (e > 0.0f) ++land; else ++sea;
    }
    CHECK(land > 20 && sea > 20, "el mundo tiene mar Y tierra (no degenerado)");
    std::printf("    muestreo: %d tierra / %d mar de 512\n", land, sea);
}
void test_cube_sphere_inverse() {
    beginTest("cube_sphere_inverse");
    uint32_t st = 4242u;
    auto rnd = [&]{ st = st * 1664525u + 1013904223u; return double(st >> 8) * (1.0 / 16777216.0); };

    double maxErrDir = 0.0, maxErrLx = 0.0;
    int wrongFace = 0;
    for (int i = 0; i < 20000; ++i) {
        // Ida: (cara, lx, ly) → dirección.
        const PlanetFace face = (PlanetFace)(int)(rnd() * 5.999);
        const double lx = rnd() * 2.0 - 1.0, ly = rnd() * 2.0 - 1.0;
        PlanetChunkKey k; k.face = face; k.lod = 0; k.x = 0; k.y = 0;
        // getLocalPosition con (x,y) sobre una rejilla fina reproduce cualquier (lx,ly):
        // u = x/res → localX = u*2-1. Elegimos res grande y x = (lx+1)/2*res.
        const int res = 4096;
        const int xi = (int)std::lround((lx * 0.5 + 0.5) * res);
        const int yi = (int)std::lround((ly * 0.5 + 0.5) * res);
        const glm::dvec3 dir = glm::normalize(TerrainGenerator::getLocalPosition(k, xi, yi, res));

        // Vuelta: dirección → (cara, lx, ly).
        PlanetFace f2; double lx2, ly2;
        TerrainGenerator::dirToFaceLocal(dir, f2, lx2, ly2);
        // En la ARISTA del cubo (|lx| o |ly| = 1) el punto pertenece a DOS caras por igual: que
        // salga la vecina no es un error. Lo que tiene que cumplirse siempre es que la dirección
        // reconstruida sea la misma — y eso se comprueba abajo, venga por la cara que venga.
        const bool onEdge = (std::abs(lx) > 0.999 || std::abs(ly) > 0.999);
        if (f2 != face && !onEdge) ++wrongFace;

        // Y de vuelta a dirección: el error angular es lo que de verdad importa (es lo que decide
        // en qué celda cae la consulta).
        PlanetChunkKey k2; k2.face = f2; k2.lod = 0; k2.x = 0; k2.y = 0;
        const int xj = (int)std::lround((lx2 * 0.5 + 0.5) * res);
        const int yj = (int)std::lround((ly2 * 0.5 + 0.5) * res);
        const glm::dvec3 d2 = glm::normalize(TerrainGenerator::getLocalPosition(k2, xj, yj, res));
        maxErrDir = std::max(maxErrDir, glm::length(d2 - dir));

        if (f2 == face) {
            const double lxRef = (double)xi / res * 2.0 - 1.0;   // el (lx,ly) EXACTO de la rejilla
            maxErrLx = std::max(maxErrLx, std::abs(lx2 - lxRef));
        }
    }
    // Error angular → metros en la Tierra (para que el número signifique algo).
    std::printf("    caras mal=%d  ·  error max en lx=%.2e  ·  error angular max=%.2e (%.3f m en la Tierra)\n",
                wrongFace, maxErrLx, maxErrDir, maxErrDir * 6.371e6);
    CHECK(wrongFace == 0, "la CARA se identifica bien (salvo en la arista, donde hay dos válidas)");
    CHECK(maxErrDir * 6.371e6 < 0.5, "la inversa cae en el mismo punto (< 0.5 m en la Tierra)");
}

// --------------------------------- TEST (F10): la altura sale de la MALLA, no de una fórmula
// El invariante: si preguntas por la dirección de UN VÉRTICE de un chunk residente, la altura debe
// ser EXACTAMENTE la de ese vértice. Si no, la física está mirando otro sitio — que es justo el bug
// que hace flotar los props (la fórmula analítica y la malla no coinciden donde el chunk es grueso).
void test_geology_plates() {
    beginTest("geology_plates");
    Haruka::PlanetGeology g;
    g.generate(1234u, 30);
    CHECK(g.plates().size() == 30, "genera el nº de placas pedido");

    // Determinismo: misma seed → mismas placas (cliente y servidor DEBEN coincidir).
    Haruka::PlanetGeology g2;
    g2.generate(1234u, 30);
    bool same = true;
    for (size_t i = 0; i < g.plates().size(); ++i)
        if (glm::length(g.plates()[i].site - g2.plates()[i].site) > 1e-6f) same = false;
    CHECK(same, "DETERMINISTA por seed (mismas placas)");

    // Muestreo uniforme sobre la esfera (Fibonacci) → estadísticas del planeta.
    const int N = 20000;
    int land = 0, nan = 0;
    int highNearBoundary = 0, highTotal = 0;
    double sumElev = 0.0;
    const float GA = 2.39996323f;
    for (int i = 0; i < N; ++i) {
        float z   = 1.0f - 2.0f * (i + 0.5f) / N;
        float r   = std::sqrt(std::max(0.0f, 1.0f - z * z));
        float phi = GA * i;
        glm::vec3 dir(r * std::cos(phi), r * std::sin(phi), z);

        Haruka::GeologySample s = g.sample(dir);
        if (!std::isfinite(s.upliftKm)) ++nan;
        sumElev += s.upliftKm;
        if (s.upliftKm > 0.0f) ++land;

        // "Alto" = relieve claramente orogénico (>2 km sobre la base continental).
        if (s.upliftKm > Haruka::PlanetGeology::kContinentalBaseKm + 2.0f) {
            ++highTotal;
            if (s.boundaryDist < Haruka::PlanetGeology::kBoundaryWidthRad) ++highNearBoundary;
        }
    }
    CHECK(nan == 0, "sin NaN/inf en toda la esfera");

    const double landFrac = (double)land / N;
    // Un planeta creíble no es todo mar ni todo tierra. (La Tierra: ~29% de tierra emergida.)
    CHECK(landFrac > 0.12 && landFrac < 0.55, "fracción de TIERRA plausible (12-55%)");

    // LA PROPIEDAD CLAVE: las montañas son CORDILLERAS (viven en los límites de placa), no manchas
    // de ruido repartidas. Si esto falla, hemos vuelto al fBm con otro nombre.
    if (highTotal > 0) {
        const double onBoundary = (double)highNearBoundary / highTotal;
        CHECK(onBoundary > 0.90, "el relieve ALTO está en los LÍMITES de placa (cordilleras, no manchas)");
    } else {
        CHECK(false, "el planeta tiene ALGO de relieve orogénico");
    }

    // Continuidad: dos direcciones vecinas no pueden dar elevaciones dispares (si no, el terreno
    // saldría roto/con acantilados aleatorios).
    float maxJump = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        float z   = 1.0f - 2.0f * (i + 0.5f) / 2000;
        float r   = std::sqrt(std::max(0.0f, 1.0f - z * z));
        float phi = GA * i;
        glm::vec3 a(r * std::cos(phi), r * std::sin(phi), z);
        glm::vec3 b = glm::normalize(a + glm::vec3(1e-4f, 0, 0));   // ~600 m en la Tierra
        maxJump = std::max(maxJump, std::abs(g.sample(a).upliftKm - g.sample(b).upliftKm));
    }
    CHECK(maxJump < 0.5f, "CONTINUO: sin saltos de elevación entre direcciones vecinas");

    // El SAMPLER COMPLETO (el que usan física y spawn) debe ver la misma tierra que la geología.
    // Si aquí sale ~0% de tierra, el jugador aparece en mitad del océano (pasó: 2026-07-13).
    {
        Haruka::WorldGenParams W = Haruka::deriveWorldParams(1234u, 6371000.0);
        int land2 = 0; float maxH = -1e9f;
        for (int i = 0; i < 4000; ++i) {
            float z   = 1.0f - 2.0f * (i + 0.5f) / 4000;
            float r   = std::sqrt(std::max(0.0f, 1.0f - z * z));
            float phi = GA * i;
            glm::vec3 dir(r * std::cos(phi), r * std::sin(phi), z);
            float e = Haruka::sampleTerrainV2(dir, W, 6371000.0).elevKm;
            if (e > 0.03f) ++land2;                 // >30 m: lo que el spawn considera tierra
            maxH = std::max(maxH, e);
        }
        std::printf("    sampler: tierra=%.1f%%  cota_max=%.2f km\n", 100.0 * land2 / 4000, maxH);
        CHECK(land2 > 400, "el SAMPLER (física/spawn) ve TIERRA (si no, se spawnea en el mar)");

        // Con la SEED REAL de la escena: ¿qué hay en +Y (la dirección de reserva del spawn)?
        Haruka::WorldGenParams Ws = Haruka::deriveWorldParams(770422u, 6371000.0);
        float ey = Haruka::sampleTerrainV2(glm::vec3(0, 1, 0), Ws, 6371000.0).elevKm;
        int landS = 0;
        for (int i = 0; i < 4000; ++i) {
            float z   = 1.0f - 2.0f * (i + 0.5f) / 4000;
            float r   = std::sqrt(std::max(0.0f, 1.0f - z * z));
            float phi = GA * i;
            glm::vec3 dir(r * std::cos(phi), r * std::sin(phi), z);
            if (Haruka::sampleTerrainV2(dir, Ws, 6371000.0).elevKm > 0.03f) ++landS;
        }
        std::printf("    escena(seed 770422): tierra=%.1f%%  elev(+Y)=%.3f km\n",
                    100.0 * landS / 4000, ey);
    }

    // HARUKA_GEO_MAP=<ruta.ppm> → mapa equirectangular del planeta (para MIRARLO: ¿parece un
    // planeta o parece ruido?). Verde = tierra (más claro = más alto) · azul = mar (más oscuro =
    // más hondo) · rojo = eje de cordillera.
    if (const char* out = getenv("HARUKA_GEO_MAP")) {
        const int W = 1024, H = 512;
        std::vector<unsigned char> px((size_t)W * H * 3);
        for (int y = 0; y < H; ++y) {
            const float lat = (0.5f - (y + 0.5f) / H) * 3.14159265f;         // +pi/2 .. -pi/2
            for (int x = 0; x < W; ++x) {
                const float lon = ((x + 0.5f) / W * 2.0f - 1.0f) * 3.14159265f;
                glm::vec3 dir(std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon));
                Haruka::GeologySample s2 = g.sample(dir);
                float e = s2.upliftKm;
                unsigned char r, gg, b;
                if (e > 0.0f) {                       // TIERRA
                    float t = glm::clamp(e / 6.0f, 0.0f, 1.0f);
                    r  = (unsigned char)(40 + 200 * t);
                    gg = (unsigned char)(110 + 100 * t);
                    b  = (unsigned char)(40 + 120 * t);
                } else {                              // MAR
                    float t = glm::clamp(-e / 8.0f, 0.0f, 1.0f);
                    r  = (unsigned char)(10);
                    gg = (unsigned char)(60 - 40 * t);
                    b  = (unsigned char)(160 - 90 * t);
                }
                size_t i = ((size_t)y * W + x) * 3;
                px[i] = r; px[i+1] = gg; px[i+2] = b;
            }
        }
        if (FILE* fp = fopen(out, "wb")) {
            fprintf(fp, "P6\n%d %d\n255\n", W, H);
            fwrite(px.data(), 1, px.size(), fp);
            fclose(fp);
            std::printf("  [GEO-MAP] escrito %s\n", out);
        }
    }
}

// ===== F3: EROSIÓN + HIDROLOGÍA =======================================================
// Lo que hace creíble una montaña no es el ruido: es que el AGUA la haya tallado. Aquí probamos
// las propiedades de un relieve fluvial de verdad, que ninguna suma de fBm produce.
// --------------------------------------------------------------- TEST: clima derivado (F5)
// Lo que hay que demostrar NO es que exista un número "humedad", sino que la humedad la decide la
// GEOGRAFÍA: (1) hace más frío en los polos y en altura; (2) **hay sombra orográfica** — a sotavento
// de una cordillera el aire llega seco. Si esto no se cumple, el clima es un ruido con otro nombre.
void test_climate_orographic() {
    beginTest("climate_orographic");
    Haruka::PlanetGeology geo; geo.generate(4242u, 30);
    Haruka::PlanetFields F;   F.generate(4242u, geo, 192, 40);
    CHECK(!F.empty(), "genera los campos");

    // (1) Temperatura: ecuador cálido, polo frío, y la altura enfría.
    const float tEq   = F.sample(glm::normalize(glm::vec3(1, 0, 0))).tempC;
    const float tPole = F.sample(glm::normalize(glm::vec3(0, 1, 0))).tempC;
    std::printf("    temp: ecuador=%.1f °C  polo=%.1f °C\n", tEq, tPole);
    CHECK(tEq > tPole + 20.0f, "el ecuador es MUCHO más cálido que el polo");

    // (2) SOMBRA OROGRÁFICA — la promesa del plan: "el desierto está DETRÁS de las montañas".
    // Se compara MANZANAS CON MANZANAS: LLANURAS (misma banda de altitud, no crestas) según si el
    // camino del viento hasta ellas atraviesa una CORDILLERA o llega despejado desde el mar.
    // (Comparar los dos lados de una cresta era mal probe: ambos lados están altos y la cordillera
    // no tiene por qué estar alineada con el viento → medía ruido, salía 53% = azar.)
    const glm::vec3 axis(0, 1, 0);
    double sumTras = 0, sumLibre = 0;
    int    nTras = 0, nLibre = 0;
    uint32_t st = 7u;
    auto rnd = [&]{ st = st * 1664525u + 1013904223u; return float(st >> 8) * (1.0f / 16777216.0f); };
    // Las llanuras tras una cordillera son GEOGRÁFICAMENTE RARAS (hay que buscarlas): con 20k
    // muestras solo salían 16. Se barre más.
    for (int i = 0; i < 200000 && (nTras < 200 || nLibre < 200); ++i) {
        glm::vec3 d = glm::normalize(glm::vec3(rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1));
        const Haruka::FieldSample s0 = F.sample(d);
        if (s0.elevKm < 0.05f || s0.elevKm > 0.8f) continue;   // LLANURA (no cresta, no mar)
        if (std::abs(d.y) > 0.7f) continue;                    // fuera de los polos (allí no hay zonal)

        // ¿Qué hay aguas arriba del viento? (barrera máxima antes de llegar al mar)
        float barrier = 0.0f; bool sea = false;
        glm::vec3 p = d;
        for (int k = 0; k < 60; ++k) {
            glm::vec3 east = glm::cross(axis, p);
            if (glm::length(east) < 1e-3f) break;
            p = glm::normalize(p - glm::normalize(east) * 0.008f);
            const float e = F.sample(p).elevKm;
            if (e <= 0.0f) { sea = true; break; }
            barrier = std::max(barrier, e);
        }
        if (!sea) continue;                                     // interior profundo: otro fenómeno
        if (barrier > 1.5f)      { sumTras  += s0.humidity; ++nTras;  }   // TRAS una cordillera
        else if (barrier < 0.4f) { sumLibre += s0.humidity; ++nLibre; }   // camino LIBRE desde el mar
    }
    const double hTras  = nTras  ? sumTras  / nTras  : 0.0;
    const double hLibre = nLibre ? sumLibre / nLibre : 0.0;
    std::printf("    humedad en llanura: tras cordillera=%.2f (n=%d)  ·  camino libre al mar=%.2f (n=%d)\n",
                hTras, nTras, hLibre, nLibre);
    CHECK(nTras > 20 && nLibre > 20, "hay llanuras de los dos tipos que comparar");
    CHECK(hTras < hLibre - 0.15, "la llanura A SOTAVENTO de una cordillera es MAS SECA (sombra orografica)");
}

void test_erosion_hydrology() {
    beginTest("erosion_hydrology");
    Haruka::PlanetGeology geo; geo.generate(4242u, 30);
    Haruka::PlanetFields F;
    F.generate(4242u, geo, 192, 40);          // resolución modesta para que el test sea rápido
    CHECK(!F.empty(), "genera los campos");

    const int N = 20000;
    const float GA2 = 2.39996323f;
    int nan = 0, land = 0, rivers = 0, lakes = 0;
    float maxFlow = 0.0f;
    for (int i = 0; i < N; ++i) {
        float z = 1.0f - 2.0f * (i + 0.5f) / N;
        float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        float th = GA2 * i;
        glm::vec3 d(r * std::cos(th), r * std::sin(th), z);
        Haruka::FieldSample s = F.sample(d);
        if (!std::isfinite(s.elevKm) || !std::isfinite(s.flow)) ++nan;
        if (s.elevKm > 0.0f) ++land;
        if (s.flow > Haruka::PlanetFields::kRiverFlow) ++rivers;
        if (s.isLake) ++lakes;
        maxFlow = std::max(maxFlow, s.flow);
    }
    CHECK(nan == 0, "sin NaN/inf tras erosionar");
    CHECK(land > N / 20, "sigue habiendo TIERRA tras la erosión (no se ha comido el planeta)");

    // RÍOS: el caudal se concentra en cauces. Si el caudal estuviera repartido por igual, no habría
    // red de drenaje — sería ruido. Un planeta creíble tiene POCAS celdas de río y mucho interfluvio.
    const double riverFrac = (double)rivers / N;
    CHECK(maxFlow > 0.5f, "hay CAUCES con caudal alto (redes de drenaje)");
    CHECK(riverFrac > 0.001 && riverFrac < 0.25, "los ríos son CAUCES (pocos), no una mancha uniforme");

    // LAGOS: salen del relleno de depresiones (donde la cuenca no drena), no de un hash.
    CHECK(lakes > 0, "hay LAGOS (cuencas endorreicas rellenadas), no sellos por hash");

    std::printf("    tierra=%.1f%%  rios=%.2f%%  lagos=%.2f%%  caudal_max=%.2f\n",
                100.0 * land / N, 100.0 * riverFrac, 100.0 * lakes / N, maxFlow);

    // HARUKA_HYDRO_MAP=<ruta.ppm> → mapa del planeta EROSIONADO: relieve + RÍOS + lagos.
    if (const char* out = getenv("HARUKA_HYDRO_MAP")) {
        const int W = 1024, H = 512;
        std::vector<unsigned char> px((size_t)W * H * 3);
        for (int y = 0; y < H; ++y) {
            const float lat = (0.5f - (y + 0.5f) / H) * 3.14159265f;
            for (int x = 0; x < W; ++x) {
                const float lon = ((x + 0.5f) / W * 2.0f - 1.0f) * 3.14159265f;
                glm::vec3 dir(std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon));
                Haruka::FieldSample fs = F.sample(dir);
                unsigned char r, g2, b;
                if (fs.elevKm <= 0.0f) {                       // MAR
                    float t = glm::clamp(-fs.elevKm / 6.0f, 0.0f, 1.0f);
                    r = 8; g2 = (unsigned char)(50 - 35 * t); b = (unsigned char)(150 - 80 * t);
                } else if (fs.isLake) {                        // LAGO (cuenca rellenada)
                    r = 40; g2 = 150; b = 190;
                } else {                                       // TIERRA: verde→marrón→blanco por altura
                    float t = glm::clamp(fs.elevKm / 5.0f, 0.0f, 1.0f);
                    r  = (unsigned char)(60 + 180 * t);
                    g2 = (unsigned char)(120 + 90 * t);
                    b  = (unsigned char)(55 + 150 * t * t);
                    // RÍO: el caudal pinta el cauce (lo que la erosión ha tallado).
                    float riv = glm::clamp((fs.flow - 0.38f) / 0.25f, 0.0f, 1.0f);
                    r  = (unsigned char)glm::mix((float)r,  30.0f, riv);
                    g2 = (unsigned char)glm::mix((float)g2,130.0f, riv);
                    b  = (unsigned char)glm::mix((float)b, 220.0f, riv);
                }
                size_t i2 = ((size_t)y * W + x) * 3;
                px[i2] = r; px[i2+1] = g2; px[i2+2] = b;
            }
        }
        if (FILE* fp = fopen(out, "wb")) {
            fprintf(fp, "P6\n%d %d\n255\n", W, H);
            fwrite(px.data(), 1, px.size(), fp);
            fclose(fp);
            std::printf("  [HYDRO-MAP] escrito %s\n", out);
        }
    }
}

// ===== F3-MESO: el detalle erosionado que SÍ pisas ====================================
// El riesgo #1 del plan son las COSTURAS: si cada tesela erosiona por su cuenta, los valles no casan
// en los bordes. Aquí se mide exactamente eso: cruzar un borde de tesela NO puede dar un escalón.
void test_meso_tiles() {
    beginTest("meso_tiles");
    Haruka::PlanetGeology geo; geo.generate(4242u, 30);
    Haruka::PlanetFields  F;   F.generate(4242u, geo, 192, 40);
    Haruka::PlanetMeso    M;   M.init(4242u, &F, /*radio Tierra*/6.371e6);
    CHECK(M.ready(), "meso inicializado");

    // Detalle: el meso debe AÑADIR relieve sobre el macro (si no, no sirve de nada).
    const int TILES = 1 << M.tileLevel();
    float maxDelta = 0.0f;
    int   samples = 0;
    for (int i = 0; i < 400; ++i) {
        float z = 1.0f - 2.0f * (i + 0.5f) / 400;
        float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        float th = 2.39996323f * i;
        glm::vec3 d(r * std::cos(th), r * std::sin(th), z);
        if (F.sample(d).elevKm < 0.2f) continue;          // solo tierra firme
        maxDelta = std::max(maxDelta, std::abs(M.elevKm(d) - F.sample(d).elevKm));
        if (++samples > 40) break;
    }
    CHECK(maxDelta > 0.01f, "el MESO añade relieve real sobre el macro (>10 m)");

    // ⚠️ COSTURAS: muestreamos a ambos lados de un borde de tesela. El salto debe ser pequeño.
    // (Si el caudal no se heredara del macro, aquí saldrían escalones de decenas de metros.)
    float maxSeam = 0.0f;
    int   seams = 0;
    for (int t = 1; t < 60 && seams < 12; ++t) {
        // Un punto justo en el borde entre dos teselas de la cara +Z.
        float u = ((float)(t * 37 % TILES) / TILES) * 2.0f - 1.0f;   // borde exacto de tesela
        float v = ((float)(t * 53 % TILES) / TILES) * 2.0f - 1.0f;
        glm::vec3 dm = glm::normalize(glm::vec3(u, -v, 1.0f));
        if (F.sample(dm).elevKm < 0.2f) continue;                    // en tierra
        const float eps = 1e-5f;                                     // ~60 m en la Tierra
        glm::vec3 a = glm::normalize(dm + glm::vec3(-eps, 0, 0));
        glm::vec3 b = glm::normalize(dm + glm::vec3( eps, 0, 0));
        maxSeam = std::max(maxSeam, std::abs(M.elevKm(a) - M.elevKm(b)));
        ++seams;
    }
    std::printf("    meso: delta_vs_macro=%.1f m  salto_max_en_borde=%.1f m  (%d bordes)\n",
                maxDelta * 1000.0f, maxSeam * 1000.0f, seams);
    CHECK(maxSeam < 0.030f, "SIN COSTURAS: cruzar un borde de tesela no da un escalón (<30 m)");
}

