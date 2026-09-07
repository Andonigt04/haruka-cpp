// ================================================================================================
// EL MAR: que la ola de la física sea la misma que la del render, y que MUEVA cosas.
//
// Tres afirmaciones, cada una con su contraprueba (una comprobación que TIENE que fallar si el
// código estuviera mal; sin ella un test de paridad se cumple solo por comparar algo consigo mismo):
//
//   1. `oceanWaveVelocity` es la DERIVADA TEMPORAL EXACTA de la superficie, no un modelo aparte.
//      Se mide contra diferencias finitas de `oceanWaveHeight`, que suma senos mientras la velocidad
//      suma cosenos: son dos caminos de código distintos, así que coincidir significa algo.
//   2. TRANSPORTE DE STOKES: donde la superficie está ALTA el agua avanza en el sentido de la ola.
//      Es la propiedad que hace que un objeto llegue a la orilla en vez de solo mecerse.
//   3. El acople real en el motor: un cuerpo soltado en agua con corriente FLOTA y ES ARRASTRADO.
//      Contraprueba: con agua quieta el mismo cuerpo no se desplaza.
// ================================================================================================
#include "test_common.h"
#include "core/planet/terrain_detail.h"   // equirectUV / sampleHeightField / terrainDetail
#include "core/planet/terrain_lod.h"      // terrainTriM

#include <cmath>
#include <memory>
#include <vector>
#include <glm/glm.hpp>

#include "core/planet/ocean_wave.h"
#include "core/planet/water_fill.h"
#include "physics/physics_engine.h"

using Haruka::Planet::oceanWaveHeight;
using Haruka::Planet::oceanWaveVelocity;
using Haruka::Planet::oceanShoalAmp;

namespace {

// Un punto de mar abierto sobre un planeta de radio terrestre, y su marco local.
constexpr double kR = 6371000.0;
glm::vec3 upAt(double lonRad, double latRad) {
    return glm::vec3(std::cos(latRad) * std::cos(lonRad),
                     std::sin(latRad),
                     std::cos(latRad) * std::sin(lonRad));
}

} // namespace

// ------------------------------------------------------- TEST 1: velocidad == d(superficie)/dt
void test_ocean_wave_velocity() {
    beginTest("ocean_wave_velocity");

    const float depth = 40.0f;      // aguas profundas: las 4 olas activas, sin recorte de rompiente
    const float dt    = 1.0e-3f;

    double worstAbs = 0.0, worstRel = 0.0, magSum = 0.0;
    int    samples  = 0;

    // Se barren puntos y tiempos: una sola muestra podría acertar por casualidad de fase.
    for (int i = 0; i < 40; ++i) {
        const double lon = 0.11 * i, lat = 0.07 * i - 1.0;
        const glm::vec3 up = upAt(lon, lat);
        const glm::vec3 wp = up * (float)kR;
        for (int j = 0; j < 5; ++j) {
            const float t = 3.7f * j + 0.9f;
            // Diferencia finita CENTRADA de la cota (error O(dt²)).
            const float hPlus  = oceanWaveHeight(wp, up, t + dt, depth);
            const float hMinus = oceanWaveHeight(wp, up, t - dt, depth);
            const double fd    = (double)(hPlus - hMinus) / (2.0 * dt);
            // La componente VERTICAL de la velocidad analítica.
            const glm::vec3 v  = oceanWaveVelocity(wp, up, t, depth);
            const double  vUp  = glm::dot(v, up);

            const double err = std::abs(fd - vUp);
            worstAbs = std::max(worstAbs, err);
            magSum  += std::abs(fd);
            ++samples;
            if (std::abs(fd) > 0.05) worstRel = std::max(worstRel, err / std::abs(fd));
        }
    }
    const double meanMag = magSum / samples;
    std::printf("    d(cota)/dt vs componente vertical de la velocidad (%d muestras):\n", samples);
    std::printf("      error peor %.3e m/s · relativo peor %.3e · velocidad tipica %.3f m/s\n",
                worstAbs, worstRel, meanMag);

    // El umbral lo pone la diferencia finita, no el código: con dt=1e-3 y float, ~1e-3 m/s es el
    // suelo de ruido. Si la velocidad fuera un modelo distinto (y no la derivada), el error sería
    // del orden de la propia velocidad (~1 m/s), o sea mil veces mayor.
    CHECK(worstAbs < 5.0e-3, "la velocidad vertical es la derivada de la cota");
    CHECK(meanMag > 0.1, "hay ola de verdad que derivar (si no, el test seria vacio)");

    // CONTRAPRUEBA: comparar contra la derivada en OTRO instante tiene que salir MUCHÍSIMO peor. Sin
    // esto el test pasaría igual con una velocidad constante, o nula, o con la de otro sitio.
    //
    // El umbral es RELATIVO al acuerdo real y no un número mágico: lo que demuestra que la prueba
    // tiene dientes es la SEPARACIÓN entre las dos comparaciones, no el valor absoluto de la mala.
    double worstShift = 0.0;
    for (int i = 0; i < 40; ++i) {
        const glm::vec3 up = upAt(0.11 * i, 0.07 * i - 1.0);
        const glm::vec3 wp = up * (float)kR;
        const float t = 2.0f;
        const double fdElsewhere = (double)(oceanWaveHeight(wp, up, t + 1.0f + dt, depth)
                                          - oceanWaveHeight(wp, up, t + 1.0f - dt, depth)) / (2.0 * dt);
        const double vUpHere = glm::dot(oceanWaveVelocity(wp, up, t, depth), up);
        worstShift = std::max(worstShift, std::abs(fdElsewhere - vUpHere));
    }
    std::printf("      CONTRAPRUEBA (derivada 1 s despues): error peor %.3f m/s = %.0fx el acuerdo real\n",
                worstShift, worstShift / worstAbs);
    CHECK(worstShift > worstAbs * 50.0, "CONTRAPRUEBA: desfasar 1 s rompe la igualdad de largo");

    // ── CON PENDIENTE: EL CASO QUE ESTE TEST NO MIRABA, Y QUE ESCONDIA UN BUG ────────────────────
    //
    // ⚠️ TODO LO DE ARRIBA USA LAS SOBRECARGAS SIN PENDIENTE, donde `oceanRefract` devuelve `d0`
    // EXACTO. O sea que la refraccion no se ejercitaba, y ahi habia un fallo real: `oceanWaveVelocity`
    // aceptaba `upSlope` en la firma y **no lo usaba nunca**. La ALTURA giraba al perder fondo y la
    // VELOCIDAD no, asi que en una playa el agua empujaba en la direccion del mar abierto. El llamante
    // de verdad si la pasa (`PlanetarySystem::sampleWaterVelocity` calcula `slopeV` y la entrega).
    //
    // Es el patron de `measure-dont-assert`: un parametro que ninguna prueba toca es un parametro que
    // puede estar ignorado. La derivada sigue siendo el oraculo; lo unico que cambia es que ahora hay
    // pendiente, o sea que la ola de verdad refracta.
    {
        const Haruka::Planet::OceanState st = Haruka::Planet::oceanDefaultState();
        const float shallow = 3.0f;                  // bajio: aqui `oceanRefract` gira de verdad
        double worstSlope = 0.0, magSlope = 0.0; int nS = 0;
        for (int i = 0; i < 40; ++i) {
            const glm::vec3 up = upAt(0.11 * i, 0.07 * i - 1.0);
            const glm::vec3 wp = up * (float)kR;
            // Pendiente del fondo en el plano tangente: una playa que sube hacia `t1`.
            const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0,1,0))
                                                                       : glm::cross(up, glm::vec3(1,0,0)));
            const glm::vec3 slope = t1 * 0.05f;
            for (int j = 0; j < 5; ++j) {
                const float t = 3.7f * j + 0.9f;
                const double fd = (double)(oceanWaveHeight(wp, up, t + dt, shallow, 1.0f, st,
                                               Haruka::Planet::WATER_FETCH_UNLIMITED, slope)
                                         - oceanWaveHeight(wp, up, t - dt, shallow, 1.0f, st,
                                               Haruka::Planet::WATER_FETCH_UNLIMITED, slope)) / (2.0 * dt);
                const glm::vec3 v = oceanWaveVelocity(wp, up, t, shallow, 1.0f, st,
                                               Haruka::Planet::WATER_FETCH_UNLIMITED, slope);
                worstSlope = std::max(worstSlope, std::abs(fd - glm::dot(v, up)));
                magSlope += std::abs(fd); ++nS;
            }
        }
        std::printf("    CON PENDIENTE (bajio 3 m, refraccion activa): error peor %.3e m/s · tipica %.3f m/s\n",
                    worstSlope, magSlope / nS);
        CHECK(worstSlope < 5.0e-3, "con pendiente la velocidad SIGUE siendo la derivada de la cota");
        CHECK(magSlope / nS > 0.1, "hay ola que derivar tambien en el bajio");

        // CONTRAPRUEBA CON DIENTES: la velocidad SIN pasarle la pendiente (que es lo que hacia el
        // codigo con el bug) tiene que discrepar de la altura CON pendiente. Si no discrepara, seria
        // que la refraccion no cambia nada y este bloque no probaria nada.
        double worstBug = 0.0;
        for (int i = 0; i < 40; ++i) {
            const glm::vec3 up = upAt(0.11 * i, 0.07 * i - 1.0);
            const glm::vec3 wp = up * (float)kR;
            const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0,1,0))
                                                                       : glm::cross(up, glm::vec3(1,0,0)));
            const glm::vec3 slope = t1 * 0.05f;
            const float t = 2.0f;
            const double fd = (double)(oceanWaveHeight(wp, up, t + dt, shallow, 1.0f, st,
                                           Haruka::Planet::WATER_FETCH_UNLIMITED, slope)
                                     - oceanWaveHeight(wp, up, t - dt, shallow, 1.0f, st,
                                           Haruka::Planet::WATER_FETCH_UNLIMITED, slope)) / (2.0 * dt);
            // SIN pendiente: exactamente el bug que habia.
            const glm::vec3 vBug = oceanWaveVelocity(wp, up, t, shallow, 1.0f, st);
            worstBug = std::max(worstBug, std::abs(fd - glm::dot(vBug, up)));
        }
        std::printf("      CONTRAPRUEBA (velocidad SIN la pendiente = el bug): error peor %.3f m/s = %.0fx\n",
                    worstBug, worstBug / std::max(worstSlope, 1e-9));
        CHECK(worstBug > worstSlope * 50.0,
              "CONTRAPRUEBA: ignorar la pendiente rompe la igualdad (era el bug, y ninguna prueba lo veia)");
    }
}

// ------------------------------------- TEST 1b: el muro de precisión del float, medido y acotado
void test_ocean_float_precision() {
    beginTest("ocean_float_precision");

    // LA FASE SE CALCULA EN FLOAT SOBRE `wp` RELATIVO AL CENTRO DEL PLANETA (~6,37e6 m), y eso tiene
    // un precio que conviene tener escrito en vez de descubrirlo mirando el mar. A ese módulo el ULP
    // de un float es 0,5 m, así que la fase de cada tren llega cuantizada.
    std::printf("    ULP de la fase por tren, a radio terrestre (%.3e m):\n", kR);
    double worstCrestM = 0.0;
    for (int i = 0; i < Haruka::Planet::OCEAN_WAVES; ++i) {
        const float lambda = Haruka::Planet::OCEAN_WAVE[i][0];
        const float k  = 6.2831853f / lambda;
        const float ph = k * (float)kR;
        const float ulp = std::nextafterf(ph, 1e30f) - ph;
        const double crestM = (double)ulp / 6.2831853 * lambda;   // desplazamiento de cresta (m)
        worstCrestM = std::max(worstCrestM, crestM);
        std::printf("      lambda %5.1f m: ULP %.4f rad = %.2f %% del ciclo = %.3f m de cresta\n",
                    lambda, ulp, 100.0 * ulp / 6.2831853, crestM);
    }
    // Es un desplazamiento SUB-MÉTRICO de dónde cae la cresta. Si algún día pasa del metro que exige
    // el CHECK, la ola empezaría a engancharse a una rejilla visible y habría que restar un origen
    // local ANTES de multiplicar por k — en los DOS lados (shader y gemelo), o se rompe la paridad.
    CHECK(worstCrestM < 1.0, "la cuantizacion de fase desplaza la cresta menos de 1 m");

    // Y LO QUE DE VERDAD IMPORTA: que la ola SE MUEVA. La cuantización es espacial; si además comiera
    // el término temporal (`- w·t`, que junto a una fase de 4,6e6 rad son unos pocos ULP), el mar
    // quedaría congelado a trompicones. Se mide el recorrido pico-valle en 10 s.
    const float depth = 40.0f;
    double worstRange = 1e9, bestRange = 0.0;
    for (int p = 0; p < 6; ++p) {
        const glm::vec3 up = upAt(0.37 * p, 0.21 * p - 0.5);
        const glm::vec3 wp = up * (float)kR;
        float lo = 1e9f, hi = -1e9f;
        for (int s = 0; s <= 1000; ++s) {
            const float h = oceanWaveHeight(wp, up, (float)s * 0.01f, depth);
            lo = std::min(lo, h); hi = std::max(hi, h);
        }
        worstRange = std::min(worstRange, (double)(hi - lo));
        bestRange  = std::max(bestRange,  (double)(hi - lo));
    }
    std::printf("      recorrido de la cota en 10 s: entre %.3f y %.3f m (6 puntos)\n",
                worstRange, bestRange);
    // La suma de las 4 amplitudes con bajío a 40 m ronda 1,3 m, o sea ~2,6 m pico-valle.
    CHECK(worstRange > 2.0, "la ola SE MUEVE: el termino temporal sobrevive al float");
    CHECK(bestRange < 3.2, "y no se dispara (la amplitud sigue siendo la del tren declarado)");
}

