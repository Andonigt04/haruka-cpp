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
    };

    // Suelta un cuerpo sobre el agua y simula. Devuelve el radio final y la deriva TANGENCIAL.
    void floatSim(const glm::dvec3& current, double seconds,
                  double& outDist, double& outDrift, double& outTangSpeed) {
        Haruka::Physics::PhysicsEngine eng;
        FakeOcean w; w.current = current;
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

    // Jacobiano minimo barriendo espacio y tiempo. `jac = 1 − Σ Q·A·k·sin φ`, la MISMA expresion que
    // acumula el shader; si baja de 0 la superficie esta plegada.
    auto minJacobian = [&](float depth) {
        const float green      = Haruka::Planet::oceanGreenGain(depth);
        const float scale      = Haruka::Planet::oceanBreakScale(depth, st);
        const float breakiness = std::clamp(1.0f - scale, 0.0f, 1.0f);
        float worst = 1e9f;
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
                    const float Q  = Haruka::Planet::oceanSteepness(k, amp, breakiness);
                    const float ph = k * glm::dot(D, p) - std::sqrt(Haruka::Planet::OCEAN_G * k) * t;
                    jac -= Q * amp * k * std::sin(ph);
                }
                worst = std::min(worst, jac);
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

    // ⚠️ NINGUNA DE LAS TRES PLIEGA, Y ES LO CORRECTO A DIA DE HOY. Se implemento la voluta (subir el
    // presupuesto de escarpado y abrir el tope de Q al romper) y el jacobiano SI se volvia negativo:
    // -0,104 a 1,2 m. Se REVIRTIO porque rompia el agua interior, y el motivo esta en la formula:
    // `Q = presupuesto/(k·A·N)` hace que el desplazamiento horizontal `Q·A = presupuesto/(k·N)` NO
    // dependa de la amplitud. En mar abierto son ~3 m y esta bien; en un lago de 30 cm son los MISMOS
    // ~3 m, asi que la lamina se desplazaba metros en horizontal y se plegaba sobre si misma — sobre
    // celdas de 4,4 m eso se veia como CUBOS DEFORMADOS, y el agua interior antes funcionaba.
    //
    // El fondo: aqui la ola no puede plegarse sola porque lambda es FIJA. Una ola real rompe porque al
    // perder fondo se le acorta la longitud de onda a la vez que crece la altura; con lambda fija,
    // H/lambda ~ 0,016 en agua somera y no hay pliegue posible sin forzarlo. La voluta de verdad pide
    // shoaling de longitud de onda, que es otro trabajo. Este test guarda el estado actual.
    CHECK(jacDeep  > 0.0f, "en mar abierto la ola NO vuelca sola");
    CHECK(jacBreak > 0.0f, "la cresta NO se pliega (la voluta se revirtio: rompia el agua interior)");

    // CONTRAPRUEBA: con el presupuesto VIEJO (0,75 fijo, sin subir con la rompiente) el jacobiano no
    // podia bajar de 1−0,75 = 0,25 a ninguna profundidad. Se recalcula aqui para dejar constancia de
    // que el cambio es lo que habilita el pliegue, y no que el barrido haya tenido suerte.
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
    std::printf("    (referencia: el mismo calculo con Q acotado a 1 da %+.3f — el escarpado actual)\n", worstOld);
    CHECK(worstOld > 0.0f, "el escarpado acotado no pliega, que es el comportamiento vigente");
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
