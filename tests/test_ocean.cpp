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