// --------------------------------------------- TEST 2: transporte de Stokes (cresta = agua adelante)
void test_ocean_stokes_transport() {
    beginTest("ocean_stokes_transport");

    const float depth = 40.0f;
    const float t     = 5.0f;
    // Dirección del tren DOMINANTE (61 m, amplitud 0.85 — casi el doble que el siguiente), en el
    // marco tangente del punto. Es la que decide el transporte neto.
    const glm::vec3 up = upAt(0.4, 0.2);
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0, 1, 0))
                                                               : glm::cross(up, glm::vec3(1, 0, 0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    const glm::vec3 D0 = glm::normalize(t1 * Haruka::Planet::OCEAN_WAVE[0][2]
                                      + t2 * Haruka::Planet::OCEAN_WAVE[0][3]);

    // Se recorre una LÍNEA a lo largo de la propagación, cubriendo varias longitudes de onda, y se
    // correlaciona la cota con la velocidad a lo largo de D0.
    double sh = 0, sv = 0, shh = 0, svv = 0, shv = 0;   // acumuladores de correlación
    double sVertV = 0, shVert = 0, sVertVV = 0;         // ídem para la contraprueba vertical
    const int N = 400;
    for (int i = 0; i < N; ++i) {
        const float s  = (float)i * 0.75f;             // metros a lo largo de D0
        const glm::vec3 wp = up * (float)kR + D0 * s;
        const float h  = oceanWaveHeight(wp, up, t, depth);
        const glm::vec3 v = oceanWaveVelocity(wp, up, t, depth);
        const double vD = glm::dot(v, D0);             // avance en el sentido de la ola
        const double vU = glm::dot(v, up);             // subida/bajada
        sh += h; sv += vD; shh += (double)h * h; svv += vD * vD; shv += (double)h * vD;
        shVert += (double)h * vU; sVertV += vU; sVertVV += vU * vU;
    }
    auto corr = [&](double sx, double sy, double sxx, double syy, double sxy) {
        const double n = N;
        const double cov = sxy / n - (sx / n) * (sy / n);
        const double vx  = sxx / n - (sx / n) * (sx / n);
        const double vy  = syy / n - (sy / n) * (sy / n);
        return (vx > 1e-12 && vy > 1e-12) ? cov / std::sqrt(vx * vy) : 0.0;
    };
    const double rHorz = corr(sh, sv,     shh, svv,     shv);
    const double rVert = corr(sh, sVertV, shh, sVertVV, shVert);

    std::printf("    correlacion cota <-> velocidad, %d puntos sobre la direccion de avance:\n", N);
    std::printf("      HORIZONTAL (a lo largo de D0): r = %+.3f   <- el transporte\n", rHorz);
    std::printf("      VERTICAL   (a lo largo de up): r = %+.3f   <- debe ser ~0 (90 deg de desfase)\n", rVert);

    // En la cresta el agua va HACIA DELANTE: cota y velocidad de avance en fase. Es la firma de
    // Stokes y es, literalmente, lo que empuja un objeto flotante hacia la playa.
    CHECK(rHorz > 0.85, "en la cresta el agua avanza en el sentido de la ola");
    // CONTRAPRUEBA sobre LOS MISMOS datos: la componente vertical va en cuadratura (h~sin, v_up~-cos),
    // así que su correlación tiene que ser ~0. Si saliera alta, el marco estaría mal construido y el
    // "transporte" de arriba sería un artefacto de proyección, no física.
    CHECK(std::abs(rVert) < 0.15, "CONTRAPRUEBA: la componente vertical va en cuadratura (r ~ 0)");
}

// ----------------------------------------------- TEST 3: bajío y rompiente (la costa, en números)
void test_ocean_shoaling() {
    beginTest("ocean_shoaling");

    const float ampDeep = Haruka::Planet::OCEAN_WAVE[0][1];   // 0.85 m, el swell que da la silueta

    // Ley de Green: al subir el fondo la ola CRECE. 50 m -> 3 m debe engordarla claramente.
    const float aDeep  = oceanShoalAmp(50.0f, ampDeep);
    const float aShelf = oceanShoalAmp(10.0f, ampDeep);
    const float a3     = oceanShoalAmp(3.0f,  ampDeep);
    std::printf("    amplitud del swell de %.2f m por profundidad:\n", ampDeep);
    std::printf("      50 m -> %.3f m · 10 m -> %.3f m · 3 m -> %.3f m\n", aDeep, aShelf, a3);
    CHECK(aShelf > aDeep, "la ola CRECE al entrar en agua somera (ley de Green)");

    // Límite de rompiente: pase lo que pase, la amplitud nunca supera 0.55·profundidad. Es lo que
    // impide que la orilla sea un pico absurdo — pasado el límite la ola no crece, ROMPE.
    bool capOk = true;
    float worstRatio = 0.0f;
    for (float d = 0.05f; d < 60.0f; d += 0.05f) {
        const float a = oceanShoalAmp(d, ampDeep);
        worstRatio = std::max(worstRatio, a / d);
        if (a > 0.55f * d + 1e-5f) capOk = false;
    }
    std::printf("      razon amplitud/profundidad peor: %.3f (tope de rompiente 0.550)\n", worstRatio);
    CHECK(capOk, "la amplitud nunca supera 0.55x la profundidad (limite de rompiente)");

    // Y LO QUE HACE QUE LA COSTA FUNCIONE: con el fondo a cero la ola se anula, así que la superficie
    // del agua se junta con el suelo EXACTAMENTE en la línea de costa. Sin esto la orilla sería una
    // frontera entre dos superficies que hay que mantener de acuerdo.
    std::printf("      en la orilla (d=0.01 m): amplitud %.4f m\n", oceanShoalAmp(0.01f, ampDeep));
    CHECK(oceanShoalAmp(0.01f, ampDeep) < 0.01f, "la ola se anula en la linea de costa");

    // CONTRAPRUEBA: sin el tope, la ley de Green sola daría una amplitud ABSURDA en 10 cm de agua.
    // Es la cifra que justifica que el tope exista.
    const float greenOnly = ampDeep * std::pow(std::min(50.0f / 0.1f, 40.0f), 0.25f);
    std::printf("      CONTRAPRUEBA sin tope, en 0.1 m de agua: %.3f m (%.0fx la profundidad)\n",
                greenOnly, greenOnly / 0.1f);
    CHECK(greenOnly > 1.0f, "CONTRAPRUEBA: sin el tope la ola en la orilla seria absurda");
}

// --------------------------------- TEST 4: el acople REAL — flotar y ser arrastrado por la corriente
namespace {
    // Mundo de juguete con MAR: plano, con una corriente horizontal constante. La corriente constante
    // (en vez de la ola de verdad) es deliberada: aísla lo que se quiere medir —que el arrastre
    // transporta— de la oscilación, que promediaría el desplazamiento a casi cero en pocos ciclos.
    struct FakeOcean : Haruka::Physics::IWorldProvider {
        glm::dvec3 center{0, 0, 0};
        double     radius  = 1000.0;
        double     terrain = -50.0;      // fondo 50 m BAJO el nivel del mar → hay columna de agua
        double     waterH  = 0.0;        // superficie del agua, sobre la esfera de referencia
        glm::dvec3 current{0, 0, 0};     // velocidad del agua (m/s)
        float      foam = 0.0f;          // 0 = agua limpia · 1 = agua blanca (rompiente)
        std::vector<Haruka::Physics::GravBody> grav;
        FakeOcean() {
            const double G = 6.67430e-11, g = 9.81;
            grav.push_back({ center, g * radius * radius / G });
        }
        bool       hasActivePlanet()    const override { return true; }
        glm::dvec3 activePlanetCenter() const override { return center; }
        double     activePlanetRadius() const override { return radius; }
        const std::vector<Haruka::Physics::GravBody>& gravBodies() const override { return grav; }
        double     terrainHeightAt(const glm::dvec3&) const override { return terrain; }
        double     waterSurfaceAt(const glm::dvec3&) const override { return waterH; }
        glm::dvec3 waterVelocityAt(const glm::dvec3&) const override { return current; }
        float      waterFoamAt(const glm::dvec3&) const override { return foam; }
    };

    // Suelta un cuerpo sobre el agua y simula. Devuelve el radio final y la deriva TANGENCIAL.
    void floatSim(const glm::dvec3& current, double seconds,
                  double& outDist, double& outDrift, double& outTangSpeed,
                  float foam = 0.0f) {
        Haruka::Physics::PhysicsEngine eng;
        FakeOcean w; w.current = current; w.foam = foam;
        eng.setWorldProvider(&w);
        auto b = std::make_shared<Haruka::Physics::RigidBody>();
        b->position = glm::dvec3(w.radius + 2.0, 0, 0);   // 2 m sobre el agua, en +X
        b->radius = 0.5; b->mass = 70.0; b->name = "boya";
        eng.addBody(b);
        for (double s = 0.0; s < seconds; s += 1.0 / 60.0) eng.advance(1.0 / 60.0);
        outDist  = glm::length(b->position - w.center);
        // Deriva tangencial: cuánto se ha movido en el plano perpendicular al radial inicial.
        const glm::dvec3 radial0(1, 0, 0);
        const glm::dvec3 rel = b->position - w.center;
        outDrift = glm::length(rel - radial0 * glm::dot(rel, radial0));
        const glm::dvec3 v = b->velocity;
        outTangSpeed = glm::length(v - radial0 * glm::dot(v, radial0));
    }
}

void test_ocean_buoyancy_drift() {
    beginTest("ocean_buoyancy_drift");

    const double seaR = 1000.0;   // radio + waterH(0)
    double dist = 0, drift = 0, tang = 0;

    // (a) AGUA QUIETA: el cuerpo tiene que quedarse FLOTANDO en la superficie y NO derivar.
    floatSim(glm::dvec3(0, 0, 0), 20.0, dist, drift, tang);
    std::printf("    agua quieta tras 20 s: radio %.3f m (superficie %.1f) · deriva %.4f m\n",
                dist, seaR, drift);
    CHECK(std::abs(dist - seaR) < 1.0, "con agua quieta el cuerpo se asienta EN la superficie");
    CHECK(drift < 0.05, "con agua quieta NO hay deriva lateral");

    // (b) MISMO cuerpo, MISMA superficie, con corriente de 2 m/s: tiene que ser ARRASTRADO.
    const glm::dvec3 cur(0, 0, 2.0);      // tangencial en el punto de suelta (+X)
    double dist2 = 0, drift2 = 0, tang2 = 0;
    floatSim(cur, 20.0, dist2, drift2, tang2);
    std::printf("    corriente de %.1f m/s tras 20 s: radio %.3f m · deriva %.2f m · v tangencial %.2f m/s\n",
                glm::length(cur), dist2, drift2, tang2);
    CHECK(std::abs(dist2 - seaR) < 1.5, "con corriente sigue FLOTANDO (el empuje no se pierde)");
    CHECK(drift2 > 5.0, "la corriente ARRASTRA el cuerpo (esto no pasaba antes del cambio)");
    // El arrastre lleva la velocidad HACIA la del agua, no más allá: sin este tope el acople estaría
    // inyectando energía.
    CHECK(tang2 <= glm::length(cur) + 0.2, "la velocidad tiende a la del agua, no la supera");

    // CONTRAPRUEBA explícita: la única diferencia entre (a) y (b) es `waterVelocityAt`. Si el
    // arrastre siguiera amortiguando hacia el marco del mundo (el código anterior), las dos deriva
    // serían iguales y esta comprobación fallaría.
    std::printf("    CONTRAPRUEBA: deriva quieta %.4f m vs con corriente %.2f m (x%.0f)\n",
                drift, drift2, drift2 / std::max(drift, 1e-3));
    CHECK(drift2 > drift * 50.0, "CONTRAPRUEBA: la deriva viene de la corriente y de nada mas");
}

// ------------------------------------- TEST: el AGUA BLANCA arrastra (la rompiente para la fisica)
//
// La espuma existia en el shader y en `oceanFoam` desde el principio, y NADIE la consultaba: un
// cuerpo en plena rompiente notaba exactamente lo mismo que en un mar en calma. La ola reventaba solo
// para la camara. Aqui se fija el acople, y lo que importa son las DOS mitades:
//  · con espuma 0 el comportamiento tiene que ser el de siempre, BIT A BIT (el mar abierto no cambia);
//  · con espuma 1 el cuerpo tiene que ser llevado mucho mas deprisa, y SEGUIR FLOTANDO.
void test_ocean_whitewater_drag() {
    beginTest("ocean_whitewater_drag");

    const double seaR = 1000.0;
    const glm::dvec3 cur(0, 0, 2.0);          // la misma corriente en los tres casos

    double d0 = 0, drift0 = 0, tang0 = 0;     // agua limpia
    double d1 = 0, drift1 = 0, tang1 = 0;     // agua blanca
    floatSim(cur, 6.0, d0, drift0, tang0, 0.0f);
    floatSim(cur, 6.0, d1, drift1, tang1, 1.0f);

    std::printf("    tras 6 s con corriente de %.1f m/s:\n", glm::length(cur));
    std::printf("      agua limpia (espuma 0): deriva %.3f m · v %.3f m/s · radio %.2f m\n",
                drift0, tang0, d0);
    std::printf("      agua BLANCA (espuma 1): deriva %.3f m · v %.3f m/s · radio %.2f m\n",
                drift1, tang1, d1);

    // 1. EL EMPUJE BAJA, Y ES EL EFECTO GRANDE. El aire mezclado quita un 25 % (`1,1·0,75 = 0,825`),
    //    asi que el cuerpo se asienta MAS HONDO en la rompiente — que es lo que hace un cuerpo en
    //    agua blanca de verdad.
    std::printf("    calado: el cuerpo flota %.2f m mas BAJO en agua blanca\n", d0 - d1);
    CHECK(d0 - d1 > 0.5, "en el agua blanca el cuerpo se hunde mas (agua aireada = menos empuje)");
    //    Pero SIGUE a flote: si alguien subiera ese 0,25 por encima de 1/1,1 = 0,909, esto lo caza.
    CHECK(std::abs(d1 - seaR) < 2.0, "en el agua blanca el cuerpo SIGUE a flote (no se hunde del todo)");

    // 2. EL ARRASTRE: SOLO SE NOTA EN EL TRANSITORIO, y hay que medirlo donde ocurre. Con tasa 3/s la
    //    constante de tiempo es 1/3 s, asi que a los 6 s los dos casos van ya al 98 % de la velocidad
    //    del agua y la diferencia se ha borrado (9,96 contra 10,49 m: un 5 %). Lo que la tasa cambia
    //    es la RAPIDEZ con que te agarra, y eso se ve en el primer medio segundo — que es justo la
    //    escala a la que pasa una rompiente por encima.
    //    ⚠️ Y hay que esperar a que ENTRE: el cuerpo se suelta 2 m sobre el agua, o sea 0,64 s de
    //    caida libre. Midiendo a 0,4 s los dos daban 0,000 m/s — no habia agua todavia, no habia
    //    efecto que medir.
    double dA = 0, driftA = 0, tangA = 0, dB = 0, driftB = 0, tangB = 0;
    floatSim(cur, 1.0, dA, driftA, tangA, 0.0f);
    floatSim(cur, 1.0, dB, driftB, tangB, 1.0f);
    std::printf("    transitorio a 1,0 s (0,36 s dentro del agua): v limpia %.3f · v blanca %.3f m/s (x%.2f)\n",
                tangA, tangB, tangB / std::max(tangA, 1e-6));
    // Medido: x1,46 a los 0,36 s dentro del agua. El umbral va en 1,3 con margen para el paso de
    // integracion; lo que se afirma es "agarra apreciablemente antes", y 1,46 lo sostiene.
    CHECK(tangB > tangA * 1.3, "el agua blanca AGARRA mucho antes (el transitorio es el efecto)");

    // 3. CONTRAPRUEBA, y es la mitad que de verdad protege: con espuma 0 el resultado tiene que ser
    //    EXACTAMENTE el de antes del acople. Si el termino nuevo se colara con espuma nula, el mar
    //    abierto entero habria cambiado sin que nada lo dijera.
    double d0b = 0, drift0b = 0, tang0b = 0;
    floatSim(cur, 6.0, d0b, drift0b, tang0b);        // la sobrecarga SIN el parametro de espuma
    std::printf("    CONTRAPRUEBA: espuma 0 explicita vs sin parametro -> deriva %.9f vs %.9f\n",
                drift0, drift0b);
    CHECK(drift0 == drift0b && tang0 == tang0b && d0 == d0b,
          "con espuma 0 el acople es INERTE (bit a bit): el mar abierto no cambia");
}

// ------------------------------------------- TEST 5: sin agua, la flotación NO se activa (regresión)
void test_ocean_no_water_no_float() {
    beginTest("ocean_no_water_no_float");

    // Proveedor SIN agua (el default de IWorldProvider: `kNoWater`). Es el caso del servidor y el de
    // cualquier punto en tierra. El cuerpo tiene que CAER hasta el terreno, no quedarse flotando.
    struct DryWorld : FakeOcean {
        double waterSurfaceAt(const glm::dvec3&) const override { return kNoWater; }
    };
    Haruka::Physics::PhysicsEngine eng;
    DryWorld w; w.terrain = -50.0;
    eng.setWorldProvider(&w);
    auto b = std::make_shared<Haruka::Physics::RigidBody>();
    b->position = glm::dvec3(w.radius + 2.0, 0, 0);
    b->radius = 0.5; b->mass = 70.0; b->name = "piedra";
    eng.addBody(b);
    for (double s = 0.0; s < 20.0; s += 1.0 / 60.0) eng.advance(1.0 / 60.0);

    const double dist   = glm::length(b->position - w.center);
    const double floorR = w.radius + w.terrain + b->radius;   // donde para el ground-snap
    std::printf("    sin agua tras 20 s: radio %.3f m · suelo en %.3f m (agua estaria en %.1f)\n",
                dist, floorR, w.radius);
    // Cae los 50 m hasta el fondo en vez de quedarse en la cota del mar: la flotación se apagó de
    // verdad, no se quedó activa con un nivel de 0 disfrazado.
    CHECK(std::abs(dist - floorR) < 1.0, "sin agua el cuerpo cae al TERRENO");
    CHECK(dist < w.radius - 10.0, "sin agua NO se queda flotando a la cota del mar");
}

// ------------------------------- TEST 6: el estado del mar por viento (Pierson-Moskowitz + rumbo)
void test_ocean_sea_state() {
    beginTest("ocean_sea_state");
    using Haruka::Planet::oceanStateFromWind;
    using Haruka::Planet::OCEAN_REF_WIND;
    using Haruka::Planet::OCEAN_WAVES;

    // Altura significativa del estado: Hs = 4·sigma, con sigma² = suma(a²)/2.
    auto hsOf = [](const Haruka::Planet::OceanState& s) {
        double var = 0.0;
        for (int i = 0; i < OCEAN_WAVES; ++i) var += 0.5 * (double)s.wave[i][1] * s.wave[i][1];
        return 4.0 * std::sqrt(var);
    };

    // (a) A VIENTO DE REFERENCIA TIENE QUE SALIR LA TABLA DE SIEMPRE. Es el ancla de todo el cambio:
    // parametrizar el oleaje no puede alterar el mar ya afinado.
    const auto ref = oceanStateFromWind(OCEAN_REF_WIND, 1.0f, 0.0f, 0.0f);
    double worstDelta = 0.0;
    for (int i = 0; i < OCEAN_WAVES; ++i)
        for (int c = 0; c < 4; ++c)
            worstDelta = std::max(worstDelta,
                                  std::abs((double)ref.wave[i][c] - Haruka::Planet::OCEAN_WAVE[i][c]));
    std::printf("    a viento de referencia (%.1f m/s) vs la tabla original: dif. peor %.2e\n",
                OCEAN_REF_WIND, worstDelta);
    CHECK(worstDelta < 1e-4, "a viento de referencia sale EXACTAMENTE el mar de siempre");

    // (b) PIERSON-MOSKOWITZ: Hs y lambda van con U². Doblar el viento cuadruplica los dos.
    const auto s6  = oceanStateFromWind(6.0f,  1.0f, 0.0f, 0.0f);
    const auto s12 = oceanStateFromWind(12.0f, 1.0f, 0.0f, 0.0f);
    const double hs6 = hsOf(s6), hs12 = hsOf(s12);
    const double lam6 = s6.wave[0][0], lam12 = s12.wave[0][0];
    std::printf("    U=6 m/s  -> Hs %.2f m · swell %.1f m\n", hs6, lam6);
    std::printf("    U=12 m/s -> Hs %.2f m · swell %.1f m   (razones Hs %.2f · lambda %.2f, se espera 4.00)\n",
                hs12, lam12, hs12 / hs6, lam12 / lam6);
    CHECK(std::abs(hs12 / hs6 - 4.0) < 0.05, "Hs va con U^2 (Pierson-Moskowitz)");
    CHECK(std::abs(lam12 / lam6 - 4.0) < 0.05, "la longitud de onda va con U^2");
    // Y la cifra absoluta cae donde manda el manual: Hs = 0,21·U²/g.
    const double hsBook = 0.21 * 12.0 * 12.0 / 9.81;
    std::printf("    contraste con el manual a 12 m/s: Hs modelo %.2f m vs 0.21U^2/g = %.2f m\n",
                hs12, hsBook);
    CHECK(std::abs(hs12 - hsBook) / hsBook < 0.25, "Hs cae donde manda Pierson-Moskowitz");

    // (c) EL TOPE DE NYQUIST: por flojo que sople, ningún tren baja del corto de referencia (8,7 m),
    // que es lo que `ocean.tesc` puede teselar a 4 m por segmento. Si bajara, esa ola sería muare.
    const auto calm = oceanStateFromWind(0.0f, 1.0f, 0.0f, 0.0f);
    float shortest = 1e9f;
    for (int i = 0; i < OCEAN_WAVES; ++i) shortest = std::min(shortest, calm.wave[i][0]);
    std::printf("    en calma: tren mas corto %.2f m (piso de teselacion 8.70 m)\n", shortest);
    CHECK(shortest >= 8.7f - 1e-3f, "ningun tren baja del piso de Nyquist de la teselacion");

    // (d) EL RUMBO: girar el viento 90 grados gira el tren principal 90 grados, sin cambiar su
    // longitud ni su amplitud (es un giro, no otro mar).
    const auto east  = oceanStateFromWind(10.0f, 1.0f, 0.0f, 0.0f);
    const auto north = oceanStateFromWind(10.0f, 0.0f, 1.0f, 0.0f);
    const double dot = (double)east.wave[0][2] * north.wave[0][2]
                     + (double)east.wave[0][3] * north.wave[0][3];
    std::printf("    rumbo: dir principal E vs N -> producto escalar %.4f (perpendicular = 0)\n", dot);
    CHECK(std::abs(dot) < 1e-3, "el tren principal sigue al viento");
    CHECK(std::abs(east.wave[0][0] - north.wave[0][0]) < 1e-4, "girar el viento no cambia lambda");

    // CONTRAPRUEBA: si el viento NO influyera (la tabla vieja fija), (b) y (d) darían razon 1 y
    // producto escalar 1. Se comprueba que el estado de referencia sí es distinto del de 6 m/s.
    std::printf("    CONTRAPRUEBA: Hs(6) %.2f m vs Hs(ref) %.2f m -> razon %.2f (fija seria 1.00)\n",
                hs6, hsOf(ref), hsOf(ref) / hs6);
    CHECK(hsOf(ref) / hs6 > 1.5, "CONTRAPRUEBA: el viento cambia el mar de verdad");
}

// -------------------------------------------------- TEST 7: la marea tiene DOS abultamientos (P2)
void test_ocean_tide() {
    beginTest("ocean_tide");
    using Haruka::Planet::oceanTideAt;

    const double R = 6371000.0;
    // Una luna en +X, a distancia lunar y con el GM de la Luna.
    const glm::dvec3 moon(3.844e8, 0, 0);
    const double gm = 4.9048695e12;

    const double under = oceanTideAt(glm::dvec3( 1, 0, 0), R, &moon, &gm, 1);  // bajo la luna
    const double anti  = oceanTideAt(glm::dvec3(-1, 0, 0), R, &moon, &gm, 1);  // antipodas
    const double side  = oceanTideAt(glm::dvec3( 0, 1, 0), R, &moon, &gm, 1);  // cuadratura
    std::printf("    marea de una luna: bajo ella %+.3f m · antipodas %+.3f m · a 90 grados %+.3f m\n",
                under, anti, side);

    // LAS DOS PLEAMARES. El término P₂ = (3cos²θ − 1)/2 vale +1 en θ=0 Y en θ=180: por eso hay dos
    // mareas altas al día y no una. Que las antípodas suban es la prueba de que esto es el potencial
    // de marea y no "atracción hacia la luna", que daría un solo abultamiento.
    CHECK(under > 0.0, "hay pleamar BAJO la luna");
    CHECK(anti  > 0.0, "y tambien en las ANTIPODAS (la segunda pleamar del dia)");
    CHECK(side  < 0.0, "y bajamar a 90 grados");
    // Los dos abultamientos son casi iguales: la pequeña asimetría es el término de distancia (1/D³
    // evaluado en la superficie, no en el centro), que es real.
    std::printf("    asimetria entre los dos abultamientos: %.2f %%\n",
                100.0 * std::abs(under - anti) / std::max(under, 1e-9));
    CHECK(std::abs(under - anti) / std::max(under, 1e-9) < 0.15, "los dos abultamientos son casi iguales");

    // CONTRAPRUEBA: sin cuerpos no hay marea. Si devolviera algo, el mar tendría un nivel inventado.
    const double none = oceanTideAt(glm::dvec3(1, 0, 0), R, nullptr, nullptr, 0);
    std::printf("    CONTRAPRUEBA: sin cuerpos masivos la marea es %.3f m\n", none);
    CHECK(none == 0.0, "CONTRAPRUEBA: sin cuerpos la marea es exactamente 0");
}

// --------------------------------------------------- TEST 8: el SWASH mueve la línea de costa
void test_ocean_swash() {
    beginTest("ocean_swash");
    using Haruka::Planet::oceanSwash;
    const auto st = Haruka::Planet::oceanDefaultState();
    const glm::vec3 up = upAt(0.3, 0.15);
    const glm::vec3 wp = up * (float)kR;

    // (a) EN MAR ABIERTO NO EXISTE. Es la condición que hace el cambio seguro: el oleaje de siempre
    // no se toca, la trepada solo vive en la franja somera.
    float deepMax = 0.0f;
    for (int s = 0; s < 400; ++s)
        deepMax = std::max(deepMax, std::abs(oceanSwash(60.0f, wp, up, (float)s * 0.05f, st)));
    std::printf("    swash a 60 m de fondo: |max| %.4f m (mar abierto: debe ser ~0)\n", deepMax);
    CHECK(deepMax < 0.01f, "en mar abierto el swash no existe");

    // (b) EN LA ORILLA OSCILA, y con amplitud del orden de la ola que rompe. Ése es el movimiento
    // que antes NO había: la banda de espuma era un smoothstep fijo sobre la profundidad.
    float lo = 1e9f, hi = -1e9f;
    for (int s = 0; s < 2000; ++s) {
        const float v = oceanSwash(0.3f, wp, up, (float)s * 0.02f, st);
        lo = std::min(lo, v); hi = std::max(hi, v);
    }
    std::printf("    swash en 0.3 m de fondo: recorrido %.3f m  (de %+.3f a %+.3f)\n", hi - lo, lo, hi);
    CHECK(hi - lo > 0.3f, "en la orilla la lamina SUBE Y BAJA de verdad");
    CHECK(hi > 0.0f && lo < 0.0f, "trepa y se retira (la orilla respira en los dos sentidos)");

    // (c) SOBRE ARENA SECA la trepada sigue siendo POSITIVA en su fase alta — es lo que permite que
    // el agua moje terreno que estaba fuera del mar. ⚠️ Es el caso que rompía la primera versión:
    // `oceanShoalAmp` acota por `0.55·d` y con `d` negativa devolvía amplitud NEGATIVA, o sea que la
    // lámina se hundía justo donde tenía que subir.
    float dryHi = -1e9f;
    for (int s = 0; s < 2000; ++s)
        dryHi = std::max(dryHi, oceanSwash(-0.5f, wp, up, (float)s * 0.02f, st));
    std::printf("    swash sobre arena a -0.5 m: pico %+.3f m (debe ser >0: moja tierra)\n", dryHi);
    CHECK(dryHi > 0.1f, "sobre tierra la trepada es positiva (moja arena seca)");

    // (d) ESCALA CON EL ESTADO DEL MAR — pero NO en el último palmo de agua, y eso es física, no un
    // defecto. En 1 m de fondo el límite de rompiente (`0.55·d`) satura por igual a la brisa y a la
    // galerna: una ola no puede ser más alta que el fondo que la sostiene, venga del temporal que
    // venga. Lo que sí distingue a los dos mares es (1) la trepada MÁS ADENTRO, donde el tope aún no
    // muerde, y (2) hasta dónde llega la zona de rompientes.
    const auto calm  = Haruka::Planet::oceanStateFromWind(5.0f,  1.0f, 0.0f, 0.0f);
    const auto storm = Haruka::Planet::oceanStateFromWind(18.0f, 1.0f, 0.0f, 0.0f);
    auto swingAt = [&](const Haruka::Planet::OceanState& s0, float depth) {
        float a = 1e9f, b = -1e9f;
        for (int s = 0; s < 2000; ++s) {
            const float v = oceanSwash(depth, wp, up, (float)s * 0.02f, s0);
            a = std::min(a, v); b = std::max(b, v);
        }
        return b - a;
    };
    const float sc = swingAt(calm, 3.0f), ss = swingAt(storm, 3.0f);
    std::printf("    a 3 m de fondo: brisa (5 m/s) %.3f m vs galerna (18 m/s) %.3f m -> x%.1f\n",
                sc, ss, ss / std::max(sc, 1e-4f));
    CHECK(ss > sc * 2.0f, "mas adentro, la trepada crece con el estado del mar");

    // El ALCANCE de la zona de rompientes: a qué profundidad el swash ya se ha apagado. Con galerna
    // la rompiente empieza mucho más lejos de la playa, que es lo que se ve en una costa de verdad.
    auto reachOf = [&](const Haruka::Planet::OceanState& s0) {
        float d = 0.5f;
        for (; d < 60.0f; d += 0.25f) if (swingAt(s0, d) < 0.05f) break;
        return d;
    };
    const float rc = reachOf(calm), rs = reachOf(storm);
    std::printf("    alcance de la rompiente: brisa hasta %.1f m de fondo · galerna hasta %.1f m\n", rc, rs);
    CHECK(rs > rc * 1.5f, "con galerna la rompiente empieza mucho mas lejos de la playa");

    // CONTRAPRUEBA: el recorrido en la orilla tiene que ser MUCHO mayor que en mar abierto. Sin esto
    // el test pasaría con un swash constante aplicado en todas partes.
    std::printf("    CONTRAPRUEBA: orilla %.3f m vs mar abierto %.4f m\n", hi - lo, deepMax);
    CHECK((hi - lo) > deepMax * 20.0f, "CONTRAPRUEBA: el swash es un fenomeno de ORILLA");
}

// ============================================================================================
// LA OCTAVA MÁS FINA DEL TERRENO — que no se vuelva a morir en silencio
// ============================================================================================
//
// Vive en test_ocean.cpp por vecindad de sesión, no por tema. Lo que vigila:
//
// La escalera de `terrain_detail.h` acaba en λ = 4,5 m con la guarda `minFeatureM < 2.25`. El piso
// de `triM` de la GEOMETRÍA vale 4 m (= el quad del clipmap), así que durante un tiempo esa octava
// no se activó en NINGÚN punto del planeta: el rasgo más fino que el terreno podía tener era λ = 22 m.
// Nadie lo notó porque nada falla — el terreno simplemente sale liso.
//
// Se murió al bajar el tope de tesela de 64 a 32 por coste. Cualquier futuro ajuste de ese tope, o
// del piso, puede volver a apagarla. Esto lo convierte en un fallo de test en vez de en una textura
// que alguien encuentra rara meses después.
#include "core/planet/terrain_lod.h"

void test_terrain_finest_octave() {
    beginTest("terrain_finest_octave");
    using namespace Haruka::Planet;

    // Gemelo de la escalera de `terrain_detail.h`: la octava mas fina y su amplitud.
    constexpr float kFinestLambda = 4.5f;
    constexpr float kFinestAmp    = 0.7f;
    // Gemelo de `harukaOctaveWeight`: es una RAMPA, no un interruptor.
    auto weight = [](float lambdaM, float triM) {
        const float t = lambdaM / std::max(triM * 2.0f, 1e-3f);
        return std::min(std::max(t - 1.0f, 0.0f), 1.0f);
    };

    const float wPix  = weight(kFinestLambda, (float)TERRAIN_TRIM_FLOOR_PIXEL);
    const float wGeom = weight(kFinestLambda, (float)TERRAIN_TRIM_FLOOR);
    std::printf("    piso de triM: geometria %.3f m · sombreado %.3f m\n",
                TERRAIN_TRIM_FLOOR, TERRAIN_TRIM_FLOOR_PIXEL);
    std::printf("    peso de la octava fina (lambda %.1f m, +-%.2f m):\n", kFinestLambda, kFinestAmp);
    std::printf("      con el piso del SOMBREADO : %.3f  -> aporta +-%.3f m\n", wPix,  wPix  * kFinestAmp);
    std::printf("      con el piso de la GEOMETRIA: %.3f  -> aporta +-%.3f m\n", wGeom, wGeom * kFinestAmp);

    // (a) EL PESO, no solo cruzar la guarda. ⚠️ Este test tuvo primero la aserción floja
    // (`piso < 2.25`), y la habría dado por buena con la octava al **12,5 %** — ±8,75 cm en vez de
    // ±0,7 m. Encender una octava al octavo de su amplitud es casi no encenderla; lo que hay que
    // exigir es que aporte lo que dice aportar.
    CHECK(wPix > 0.99f, "la octava mas fina entra a peso COMPLETO en el sombreado");

    // (b) Y no mas fino de lo necesario: el peso ya esta topado en 1, asi que bajar el piso solo
    // pagaria evaluaciones. `lambda/4` es el valor exacto donde llega a 1.
    CHECK(TERRAIN_TRIM_FLOOR_PIXEL >= (double)kFinestLambda / 4.0 - 1e-6,
          "y no baja de lambda/4 (por debajo se paga sin ganar amplitud)");

    // (c) ⚠️ ESTA CONTRAPRUEBA SE DIO LA VUELTA AL BORRARSE EL CLIPMAP (2026-08-24), y el cambio ES
    // el arreglo, no un efecto colateral.
    //
    // Exigia que con el piso de la GEOMETRIA la octava siguiera MUERTA: con quads de 4 m lo estaba, y
    // por eso hacian falta dos pisos. Ahora la geometria de colision son celdas de 0,5 m, asi que esa
    // octava SI cabe (Nyquist pide < 2,25 m) — y que entre es justo lo que hace que el suelo que se
    // pisa tenga el relieve que se ve. Era la disparidad ver<->pisar de 0,15-0,25 m.
    std::printf("    con el piso de la geometria (%.2f m) el peso es %.3f\n", TERRAIN_TRIM_FLOOR, wGeom);
    CHECK(wGeom > 0.99f, "con la rejilla fina la octava tambien entra en la GEOMETRIA");
    // La contraprueba con dientes pasa a ser esta: con el piso VIEJO (el quad del clipmap) estaba
    // muerta. Sin este numero no se veria lo que se ha arreglado.
    const float wOld = weight(kFinestLambda, (float)TERRAIN_CLIP_QUAD_M);
    std::printf("    CONTRAPRUEBA: con el piso VIEJO (quad del clipmap, %.2f m) el peso era %.3f\n",
                TERRAIN_CLIP_QUAD_M, wOld);
    CHECK(wOld <= 0.0f, "CONTRAPRUEBA: con el quad de 4 m la octava estaba MUERTA (era la disparidad)");
    // (d) El piso de la GEOMETRIA sigue atado a la rejilla que lo consume — misma regla, otra
    // rejilla: antes el quad del clipmap, ahora la celda del anillo de colision mas fino.
    CHECK(TERRAIN_TRIM_FLOOR == TERRAIN_RING_FINE_CELL,
          "el piso de la GEOMETRIA sigue atado a su rejilla (Nyquist, no un literal)");
}

// ------------------------------------------------------- TEST 10: el tope de rompiente es de la OLA
//
// La ola no puede ser mas alta que el agua que la sostiene. Es el indice de rompiente clasico: una
// ola revienta cuando su altura se acerca a la profundidad. Suena obvio y el motor lo incumplia:
// `oceanShoalAmp` acotaba CADA uno de los 4 trenes a `0.55·d` y luego `oceanWaveHeight` los sumaba,
// asi que el total podia llegar a `4 x 0.55·d = 2,2·d`. Medido antes del arreglo: un lago de 3,21 m
// de fondo daba 5,93 m de ola pico a pico, casi el doble que en mar abierto (3,16 m).
//
// Se veia en TODA la costa —el bajio es un fenomeno de agua somera— pero saltaba a la vista en los
// lagos de montana, que son someros enteros. Este test es el guardian: si alguien vuelve a mover el
// tope al bucle, el barrido de profundidades lo caza.
void test_ocean_break_limit() {
    beginTest("ocean_break_limit");

    const glm::vec3 up = upAt(0.4, 0.3);
    const glm::vec3 wp = up * (float)kR;

    // Altura pico a pico barriendo tiempo y espacio: una sola muestra podria caer en un nodo.
    auto peakToPeak = [&](float depth) {
        float lo = 1e9f, hi = -1e9f;
        for (int s = 0; s < 20; ++s) {
            const glm::vec3 p = wp + glm::vec3(1.0f, 0.0f, 0.3f) * (float)(s * 11);
            for (int k = 0; k < 48; ++k) {
                const float h = oceanWaveHeight(p, up, 0.29f * k, depth);
                lo = std::min(lo, h); hi = std::max(hi, h);
            }
        }
        return hi - lo;
    };

    // El tope es sobre la AMPLITUD (0.55·d), asi que la altura pico a pico no puede pasar de 1,1·d.
    const float kMaxRatio = 1.1f;
    std::printf("    altura de ola frente a profundidad (el tope es %.2f x la profundidad):\n", kMaxRatio);

    int worstIdx = -1; float worstRatio = 0.0f;
    const float depths[] = { 0.5f, 1.0f, 2.0f, 3.21f, 5.0f, 8.0f, 15.0f, 30.0f };
    for (int i = 0; i < 8; ++i) {
        const float d  = depths[i];
        const float pp = peakToPeak(d);
        const float r  = pp / d;
        std::printf("      fondo %5.2f m -> ola %5.2f m  (%.2f x la profundidad)%s\n",
                    d, pp, r, r > kMaxRatio ? "   <-- MAS ALTA QUE EL AGUA" : "");
        if (r > worstRatio) { worstRatio = r; worstIdx = i; }
        CHECK(r <= kMaxRatio + 0.02f, "la ola no supera el limite de rompiente a esta profundidad");
    }
    std::printf("    peor caso: %.2f x a %.2f m de fondo (antes del arreglo se llegaba a ~1,85 x)\n",
                worstRatio, worstIdx >= 0 ? depths[worstIdx] : 0.0f);

    // CONTRAPRUEBA 1: en agua HONDA el tope no debe morder, o el test estaria pasando simplemente
    // porque la ola es pequena en todas partes y no porque el limite funcione.
    const float ppDeep = peakToPeak(400.0f);
    const float ppAbyss = peakToPeak(4000.0f);
    std::printf("    CONTRAPRUEBA: a 400 m %.3f m y a 4000 m %.3f m — el tope no muerde (iguales)\n",
                ppDeep, ppAbyss);
    CHECK(std::abs(ppDeep - ppAbyss) < 0.05f * ppAbyss,
          "en agua honda el tope no toca la ola (no esta aplastando el mar entero)");

    // CONTRAPRUEBA 2: el tope tiene que DEPENDER de la profundidad. Si diera lo mismo a 1 m que a
    // 400 m, no seria un limite de rompiente sino una constante disfrazada.
    const float pp1 = peakToPeak(1.0f);
    std::printf("    CONTRAPRUEBA: a 1 m la ola es %.3f m y a 400 m %.3f m (%.1fx)\n",
                pp1, ppDeep, ppDeep / std::max(pp1, 1e-3f));
    CHECK(pp1 < 0.6f * ppDeep, "el limite depende de la profundidad (no es una constante)");
}

// ------------------------------------------------------- TEST 11: la ola PUEDE plegarse al romper
//
// Una ola real rompe volcando: la cresta se echa hacia delante hasta que la superficie se pliega
// sobre si misma. En Gerstner eso ocurre cuando `Σ Q·A·k > 1`, porque ahi el jacobiano del
// desplazamiento se vuelve negativo — y el propio shader lo usa como senal de rompiente.
//
// ⚠️ CON LA FORMULA ANTERIOR NO PODIA PASAR NUNCA. Era `Q = 0.75/(k·A·N)` acotada a 1, lo que deja
// `Q·A·k ≤ 0.75/N` por tren y `Σ ≤ 0.75`: el jacobiano se quedaba en 0,25 como suelo. El comentario
// de `harukaGerstner` decia "si pasa de 1, la pliega" describiendo algo que su propia formula
// impedia, y la espuma por plegado no se encendia jamas. Este test fija las dos mitades: que en mar
// abierto NO se pliegue (el oleaje de fondo no vuelca solo) y que al romper SI.
void test_ocean_break_fold() {
    beginTest("ocean_break_fold");

    const Haruka::Planet::OceanState st = Haruka::Planet::oceanDefaultState();
    const glm::vec3 up = upAt(0.2, -0.4);
    const glm::vec3 wp = up * (float)kR;
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0,1,0))
                                                               : glm::cross(up, glm::vec3(1,0,0)));
    const glm::vec3 t2 = glm::cross(up, t1);

    // ⚠️ ESTE TEST REIMPLEMENTABA EL JACOBIANO A MANO Y SU COPIA SE QUEDO ANCLADA A LA CADENA VIEJA:
    // `oceanSteepness` como `Q` y `k = 2π/λ` de aguas profundas, sin `oceanWaveNumber`, sin
    // `oceanHorizAmp`, sin `oceanSteepScale` y sin `oceanCurlGain`. Seguia VERDE midiendo un motor que
    // ya no existe, y sus comentarios afirmaban "la voluta se revirtio" y "aqui lambda es FIJA" —
    // las dos cosas dejaron de ser ciertas el 2026-09-01. Es el mismo modo de fallo que
    // `terrain_two_bakes_disagree`: un numero de test se cita con la confianza de una medida.
    //
    // Ahora llama a `oceanJacobian`, que es LO QUE EJECUTA EL MOTOR (y el gemelo de la `jac` que
    // acumula `harukaGerstner`). Si la formula cambia, este test se entera.
    auto minJacobian = [&](float depth) {
        float worst = 1e9f;
        for (int s = 0; s < 30; ++s) {
            const glm::vec3 p = wp + glm::vec3(1.0f, 0.0f, 0.4f) * (float)(s * 5);
            for (int n = 0; n < 60; ++n) {
                const float t = 0.21f * n;
                worst = std::min(worst, Haruka::Planet::oceanJacobian(p, up, t, depth, 1.0f, st));
            }
        }
        return worst;
    };

    const float jacDeep  = minJacobian(60.0f);
    const float jacMid   = minJacobian(6.0f);
    const float jacBreak = minJacobian(1.2f);

    std::printf("    jacobiano minimo (negativo = superficie PLEGADA = la cresta vuelca):\n");
    std::printf("      mar abierto 60,0 m -> %+.3f %s\n", jacDeep,  jacDeep  < 0 ? "(pliega)" : "(no pliega)");
    std::printf("      transicion   6,0 m -> %+.3f %s\n", jacMid,   jacMid   < 0 ? "(pliega)" : "(no pliega)");
    std::printf("      rompiendo    1,2 m -> %+.3f %s\n", jacBreak, jacBreak < 0 ? "(pliega)" : "(no pliega)");

    // LA VOLUTA YA SALE, y lo que la desbloqueo no fue mas compresion sino CONCENTRARLA. Historia
    // corta, porque las dos hipotesis intermedias eran falsas:
    //  1. "el adelanto vive en `sin φ = 0`" — FALSO: la derivada de `H·cos φ·(1+g·cos φ)` es
    //     `−H·sin φ·(1+2g·cos φ)`, o sea que entra por el producto.
    //  2. habia un error de derivada de verdad (se acumulaba `(1+g·cos φ)`, la mitad), arreglado y
    //     comprobado abajo contra la derivada NUMERICA — y aun asi no plegaba.
    // La causa real: el escarpado por divergencia ya pasaba de 1, pero se REPARTIA entre las dos
    // direcciones del mapa horizontal, y un determinante no se anula asi. `oceanBreakerGain` concentra
    // el refuerzo en el tren 0 REFRACTADO —crestas paralelas a la orilla, como un surf real— y con eso
    // un autovalor cruza el cero.
    //
    // ⚠️ El fondo de 1,2 m NO pliega, y es correcto: ahi el limite de rompiente ya ha aplastado la
    // amplitud (`Σamp ≤ 0,55·d`), asi que queda poca ola que volcar. La voluta vive en la BARRA, no en
    // la orilla — que es donde rompe una ola de verdad.
    CHECK(jacDeep  > 0.0f, "en mar abierto la ola NO vuelca sola");
    CHECK(jacMid   < 0.0f, "en la franja de rompiente la cresta SI se pliega");

    // ── EL ORACULO: ¿es `oceanJacobian` la derivada del desplazamiento que el motor APLICA? ───────
    //
    // Hasta ahora la formula era su propio juez: la unica referencia del jacobiano era su propia
    // acumulacion analitica. Con `oceanDisplacement` (el desplazamiento COMPLETO, gemelo del valor que
    // devuelve `harukaGerstner`) se puede medir por diferencias finitas y comparar.
    //
    // ⚠️ Y HAY DOS COSAS DISTINTAS QUE MEDIR, porque con varios trenes NO son la misma:
    //  · el escalar que acumula el motor, `1 − Σ k·horiz·sin φ`, que es la traza;
    //  · el DETERMINANTE del mapa horizontal 2x2, `det(I + ∂disp_h/∂x)`, que es el criterio real de
    //    que la superficie deje de ser inyectiva. Solo coinciden si todos los trenes van paralelos.
    {
        const float d = 5.15f;
        // ⚠️ SIN PENDIENTE NO HAY REFRACCION, y en una playa de verdad la hay: al perder fondo los
        // trenes GIRAN hacia la orilla y se alinean entre si. Ocho trenes en ocho direcciones no
        // pliegan porque nunca coinciden en fase; ocho trenes casi paralelos son otra cosa. Se mide
        // con y sin, porque la diferencia es justo la hipotesis.
        const glm::vec3 haciaLaOrilla = t1;      // solo cuenta la direccion: `oceanRefract` normaliza
        double peorTraza = 0.0, minDet = 1e9, minAnalitico = 1e9;
        double minDetRefr = 1e9, minAnaRefr = 1e9;
        // ⚠️ LAS DIFERENCIAS FINITAS NO SE PUEDEN TOMAR EN COORDENADAS DE PLANETA. A radio terrestre
        // (6,4e6 m) un float32 tiene 1 ulp ≈ 0,5 m, asi que `p + t1*0.02f` **es exactamente `p`**: la
        // derivada sale 0 y el determinante un +1,000 perfecto que parece "no hay deformacion" y es
        // "no hay medida". Paso primero por aqui. La ola solo depende de la posicion a traves de
        // `k·dot(D, wp)`, asi que evaluar cerca del origen es la MISMA ola con otra fase — y ahi el
        // paso de 2 cm si existe.
        for (int s = 0; s < 40; ++s) {
            const glm::vec3 p = glm::vec3(1.0f, 0.0f, 0.4f) * (float)(s * 3);
            for (int n = 0; n < 40; ++n) {
                const float tt = 0.17f * n;
                // Derivada numerica del desplazamiento sobre el plano tangente. `h` en metros: lo
                // bastante fino para la onda mas corta (8,7 m) y lo bastante grueso para no ahogarse
                // en el ruido del float.
                const float h = 0.02f;
                const glm::vec3 d1 = (Haruka::Planet::oceanDisplacement(p + t1*h, up, tt, d, 1.0f, st)
                                    - Haruka::Planet::oceanDisplacement(p - t1*h, up, tt, d, 1.0f, st))
                                   / (2.0f * h);
                const glm::vec3 d2 = (Haruka::Planet::oceanDisplacement(p + t2*h, up, tt, d, 1.0f, st)
                                    - Haruka::Planet::oceanDisplacement(p - t2*h, up, tt, d, 1.0f, st))
                                   / (2.0f * h);
                // Mapa horizontal: I + [∂disp/∂t1 , ∂disp/∂t2] proyectado en (t1,t2).
                const float a11 = 1.0f + glm::dot(d1, t1), a12 = glm::dot(d2, t1);
                const float a21 = glm::dot(d1, t2),        a22 = 1.0f + glm::dot(d2, t2);
                const double det   = (double)a11 * a22 - (double)a12 * a21;
                // ⚠️ `1 + divergencia`, NO la media de la diagonal. `a11 + a22 = 2 + div`, asi que la
                // media es `1 + div/2` y comparar contra ella mete un factor 2 — me paso, y el
                // "error" que salia era el de la metrica, no el del motor.
                const double traza = (double)a11 + a22 - 1.0;
                const double ana   = Haruka::Planet::oceanJacobian(p, up, tt, d, 1.0f, st);
                minDet = std::min(minDet, det);
                minAnalitico = std::min(minAnalitico, ana);
                peorTraza = std::max(peorTraza, std::abs(ana - traza));

                // Lo mismo CON refraccion: los trenes giran hacia la orilla y se alinean.
                const glm::vec3 r1 = (Haruka::Planet::oceanDisplacement(p + t1*h, up, tt, d, 1.0f, st,
                                          Haruka::Planet::WATER_FETCH_UNLIMITED, haciaLaOrilla)
                                    - Haruka::Planet::oceanDisplacement(p - t1*h, up, tt, d, 1.0f, st,
                                          Haruka::Planet::WATER_FETCH_UNLIMITED, haciaLaOrilla))
                                   / (2.0f * h);
                const glm::vec3 r2 = (Haruka::Planet::oceanDisplacement(p + t2*h, up, tt, d, 1.0f, st,
                                          Haruka::Planet::WATER_FETCH_UNLIMITED, haciaLaOrilla)
                                    - Haruka::Planet::oceanDisplacement(p - t2*h, up, tt, d, 1.0f, st,
                                          Haruka::Planet::WATER_FETCH_UNLIMITED, haciaLaOrilla))
                                   / (2.0f * h);
                const float b11 = 1.0f + glm::dot(r1, t1), b12 = glm::dot(r2, t1);
                const float b21 = glm::dot(r1, t2),        b22 = 1.0f + glm::dot(r2, t2);
                minDetRefr = std::min(minDetRefr, (double)b11 * b22 - (double)b12 * b21);
                minAnaRefr = std::min(minAnaRefr,
                    (double)Haruka::Planet::oceanJacobian(p, up, tt, d, 1.0f, st,
                                Haruka::Planet::WATER_FETCH_UNLIMITED, haciaLaOrilla));
            }
        }
        std::printf("    ORACULO a %.2f m (40 puntos x 40 instantes, derivada numerica):\n", d);
        std::printf("      jacobiano ANALITICO minimo   %+.3f\n", minAnalitico);
        std::printf("      DETERMINANTE 2x2 minimo      %+.3f   <- el criterio real de pliegue\n", minDet);
        std::printf("      peor |analitico - traza|     %.4f   <- si no es ~0, la formula no es su derivada\n",
                    peorTraza);
        std::printf("      CON REFRACCION (playa):  analitico %+.3f · DETERMINANTE %+.3f\n",
                    minAnaRefr, minDetRefr);

        // EL ORACULO, y es lo que impide que la formula vuelva a ser su propio juez: el jacobiano que
        // acumula el motor tiene que ser la divergencia del desplazamiento que el motor APLICA.
        CHECK(peorTraza < 0.01, "`oceanJacobian` ES la divergencia de `oceanDisplacement` (derivada numerica)");

        // EL CRITERIO REAL DE PLIEGUE, y ahora se cruza: con refraccion el determinante es NEGATIVO,
        // o sea que la superficie deja de ser inyectiva. Eso es una ola volcando.
        CHECK(minDetRefr < 0.0, "CON REFRACCION la superficie SE PLIEGA (determinante < 0)");
        // Y SIN refraccion no pliega: hace falta que los trenes se alineen. Las dos mitades juntas son
        // lo que demuestra que el mecanismo es la CONCENTRACION y no simplemente "mas compresion".
        CHECK(minDet > 0.0, "sin alinear los trenes NO pliega: el mecanismo es concentrar, no apretar");
        std::printf("      CONTRAPRUEBA: la refraccion es lo que pliega (%.3f -> %.3f)\n",
                    minDet, minDetRefr);

        // ⚠️ LA GUARDA QUE DE VERDAD IMPORTA: EL AGUA INTERIOR. El intento de voluta de 2026-08-29 se
        // revirtio porque metia metros de desplazamiento horizontal en una charca y la lamina se
        // plegaba sobre celdas de 4,4 m. Aqui el refuerzo esta cerrado por el indice de rompiente
        // (arranca en 0,78) y es una FRACCION del semieje, que ya escala con la amplitud. Medido: una
        // charca de 30 cm con fetch de 100 m sale IDENTICA a como estaba antes de todo esto.
        {
            const float dCharca = 0.30f, fetchCharca = 100.0f;
            float horizMax = 0.0f; double detMin = 1e9;
            for (int a = 0; a < 60; ++a) for (int b = 0; b < 60; ++b) {
                const glm::vec3 pc((float)a * 1.7f, 0.0f, (float)b * 1.3f);
                const float tt = 0.11f * b;
                const glm::vec3 dsp = Haruka::Planet::oceanDisplacement(pc, up, tt, dCharca, 1.0f, st,
                                                                        fetchCharca, haciaLaOrilla);
                horizMax = std::max(horizMax, glm::length(dsp - up * glm::dot(dsp, up)));
                const float e = 0.01f;
                const glm::vec3 q1 = (Haruka::Planet::oceanDisplacement(pc + t1*e, up, tt, dCharca, 1.0f, st, fetchCharca, haciaLaOrilla)
                                    - Haruka::Planet::oceanDisplacement(pc - t1*e, up, tt, dCharca, 1.0f, st, fetchCharca, haciaLaOrilla)) / (2.0f*e);
                const glm::vec3 q2 = (Haruka::Planet::oceanDisplacement(pc + t2*e, up, tt, dCharca, 1.0f, st, fetchCharca, haciaLaOrilla)
                                    - Haruka::Planet::oceanDisplacement(pc - t2*e, up, tt, dCharca, 1.0f, st, fetchCharca, haciaLaOrilla)) / (2.0f*e);
                const float c11 = 1.0f + glm::dot(q1,t1), c12 = glm::dot(q2,t1);
                const float c21 = glm::dot(q1,t2),        c22 = 1.0f + glm::dot(q2,t2);
                detMin = std::min(detMin, (double)c11*c22 - (double)c12*c21);
            }
            std::printf("    CHARCA de %.2f m (fetch %.0f m): horizontal max %.3f m · determinante min %+.3f\n",
                        dCharca, fetchCharca, horizMax, detMin);
            CHECK(detMin > 0.5, "el AGUA INTERIOR no se pliega (la regresion de 2026-08-29 no vuelve)");
            CHECK(horizMax < 1.5f, "el vaiven de una charca sigue por debajo de su celda de 4,4 m");
        }
    }

    // REPARTO POR TREN en la franja de rompiente: quien lleva el escarpado y quien es el DOMINANTE.
    // Un rompiente real es UNA ola que vuelca, no ocho repartiendose el presupuesto.
    for (float d : { 5.15f, 2.0f }) {
        const float green = Haruka::Planet::oceanGreenGain(d);
        const float scale = Haruka::Planet::oceanBreakScale(d, st);
        const float steep = Haruka::Planet::oceanSteepScale(d, st);
        std::printf("    reparto a %.2f m (lambda0 -> lambda local · amp · k*horiz):\n", d);
        float total = 0.0f;
        for (int i = 0; i < Haruka::Planet::OCEAN_WAVES; ++i) {
            const float k0 = 6.2831853f / st.wave[i][0];
            const float k  = Haruka::Planet::oceanWaveNumber(k0, d);
            const float amp = st.wave[i][1] * green * scale * Haruka::Planet::oceanShortWaveFade(k);
            const float kh = k * Haruka::Planet::oceanHorizAmp(k, amp, d) * steep;
            total += kh;
            std::printf("      tren %d: %6.1f -> %5.1f m · amp %.3f · k*horiz %.3f\n",
                        i, st.wave[i][0], 6.2831853f / k, amp, kh);
        }
        std::printf("      TOTAL k*horiz %.3f  (un tren SOLO pliega si supera ~0,54 con el adelanto)\n",
                    total);
    }

    // La cuenta que separa las dos magnitudes, para que no haya que fiarse del parrafo: en la franja
    // de rompiente se mide el escarpado maximo (sobre la cresta, `cos φ = 1`) y el jacobiano minimo.
    // Si el primero pasa de 1 y el segundo no baja de 0, es exactamente el desajuste de fase.
    {
        const float d = 5.15f;   // donde la memoria situa el pico de escarpado
        const float green = Haruka::Planet::oceanGreenGain(d);
        const float scale = Haruka::Planet::oceanBreakScale(d, st);
        const float steep = Haruka::Planet::oceanSteepScale(d, st);
        const float curl  = Haruka::Planet::oceanCurlGain(d, st);
        float sumCrest = 0.0f;   // Σ k·horiz con el adelanto puesto (cos φ = 1)
        for (int i = 0; i < Haruka::Planet::OCEAN_WAVES; ++i) {
            const float k0 = 6.2831853f / st.wave[i][0];
            const float k  = Haruka::Planet::oceanWaveNumber(k0, d);
            const float amp = st.wave[i][1] * green * scale * Haruka::Planet::oceanShortWaveFade(k);
            if (amp <= 1e-4f) continue;
            sumCrest += k * Haruka::Planet::oceanHorizAmp(k, amp, d) * steep * (1.0f + curl);
        }
        std::printf("    a %.2f m: escarpado en la CRESTA (cos=1) %.3f · jacobiano minimo %+.3f\n",
                    d, sumCrest, minJacobian(d));
        std::printf("      el escarpado pasa de 1 y aun asi no pliega: los OCHO trenes no coinciden en fase\n");
    }

    // CONTRAPRUEBA: la cadena VIEJA (Q = 0,75/(k·A·N) acotada a 1, con `k` de aguas profundas) no
    // podia bajar de 1−0,75 = 0,25 a NINGUNA profundidad — el pliegue estaba prohibido por
    // construccion, valga lo que valga el fondo. Se recalcula aqui para dejar constancia de que lo que
    // habilita la voluta es el cambio de formula, y no que el barrido haya tenido suerte.
    float worstOld = 1e9f;
    for (float depth : { 60.0f, 6.0f, 1.2f }) {
        const float green = Haruka::Planet::oceanGreenGain(depth);
        const float scale = Haruka::Planet::oceanBreakScale(depth, st);
        for (int s = 0; s < 30; ++s) {
            const glm::vec3 p = wp + glm::vec3(1.0f, 0.0f, 0.4f) * (float)(s * 5);
            for (int n = 0; n < 60; ++n) {
                const float t = 0.21f * n;
                float jac = 1.0f;
                for (int i = 0; i < Haruka::Planet::OCEAN_WAVES; ++i) {
                    const float k   = 6.2831853f / st.wave[i][0];
                    const float amp = st.wave[i][1] * green * scale;
                    if (amp <= 1e-4f) continue;
                    const glm::vec3 D = glm::normalize(t1 * st.wave[i][2] + t2 * st.wave[i][3]);
                    float Q = 0.75f / (k * amp * float(Haruka::Planet::OCEAN_WAVES) + 1e-4f);
                    Q = std::min(Q, 1.0f);                     // el tope viejo
                    const float ph = k * glm::dot(D, p) - std::sqrt(Haruka::Planet::OCEAN_G * k) * t;
                    jac -= Q * amp * k * std::sin(ph);
                }
                worstOld = std::min(worstOld, jac);
            }
        }
    }
    std::printf("    CONTRAPRUEBA: la cadena vieja (Q acotado a 1, k de aguas profundas) da %+.3f en el mejor caso\n",
                worstOld);
    CHECK(worstOld > 0.0f, "la cadena vieja NO podia plegar: el pliegue lo trae la formula nueva");
}

// ------------------------------------------------- TEST: la ola de bajio es ASIMETRICA (cnoidal)
//
// El perfil de la ola era un SENO PURO incluso rompiendo: `oceanCurlGain` solo tocaba la horizontal,
// asi que la elevacion `A·sin φ` tenia crestas y senos con la misma forma. Una ola de bajio real es
// cnoidal: cresta estrecha y alta, seno ancho y poco profundo. `oceanSkewness` mete el 2º armonico.
//
// Las tres propiedades que lo hacen SEGURO, y cada una es un CHECK:
//  1. en mar abierto no cambia NADA (el termino esta apagado por el indice de rompiente);
//  2. la altura pico-valle NO cambia — la ola cambia de forma, no de tamaño, asi que el limite de
//     rompiente, la flotabilidad y el indice de rompiente siguen significando lo mismo;
//  3. el nivel medio no se mueve.
void test_ocean_skewness() {
    beginTest("ocean_skewness");
    using namespace Haruka::Planet;

    const OceanState st = oceanDefaultState();
    const glm::vec3 up = upAt(0.2, -0.4);

    // ⚠️ EL INSTRUMENTO NO PUEDE SER "cresta contra seno EN UN PUNTO". Con ocho frecuencias
    // inconmensurables, el maximo y el minimo de una ventana finita no tienen por que ser simetricos
    // ni con el termino APAGADO: medido asi, el mar abierto daba una razon de 0,889 sin que hubiera
    // ninguna asimetria. Eso es ruido de muestreo, no forma de la ola.
    //
    // El estimador correcto es el que usa la oceanografia: la ASIMETRIA ESTADISTICA (tercer momento
    // normalizado) del campo de elevacion. Es 0 para un mar lineal simetrico y positiva cuando las
    // crestas se peraltan — exactamente lo que se quiere medir, y es robusto al muestreo.
    // ⚠️ Y LA REJILLA REGULAR TAMPOCO VALE: con 24x24x24 pasos fijos y ocho frecuencias, el tercer
    // momento aliasea y el mar abierto daba −0,133 con el termino APAGADO A CERO. Se muestrea con paso
    // IRREGULAR (un hash barato por muestra) y muchas mas muestras, que es lo que hace converger un
    // momento de orden 3. Un 0,000 o un valor "casi cero" en el lado apagado es la unica prueba de que
    // el instrumento no esta inventando asimetria.
    auto muestra = [&](int i, int c) {                 // ruido determinista en [0,1)
        uint32_t h = (uint32_t)i * 2654435761u + (uint32_t)c * 40503u;
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        return (float)(h & 0xFFFFFFu) / (float)0x1000000u;
    };
    const int N = 200000;
    auto estadistica = [&](float depth, float& skewStat, float& sigma) {
        double s1 = 0, s2 = 0, s3 = 0;
        std::vector<float> hs((size_t)N);
        for (int i = 0; i < N; ++i) {
            const glm::vec3 p(muestra(i, 0) * 400.0f, 0.0f, muestra(i, 1) * 400.0f);
            const float h = oceanWaveHeight(p, up, muestra(i, 2) * 120.0f, depth, 1.0f, st);
            hs[(size_t)i] = h; s1 += h;
        }
        const double mu = s1 / N;
        for (int i = 0; i < N; ++i) {
            const double d = hs[(size_t)i] - mu;
            s2 += d * d; s3 += d * d * d;
        }
        sigma    = (float)std::sqrt(s2 / N);
        skewStat = (float)((s3 / N) / std::pow(s2 / N, 1.5));
    };

    float skD, sgD, skS, sgS;
    estadistica(200.0f, skD, sgD);
    estadistica(2.0f,   skS, sgS);
    std::printf("    asimetria estadistica del campo (200000 muestras, paso irregular):\n");
    std::printf("      fondo      2o armonico   asimetria   sigma\n");
    std::printf("      200,0 m       %.3f        %+.3f     %.3f m\n", oceanSkewness(200.0f, st), skD, sgD);
    std::printf("        2,0 m       %.3f        %+.3f     %.3f m\n", oceanSkewness(2.0f, st), skS, sgS);

    // 1. EL LIMITE DE AGUAS PROFUNDAS ES EL ORACULO. `oceanStokesSkew` es una expresion general; su
    //    limite `kd -> inf` tiene que dar el Stokes de toda la vida, `S = k·a/2`, que se deriva por
    //    separado. Si la expresion estuviera mal transcrita, este limite no saldria.
    //    ⚠️ Y ES != 0 A PROPOSITO: antes el termino era una constante con una puerta que lo apagaba en
    //    mar abierto. Una ola profunda REAL tiene crestas algo mas agudas que sus senos; el 0 exacto
    //    de antes era el modelo simplificado, no la fisica.
    {
        const float k = 0.0706f, a = 0.62f, d = 5000.0f;    // kd enorme
        const float esperado = 0.5f * k * a;
        const float dado = oceanStokesSkew(k, a, d);
        std::printf("    limite PROFUNDO: S = %.6f · k*a/2 = %.6f (Stokes clasico)\n", dado, esperado);
        CHECK(std::fabs(dado - esperado) < 1e-4f, "kd -> inf da el Stokes profundo S = k*a/2");
    }
    // Y el limite SOMERO: el factor va como `3/(kd)³`, que es el numero de Ursell. Se comprueba por
    // debajo del tope para que sea la formula lo que se mide y no el `min`.
    {
        // ⚠️ LOS PARAMETROS TIENEN QUE DEJAR `S` POR DEBAJO DEL TOPE, o lo que se mide es el `min` y
        // no la formula. Con k=0,02 a=0,02 d=3 salia S=1,39 (topado a 0,25) y el test comparaba el
        // tope contra Ursell. Con a=0,002: S = 0,139, dentro de la formula.
        const float k = 0.02f, a = 0.002f, d = 3.0f;        // kd = 0,06 -> S = 0,139 < 0,25
        const float esperado = 0.75f * k * a / std::pow(k * d, 3.0f);
        const float dado = oceanStokesSkew(k, a, d);
        std::printf("    limite SOMERO:  S = %.6f · 3ka/(4(kd)^3) = %.6f (Ursell)\n", dado, esperado);
        CHECK(std::fabs(dado / esperado - 1.0f) < 0.02f, "kd -> 0 va como el numero de Ursell");
    }
    CHECK(std::fabs(skD) < 0.05f, "en mar abierto la ola sigue siendo CASI simetrica");

    // 2. EN EL BAJIO LAS CRESTAS SE PERALTAN. El umbral NO es a ojo: la asimetria medida en campo
    //    sobre olas de bajio va de ~0,2 (empezando a peraltarse) a ~0,8 (a punto de romper). Con
    //    `OCEAN_SKEW_MAX = 0,30` el modelo da 0,247 a 2 m, o sea el extremo bajo de ese rango — que es
    //    lo prudente mientras el termino sea un tope constante y no la formula de Stokes completa.
    CHECK(skS > 0.20f, "en el bajio la ola es CNOIDAL: crestas picudas, senos planos");

    // 3. EL TAMAÑO DE LA OLA APENAS SE MUEVE. Es la propiedad que protege al resto del modelo: si el
    //    2o armonico agrandara la ola, arrastraria consigo el indice de rompiente, el tope y la
    //    flotabilidad. Analiticamente `sigma` sube por `sqrt(1 + S²)`, o sea un 4,4% con S = 0,30.
    float sigmaOff = 0.0f;
    {
        const float green = oceanGreenGain(2.0f) * oceanFetchFactor(st, WATER_FETCH_UNLIMITED);
        const float scale = oceanBreakScale(2.0f, st);
        double s2 = 0;
        const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0,1,0))
                                                                   : glm::cross(up, glm::vec3(1,0,0)));
        const glm::vec3 t2 = glm::cross(up, t1);
        for (int i = 0; i < N; ++i) {
            const glm::vec3 p(muestra(i, 0) * 400.0f, 0.0f, muestra(i, 1) * 400.0f);
            const float tt = muestra(i, 2) * 120.0f;
            float h = 0.0f;
            for (int j = 0; j < OCEAN_WAVES; ++j) {              // la suma de senos de siempre
                const float k0 = 6.2831853f / st.wave[j][0];
                const float k  = oceanWaveNumber(k0, 2.0f);
                const float amp = st.wave[j][1] * green * scale * oceanShortWaveFade(k);
                if (amp <= 1e-4f) continue;
                const glm::vec3 D = glm::normalize(t1 * st.wave[j][2] + t2 * st.wave[j][3]);
                h += amp * std::sin(k * glm::dot(D, p) - std::sqrt(OCEAN_G * k0) * tt);
            }
            s2 += (double)h * h;
        }
        sigmaOff = (float)std::sqrt(s2 / N);
    }
    // El teorico ya no es un numero fijo: con `S` POR TREN la varianza sube por `Σa²(1+S²)/Σa²`, asi
    // que se calcula del propio estado en vez de citar el 4,4 % que valia cuando `S` era comun.
    float sa2 = 0.0f, sa2s2 = 0.0f;
    {
        const float green = oceanGreenGain(2.0f) * oceanFetchFactor(st, WATER_FETCH_UNLIMITED);
        const float scale = oceanBreakScale(2.0f, st);
        for (int j = 0; j < OCEAN_WAVES; ++j) {
            const float k0 = 6.2831853f / st.wave[j][0];
            const float k  = oceanWaveNumber(k0, 2.0f);
            const float amp = st.wave[j][1] * green * scale * oceanShortWaveFade(k);
            if (amp <= 1e-4f) continue;
            const float S = oceanStokesSkew(k, amp, 2.0f);
            sa2 += amp * amp; sa2s2 += amp * amp * S * S;
        }
    }
    const float teorico = 100.0f * (std::sqrt(1.0f + sa2s2 / std::max(sa2, 1e-6f)) - 1.0f);
    std::printf("    sigma a 2,0 m: con 2o armonico %.4f m · sin el %.4f m (%+.2f%%, teorico %+.2f%%)\n",
                sgS, sigmaOff, 100.0f * (sgS / sigmaOff - 1.0f), teorico);
    CHECK(std::fabs(100.0f * (sgS / sigmaOff - 1.0f) - teorico) < 0.5f,
          "el crecimiento de sigma es el que predice sqrt(1 + <S^2>)");
    CHECK(std::fabs(sgS / sigmaOff - 1.0f) < 0.08f, "el 2o armonico no cambia el TAMAÑO de la ola");

    // CONTRAPRUEBA: si la asimetria fuera un sesgo constante y no una funcion del FONDO, saldria igual
    // en mar abierto. Se compara por RAZON y no por diferencia: en profundo ya no es cero (es el
    // Stokes real), asi que lo que prueba el punto es cuanto MULTIPLICA el bajio.
    std::printf("    CONTRAPRUEBA: asimetria en mar abierto %+.3f vs bajio %+.3f (x%.1f)\n",
                skD, skS, skS / std::max(skD, 1e-4f));
    CHECK(skS > 8.0f * skD, "la asimetria SALE DEL FONDO (el bajio la multiplica, no es un sesgo)");

    // ── EL TOPE DE 0,25 TIENE DERIVACION, Y SE COMPRUEBA CONTANDO EXTREMOS ───────────────────────
    //
    // `η = a·(sin φ − S·cos 2φ)` tiene derivada `a·cos φ·(1 + 4S·sin φ)`. Se anula en `cos φ = 0`
    // (la cresta y el seno, que es lo que debe haber) y ADEMAS en `sin φ = −1/(4S)`, que solo tiene
    // solucion si `S > 0,25`. Pasado ese punto al seno le crece un monticulo secundario: deja de ser
    // el perfil de una ola. Se cuentan los extremos de un periodo, que es la prueba directa.
    auto extremos = [](float S) {
        int n = 0;
        const int M = 20000;
        float prev = 0.0f;
        for (int i = 0; i <= M; ++i) {
            const float ph = 6.2831853f * (float)i / (float)M;
            const float dv = std::cos(ph) * (1.0f + 4.0f * S * std::sin(ph));
            if (i > 0 && ((prev > 0.0f) != (dv > 0.0f))) ++n;
            prev = dv;
        }
        return n;
    };
    std::printf("    extremos por periodo: S=0,25 -> %d · S=0,30 (el valor que HABIA) -> %d\n",
                extremos(0.25f), extremos(0.30f));
    CHECK(extremos(OCEAN_SKEW_MAX) == 2, "con el tope vigente la ola tiene UNA cresta y UN seno");
    CHECK(extremos(0.30f) == 4, "CONTRAPRUEBA: con 0,30 al seno le sale un monticulo (4 extremos)");
    CHECK(OCEAN_SKEW_MAX <= 0.25f, "el tope esta en el limite derivado, no por encima");
}

// ------------------------------------------------ TEST: BORREGUILLOS en mar abierto, contra Monahan
//
// El oceano abierto salia LISO Y OSCURO hasta la costa: la unica espuma era la de orilla y la de
// plegado, y esta ultima pedia un escarpado local de 0,45 que en mar abierto se alcanza el 0,000 % de
// las veces. Un mar de 11,4 m/s tiene crestas blancas.
//
// ⚠️ EL ORACULO NO SOY YO: es Monahan & O'Muircheartaigh (1980), `W = 3,84e-6·U^3,41`, con el viento
// que el propio estado del mar DEDUCE (no uno que yo elija). El umbral de espuma se calibro contra
// esa cobertura; este test comprueba que sigue casando, que es lo que impide que alguien lo mueva
// "porque se ve mejor" sin enterarse de que se ha salido de la fisica.
void test_ocean_whitecaps() {
    beginTest("ocean_whitecaps");
    using namespace Haruka::Planet;

    const OceanState st = oceanDefaultState();
    const glm::vec3 up = upAt(0.2, -0.4);

    auto muestra = [&](int i, int c) {
        uint32_t h = (uint32_t)i * 2654435761u + (uint32_t)c * 40503u;
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        return (float)(h & 0xFFFFFFu) / (float)0x1000000u;
    };
    const int N = 200000;
    auto cobertura = [&](float depth, const OceanState& s) {
        int blancos = 0;
        for (int i = 0; i < N; ++i) {
            const glm::vec3 p(muestra(i, 0) * 800.0f, 0.0f, muestra(i, 1) * 800.0f);
            if (oceanFoam(p, up, muestra(i, 2) * 200.0f, depth, 1.0f, s) > 0.5f) ++blancos;
        }
        return (float)blancos / (float)N;
    };

    const float U = oceanWindSpeed(st);
    const float W = oceanWhitecapCoverage(st);
    std::printf("    viento DEDUCIDO del estado %.3f m/s · Hs %.4f m\n", U, oceanSignificantHeight(st));
    std::printf("    cobertura de Monahan: %.4f%%\n", 100.0f * W);

    const float c400 = cobertura(400.0f, st);
    const float c60  = cobertura(60.0f,  st);
    const float c20  = cobertura(20.0f,  st);
    const float c8   = cobertura(8.0f,   st);
    std::printf("    cobertura medida: 400 m %.3f%% · 60 m %.3f%% · 20 m %.3f%% · 8 m %.3f%%\n",
                100.0f * c400, 100.0f * c60, 100.0f * c20, 100.0f * c8);

    // 1. HAY BORREGUILLOS. Antes esto era 0,000 % exacto.
    CHECK(c400 > 0.002f, "en mar abierto HAY crestas blancas (antes: ninguna)");
    // 2. Y SON LOS QUE TOCAN. El margen es amplio a proposito: Monahan es una correlacion empirica con
    //    dispersion de un factor ~2 entre campanas, asi que exigir mas seria fingir precision. Lo que
    //    se afirma es el ORDEN DE MAGNITUD correcto, y eso si lo fija.
    CHECK(c400 > 0.5f * W && c400 < 2.0f * W, "la cobertura casa con Monahan (mismo orden)");
    // 3. SUBE HACIA LA COSTA: al perder fondo la ola se peralta y rompe mas.
    CHECK(c20 > c60 && c8 > c20, "la cobertura crece al perder fondo");
    // 4. Y NO SATURA: si el bajio saliera al 100 % la costa seria una sabana blanca.
    CHECK(c8 < 0.60f, "ni siquiera la rompiente satura la superficie entera");

    // CONTRAPRUEBA: MAS VIENTO, MAS BORREGUILLOS. Es lo que separa "espuma calibrada" de "una
    // constante que casualmente daba 1,5 %". Monahan va como U^3,41, asi que subir el mar tiene que
    // subir la cobertura MUCHO.
    // ⚠️ LA CONTRAPRUEBA ANTERIOR NO ERA UN CAMBIO DE VIENTO. Multiplicaba las amplitudes por 1,6
    // dejando las longitudes de onda quietas: eso es un mar MAS ESCARPADO, no uno mas ventoso, y
    // Monahan no habla de eso. Un viento mayor sube `Hs` Y `lambda` a la vez (los dos con `U²`), que
    // es justo lo que hace `oceanStateFromWind` — el instrumento correcto, y ya existia.
    const OceanState fuerte = oceanStateFromWind(15.0f, 1.0f, 0.0f);
    const float Ufuerte = oceanWindSpeed(fuerte);
    const float cFuerte = cobertura(400.0f, fuerte);
    const float Wfuerte = oceanWhitecapCoverage(fuerte);
    std::printf("    CONTRAPRUEBA (mar de PM a otro viento): %.2f -> %.2f m/s · cobertura %.3f%% -> %.3f%%\n",
                U, Ufuerte, 100.0f * c400, 100.0f * cFuerte);
    std::printf("      Monahan a ese viento: %.3f%% · el modelo da %.3f%%\n",
                100.0f * Wfuerte, 100.0f * cFuerte);
    CHECK(cFuerte > c400 * 1.5f, "CONTRAPRUEBA: mas viento da mas borreguillos");
    CHECK(cFuerte > 0.5f * Wfuerte && cFuerte < 2.0f * Wfuerte,
          "la cobertura sigue a Monahan TAMBIEN a otro viento (el umbral se deriva de ella)");
}

// --------------------------------------------------------------- TEST: la ESPUMA existe en CPU
//
// La espuma era la UNICA magnitud de la ola que se calculaba solo en el shader. Sin gemelo en CPU no
// habia con que carearla (eso lo hace ahora `rhi_test_main`, canal alfa de `rhitest_waveprobe`), y —
// mas importante— ni la fisica ni el juego podian preguntar "¿esto es agua blanca?".
//
// Este test no compara contra la GPU (eso es del banco RHI): fija las PROPIEDADES que la espuma tiene
// que cumplir para significar algo, cada una con su contraprueba.
void test_ocean_foam() {
    beginTest("ocean_foam");
    using namespace Haruka::Planet;

    const OceanState st = oceanDefaultState();
    const glm::vec3 up = upAt(0.2, -0.4);
    const glm::vec3 wp = up * (float)kR;

    // ⚠️ EL MAXIMO DEJO DE SERVIR AL LLEGAR LOS BORREGUILLOS. Este test medía la espuma MAXIMA de un
    // barrido; con espuma de cresta calibrada, la cresta mas escarpada del barrido llega a 1,000 a
    // CUALQUIER profundidad, asi que el maximo salia 1,000 en todas partes y dejaba de distinguir el
    // mar abierto de la orilla. El estadistico que significa algo es la MEDIA (cuanta superficie hay
    // blanca), que es tambien la que se carea contra Monahan en `ocean_whitecaps`.
    auto meanFoam = [&](float depth, float fade, const OceanState& s) {
        double acc = 0.0; int n = 0;
        for (int i = 0; i < 30; ++i) {
            const glm::vec3 p = wp + glm::vec3(1.0f, 0.0f, 0.4f) * (float)(i * 5);
            for (int j = 0; j < 60; ++j) { acc += oceanFoam(p, up, 0.21f * j, depth, fade, s); ++n; }
        }
        return (float)(acc / n);
    };

    std::printf("    espuma MEDIA por profundidad (barrido de 30 puntos x 60 instantes):\n");
    const float fDeep  = meanFoam(200.0f, 1.0f, st);
    const float fMid   = meanFoam(20.0f,  1.0f, st);
    const float fBreak = meanFoam(2.0f,   1.0f, st);
    const float fShore = meanFoam(0.4f,   1.0f, st);
    std::printf("      mar abierto 200,0 m -> %.4f\n", fDeep);
    std::printf("      transicion   20,0 m -> %.4f\n", fMid);
    std::printf("      rompiendo     2,0 m -> %.4f\n", fBreak);
    std::printf("      orilla        0,4 m -> %.4f\n", fShore);

    // LA GRADACION es lo que hace que la espuma signifique algo: si fuera un tinte uniforme, o si
    // saltara de 0 a 1 en la orilla, no estaria describiendo un mar.
    CHECK(fShore > 0.9f,  "la orilla es agua blanca");
    CHECK(fDeep  < 0.05f, "el mar abierto tiene borreguillos, no espuma (poca superficie)");
    // ⚠️ A 20 m NO tiene por que haber mas: los borreguillos son de VIENTO y su cobertura es Monahan a
    // cualquier profundidad (el umbral se deriva de ella). Lo que crece hacia la costa es la espuma de
    // ROMPIENTE, y esa aparece cuando el indice cruza 0,78 — no a 20 m, donde vale 0,31.
    CHECK(std::fabs(fMid - fDeep) < 0.02f, "a 20 m la ola aun no rompe: espuma de viento, como en profundo");
    CHECK(fBreak > fMid,   "y en la franja de rompiente mas todavia");
    CHECK(fShore >= fBreak, "y la orilla al menos tanto como la rompiente (las dos saturan)");

    // `fade` apaga la ola fuera del alcance del clipmap, y tiene que apagar la espuma con ella: si no,
    // quedaria un cerco de blanco flotando sobre un mar que ya esta liso.
    const float fFaded = meanFoam(0.4f, 0.0f, st);
    std::printf("    con fade = 0 (fuera del alcance de la ola): %.3f\n", fFaded);
    CHECK(fFaded == 0.0f, "sin ola no hay espuma (el fade apaga las dos)");

    // CONTRAPRUEBA: con un mar MAS GRUESO la espuma tiene que llegar mas adentro. Si `oceanFoam`
    // ignorase el estado y mirase solo la profundidad, esto no se moveria — que es exactamente el
    // error que tendria un gemelo escrito a medias.
    OceanState alt = st;
    for (int i = 0; i < OCEAN_WAVES; ++i) alt.wave[i][1] *= 2.0f;
    const float fMidAlt = meanFoam(20.0f, 1.0f, alt);
    std::printf("    CONTRAPRUEBA: con el mar al doble de altura, a 20,0 m de fondo %.4f -> %.4f\n",
                fMid, fMidAlt);
    CHECK(fMidAlt > fMid * 1.5f, "un mar mas grueso espuma mas adentro (la espuma depende de la OLA)");
}

/**
 * @brief DISPERSIÓN EN PROFUNDIDAD FINITA: la ola se acorta al perder fondo.
 *
 * Todo el oleaje usaba `ω = √(g·k)` —aguas profundas— también en 30 cm de agua. La relación correcta
 * es `ω² = g·k·tanh(k·d)`, y lo que se conserva al entrar en el bajío es la FRECUENCIA, no la
 * longitud de onda.
 *
 * ⚠️ EL ORÁCULO ES LA PROPIA ECUACIÓN, no una tabla de números que yo haya elegido. Se resuelve `k`
 * y se comprueba que satisface `g·k·tanh(k·d) = ω²`. Eso no se puede hacer trampa: o la cumple o no.
 */
void test_ocean_finite_depth() {
    beginTest("ocean_finite_depth");
    using namespace Haruka::Planet;
    const OceanState st = oceanDefaultState();

    // (1) EL RESIDUO DE LA ECUACIÓN, sobre los cuatro trenes y toda la franja interesante.
    double worstRel = 0.0; float worstD = 0.0f; int worstI = -1;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float k0 = 6.2831853f / st.wave[i][0];
        const float w2 = OCEAN_G * k0;
        for (float d : { 0.1f, 0.3f, 1.0f, 3.0f, 10.0f, 30.0f, 100.0f, 400.0f }) {
            const float k   = oceanWaveNumber(k0, d);
            const double res = std::fabs((double)(OCEAN_G * k * std::tanh(k * d)) - (double)w2)
                             / (double)w2;
            if (res > worstRel) { worstRel = res; worstD = d; worstI = i; }
        }
    }
    std::printf("    residuo peor de `g k tanh(kd) = w2`: %.3e (tren %d a %.1f m)\n",
                worstRel, worstI, worstD);

    // (2) LA TABLA, para que se vea lo que hace.
    std::printf("    longitud de onda LOCAL por profundidad (la de la tabla es la PROFUNDA):\n");
    std::printf("      tren      prof:   0.3 m    1 m     3 m    10 m    40 m   400 m\n");
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float k0 = 6.2831853f / st.wave[i][0];
        std::printf("      %6.1f m         ", st.wave[i][0]);
        for (float d : { 0.3f, 1.0f, 3.0f, 10.0f, 40.0f, 400.0f })
            std::printf("%7.2f", 6.2831853f / oceanWaveNumber(k0, d));
        std::printf("\n");
    }

    // (3) EL MAR PROFUNDO NO CAMBIA, Y ES BIT A BIT. Con `k0·d >= 10` la salida rápida devuelve `k0`
    //     sin tocar nada, y eso es lo que garantiza que este cambio no toca el mar abierto.
    bool deepExact = true;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float k0 = 6.2831853f / st.wave[i][0];
        const float d  = 10.0f / k0 + 1.0f;           // por encima del umbral de la salida rápida
        if (oceanWaveNumber(k0, d) != k0) deepExact = false;
    }
    // (4) Y EN SOMERO SÍ CAMBIA, MUCHO. Sin esto, (3) se leería como "la función no hace nada".
    const float k0Long  = 6.2831853f / st.wave[0][0];
    const float kShallow = oceanWaveNumber(k0Long, 0.3f);
    const float ratio    = kShallow / k0Long;
    std::printf("    tren de %.0f m: k x%.2f en 30 cm de agua (lambda %.1f -> %.2f m)\n",
                st.wave[0][0], ratio, st.wave[0][0], 6.2831853f / kShallow);

    // (5) EL LÍMITE SOMERO ANALÍTICO: `k -> sqrt(k0/d)`. Es otra comprobación independiente de (1),
    //     y con una fórmula distinta: si las dos casan, el solver está resolviendo lo que dice.
    const float kAsym = std::sqrt(k0Long / 0.05f);
    const float kNum  = oceanWaveNumber(k0Long, 0.05f);
    std::printf("    limite somero a 5 cm: numerico %.4f vs analitico sqrt(k0/d) %.4f (%.2f %%)\n",
                kNum, kAsym, 100.0 * std::fabs(kNum - kAsym) / kAsym);

    CHECK(worstRel < 1e-5, "el `k` resuelto satisface la relacion de dispersion de profundidad finita");
    CHECK(deepExact, "en agua profunda devuelve el `k` de la tabla EXACTO: el mar abierto no cambia");
    CHECK(ratio > 4.0f, "CONTRAPRUEBA: en agua somera el numero de onda cambia MUCHO (si no, (3) no "
                        "demostraria nada)");
    CHECK(std::fabs(kNum - kAsym) / kAsym < 0.05f,
          "y casa con el limite somero analitico, que es una formula distinta");
}

/**
 * @brief EL AGUA INTERIOR COMO CAMPO DEL MUNDO: priority-flood global sobre el bake.
 *
 * Sustituye la siembra por parche de 420 m, de la que colgaban tres limitaciones: no había agua
 * fuera del parche, el resultado dependía de su BORDE (un lago podía aparecer al caminar) y no era
 * función de la posición, así que dos clientes no podían coincidir.
 *
 * ⚠️ EL ORÁCULO NO ES UNA TABLA MÍA: son propiedades que el relleno tiene que cumplir por definición
 * —una cuenca cerrada se llena EXACTAMENTE hasta su punto de derrame, y ni un téxel queda por encima
 * de él— más el caso del planeta seco, que es el requisito que Andoni pidió.
 */
void test_water_fill_global() {
    beginTest("water_fill_global");
    using namespace Haruka::Planet;

    const int W = 64, H = 32;
    auto idx = [&](int x, int y) { return (size_t)y * W + x; };

    // ── MUNDO 1: un continente con una cuenca cerrada de fondo conocido ─────────────────────────
    //
    // Terreno a +100 m, un mar en la franja izquierda (por debajo de 0) y una cuenca cuadrada a
    // +10 m rodeada de pared a +100. El punto de derrame es la pared: la cuenca tiene que llenarse
    // hasta 100 m EXACTOS, o sea 90 m de fondo.
    std::vector<float> land((size_t)W * H, 100.0f);
    for (int y = 0; y < H; ++y) for (int x = 0; x < 6; ++x) land[idx(x, y)] = -500.0f;  // océano
    for (int y = 12; y < 20; ++y) for (int x = 30; x < 38; ++x) land[idx(x, y)] = 10.0f; // cuenca
    const WaterFillResult r1 = waterFillEquirect(land.data(), W, H, 0.0f);

    double worstLevel = 0.0; size_t basinWet = 0;
    for (int y = 12; y < 20; ++y) for (int x = 30; x < 38; ++x) {
        const float lv = r1.levelM[idx(x, y)];
        if (lv > WATER_FILL_DRY) { ++basinWet; worstLevel = std::max(worstLevel, std::fabs(lv - 100.0)); }
    }
    std::printf("    cuenca cerrada: %zu de 64 texeles con agua · desvio del punto de derrame %.6f m\n",
                basinWet, worstLevel);
    std::printf("    mapa: %zu texeles de mar · %zu de lago · el mas hondo %.1f m\n",
                r1.seaCells, r1.lakeCells, r1.deepestM);

    // Ni una gota fuera de la cuenca: la meseta a +100 no puede retener nada.
    size_t spill = 0;
    for (int y = 0; y < H; ++y) for (int x = 6; x < W; ++x) {
        const bool inBasin = (x >= 30 && x < 38 && y >= 12 && y < 20);
        if (!inBasin && r1.levelM[idx(x, y)] > WATER_FILL_DRY) ++spill;
    }
    std::printf("    texeles con agua FUERA de la cuenca: %zu (debe ser 0: la meseta no retiene)\n", spill);

    // ── MUNDO 2: SIN MAR. El requisito del planeta seco ─────────────────────────────────────────
    std::vector<float> dry((size_t)W * H, 100.0f);
    for (int y = 12; y < 20; ++y) for (int x = 30; x < 38; ++x) dry[idx(x, y)] = 10.0f;  // la MISMA cuenca
    const WaterFillResult r2 = waterFillEquirect(dry.data(), W, H, 0.0f);
    std::printf("    planeta SECO (misma cuenca, sin mar): %zu de mar · %zu de lago · hasWater=%d\n",
                r2.seaCells, r2.lakeCells, (int)r2.hasWater);

    // ── MUNDO 3: la costura de longitud NO es un borde ──────────────────────────────────────────
    //
    // La misma cuenca, pero a caballo del meridiano ±180 (mitad en x=62,63 y mitad en x=0,1). Si el
    // mapa se tratara como un rectángulo, ese meridiano sería un BORDE por el que el agua se fuga y
    // la cuenca no se llenaría. Con la esfera bien cosida se llena igual que la de en medio.
    std::vector<float> seam((size_t)W * H, 100.0f);
    for (int y = 0; y < H; ++y) for (int x = 8; x < 14; ++x) seam[idx(x, y)] = -500.0f;   // océano
    for (int y = 12; y < 20; ++y) { for (int x = 60; x < 64; ++x) seam[idx(x, y)] = 10.0f;
                                    for (int x = 0;  x < 4;  ++x) seam[idx(x, y)] = 10.0f; }
    const WaterFillResult r3 = waterFillEquirect(seam.data(), W, H, 0.0f);
    size_t seamWet = 0;
    for (int y = 12; y < 20; ++y) { for (int x = 60; x < 64; ++x) if (r3.levelM[idx(x,y)] > WATER_FILL_DRY) ++seamWet;
                                    for (int x = 0;  x < 4;  ++x) if (r3.levelM[idx(x,y)] > WATER_FILL_DRY) ++seamWet; }
    std::printf("    cuenca a caballo del meridiano +-180: %zu de 64 texeles con agua\n", seamWet);

    // ── DETERMINISMO: dos pasadas, los mismos bits ──────────────────────────────────────────────
    const WaterFillResult r1b = waterFillEquirect(land.data(), W, H, 0.0f);
    bool identical = (r1b.levelM.size() == r1.levelM.size());
    for (size_t c = 0; identical && c < r1.levelM.size(); ++c)
        if (r1b.levelM[c] != r1.levelM[c]) identical = false;

    CHECK(basinWet == 64, "la cuenca cerrada se llena ENTERA");
    CHECK(worstLevel == 0.0, "y EXACTAMENTE hasta su punto de derrame (no 'cerca': el mismo numero)");
    CHECK(spill == 0, "CONTRAPRUEBA: la meseta no retiene ni un texel — el relleno no inunda de mas");
    // El requisito de Andoni: un cuerpo sin oceano no tiene agua, y sale del CAMPO, no de un ajuste.
    CHECK(r2.lakeCells == 0 && !r2.hasWater,
          "planeta SIN MAR: cero agua y `hasWater` false, DERIVADO del campo y no configurado");
    CHECK(r1.hasWater, "CONTRAPRUEBA: el que si tiene mar da hasWater true (si no, (2) no diria nada)");
    CHECK(seamWet == 64, "la costura de longitud NO es un borde: la cuenca del meridiano se llena igual");
    CHECK(identical, "el relleno es DETERMINISTA bit a bit (dos clientes tienen que ver los mismos lagos)");
}

/**
 * @brief EL FETCH: un lago de montaña deja de tener olas de mar abierto.
 *
 * Era la limitación 2 del agua, medida: un lago de 3,21 m de fondo recibía **3,46 m de ola**, más
 * alta que profundo era el lago, porque el oleaje sale de UN estado global (el viento sobre el
 * océano) y el bajío lo AMPLIFICA en agua somera. Ni la dispersión de profundidad finita lo arregla
 * —cambia la longitud de onda, no la altura— porque la altura la fijan Green y el tope de rompiente.
 *
 * ⚠️ EL ORÁCULO ES JONSWAP, no un número elegido: `Hs = 0,0016·U·sqrt(F/g)` contra el
 * `Hs = 0,21·U²/g` de Pierson-Moskowitz. Lo que se comprueba es que el factor los relaciona.
 */
void test_ocean_fetch() {
    beginTest("ocean_fetch");
    using namespace Haruka::Planet;
    const OceanState st = oceanDefaultState();

    // El viento que el estado de referencia representa, deducido de su propia Hs (ver la función).
    float sum2 = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; ++i) sum2 += st.wave[i][1] * st.wave[i][1];
    const float Hs = 4.0f * std::sqrt(sum2 * 0.5f);
    const float U  = std::sqrt(Hs * OCEAN_G / 0.21f);
    std::printf("    estado de referencia: Hs %.2f m -> viento deducido %.1f m/s (el declarado es %.1f)\n",
                Hs, U, OCEAN_REF_WIND);

    std::printf("    fetch        factor    Hs resultante   (Hs de mar abierto %.2f m)\n", Hs);
    for (float F : { 100.0f, 400.0f, 2000.0f, 20000.0f, 200000.0f, WATER_FETCH_UNLIMITED }) {
        const float f = oceanFetchFactor(st, F);
        std::printf("    %9.0f m  %8.4f   %8.3f m\n", F, f, f * Hs);
    }

    // La ola de verdad en el lago que se medía: 3,21 m de fondo, 400 m de diámetro.
    const glm::vec3 up(0, 1, 0), wp(1234.0f, 0.0f, -567.0f);
    float pkOpen = 0.0f, pkLake = 0.0f;
    float loOpen = 1e9f, loLake = 1e9f;
    for (int k = 0; k < 240; ++k) {
        const float t = 0.05f * (float)k;
        const float hO = oceanWaveHeight(wp, up, t, 40.0f, 1.0f, st, WATER_FETCH_UNLIMITED);
        const float hL = oceanWaveHeight(wp, up, t, 3.21f, 1.0f, st, 400.0f);
        pkOpen = std::max(pkOpen, hO); loOpen = std::min(loOpen, hO);
        pkLake = std::max(pkLake, hL); loLake = std::min(loLake, hL);
    }
    const float ptpOpen = pkOpen - loOpen, ptpLake = pkLake - loLake;
    std::printf("    ola pico a pico: mar abierto (40 m) %.3f m · lago de montana (3,21 m fondo, "
                "400 m de diametro) %.3f m\n", ptpOpen, ptpLake);
    std::printf("    razon lago/mar: %.3f  (antes del fetch era 1,08 — el lago tenia MAS ola)\n",
                ptpOpen > 1e-6f ? ptpLake / ptpOpen : 0.0f);

    // ── LO QUE SE AFIRMA ────────────────────────────────────────────────────────────────────────
    CHECK(std::fabs(oceanFetchFactor(st, WATER_FETCH_UNLIMITED) - 1.0f) < 1e-6f,
          "el OCEANO no se limita: factor 1 exacto");
    CHECK(ptpLake < ptpOpen * 0.25f,
          "el lago de montana ya NO hereda el swell del oceano: menos de un cuarto de su ola");
    CHECK(ptpLake < 3.21f * 0.5f,
          "y la ola cabe holgada en el lago (antes era MAS alta que profundo era el lago)");
    // CONTRAPRUEBA: sin el fetch, la MISMA llamada da la ola grande. Si no, este test no estaria
    // midiendo el fetch sino la profundidad.
    float pkNo = 0.0f, loNo = 1e9f;
    for (int k = 0; k < 240; ++k) {
        const float h = oceanWaveHeight(wp, up, 0.05f * (float)k, 3.21f, 1.0f, st,
                                        WATER_FETCH_UNLIMITED);
        pkNo = std::max(pkNo, h); loNo = std::min(loNo, h);
    }
    std::printf("    CONTRAPRUEBA: el MISMO lago sin limitar el fetch da %.3f m (x%.1f)\n",
                pkNo - loNo, ptpLake > 1e-6f ? (pkNo - loNo) / ptpLake : 0.0f);
    CHECK((pkNo - loNo) > ptpLake * 4.0f,
          "CONTRAPRUEBA: sin fetch el MISMO punto da mas de cuatro veces la ola (es el fetch, no el fondo)");
    // Un mar interior grande SI tiene oleaje: el fetch no es un interruptor de "lago = sin olas".
    CHECK(oceanFetchFactor(st, 200000.0f) > 0.5f,
          "un mar interior de 200 km si levanta ola: el fetch gradua, no apaga");
}

/**
 * @brief ¿HASTA DÓNDE HAY OLAS, Y HAY HUECOS POR EL CAMINO?
 *
 * Reportado por Andoni: *"el agua de la costa funciona con el método anterior de clipmap según si
 * está cerca o no dentro del cuadrado"*. El mar cercano no tiene geometría propia: reusa la rejilla
 * de anillos que dejó el clipmap, que es CUADRADA y va anclada bajo la cámara.
 *
 * Aquí se replica la ley del shader (`ocean.tese`) sobre el reparto real de anillos y se recorre un
 * radio. El gemelo es de dos líneas y está a la vista, así que lo que salga es lo que dibuja.
 *
 * ⚠️ LA SOSPECHA A COMPROBAR: el desvanecido es `1 - smoothstep(cover*0.85, cover*0.98, rad)` y
 * `cover` es el semi-lado **DE CADA ANILLO**, no del último. La intención escrita al lado dice otra
 * cosa —*"que el ÚLTIMO anillo entregue la misma superficie que la esfera lisa"*—, así que si el
 * dato es por anillo, cada uno apaga su propio 15% exterior y quedan ANILLOS PLANOS concéntricos.
 */
void test_ocean_wave_reach() {
    beginTest("ocean_wave_reach");
    using namespace Haruka::Planet;

    // El reparto real: `m_ringCoverM = NC·PATCH/2` (NC=31 en calidad Low) y el bucle del dibujo
    // corta en `kWaveReachM` = 16 km.
    const double PATCH = TERRAIN_CLIP_PATCH_M;
    const double cover0 = 31.0 * PATCH * 0.5;
    // DOS alcances distintos, y confundirlos era el bug: la GEOMETRÍA del agua llega a 60 km (lámina
    // lisa, para que un lago lejano tenga con qué dibujarse) y la OLA se desvanece mucho antes.
    const double kWaterReachM = 60000.0;   // gemelo del bucle del dibujo
    const double kWaveReachM  = 16000.0;   // gemelo del que calcula `waveOuterM`
    int rings = 0, waveRings = 0;
    while (cover0 * std::exp2((double)rings) <= kWaterReachM || rings == 0) { ++rings; if (rings > 16) break; }
    while (cover0 * std::exp2((double)waveRings) <= kWaveReachM || waveRings == 0) { ++waveRings; if (waveRings > 16) break; }
    std::printf("    semi-lado del anillo 0: %.0f m\n", cover0);
    std::printf("    GEOMETRIA del agua: %d anillos, hasta %.0f m (lamina lisa: los lagos lejanos)\n",
                rings, cover0 * std::exp2((double)(rings - 1)));
    std::printf("    OLA: se desvanece a partir de %.0f m (%d anillos)\n",
                cover0 * std::exp2((double)(waveRings - 1)) * 0.85, waveRings);

    // La ley del shader, gemela de `ocean.tese`: para un radio, ¿qué anillo lo cubre y cuánta ola deja?
    // El ALCANCE es uno solo para todos los anillos (`uClipTanV.w`), no el semi-lado de cada uno.
    // Con el dato por anillo —que era lo que había— cada uno apagaba su 15% exterior y quedaban aros
    // de mar liso; el `reachPerRing` de abajo lo reproduce para la contraprueba.
    const double reach = cover0 * std::exp2((double)(waveRings - 1));   // el de la OLA
    auto factorWith = [&](double rad, bool perRing) -> double {
        for (int r = 0; r < rings; ++r) {
            const double half = cover0 * std::exp2((double)r);
            const double hole = (r == 0) ? 0.0 : cover0 * std::exp2((double)(r - 1));
            if (rad < hole || rad > half) continue;            // no es su banda
            const double ref = perRing ? half : reach;
            const double s = ref * 0.85, e = ref * 0.98;
            const double t = std::clamp((rad - s) / (e - s), 0.0, 1.0);
            return 1.0 - (t * t * (3.0 - 2.0 * t));            // smoothstep
        }
        return 0.0;                                            // fuera de todo anillo: esfera lisa
    };
    auto waveFactorAt = [&](double rad) { return factorWith(rad, false); };

    std::printf("    radio (m)   factor de ola\n");
    // ⚠️ SE BARRE HASTA DONDE EMPIEZA EL FUNDIDO EXTERIOR, no hasta el borde. Ese fundido es
    // INTENCIONADO —es lo que hace que el mar cercano entregue la misma superficie que la esfera lisa
    // que lo releva—, así que contarlo como "aro plano" acusaría al arreglo de hacer lo que el diseño
    // pide. Lo que se busca son los aros de EN MEDIO, con oleaje a los dos lados.
    int flatBands = 0; bool inFlat = false; double firstFlat = 0.0;
    for (double rad = 50.0; rad <= reach * 0.85; rad += 50.0) {
        const double f = waveFactorAt(rad);
        const bool flat = (f < 0.02);
        if (flat && !inFlat) { ++flatBands; if (firstFlat == 0.0) firstFlat = rad; }
        inFlat = flat;
    }
    for (double rad : { 500.0, 1500.0, 1750.0, 1900.0, 2100.0, 3400.0, 3900.0, 4200.0,
                        7000.0, 7800.0, 8200.0, 15000.0, 16000.0 })
        std::printf("    %9.0f   %.3f\n", rad, waveFactorAt(rad));
    std::printf("    ANILLOS PLANOS antes del borde exterior: %d (el primero a %.0f m)\n",
                flatBands, firstFlat);

    // ── CONTRAPRUEBA: LA LEY VIEJA, la que usaba el semi-lado de CADA anillo ────────────────────
    //
    // Sin esto, "0 aros planos" no distingue "lo arreglé" de "esta medida no ve aros planos".
    int flatOld = 0; bool inOld = false;
    for (double rad = 50.0; rad <= reach * 0.85; rad += 50.0) {
        const bool flat = (factorWith(rad, true) < 0.02);
        if (flat && !inOld) ++flatOld;
        inOld = flat;
    }
    std::printf("    CONTRAPRUEBA con la ley VIEJA (semi-lado por anillo): %d aros planos\n", flatOld);

    // ⚠️ EL CUADRADO. La rejilla es cuadrada y el desvanecido es RADIAL, así que en las esquinas hay
    // geometría pero el factor ya vale 0: el alcance en diagonal es sqrt(2) veces el del eje, y toda
    // esa banda va plana.
    // ⚠️ LO QUE ESTE ARREGLO NO TOCA: la rejilla sigue siendo CUADRADA y anclada bajo la cámara, y el
    // fundido es RADIAL. El oleaje vive en un DISCO inscrito en el cuadrado: en la diagonal hay
    // geometría hasta sqrt(2) veces más lejos y va plana. Y todo ello se mueve contigo.
    std::printf("    PENDIENTE: rejilla CUADRADA, fundido RADIAL — ola en un disco de %.0f m,\n"
                "      geometria hasta %.0f m en diagonal (plana), y todo anclado bajo la camara\n",
                reach * 0.85, reach * 1.41421356);

    CHECK(rings >= 3, "se dibujan varios anillos de mar");
    CHECK(cover0 * std::exp2((double)(rings - 1)) > 30000.0,
          "la GEOMETRIA del agua llega mas alla de 30 km: un lago lejano tiene con que dibujarse");
    CHECK(reach < cover0 * std::exp2((double)(rings - 1)),
          "y la OLA se desvanece ANTES que la geometria (lamina lisa de ahi en adelante)");
    // Lo que se afirma: si el desvanecido fuera SOLO del ultimo anillo (la intencion escrita en el
    // shader), no habria ni una banda plana antes del borde. Cada una es un aro de mar liso.
    CHECK(flatBands == 0, "no hay ANILLOS PLANOS de mar antes del borde exterior");
    CHECK(flatOld >= 2, "CONTRAPRUEBA: la ley vieja SI los tenia (si no, este test no mide el arreglo)");
}

/**
 * @brief ¿PUEDE LA REJILLA LLEVAR LA OLA QUE SE LE PIDE? Nyquist contra la distancia.
 *
 * El campo de olas es función pura de la posición del mundo (el marco sale del radial POR VÉRTICE,
 * no de la cámara), así que la rejilla moviéndose no mueve la ola. Lo que sí decide es si la ola
 * CABE: la superficie dibujada es lineal a trozos entre vértices, y una ola de 8,7 m sobre un quad
 * de 512 m no es una ola, es muaré.
 *
 * ⚠️ Y AQUÍ HAY DOS LEYES QUE NO SE HABLAN. La teselación se desvanece con `smoothstep(1200, 9000)`
 * en `ocean.tesc` —a 9 km el quad ya es el parche entero— mientras la AMPLITUD se desvanece mucho
 * más lejos, a partir de 13,5 km. Entre medias se dibuja ola a plena amplitud sobre una rejilla que
 * no puede llevarla.
 *
 * Se replican las dos leyes y se cuentan las MUESTRAS POR LONGITUD DE ONDA. Nyquist pide 2.
 */
void test_ocean_grid_carries_wave() {
    beginTest("ocean_grid_carries_wave");
    using namespace Haruka::Planet;
    const OceanState st = oceanDefaultState();
    const double PATCH = TERRAIN_CLIP_PATCH_M;
    const double cover0 = 31.0 * PATCH * 0.5;

    // Gemelo de `edgeF` en `ocean.tesc`: el lado del quad a una distancia dada.
    auto quadAt = [&](double rad) {
        int r = 0;
        while (r < 5 && cover0 * std::exp2((double)r) < rad) ++r;
        const double arc = PATCH * std::exp2((double)r);      // lado del parche de ese anillo
        const double t   = std::clamp((rad - 1200.0) / (9000.0 - 1200.0), 0.0, 1.0);
        const double s   = t * t * (3.0 - 2.0 * t);
        const double lvl = std::clamp(arc / 4.0 * (1.0 - s), 1.0, 32.0);
        return arc / lvl;
    };
    // Gemelo del desvanecido de amplitud de `ocean.tese` (alcance de la ola).
    const double reach = cover0 * std::exp2(3.0);             // 4 anillos de ola
    auto ampFadeAt = [&](double rad) {
        const double s = reach * 0.85, e = reach * 0.98;
        const double t = std::clamp((rad - s) / (e - s), 0.0, 1.0);
        return 1.0 - (t * t * (3.0 - 2.0 * t));
    };

    const double lamShort = st.wave[OCEAN_WAVES - 1][0];      // el tren mas corto (8,7 m)
    const double lamLong  = st.wave[0][0];                    // el mas largo (61 m)
    std::printf("    tren mas corto %.1f m · mas largo %.1f m · Nyquist pide 2 muestras por onda\n",
                lamShort, lamLong);
    std::printf("    distancia    quad     muestras/onda corta   muestras/onda larga   amplitud\n");
    // El desvanecido POR TREN según el quad local, gemelo de `harukaShortWaveFade(k, 2·quadM)`.
    auto trainAmpAt = [&](double lam, double rad) {
        const double lo = std::max(quadAt(rad), (double)OCEAN_MIN_QUAD_M), hi = lo * 2.0;
        return std::clamp((lam - lo) / (hi - lo), 0.0, 1.0) * ampFadeAt(rad);
    };
    double worstShort = 1e9, worstLong = 1e9; double firstBad = 0.0;
    for (double rad : { 100.0, 500.0, 1200.0, 2000.0, 3000.0, 5000.0, 9000.0, 12000.0, 13000.0 }) {
        const double q = quadAt(rad);
        const double nS = lamShort / q, nL = lamLong / q;
        std::printf("    %8.0f m %8.1f m %14.2f %21.2f %11.2f\n",
                    rad, q, nS, nL, trainAmpAt(lamLong, rad));
        // Sólo cuenta donde el tren SE DIBUJA de verdad: si su amplitud ya está apagada, que la
        // rejilla no lo pueda llevar da igual — es justo lo que el desvanecido viene a conseguir.
        //
        // ⚠️ EL UMBRAL ES 20 %, NO 5 %. La banda de desvanecido es continua, así que siempre hay un
        // radio donde un tren está al 8 % y ligeramente por debajo de Nyquist; pedirle a ESO que
        // cumpla Nyquist es pedir que la rampa sea un escalón, y un escalón se ve como un anillo. A
        // 20 % de amplitud el tren aporta centímetros sobre metros y su aliasing no es visible.
        if (trainAmpAt(lamShort, rad) > 0.20) worstShort = std::min(worstShort, nS);
        if (trainAmpAt(lamLong,  rad) > 0.20) {
            worstLong = std::min(worstLong, nL);
            if (nL < 2.0 && firstBad == 0.0) firstBad = rad;
        }
    }
    std::printf("    con amplitud VIVA: peor %.2f muestras/onda corta · %.2f de la larga\n",
                worstShort, worstLong);
    if (firstBad > 0.0)
        std::printf("    ⚠ desde %.0f m se dibuja la ola LARGA por debajo de Nyquist (muaré)\n", firstBad);

    // ── CONTRAPRUEBA: LA LEY VIEJA, con el piso constante de 8 m ────────────────────────────────
    //
    // Sin ella, "todo por encima de Nyquist" no distingue el arreglo de una medida que no mira nada.
    double oldWorstShort = 1e9, oldWorstLong = 1e9;
    for (double rad : { 2000.0, 5000.0, 9000.0, 12000.0 }) {
        if (ampFadeAt(rad) <= 0.20) continue;
        oldWorstShort = std::min(oldWorstShort, lamShort / quadAt(rad));
        oldWorstLong  = std::min(oldWorstLong,  lamLong  / quadAt(rad));
    }
    std::printf("    CONTRAPRUEBA con el piso FIJO de 8 m: %.2f muestras/onda corta · %.2f de la larga\n",
                oldWorstShort, oldWorstLong);

    // ── ⚠️ Y QUE A LOS PIES DEL JUGADOR NO SE APAGUE NADA ───────────────────────────────────────
    //
    // Este es el test que faltaba y que dejo pasar "se ve plano". Con el quad mas fino (4 m) los
    // CUATRO trenes tienen que estar vivos: el mas corto mide 8,7 m, o sea 2,17 muestras por onda —
    // Nyquist cumplido. La banda de desvanecido estuvo una octava corrida y lo dejaba al **8,75 %**,
    // asi que el mar perdia su rizo justo donde mas se mira.
    double minNear = 1.0;
    for (int i = 0; i < OCEAN_WAVES; ++i)
        minNear = std::min(minNear, (double)oceanShortWaveFade(
                      6.2831853f / st.wave[i][0], OCEAN_MIN_QUAD_M));
    std::printf("    a los PIES (quad %.1f m): el tren mas apagado de los cuatro esta al %.1f %%\n",
                (double)OCEAN_MIN_QUAD_M, 100.0 * minNear);
    CHECK(minNear > 0.99,
          "a los pies del jugador NINGUN tren se apaga: el quad fino los lleva todos");
    // CONTRAPRUEBA: con el quad de 2 km (8 m) el mas corto SI se apaga. Si no, el desvanecido no
    // estaria haciendo nada y el test de arriba no diria nada.
    const double farShort = (double)oceanShortWaveFade(
                                6.2831853f / st.wave[OCEAN_WAVES - 1][0], 8.0f);
    std::printf("    CONTRAPRUEBA a 2 km (quad 8 m): el tren corto baja al %.1f %%\n", 100.0 * farShort);
    CHECK(farShort < 0.2, "CONTRAPRUEBA: con el quad grueso el tren corto SI se apaga (Nyquist)");

    CHECK(worstLong >= 2.0, "la rejilla puede llevar la ola LARGA en todo el alcance en que se dibuja");
    CHECK(worstShort >= 2.0, "y la CORTA tambien");
    CHECK(oldWorstLong < 1.0, "CONTRAPRUEBA: con el piso fijo la ola larga SI iba por debajo de Nyquist");
    // CONTRAPRUEBA: cerca la rejilla sobra de largo. Si tambien fallara ahi, la medida estaria mal.
    CHECK(lamShort / quadAt(100.0) > 2.0,
          "CONTRAPRUEBA: cerca del jugador la rejilla lleva hasta la ola mas corta");
}

/**
 * @brief ¿QUÉ LAGO MÁS PEQUEÑO CABE EN EL BAKE? El límite de resolución del campo de agua.
 *
 * ⚠️ NO ES UN FALLO QUE ARREGLAR: ES EL LÍMITE DE LO QUE SE CONSTRUYÓ, y hay que tenerlo escrito
 * porque no es evidente. `bakeWaterMap` rellena las cuencas sobre el bake de altura, que es equirect
 * a `m_mapRes` (512 por defecto). Un téxel cubre `2πR/W` metros — en la Tierra, **78 km**. Un lago
 * más pequeño que un téxel no existe para el relleno: ni se llena, ni sale en la textura, ni la
 * física lo ve.
 *
 * O sea que el campo horneado representa MARES INTERIORES, no lagos. Los lagos de verdad (1-50 km)
 * siguen dependiendo del parche dinámico, que es local. Cerrarlo pide direccionar el agua por el
 * quadtree del terreno —(cara, nivel, i, j)— en vez de por un equirect: era la propuesta original y
 * quedó a medias, porque el relleno sí es global pero su rejilla no.
 */
void test_water_bake_resolution_limit() {
    beginTest("water_bake_resolution_limit");
    using namespace Haruka::Planet;
    const double R = 6371000.0;
    const double kPi = 3.14159265358979323846;

    std::printf("    texel del bake de agua y lago minimo que puede representar:\n");
    std::printf("      resolucion     texel (ecuador)    lago minimo (~2 texeles)\n");
    double texel512 = 0.0;
    for (int w : { 512, 1024, 2048, 4096, 8192 }) {
        const double texel = 2.0 * kPi * R / (double)w;
        if (w == 512) texel512 = texel;
        std::printf("      %5d x %-5d %13.0f m %20.0f m\n", w, w / 2, texel, texel * 2.0);
    }

    // Y comprobado sobre el relleno REAL: una cuenca de un solo texel da un fetch del orden del
    // texel, no del lago que uno se imagina.
    const int W = 128, H = 64;
    std::vector<float> land((size_t)W * H, 100.0f);
    for (int y = 0; y < H; ++y) for (int x = 0; x < 8; ++x) land[(size_t)y * W + x] = -500.0f;
    land[(size_t)32 * W + 64] = 10.0f;                       // UNA sola celda hundida
    const WaterFillResult one = waterFillEquirect(land.data(), W, H, 0.0f, R);
    const double texel128 = 2.0 * kPi * R / (double)W;
    std::printf("    cuenca de UN texel a %.0f km/texel: %zu texeles de lago · fetch %.0f m\n",
                texel128 / 1000.0, one.lakeCells, one.biggestFetchM);

    // ── LO QUE SE AFIRMA ────────────────────────────────────────────────────────────────────────
    //
    // No hay nada que "pase" o "falle" en la fisica: se fija el LIMITE por escrito, para que el dia
    // que alguien pregunte "por que no hay lagos" la respuesta este medida y no haya que buscarla.
    CHECK(texel512 > 50000.0,
          "a 512 de resolucion el texel del bake pasa de 50 km: el campo horneado da MARES "
          "INTERIORES, no lagos — los lagos siguen siendo del parche dinamico");
    CHECK(one.lakeCells > 0 && one.biggestFetchM > texel128 * 0.5,
          "y el lago mas pequeno posible es del tamano de un texel, no menor");
    // CONTRAPRUEBA: subiendo la resolucion el limite baja proporcionalmente. Si no lo hiciera, el
    // problema no seria de resolucion y la salida propuesta (rejilla del quadtree) seria la equivocada.
    const double texel8k = 2.0 * kPi * R / 8192.0;
    std::printf("    a 8192 el texel baja a %.1f km: la salida es la RESOLUCION, no el algoritmo\n",
                texel8k / 1000.0);
    CHECK(texel8k < texel512 / 10.0,
          "CONTRAPRUEBA: el limite escala con la resolucion — es de rejilla, no del relleno");
}

// ------------------------------------------ TEST: la ORBITA de Gerstner es una elipse de fondo ---
// La orbita de una particula bajo una ola de Gerstner es una ELIPSE de semiejes `A/tanh(k·d)`
// (horizontal) y `A` (vertical): circulo en agua profunda, ancha y aplastada en somero.
//
// ⚠️⚠️ UNA PREMISA MIA QUE ERA FALSA, Y LA DEJO ESCRITA PORQUE CASI ME LLEVA A "ARREGLAR" ALGO QUE NO
// ESTABA ROTO. Yo dije que el reparto viejo daba "2-3 veces la amplitud" de desplazamiento horizontal
// porque `Q·A = 0.75/(k·N)` no depende de `A`. Me deje el `min(..., 1)`: el reparto real es
//
//     horizontal = min(0.75/(k·A·N), 1) · A = **min(A, 0.75/(k·N))**
//
// o sea el MENOR de los dos. Con la tabla de referencia en agua profunda, `A` siempre es el menor, asi
// que la formula vieja YA daba un circulo exacto — medido aqui, x1.0. La independencia de la amplitud
// solo aparece cuando la ola es MAS escarpada que el tope, que es el tope haciendo su trabajo.
//
// Lo que si cambia la elipse es el SOMERO: `A/tanh(k·d)` crece al perder fondo, que es lo que hace el
// agua de verdad (en poca agua te mueve de lado mucho mas que arriba y abajo), mientras que la vieja
// se quedaba en `A` o por debajo. Medido: en una charca de 30 cm el vaiven pasa de 0,10 m a 0,54 m.
//
// ⚠️ Y LA VOLUTA SIGUE SIN SALIR, medido y no supuesto (caso 5).
void test_ocean_gerstner_ellipse() {
    beginTest("ocean_gerstner_ellipse");
    using namespace Haruka::Planet;
    const OceanState st = oceanDefaultState();

    // (1) AGUA PROFUNDA: circulo (horizontal == vertical == A). Lo cumplen LAS DOS formulas — que es
    //     justo lo que hay que comprobar para no romper el mar abierto al tocar el somero.
    std::printf("    tren     λ(m)   A(m)   horiz VIEJO   horiz NUEVO   (deberia ser A)\n");
    double peorRel = 0.0, peorViejo = 0.0;
    for (int i = 0; i < OCEAN_WAVES; ++i) {
        const float k = 6.2831853f / st.wave[i][0];       // en profundo k == k0
        const float A = st.wave[i][1];
        const float viejo = oceanSteepness(k, A, 0.0f) * A;
        const float nuevo = oceanHorizAmp(k, A, 4000.0f);  // 4 km de fondo = profundo
        std::printf("      %d    %5.1f  %5.2f   %8.3f      %8.3f\n", i, st.wave[i][0], A, viejo, nuevo);
        peorRel   = std::max(peorRel,   (double)std::fabs(nuevo - A) / A);
        peorViejo = std::max(peorViejo, (double)std::fabs(viejo - A) / A);
    }
    CHECK(peorRel   < 0.01, "NUEVO: en agua PROFUNDA la orbita es un circulo (horizontal == A, <1 %)");
    CHECK(peorViejo < 0.01, "VIEJO: tambien lo era — el mar abierto NO cambia con este arreglo");

    // (2) DONDE SI se separaban: cuando la ola es mas escarpada que el tope, el reparto viejo deja de
    //     depender de la amplitud. Con `k=0.5`, el tope vale 0,375 m: una ola de 1 m se recorta a eso.
    {
        const float k = 0.5f;
        const float vGrande = oceanSteepness(k, 1.00f, 0.0f) * 1.00f;
        const float vChica  = oceanSteepness(k, 0.05f, 0.0f) * 0.05f;
        const float nGrande = oceanHorizAmp(k, 1.00f, 100.0f);
        const float nChica  = oceanHorizAmp(k, 0.05f, 100.0f);
        std::printf("    misma λ, A=1,00 vs A=0,05:  VIEJO %.3f / %.3f (x%.1f)   NUEVO %.3f / %.3f (x%.1f)\n",
                    vGrande, vChica, vGrande / vChica, nGrande, nChica, nGrande / nChica);
        CHECK(vGrande / vChica < 10.0f,
              "VIEJO: 20x de ola daba solo 7,5x de vaiven (el tope se comia la diferencia)");
        CHECK(std::fabs(nGrande / nChica - 20.0f) < 0.1f,
              "NUEVO: el horizontal escala con la amplitud (x20 de ola = x20 de vaiven)");
    }

    // (3) EN SOMERO la elipse se ensancha, que es lo que hace el agua de verdad.
    {
        const float k0 = 6.2831853f / st.wave[0][0];
        const float k  = oceanWaveNumber(k0, 2.0f);
        const float A  = 0.3f;
        const float r  = oceanHorizAmp(k, A, 2.0f) / A;
        std::printf("    a 2 m de fondo la orbita es %.2f veces mas ancha que alta\n", r);
        CHECK(r > 1.5f, "en somero la orbita se ensancha (horizontal > vertical)");
    }

    // (4) ⚠️ EL PLIEGUE, MEDIDO — y las DOS cifras, con tope y sin el. Sin la de "sin tope" esto no
    //     distingue "la ola no puede plegarse" de "elegimos que no se pliegue", que es otra cosa.
    {
        double peorCap = 0.0, peorLibre = 0.0; float dCap = 0.0f, dLibre = 0.0f;
        for (int i = 0; i < 400; ++i) {
            const float d = 0.05f + (float)i * 0.05f;           // 5 cm .. 20 m
            const float green = oceanGreenGain(d) * oceanFetchFactor(st, WATER_FETCH_UNLIMITED);
            const float brk   = oceanBreakScale(d, st);
            const float ss    = oceanSteepScale(d, st);
            double libre = 0.0;
            for (int j = 0; j < OCEAN_WAVES; ++j) {
                const float k0 = 6.2831853f / st.wave[j][0];
                const float k  = oceanWaveNumber(k0, d);
                const float amp = st.wave[j][1] * green * brk * oceanShortWaveFade(k);
                libre += (double)k * oceanHorizAmp(k, amp, d);
            }
            if (libre > peorLibre)      { peorLibre = libre;      dLibre = d; }
            if (libre * ss > peorCap)   { peorCap   = libre * ss; dCap   = d; }
        }
        std::printf("    escarpado total en 5 cm..20 m de fondo:  CON tope %.3f (a %.2f m)  ·  "
                    "SIN tope %.3f (a %.2f m)  ·  pliega si >1\n", peorCap, dCap, peorLibre, dLibre);
        // ⚠️ ESTA LINEA DECIA "el escarpado NUNCA pasa del techo" Y YA NO ES CIERTO, a proposito: desde
        // que hay rompiente (`oceanCurlGain`) el techo SUBE donde la ola revienta, que es justo lo que
        // permite que la cresta se pliegue. Lo que sigue siendo cierto —y es lo que hay que fijar— es
        // que **el cuerpo de la ola, sin el adelanto de cresta, no se pliega solo**: el pliegue es cosa
        // de la cresta y de la franja de rompiente, no del mar entero. Ver `ocean_refraction_and_break`.
        CHECK(peorCap <= 1.0 + 1e-3,
              "el cuerpo de la ola (sin el adelanto de cresta) NO se pliega por si solo");
        // ⚠️ La cifra SIN tope es la que dice si la voluta esta al alcance con la ola SIMETRICA. Salio
        // 0,818 —por debajo de 1— y ese fue el dato que obligo a meter asimetria en vez de mas
        // escarpado. Se imprime siempre; el CHECK solo fija que se ha medido.
        CHECK(peorLibre > 0.0, "y la cifra SIN tope queda MEDIDA (la que dice que hacia falta asimetria)");
    }

    // (5) PARIDAD: la velocidad sigue siendo finita y con la misma fase que la altura.
    {
        const glm::vec3 up(0, 1, 0), wp(123.0f, 0.0f, 77.0f);
        const float t = 3.3f, d = 6.0f, eps = 0.01f;
        const glm::vec3 D = glm::normalize(glm::vec3(1, 0, 0));
        const float h0 = oceanWaveHeight(wp - D * eps, up, t, d, 1.0f, st);
        const float h1 = oceanWaveHeight(wp + D * eps, up, t, d, 1.0f, st);
        const glm::vec3 v = oceanWaveVelocity(wp, up, t, d, 1.0f, st);
        std::printf("    pendiente numerica %.5f · velocidad vertical %.5f m/s\n",
                    (h1 - h0) / (2.0f * eps), v.y);
        CHECK(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z),
              "la velocidad sigue siendo finita con la elipse (el suelo del tanh hace su trabajo)");
    }

    // (6) EL CASO QUE ROMPIO EL INTENTO ANTERIOR: una charca de 30 cm sobre celdas de 4,4 m.
    {
        const float d = 0.30f;
        const float green = oceanGreenGain(d) * oceanFetchFactor(st, 200.0f);
        const float brk   = oceanBreakScale(d, st, 200.0f);
        const float ss    = oceanSteepScale(d, st, 200.0f);
        float nuevo = 0.0f, viejo = 0.0f;
        for (int j = 0; j < OCEAN_WAVES; ++j) {
            const float k0 = 6.2831853f / st.wave[j][0];
            const float k  = oceanWaveNumber(k0, d);
            const float amp = st.wave[j][1] * green * brk * oceanShortWaveFade(k);
            nuevo += oceanHorizAmp(k, amp, d) * ss;
            viejo += oceanSteepness(k, amp, 0.0f) * amp;
        }
        std::printf("    charca de 30 cm: vaiven horizontal VIEJO %.3f m · NUEVO %.3f m (celda 4,4 m)\n",
                    viejo, nuevo);
        CHECK(nuevo < 4.4f, "la charca no se desplaza mas que su propia celda (no hay cubos deformados)");
    }
}

// ------------------------- TEST: el relleno de DOS NIVELES encuentra lagos que el global no ve ---
// ⚠️ EL LIMITE QUE CIERRA. `water_bake_resolution_limit` deja medido que el bake de agua a 512 son
// 78,2 km/texel en la Tierra, asi que una cuenca menor que eso no existe en el campo: salen mares
// interiores, no lagos. Y subir la resolucion global no llega — a 8192 el texel sigue siendo 4,9 km
// y cuesta 402 MB, dos ordenes por encima del tamaño de un lago.
//
// La salida es partir el problema: la COTA DE DERRAME es global (depende de la cuenca entera) y la
// ORILLA es local (`h(x) < nivel`). `waterFillWindow` hace el segundo paso sobre el terreno de verdad
// con el relleno grueso de condicion de contorno.
//
// Este test pone una cuenca de ~3 km en un planeta terrestre y comprueba las dos mitades:
//   · el relleno GLOBAL a 512 no la ve (es sub-texel) — si la viera, la ventana no haria falta,
//   · la VENTANA si la ve, con cota y fetch plausibles,
//   · y la condicion de contorno IMPORTA: sin ella la ventana saldria seca.
void test_water_fill_window() {
    beginTest("water_fill_window");
    using namespace Haruka::Planet;
    const double R = 6371000.0;
    const double kPi = 3.14159265358979323846;

    // Un terreno analitico: meseta a 500 m con un CRATER de 3 km de radio y 40 m de hondo, y mar
    // lejos. Se define como funcion de (lon,lat) para que la ventana pueda muestrearlo tan fino como
    // quiera — que es justo lo que el bake grueso no puede.
    const double lonL = 0.6, latL = 0.3;      // centro del lago
    const double radLagoM = 3000.0;
    auto terreno = [&](double lon, double lat) -> float {
        // Distancia angular al centro del lago -> metros.
        const double dx = (lon - lonL) * std::cos(lat), dy = lat - latL;
        const double dM = std::sqrt(dx*dx + dy*dy) * R;
        float hM = 500.0f;
        if (dM < radLagoM) {
            const double t = dM / radLagoM;
            hM = (float)(500.0 - 40.0 * (1.0 - t*t));    // cuenco parabolico de 40 m
        }
        // Y un mar en el otro hemisferio, para que el relleno global tenga semilla.
        if (lon < -1.0) hM = -200.0f;
        return hM;
    };

    // ── (1) EL RELLENO GLOBAL, a la resolucion del bake. ────────────────────────────────────────
    const int GW = 512, GH = 256;
    std::vector<float> global((size_t)GW * GH);
    for (int y = 0; y < GH; ++y) for (int x = 0; x < GW; ++x) {
        const double lon = (((double)x + 0.5) / GW - 0.5) * 2.0 * kPi;
        const double lat = (0.5 - ((double)y + 0.5) / GH) * kPi;
        global[(size_t)y * GW + x] = terreno(lon, lat);
    }
    const WaterFillResult g = waterFillEquirect(global.data(), GW, GH, 0.0f, R);
    const double texelG = 2.0 * kPi * R / GW;
    std::printf("    GLOBAL %dx%d (%.0f km/texel): %zu texeles de lago · %zu masas\n",
                GW, GH, texelG / 1000.0, g.lakeCells, g.bodies);
    CHECK(g.seaCells > 0, "el relleno global tiene mar (si no, no habria semilla y todo saldria seco)");
    CHECK(g.lakeCells == 0,
          "el GLOBAL no ve un lago de 3 km: es sub-texel a 78 km/texel (por eso hace falta el paso fino)");

    // El relleno grueso, consultado por la ventana en su borde.
    auto nivelGrueso = [&](double lon, double lat) -> float {
        double u = lon / (2.0 * kPi) + 0.5;
        u -= std::floor(u);
        int x = (int)(u * GW); if (x >= GW) x = GW - 1;
        int y = (int)((0.5 - lat / kPi) * GH);
        y = y < 0 ? 0 : (y >= GH ? GH - 1 : y);
        return g.levelM[(size_t)y * GW + x];
    };

    // ── (2) LA VENTANA, sobre el terreno de verdad. ─────────────────────────────────────────────
    const double medio = 8000.0 / R;    // ventana de ±8 km alrededor del lago
    const WaterWindowResult win = waterFillWindow(256, 256, lonL, latL, medio, medio, R,
                                                 terreno, nivelGrueso, 0.0f);
    std::printf("    VENTANA 256x256 (%.0f m/texel): %zu texeles de lago · %zu masas · hondo %.1f m\n"
                "    fetch %.0f m · masas cortadas por el borde %zu\n",
                win.texelM, win.lakeCells, win.bodies, win.deepestM,
                win.lakeCells ? win.fetchM[(size_t)128*256+128] : 0.0f, win.clippedBodies);
    CHECK(win.lakeCells > 0, "la VENTANA si encuentra el lago que el global no ve");
    CHECK(win.texelM < texelG / 100.0,
          "y lo hace porque su texel es dos ordenes mas fino que el del bake global");
    // La cota tiene que ser la del borde del crater (500 m), no el fondo (460 m).
    const float nivel = win.levelM[(size_t)128*256+128];
    std::printf("    cota de la lamina en el centro: %.2f m (el borde del crater esta a 500 m)\n", nivel);
    CHECK(std::fabs(nivel - 500.0f) < 1.0f,
          "la cota es la del punto de derrame (borde del crater), no el fondo de la cuenca");
    CHECK(win.deepestM > 35.0f && win.deepestM < 41.0f,
          "y la profundidad maxima es la del cuenco (40 m), medida y no supuesta");
    CHECK(win.clippedBodies == 0,
          "el lago cabe entero en la ventana → su fetch es exacto, no una cota inferior");

    // ── (3) ⚠️ LA CONDICION DE CONTORNO SE CONSULTA DE VERDAD, y hay que probarlo: sin esto, un
    //     `waterFillWindow` que ignorara el borde entero daria EXACTAMENTE el mismo 500,00 m de
    //     arriba y el test no notaria nada. Se le dice que fuera hay una lamina a 520 m —un lago
    //     mayor que se derrama hacia aqui— y la ventana tiene que inundarse hasta ESA cota.
    //
    //     ⚠️ Mi primera contraprueba fue mala y la dejo escrita: puse el borde a cota 0 esperando
    //     "salida libre, todo drena", y salio el mismo lago. El motivo es que el nivel del borde
    //     nunca baja de su propia altura (`max(coarse, height)`) — el agua no puede estar bajo el
    //     suelo. O sea que un relleno grueso equivocado puede inundar de mas, pero NO puede vaciar
    //     inventando un desague. Eso es una propiedad buena de la funcion, y la contraprueba tenia
    //     que ir en la direccion en la que si se puede fallar.
    {
        auto lamAlta = [&](double, double) -> float { return 520.0f; };
        const WaterWindowResult sube = waterFillWindow(256, 256, lonL, latL, medio, medio, R,
                                                       terreno, lamAlta, 0.0f);
        const float nivelAlto = sube.levelM[(size_t)128*256+128];
        std::printf("    CONTRAPRUEBA con lamina exterior a 520 m: cota %.2f m · %zu texeles (antes %zu)\n",
                    nivelAlto, sube.lakeCells, win.lakeCells);
        CHECK(std::fabs(nivelAlto - 520.0f) < 1.0f,
              "CONTRAPRUEBA: el borde MANDA — con lamina exterior a 520 m la ventana se inunda a 520, "
              "no a los 500 de su propio crater");
        CHECK(sube.lakeCells > win.lakeCells,
              "y moja mas superficie que con el exterior seco (la ventana no ignora su contorno)");
    }

    // ── (4) Y una ventana en terreno LISO no inventa agua. ──────────────────────────────────────
    {
        auto llano = [](double, double) -> float { return 500.0f; };
        const WaterWindowResult seco = waterFillWindow(128, 128, lonL, latL, medio, medio, R,
                                                       llano, nivelGrueso, 0.0f);
        std::printf("    ventana sobre terreno LISO: %zu texeles de lago\n", seco.lakeCells);
        CHECK(seco.lakeCells == 0, "sobre terreno liso la ventana no inventa lagos");
    }

    // ── (5) EL COSTE, que es lo que decide si esto puede ir por streaming. ──────────────────────
    {
        const auto t0 = std::chrono::high_resolution_clock::now();
        const int reps = 20;
        for (int i = 0; i < reps; ++i)
            (void)waterFillWindow(256, 256, lonL, latL, medio, medio, R, terreno, nivelGrueso, 0.0f);
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - t0).count() / reps;
        std::printf("    coste de una ventana 256x256: %.2f ms (65 536 muestras de terreno + flood)\n", ms);
        CHECK(ms < 200.0, "una ventana cuesta menos de 200 ms: cabe en un hilo de fondo");
    }
}

// -------------------- TEST: la POLITICA de la ventana de lagos (que recuadro, cuando, cuanto) ----
// ⚠️ ESTO ES LA MITAD QUE FALTABA. `waterFillWindow` sabe inundar un recuadro; lo que decide si el
// sistema sirve es CUANDO se recentra. Dos formas de equivocarse, las dos caras:
//   · recalcular al pisar el borde → un jugador andando por el limite dispara un recalculo POR FRAME,
//     y son 65 536 muestras del terreno real cada vez;
//   · recalcular solo al salir del todo → hay frames con el observador FUERA de la ventana, o sea sin
//     lago donde si lo hay.
// La regla es recentrar al salir de la MITAD interior, y lo que compra es margen garantizado.
void test_water_window_policy() {
    beginTest("water_window_policy");
    using namespace Haruka::Planet;
    const double R = 6371000.0, halfM = 8000.0;

    // Centro de la ventana y una direccion a `d` metros de el, por la esfera.
    const double lonC = 0.4, latC = -0.2;
    auto dirAt = [&](double metros) {
        const double ang = metros / R;
        const double lat = latC + ang;                  // se separa en latitud, que es distancia pura
        return glm::dvec3(std::cos(lat) * std::cos(lonC), std::sin(lat), std::cos(lat) * std::sin(lonC));
    };
    const glm::dvec3 centro = dirAt(0.0);

    CHECK(waterWindowNeedsRecenter(centro, false, lonC, latC, halfM, R),
          "sin ventana valida SIEMPRE se calcula (si no, el primer frame no tendria lagos)");
    CHECK(!waterWindowNeedsRecenter(centro, true, lonC, latC, halfM, R),
          "quieto en el centro NO se recalcula");

    // El umbral, barrido: donde esta y que margen deja.
    double umbral = -1.0;
    for (int i = 0; i <= 200; ++i) {
        const double m = (double)i * (halfM / 100.0);   // 0 .. 2·half
        if (waterWindowNeedsRecenter(dirAt(m), true, lonC, latC, halfM, R)) { umbral = m; break; }
    }
    std::printf("    recuadro ±%.0f m · se recentra al pasar de %.0f m del centro (%.0f %% del medio lado)\n"
                "    margen GARANTIZADO por delante en el peor caso: %.0f m\n",
                halfM, umbral, 100.0 * umbral / halfM, halfM - umbral);
    CHECK(umbral > 0.0 && umbral < halfM,
          "el umbral cae DENTRO del recuadro (si fuera >= al medio lado, habria frames sin ventana bajo los pies)");
    CHECK(halfM - umbral >= halfM * 0.45,
          "y deja al menos medio recuadro de margen por delante");

    // ⚠️ CONTRAPRUEBA de que la histeresis existe: sin ella (umbral = el medio lado) el margen seria
    // CERO en el peor caso. Es la diferencia entre "se ve venir el lago" y "aparece bajo los pies".
    {
        const bool alBorde = waterWindowNeedsRecenter(dirAt(halfM * 0.95), true, lonC, latC, halfM, R);
        std::printf("    CONTRAPRUEBA: a 95%% del medio lado ya se ha recentrado hace rato (%s)\n",
                    alBorde ? "si" : "NO");
        CHECK(alBorde, "CONTRAPRUEBA: cerca del borde el sistema YA ha pedido ventana nueva");
    }
    // Y que no se recentra por moverse un metro: si lo hiciera, cada paso costaria el recalculo entero.
    CHECK(!waterWindowNeedsRecenter(dirAt(1.0), true, lonC, latC, halfM, R),
          "andar un metro NO dispara un recalculo");

    // ── EL COSTE REAL, con el terreno de VERDAD (base + detalle), que es lo que decide si esto puede
    //    ir en un hilo o tendria que ir troceado. Con un terreno analitico de juguete la cifra no
    //    valdria: el 73 % del coste de un bake es evaluar el relieve.
    {
        const int HW = 256, HH = 128;
        std::vector<float> base((size_t)HW * HH, 300.0f);
        const float triM = terrainTriM(0.0);
        auto dirOfLL = [](double lo, double la) {
            return glm::dvec3(std::cos(la)*std::cos(lo), std::sin(la), std::cos(la)*std::sin(lo));
        };
        auto terreno = [&](double lo, double la) -> float {
            const glm::dvec3 d = dirOfLL(lo, la);
            const glm::vec2 uv = equirectUV(glm::vec3(d));
            const float baseH = sampleHeightField(uv, HW, HH, base.data());
            float det = terrainDetail(glm::vec3(d), (float)R + baseH, triM) * seaLevelAttenuation(baseH);
            if (baseH > 0.0f) det = glm::max(det, -baseH);
            return baseH + det;
        };
        auto contorno = [](double, double) -> float { return WATER_FILL_DRY; };
        const double hLat = halfM / R, hLon = hLat / std::max(std::cos(latC), 1e-6);
        const auto t0 = std::chrono::high_resolution_clock::now();
        const int reps = 5;
        for (int i = 0; i < reps; ++i)
            (void)waterFillWindow(256, 256, lonC, latC, hLon, hLat, R, terreno, contorno, 0.0f);
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - t0).count() / reps;
        std::printf("    coste con el terreno REAL (base + terrainDetail), 256x256: %.1f ms\n"
                    "    → %s del frame de 16,7 ms: por eso va en un hilo y no en `prepare`\n",
                    ms, ms > 16.7 ? "POR ENCIMA" : "por debajo");
        CHECK(ms > 0.0, "el coste queda MEDIDO con el terreno real, no con uno de juguete");
        // Se recentra cada `halfM/2` = 4 km. A 5 m/s son 800 s entre recalculos: el coste amortizado
        // es despreciable AUNQUE cada uno sea caro. Lo que no puede es bloquear el frame.
        const double segundos = (halfM * 0.5) / 5.0;
        std::printf("    a 5 m/s se recentra cada %.0f s → %.4f ms/frame amortizados\n",
                    segundos, ms / (segundos * 60.0));
        CHECK(ms / (segundos * 60.0) < 1.0, "amortizado cuesta menos de 1 ms por frame");
    }
}

// ---------------------------------- TEST: REFRACCION y ROMPIENTE (los dos que faltaban) ----------
// ⚠️ LOS DOS AGUJEROS QUE QUEDABAN DE "OLAS REALES", y los dos estaban medidos antes de tocarlos:
//
//   · REFRACCION: no existia en ninguna parte del motor. `D = normalize(t1*W.z + t2*W.w)` — la
//     direccion del tren NO dependia del fondo, ni en CPU ni en GLSL. Las olas llegaban a la playa
//     con el angulo del mar abierto; en el mar real giran al perder fondo y entran casi paralelas a
//     la orilla, que es por lo que las crestas se ven paralelas a la playa desde cualquier sitio.
//   · ROMPIENTE: el escarpado total llegaba a 0,818 y plegar pide pasar de 1 — y QUITANDO EL TOPE
//     ENTERO se quedaba igual. O sea que no era el tope: una ola simetrica con la altura que permite
//     el limite de rompiente no se pliega por mucho escarpado que se le eche. Falta ASIMETRIA.
void test_ocean_refraction_and_break() {
    beginTest("ocean_refraction_and_break");
    using namespace Haruka::Planet;
    const OceanState st = oceanDefaultState();
    const glm::vec3 up(0, 1, 0);
    const glm::vec3 costa(1, 0, 0);      // hacia agua MENOS profunda (la orilla esta al +X)
    const glm::vec3 isobata(0, 0, 1);    // a lo largo de la playa

    // ── (1) EN AGUA PROFUNDA NO PASA NADA. Si esto fallara, el arreglo habria cambiado el mar
    //        abierto entero, que es el 99 % de lo que se ve.
    {
        const float k0 = 6.2831853f / st.wave[0][0];
        const float k  = oceanWaveNumber(k0, 4000.0f);
        const glm::vec3 d0 = glm::normalize(costa * 0.5f + isobata * 0.866f);   // 60 grados oblicuo
        const glm::vec3 d  = oceanRefract(d0, costa, up, k0, k);
        const double giro = std::acos(glm::clamp((double)glm::dot(d, d0), -1.0, 1.0)) * 180.0 / 3.14159265;
        std::printf("    a 4000 m de fondo la ola gira %.4f grados (k/k0 = %.6f)\n", giro, k / k0);
        CHECK(giro < 0.01, "en agua PROFUNDA la refraccion no toca la direccion");
    }

    // ── (2) AL PERDER FONDO, LA OLA SE ENDEREZA. Se barre la profundidad y se mide el angulo que
    //        forma la cresta con la orilla. Tiene que ir a cero.
    {
        const float k0 = 6.2831853f / st.wave[0][0];
        const glm::vec3 d0 = glm::normalize(costa * 0.342f + isobata * 0.940f);   // 70 grados oblicuo
        const double ang0 = std::acos(glm::clamp((double)glm::dot(d0, costa), -1.0, 1.0)) * 180.0/3.14159265;
        std::printf("    fondo(m)   angulo con la NORMAL a la costa   (empieza en %.1f grados)\n", ang0);
        double prev = 1e9, ultimo = 0.0;
        bool monotono = true;
        for (float d : { 200.0f, 50.0f, 20.0f, 10.0f, 5.0f, 2.0f, 1.0f, 0.5f }) {
            const float k = oceanWaveNumber(k0, d);
            const glm::vec3 dd = oceanRefract(d0, costa, up, k0, k);
            const double a = std::acos(glm::clamp((double)glm::dot(dd, costa), -1.0, 1.0)) * 180.0/3.14159265;
            std::printf("      %7.1f   %10.2f\n", d, a);
            if (a > prev + 1e-3) monotono = false;
            prev = a; ultimo = a;
        }
        CHECK(monotono, "el angulo con la normal a la costa NO crece al perder fondo (se endereza)");
        CHECK(ultimo < ang0 * 0.5, "y a 0,5 m de fondo se ha enderezado a menos de la mitad");
        // ⚠️ CONTRAPRUEBA: la LEY, no solo la tendencia. Snell conserva `k·sinθ`; si esto no se
        // comprobara, cualquier funcion que gire la ola hacia la costa pasaria el test de arriba.
        double peorSnell = 0.0;
        for (float d : { 200.0f, 20.0f, 5.0f, 1.0f }) {
            const float k = oceanWaveNumber(k0, d);
            const glm::vec3 dd = oceanRefract(d0, costa, up, k0, k);
            const double kAlong0 = (double)k0 * glm::dot(d0, isobata);
            const double kAlong  = (double)k  * glm::dot(dd, isobata);
            peorSnell = std::max(peorSnell, std::fabs(kAlong - kAlong0) / std::fabs(kAlong0));
        }
        std::printf("    LEY DE SNELL: peor desvio de `k·sin(θ)` conservado: %.3e relativo\n", peorSnell);
        CHECK(peorSnell < 1e-5, "se conserva `k·sin(θ)` — es Snell, no una interpolacion hacia la costa");
    }

    // ── (3) SIN PENDIENTE NO REFRACTA. Un fondo llano no gira nada, y el centinela lo dice.
    {
        const float k0 = 6.2831853f / st.wave[0][0];
        const float k  = oceanWaveNumber(k0, 3.0f);
        const glm::vec3 d0 = glm::normalize(costa * 0.5f + isobata * 0.866f);
        const glm::vec3 d  = oceanRefract(d0, glm::vec3(0.0f), up, k0, k);
        CHECK(glm::length(d - d0) < 1e-6f,
              "CONTRAPRUEBA: sin dato de pendiente la direccion queda INTACTA (no se inventa una costa)");
    }

    // ── (4) LA ROMPIENTE: ¿pliega ahora la cresta? Se barre el fondo y se mira el escarpado total,
    //        que es `1 − jacobiano minimo`. Pasar de 1 ES el pliegue.
    {
        double peor = 0.0; float peorD = 0.0f; int pliegan = 0;
        float dMin = 1e9f, dMax = 0.0f;
        for (int i = 0; i < 400; ++i) {
            const float d = 0.05f + (float)i * 0.05f;
            const float green = oceanGreenGain(d) * oceanFetchFactor(st, WATER_FETCH_UNLIMITED);
            const float brk   = oceanBreakScale(d, st);
            const float ss    = oceanSteepScale(d, st);
            const float curl  = oceanCurlGain(d, st);
            double sum = 0.0;
            for (int j = 0; j < OCEAN_WAVES; ++j) {
                const float k0 = 6.2831853f / st.wave[j][0];
                const float k  = oceanWaveNumber(k0, d);
                const float amp = st.wave[j][1] * green * brk * oceanShortWaveFade(k);
                // En la cresta `cos φ = 1`, que es donde el adelanto es maximo.
                sum += (double)k * oceanHorizAmp(k, amp, d) * ss * (1.0 + curl);
            }
            if (sum > 1.0) { ++pliegan; dMin = std::min(dMin, d); dMax = std::max(dMax, d); }
            if (sum > peor) { peor = sum; peorD = d; }
        }
        std::printf("    escarpado total en la CRESTA, 5 cm..20 m de fondo: maximo %.3f (a %.2f m)\n"
                    "    LA FRANJA QUE ROMPE: de %.2f m a %.2f m de fondo (%d de 400 muestras)\n",
                    peor, peorD, dMin, dMax, pliegan);
        // La ola de referencia mide 2,8 m de Hs, y una ola rompe con H/d ~ 0,78 → alrededor de 3,6 m
        // de fondo. Si la franja cayera en centimetros, romperia en la orilla misma y no se veria.
        // Y donde dice el INDICE que deberia romper, que es contra lo que se compara la franja.
        float dGamma = 0.0f;
        for (int i = 400; i >= 1; --i) {
            const float d = (float)i * 0.05f;
            if (oceanBreakIndex(d, st) >= 0.78f) { dGamma = d; break; }
        }
        std::printf("    el INDICE de rompiente cruza 0,78 a %.2f m de fondo — ahi es donde debe romper\n", dGamma);
        CHECK(dMax > dGamma * 0.5f && dMin < dGamma * 1.5f,
              "la franja que pliega cae DONDE el indice dice que rompe, no en la orilla");
        CHECK(peor > 1.0, "la cresta AHORA se pliega: el escarpado pasa de 1 (antes el maximo era 0,818)");
        CHECK(pliegan > 0 && pliegan < 400,
              "y pliega en una FRANJA, no en todas partes (una ola que rompe siempre no es una ola)");
    }

    // ── (5) EL MAR ABIERTO NO CAMBIA. Es la mitad del arreglo: si el adelanto tocara el agua honda,
    //        el oceano entero tendria la cresta torcida.
    {
        const float curlHondo = oceanCurlGain(400.0f, st);
        const float ssHondo   = oceanSteepScale(400.0f, st);
        std::printf("    a 400 m de fondo: adelanto de cresta %.6f · escala de escarpado %.6f\n",
                    curlHondo, ssHondo);
        CHECK(curlHondo < 1e-4f, "en agua honda NO hay adelanto de cresta (el mar abierto no cambia)");
        CHECK(std::fabs(ssHondo - 1.0f) < 1e-4f, "ni recorte de escarpado");
    }

    // ── (6) ⚠️ EL AGUA INTERIOR, que es lo que rompio el intento anterior. Una charca de 30 cm sobre
    //        celdas de 4,4 m: el adelanto es una FRACCION del semieje, y el semieje escala con la
    //        amplitud, asi que una ola de centimetros se adelanta centimetros.
    {
        const float d = 0.30f;
        const float green = oceanGreenGain(d) * oceanFetchFactor(st, 200.0f);
        const float brk   = oceanBreakScale(d, st, 200.0f);
        const float ss    = oceanSteepScale(d, st, 200.0f);
        const float curl  = oceanCurlGain(d, st, 200.0f);
        float total = 0.0f;
        for (int j = 0; j < OCEAN_WAVES; ++j) {
            const float k0 = 6.2831853f / st.wave[j][0];
            const float k  = oceanWaveNumber(k0, d);
            const float amp = st.wave[j][1] * green * brk * oceanShortWaveFade(k);
            total += oceanHorizAmp(k, amp, d) * ss * (1.0f + curl);
        }
        std::printf("    charca de 30 cm CON la rompiente puesta: vaiven horizontal %.3f m (celda 4,4 m)\n",
                    total);
        CHECK(total < 4.4f,
              "la charca sigue sin desplazarse mas que su celda (el adelanto es fraccion de la amplitud)");
    }

    // ── (7) Y que la altura sigue siendo finita y acotada por el fondo en todo el barrido.
    {
        bool ok = true; float peorH = 0.0f, peorD = 0.0f;
        for (int i = 0; i < 200; ++i) {
            const float d = 0.05f + (float)i * 0.1f;
            const glm::vec3 wp(37.0f, 0.0f, 11.0f);
            const float h = oceanWaveHeight(wp, up, 5.5f, d, 1.0f, st, WATER_FETCH_UNLIMITED, costa);
            if (!std::isfinite(h)) ok = false;
            if (std::fabs(h) > peorH) { peorH = std::fabs(h); peorD = d; }
        }
        std::printf("    altura maxima en el barrido: %.3f m (a %.2f m de fondo) · toda finita: %s\n",
                    peorH, peorD, ok ? "si" : "NO");
        CHECK(ok, "la altura es finita en todo el barrido (con refraccion y rompiente puestas)");
    }
}

// ------------- TEST: ¿es un MAR o son cuatro senos? El espectro, medido antes de tocarlo ---------
// ⚠️ NO ARREGLA NADA: MIDE. La tabla tiene `OCEAN_WAVES = 4` trenes, y un mar real es un espectro
// continuo. La pregunta no es "¿4 es poco?" —siempre lo es— sino **cuanto se nota y cuanto cuesta**,
// que es lo que decide si vale la pena. Sin esta cifra, subir a 8 trenes es doblar el coste del
// vertice del agua a ciegas, sobre un pase cuyo coste ademas no se ha medido nunca.
//
// Dos sintomas medibles de "son pocos trenes":
//   · la altura no es GAUSIANA. Un mar real lo es por el teorema central del limite (muchas
//     componentes independientes); con 4 la curtosis se separa de 3.
//   · el patron SE REPITE en el espacio. Cuatro senos dan una figura casi periodica, y a partir de
//     cierta distancia se ve el mismo trozo de mar otra vez.
void test_ocean_spectrum_limits() {
    beginTest("ocean_spectrum_limits");
    using namespace Haruka::Planet;
    const OceanState st = oceanDefaultState();
    const glm::vec3 up(0, 1, 0);

    // ── (1) ¿ES GAUSIANA LA SUPERFICIE? ─────────────────────────────────────────────────────────
    {
        const int N = 200;
        std::vector<double> h; h.reserve((size_t)N * N);
        double s1 = 0.0;
        for (int j = 0; j < N; ++j) for (int i = 0; i < N; ++i) {
            const glm::vec3 wp((float)i * 3.7f, 0.0f, (float)j * 4.3f);   // pasos no conmensurables
            const double v = oceanWaveHeight(wp, up, 11.0f, 400.0f, 1.0f, st);
            h.push_back(v); s1 += v;
        }
        const double mean = s1 / (double)h.size();
        double m2 = 0.0, m4 = 0.0;
        for (double v : h) { const double d = v - mean; m2 += d*d; m4 += d*d*d*d; }
        m2 /= (double)h.size(); m4 /= (double)h.size();
        const double kurt = (m2 > 1e-12) ? m4 / (m2 * m2) : 0.0;
        const double Hs = 4.0 * std::sqrt(m2);
        std::printf("    superficie en mar abierto: Hs %.2f m · curtosis %.3f (gausiana = 3,00)\n"
                    "    desvio de gausiana: %.1f %%\n", Hs, kurt, 100.0 * std::fabs(kurt - 3.0) / 3.0);
        CHECK(m2 > 1e-6, "la superficie tiene varianza (si no, no habria nada que medir)");
        // No se afirma que 3,00 sea obligatorio: se DEJA ESCRITO cuanto se separa, que es el dato.
        CHECK(kurt > 1.0 && kurt < 6.0, "la curtosis queda MEDIDA y en un rango sano");
    }

    // ── (2) ¿SE REPITE EL PATRON? Autocorrelacion a lo largo del tren dominante. ────────────────
    //
    // Con 4 trenes la figura vuelve a parecerse a si misma; en un mar real la correlacion cae y no
    // vuelve. Se busca el pico de correlacion MAS ALTO fuera del origen: es "a esta distancia se ve
    // el mismo trozo de mar".
    {
        const int    N = 4096;
        const double paso = 2.0;                     // 2 m entre muestras → 8,2 km de recorrido
        // ⚠️ SE MUESTREA EN EL EJE DEL TREN DOMINANTE, y esto ya me costo un falso fallo: con
        // `up = +Y` el marco tangente da `t1 = -Z`, asi que el tren 0 (W.z = 1) se propaga en Z. La
        // primera version barria en X, donde ese tren es CONSTANTE — la contraprueba de un solo tren
        // daba varianza cero y autocorrelacion 0,000. Una medida a lo largo de la cresta no mide la ola.
        auto muestrear = [&](const OceanState& s, std::vector<double>& out) {
            out.resize((size_t)N);
            for (int i = 0; i < N; ++i)
                out[(size_t)i] = oceanWaveHeight(glm::vec3(0.0f, 0.0f, (float)(i * paso)), up,
                                                 7.0f, 400.0f, 1.0f, s);
        };
        std::vector<double> h;
        muestrear(st, h);
        double mean = 0.0; for (double v : h) mean += v; mean /= (double)N;
        double var = 0.0;  for (double v : h) var += (v - mean) * (v - mean); var /= (double)N;
        double mejor = 0.0; int lagMejor = 0;
        for (int lag = 20; lag < N / 2; ++lag) {     // desde 40 m: los lags cortos siempre correlan
            double c = 0.0;
            for (int i = 0; i + lag < N; ++i) c += (h[(size_t)i] - mean) * (h[(size_t)(i + lag)] - mean);
            c /= (double)(N - lag) * var;
            if (c > mejor) { mejor = c; lagMejor = lag; }
        }
        std::printf("    autocorrelacion maxima fuera del origen: %.3f a %.0f m\n"
                    "    (1,00 = el mar se repite EXACTO a esa distancia · 0 = no se repite)\n",
                    mejor, lagMejor * paso);
        CHECK(mejor < 1.0, "el mar no se repite EXACTAMENTE (los trenes no son conmensurables)");
        // ⚠️ CONTRAPRUEBA: con UN solo tren la correlacion tiene que dar ~1, o esta medida no
        // distingue "no se repite" de "la medida no ve la repeticion".
        {
            OceanState uno = st;
            for (int i = 1; i < OCEAN_WAVES; ++i) uno.wave[i][1] = 0.0f;   // apaga los otros tres
            std::vector<double> g;
            muestrear(uno, g);
            double m = 0.0; for (double v : g) m += v; m /= (double)N;
            double vr = 0.0; for (double v : g) vr += (v - m) * (v - m); vr /= (double)N;
            std::printf("    (contraprueba: varianza del tren unico %.4f m² — si fuera ~0, se estaria "
                        "midiendo a lo largo de la cresta)\n", vr);
            CHECK(vr > 1e-4, "el tren unico VARIA en el eje que se barre (si no, la medida no mide)");
            double best = 0.0;
            for (int lag = 20; lag < N / 2; ++lag) {
                double c = 0.0;
                for (int i = 0; i + lag < N; ++i) c += (g[(size_t)i] - m) * (g[(size_t)(i + lag)] - m);
                c /= (double)(N - lag) * vr;
                best = std::max(best, c);
            }
            std::printf("    CONTRAPRUEBA con UN solo tren: autocorrelacion maxima %.3f (debe ser ~1)\n", best);
            CHECK(best > 0.95, "CONTRAPRUEBA: la medida SI ve la repeticion cuando la hay");
        }
    }

    // ── (3) EL COSTE de subir el numero de trenes, para que la decision tenga precio. ───────────
    {
        const int M = 60000;
        const auto t0 = std::chrono::high_resolution_clock::now();
        volatile float acc = 0.0f;
        for (int i = 0; i < M; ++i)
            acc += oceanWaveHeight(glm::vec3((float)i * 0.37f, 0.0f, 11.0f), up, 3.0f, 40.0f, 1.0f, st);
        const double ns = std::chrono::duration<double, std::nano>(
            std::chrono::high_resolution_clock::now() - t0).count() / (double)M;
        std::printf("    coste de UNA evaluacion de ola con %d trenes: %.0f ns\n"
                    "    → 8 trenes costarian ~%.0f ns (el bucle domina) · 16 → ~%.0f ns\n",
                    OCEAN_WAVES, ns, ns * 2.0, ns * 4.0);
        CHECK(ns > 0.0, "el coste por evaluacion queda MEDIDO (subir trenes tiene precio conocido)");
    }
}
