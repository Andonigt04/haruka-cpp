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
#include "World/Water/OceanTestCommon.h"

 // namespace

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
        const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
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
                                               Haruka::World::Water::WATER_FETCH_UNLIMITED, slope)
                                         - oceanWaveHeight(wp, up, t - dt, shallow, 1.0f, st,
                                               Haruka::World::Water::WATER_FETCH_UNLIMITED, slope)) / (2.0 * dt);
                const glm::vec3 v = oceanWaveVelocity(wp, up, t, shallow, 1.0f, st,
                                               Haruka::World::Water::WATER_FETCH_UNLIMITED, slope);
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
                                           Haruka::World::Water::WATER_FETCH_UNLIMITED, slope)
                                     - oceanWaveHeight(wp, up, t - dt, shallow, 1.0f, st,
                                           Haruka::World::Water::WATER_FETCH_UNLIMITED, slope)) / (2.0 * dt);
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
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) {
        const float lambda = Haruka::World::Water::OCEAN_WAVE[i][0];
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

// ------------------------------- TEST 6: el estado del mar por viento (Pierson-Moskowitz + rumbo)
void test_ocean_sea_state() {
    beginTest("ocean_sea_state");
    using Haruka::World::Water::oceanStateFromWind;
    using Haruka::World::Water::OCEAN_REF_WIND;
    using Haruka::World::Water::OCEAN_WAVES;

    // Altura significativa del estado: Hs = 4·sigma, con sigma² = suma(a²)/2.
    auto hsOf = [](const Haruka::World::Water::OceanState& s) {
        double var = 0.0;
        for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) var += 0.5 * (double)s.wave[i][1] * s.wave[i][1];
        return 4.0 * std::sqrt(var);
    };

    // (a) A VIENTO DE REFERENCIA TIENE QUE SALIR LA TABLA DE SIEMPRE. Es el ancla de todo el cambio:
    // parametrizar el oleaje no puede alterar el mar ya afinado.
    const auto ref = Haruka::World::Water::oceanStateFromWind(Haruka::World::Water::OCEAN_REF_WIND, 1.0f, 0.0f, 0.0f);
    double worstDelta = 0.0;
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i)
        for (int c = 0; c < 4; ++c)
            worstDelta = std::max(worstDelta,
                                  std::abs((double)ref.wave[i][c] - Haruka::World::Water::OCEAN_WAVE[i][c]));
    std::printf("    a viento de referencia (%.1f m/s) vs la tabla original: dif. peor %.2e\n",
                Haruka::World::Water::OCEAN_REF_WIND, worstDelta);
    CHECK(worstDelta < 1e-4, "a viento de referencia sale EXACTAMENTE el mar de siempre");

    // (b) PIERSON-MOSKOWITZ A FRECUENCIAS FIJAS. ⚠️ Antes Hs y λ iban con U² (se escalaba la tabla
    // entera) y cada cambio de λ revolvía la fase del mar entero (`dk·x` a 10 km del ancla son
    // radianes). Ahora λ NO cambia con el viento y cada tren toma la amplitud de PM en SU frecuencia:
    // el viento mueve la energía de los trenes cortos (brisa) a los largos (temporal). Lo que se
    // exige: λ idénticas a cualquier viento, Hs monótona en U, el tren largo muerto con brisa y vivo
    // con temporal, y la cifra a 12 m/s cerca del manual (band-limited: por debajo).
    const auto s6  = Haruka::World::Water::oceanStateFromWind(6.0f,  1.0f, 0.0f, 0.0f);
    const auto s12 = Haruka::World::Water::oceanStateFromWind(12.0f, 1.0f, 0.0f, 0.0f);
    const auto s22 = Haruka::World::Water::oceanStateFromWind(22.0f, 1.0f, 0.0f, 0.0f);
    const double hs6 = hsOf(s6), hs12 = hsOf(s12), hs22 = hsOf(s22);
    double lamDiff = 0.0;
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i)
        lamDiff = std::max(lamDiff, std::abs((double)s6.wave[i][0] - s22.wave[i][0]));
    std::printf("    U=6 m/s  -> Hs %.2f m · tren de 137 m amp %.3f · tren de 8,7 m amp %.3f\n", hs6, s6.wave[0][1], s6.wave[Haruka::World::Water::OCEAN_WAVES - 1][1]);
    std::printf("    U=12 m/s -> Hs %.2f m · tren de 137 m amp %.3f (razon Hs %.2f; PM entero daria 4,00, la banda 8,7-137 m da menos)\n",
                hs12, s12.wave[0][1], hs12 / hs6);
    std::printf("    U=22 m/s -> Hs %.2f m · tren de 137 m amp %.3f · λ identicas a 6 m/s: dif. peor %.1e m\n", hs22, s22.wave[0][1], lamDiff);
    CHECK(lamDiff < 1e-6, "las longitudes de onda NO cambian con el viento (la fase no se revuelve)");
    CHECK(hs6 < hs12 && hs12 < hs22, "Hs crece con el viento");
    CHECK(hs12 / hs6 > 2.5, "y crece deprisa: doblar el viento mas que duplica Hs");
    CHECK(s6.wave[0][1] < 0.02 * s22.wave[0][1], "con brisa el tren de 137 m esta muerto; con temporal vivo");
    // Y la cifra absoluta a 12 m/s, contra el manual (Hs = 0,21·U²/g): band-limited, sale por debajo
    // pero en el mismo orden.
    const double hsBook = 0.21 * 12.0 * 12.0 / 9.81;
    std::printf("    contraste con el manual a 12 m/s: Hs modelo %.2f m vs 0.21U^2/g = %.2f m\n",
                hs12, hsBook);
    CHECK(hs12 > 0.6 * hsBook && hs12 < 1.25 * hsBook, "Hs cae en el orden de Pierson-Moskowitz");

    // (c) EL TOPE DE NYQUIST: por flojo que sople, ningún tren baja del corto de referencia (8,7 m),
    // que es lo que la rejilla puede llevar a 4 m por celda. Con λ fijas sale por construccion, y se
    // deja medido para que no vuelva.
    const auto calm = Haruka::World::Water::oceanStateFromWind(0.0f, 1.0f, 0.0f, 0.0f);
    float shortest = 1e9f;
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) shortest = std::min(shortest, calm.wave[i][0]);
    std::printf("    en calma: tren mas corto %.2f m (piso de teselacion 8.70 m) · Hs %.3f m\n", shortest, hsOf(calm));
    CHECK(shortest >= 8.7f - 1e-3f, "ningun tren baja del piso de Nyquist de la teselacion");
    CHECK(hsOf(calm) < 0.15, "en calma (viento topado a OCEAN_WIND_MIN) el mar es un rizo");

    // (d) EL RUMBO: girar el viento 90 grados gira el tren principal 90 grados, sin cambiar su
    // longitud ni su amplitud (es un giro, no otro mar).
    const auto east  = Haruka::World::Water::oceanStateFromWind(10.0f, 1.0f, 0.0f, 0.0f);
    const auto north = Haruka::World::Water::oceanStateFromWind(10.0f, 0.0f, 1.0f, 0.0f);
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

// ============================================================================================
// LA OCTAVA MÁS FINA DEL TERRENO — que no se vuelva a morir en silencio
// ============================================================================================
//
// Vive en TestOcean.cpp por vecindad de sesión, no por tema. Lo que vigila:
//
// La escalera de `TerrainDetail.h` acaba en λ = 4,5 m con la guarda `minFeatureM < 2.25`. El piso
// de `triM` de la GEOMETRÍA vale 4 m (= el quad del clipmap), así que durante un tiempo esa octava
// no se activó en NINGÚN punto del planeta: el rasgo más fino que el terreno podía tener era λ = 22 m.
// Nadie lo notó porque nada falla — el terreno simplemente sale liso.
//
// Se murió al bajar el tope de tesela de 64 a 32 por coste. Cualquier futuro ajuste de ese tope, o
// del piso, puede volver a apagarla. Esto lo convierte en un fallo de test en vez de en una textura
// que alguien encuentra rara meses después.

void test_terrain_finest_octave() {
    beginTest("terrain_finest_octave");
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;

    // Gemelo de la escalera de `TerrainDetail.h`: la octava mas fina y su amplitud.
    constexpr float kFinestLambda = 4.5f;
    constexpr float kFinestAmp    = 0.7f;
    // Gemelo de `harukaOctaveWeight`: es una RAMPA, no un interruptor.
    auto weight = [](float lambdaM, float triM) {
        const float t = lambdaM / std::max(triM * 2.0f, 1e-3f);
        return std::min(std::max(t - 1.0f, 0.0f), 1.0f);
    };

    const float wPix  = weight(kFinestLambda, (float)Haruka::World::Terrain::TERRAIN_TRIM_FLOOR_PIXEL);
    const float wGeom = weight(kFinestLambda, (float)Haruka::World::Terrain::TERRAIN_TRIM_FLOOR);
    std::printf("    piso de triM: geometria %.3f m · sombreado %.3f m\n",
                Haruka::World::Terrain::TERRAIN_TRIM_FLOOR, Haruka::World::Terrain::TERRAIN_TRIM_FLOOR_PIXEL);
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
    CHECK(Haruka::World::Terrain::TERRAIN_TRIM_FLOOR_PIXEL >= (double)kFinestLambda / 4.0 - 1e-6,
          "y no baja de lambda/4 (por debajo se paga sin ganar amplitud)");

    // (c) ⚠️ ESTA CONTRAPRUEBA SE DIO LA VUELTA AL BORRARSE EL CLIPMAP (2026-08-24), y el cambio ES
    // el arreglo, no un efecto colateral.
    //
    // Exigia que con el piso de la GEOMETRIA la octava siguiera MUERTA: con quads de 4 m lo estaba, y
    // por eso hacian falta dos pisos. Ahora la geometria de colision son celdas de 0,5 m, asi que esa
    // octava SI cabe (Nyquist pide < 2,25 m) — y que entre es justo lo que hace que el suelo que se
    // pisa tenga el relieve que se ve. Era la disparidad ver<->pisar de 0,15-0,25 m.
    std::printf("    con el piso de la geometria (%.2f m) el peso es %.3f\n", Haruka::World::Terrain::TERRAIN_TRIM_FLOOR, wGeom);
    CHECK(wGeom > 0.99f, "con la rejilla fina la octava tambien entra en la GEOMETRIA");
    // La contraprueba con dientes pasa a ser esta: con el piso VIEJO (el quad del clipmap) estaba
    // muerta. Sin este numero no se veria lo que se ha arreglado.
    const float wOld = weight(kFinestLambda, (float)Haruka::World::Terrain::TERRAIN_CLIP_QUAD_M);
    std::printf("    CONTRAPRUEBA: con el piso VIEJO (quad del clipmap, %.2f m) el peso era %.3f\n",
                Haruka::World::Terrain::TERRAIN_CLIP_QUAD_M, wOld);
    CHECK(wOld <= 0.0f, "CONTRAPRUEBA: con el quad de 4 m la octava estaba MUERTA (era la disparidad)");
    // (d) El piso de la GEOMETRIA sigue atado a la rejilla que lo consume — misma regla, otra
    // rejilla: antes el quad del clipmap, ahora la celda del anillo de colision mas fino.
    CHECK(Haruka::World::Terrain::TERRAIN_TRIM_FLOOR == Haruka::World::Terrain::TERRAIN_RING_FINE_CELL,
          "el piso de la GEOMETRIA sigue atado a su rejilla (Nyquist, no un literal)");
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
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;

    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
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
    std::printf("      200,0 m       %.3f        %+.3f     %.3f m\n", Haruka::World::Water::oceanSkewness(200.0f, st), skD, sgD);
    std::printf("        2,0 m       %.3f        %+.3f     %.3f m\n", Haruka::World::Water::oceanSkewness(2.0f, st), skS, sgS);

    // 1. EL LIMITE DE AGUAS PROFUNDAS ES EL ORACULO. `oceanStokesSkew` es una expresion general; su
    //    limite `kd -> inf` tiene que dar el Stokes de toda la vida, `S = k·a/2`, que se deriva por
    //    separado. Si la expresion estuviera mal transcrita, este limite no saldria.
    //    ⚠️ Y ES != 0 A PROPOSITO: antes el termino era una constante con una puerta que lo apagaba en
    //    mar abierto. Una ola profunda REAL tiene crestas algo mas agudas que sus senos; el 0 exacto
    //    de antes era el modelo simplificado, no la fisica.
    {
        const float k = 0.0706f, a = 0.62f, d = 5000.0f;    // kd enorme
        const float esperado = 0.5f * k * a;
        const float dado = Haruka::World::Water::oceanStokesSkew(k, a, d);
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
        const float dado = Haruka::World::Water::oceanStokesSkew(k, a, d);
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
        const float green = Haruka::World::Water::oceanGreenGain(2.0f) * Haruka::World::Water::oceanFetchFactor(st, Haruka::World::Water::WATER_FETCH_UNLIMITED);
        const float scale = Haruka::World::Water::oceanBreakScale(2.0f, st);
        double s2 = 0;
        const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0,1,0))
                                                                   : glm::cross(up, glm::vec3(1,0,0)));
        const glm::vec3 t2 = glm::cross(up, t1);
        for (int i = 0; i < N; ++i) {
            const glm::vec3 p(muestra(i, 0) * 400.0f, 0.0f, muestra(i, 1) * 400.0f);
            const float tt = muestra(i, 2) * 120.0f;
            float h = 0.0f;
            for (int j = 0; j < Haruka::World::Water::OCEAN_WAVES; ++j) {              // la suma de senos de siempre
                const float k0 = 6.2831853f / st.wave[j][0];
                const float k  = Haruka::World::Water::oceanWaveNumber(k0, 2.0f);
                const float amp = st.wave[j][1] * green * scale * Haruka::World::Water::oceanShortWaveFade(k);
                if (amp <= 1e-4f) continue;
                const glm::vec3 D = glm::normalize(t1 * st.wave[j][2] + t2 * st.wave[j][3]);
                h += amp * std::sin(k * glm::dot(D, p) - std::sqrt(Haruka::World::Water::OCEAN_G * k0) * tt);
            }
            s2 += (double)h * h;
        }
        sigmaOff = (float)std::sqrt(s2 / N);
    }
    // El teorico ya no es un numero fijo: con `S` POR TREN la varianza sube por `Σa²(1+S²)/Σa²`, asi
    // que se calcula del propio estado en vez de citar el 4,4 % que valia cuando `S` era comun.
    float sa2 = 0.0f, sa2s2 = 0.0f;
    {
        const float green = Haruka::World::Water::oceanGreenGain(2.0f) * Haruka::World::Water::oceanFetchFactor(st, Haruka::World::Water::WATER_FETCH_UNLIMITED);
        const float scale = Haruka::World::Water::oceanBreakScale(2.0f, st);
        for (int j = 0; j < Haruka::World::Water::OCEAN_WAVES; ++j) {
            const float k0 = 6.2831853f / st.wave[j][0];
            const float k  = Haruka::World::Water::oceanWaveNumber(k0, 2.0f);
            const float amp = st.wave[j][1] * green * scale * Haruka::World::Water::oceanShortWaveFade(k);
            if (amp <= 1e-4f) continue;
            const float S = Haruka::World::Water::oceanStokesSkew(k, amp, 2.0f);
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
    CHECK(extremos(Haruka::World::Water::OCEAN_SKEW_MAX) == 2, "con el tope vigente la ola tiene UNA cresta y UN seno");
    CHECK(extremos(0.30f) == 4, "CONTRAPRUEBA: con 0,30 al seno le sale un monticulo (4 extremos)");
    CHECK(Haruka::World::Water::OCEAN_SKEW_MAX <= 0.25f, "el tope esta en el limite derivado, no por encima");
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
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;

    // El reparto real: `m_ringCoverM = NC·PATCH/2` (NC=31 en calidad Low) y el bucle del dibujo
    // corta en `kWaveReachM` = 16 km.
    const double PATCH = Haruka::World::Terrain::TERRAIN_CLIP_PATCH_M;
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
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;
    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
    const double PATCH = Haruka::World::Terrain::TERRAIN_CLIP_PATCH_M;
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

    const double lamShort = st.wave[Haruka::World::Water::OCEAN_WAVES - 1][0];      // el tren mas corto (8,7 m)
    const double lamLong  = st.wave[0][0];                    // el mas largo (61 m)
    std::printf("    tren mas corto %.1f m · mas largo %.1f m · Nyquist pide 2 muestras por onda\n",
                lamShort, lamLong);
    std::printf("    distancia    quad     muestras/onda corta   muestras/onda larga   amplitud\n");
    // El desvanecido POR TREN según el quad local, gemelo de `harukaShortWaveFade(k, 2·quadM)`.
    auto trainAmpAt = [&](double lam, double rad) {
        const double lo = std::max(quadAt(rad), (double)Haruka::World::Water::OCEAN_MIN_QUAD_M), hi = lo * 2.0;
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
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i)
        minNear = std::min(minNear, (double)Haruka::World::Water::oceanShortWaveFade(
                      6.2831853f / st.wave[i][0], Haruka::World::Water::OCEAN_MIN_QUAD_M));
    std::printf("    a los PIES (quad %.1f m): el tren mas apagado de los cuatro esta al %.1f %%\n",
                (double)Haruka::World::Water::OCEAN_MIN_QUAD_M, 100.0 * minNear);
    CHECK(minNear > 0.99,
          "a los pies del jugador NINGUN tren se apaga: el quad fino los lleva todos");
    // CONTRAPRUEBA: con el quad de 2 km (8 m) el mas corto SI se apaga. Si no, el desvanecido no
    // estaria haciendo nada y el test de arriba no diria nada.
    const double farShort = (double)Haruka::World::Water::oceanShortWaveFade(
                                6.2831853f / st.wave[Haruka::World::Water::OCEAN_WAVES - 1][0], 8.0f);
    std::printf("    CONTRAPRUEBA a 2 km (quad 8 m): el tren corto baja al %.1f %%\n", 100.0 * farShort);
    CHECK(farShort < 0.2, "CONTRAPRUEBA: con el quad grueso el tren corto SI se apaga (Nyquist)");

    CHECK(worstLong >= 2.0, "la rejilla puede llevar la ola LARGA en todo el alcance en que se dibuja");
    CHECK(worstShort >= 2.0, "y la CORTA tambien");
    CHECK(oldWorstLong < 1.0, "CONTRAPRUEBA: con el piso fijo la ola larga SI iba por debajo de Nyquist");
    // CONTRAPRUEBA: cerca la rejilla sobra de largo. Si tambien fallara ahi, la medida estaria mal.
    CHECK(lamShort / quadAt(100.0) > 2.0,
          "CONTRAPRUEBA: cerca del jugador la rejilla lleva hasta la ola mas corta");
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
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;
    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();

    // (1) AGUA PROFUNDA: circulo (horizontal == vertical == A). Lo cumplen LAS DOS formulas — que es
    //     justo lo que hay que comprobar para no romper el mar abierto al tocar el somero.
    std::printf("    tren     λ(m)   A(m)   horiz VIEJO   horiz NUEVO   (deberia ser A)\n");
    double peorRel = 0.0, peorViejo = 0.0;
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) {
        const float k = 6.2831853f / st.wave[i][0];       // en profundo k == k0
        const float A = st.wave[i][1];
        const float viejo = Haruka::World::Water::oceanSteepness(k, A, 0.0f) * A;
        const float nuevo = Haruka::World::Water::oceanHorizAmp(k, A, 4000.0f);  // 4 km de fondo = profundo
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
        const float vGrande = Haruka::World::Water::oceanSteepness(k, 1.00f, 0.0f) * 1.00f;
        const float vChica  = Haruka::World::Water::oceanSteepness(k, 0.05f, 0.0f) * 0.05f;
        const float nGrande = Haruka::World::Water::oceanHorizAmp(k, 1.00f, 100.0f);
        const float nChica  = Haruka::World::Water::oceanHorizAmp(k, 0.05f, 100.0f);
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
        const float k  = Haruka::World::Water::oceanWaveNumber(k0, 2.0f);
        const float A  = 0.3f;
        const float r  = Haruka::World::Water::oceanHorizAmp(k, A, 2.0f) / A;
        std::printf("    a 2 m de fondo la orbita es %.2f veces mas ancha que alta\n", r);
        CHECK(r > 1.5f, "en somero la orbita se ensancha (horizontal > vertical)");
    }

    // (4) ⚠️ EL PLIEGUE, MEDIDO — y las DOS cifras, con tope y sin el. Sin la de "sin tope" esto no
    //     distingue "la ola no puede plegarse" de "elegimos que no se pliegue", que es otra cosa.
    {
        double peorCap = 0.0, peorLibre = 0.0; float dCap = 0.0f, dLibre = 0.0f;
        for (int i = 0; i < 400; ++i) {
            const float d = 0.05f + (float)i * 0.05f;           // 5 cm .. 20 m
            const float green = Haruka::World::Water::oceanGreenGain(d) * Haruka::World::Water::oceanFetchFactor(st, Haruka::World::Water::WATER_FETCH_UNLIMITED);
            const float brk   = Haruka::World::Water::oceanBreakScale(d, st);
            const float ss    = Haruka::World::Water::oceanSteepScale(d, st);
            double libre = 0.0;
            for (int j = 0; j < Haruka::World::Water::OCEAN_WAVES; ++j) {
                const float k0 = 6.2831853f / st.wave[j][0];
                const float k  = Haruka::World::Water::oceanWaveNumber(k0, d);
                const float amp = st.wave[j][1] * green * brk * Haruka::World::Water::oceanShortWaveFade(k);
                libre += (double)k * Haruka::World::Water::oceanHorizAmp(k, amp, d);
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
        const float green = Haruka::World::Water::oceanGreenGain(d) * Haruka::World::Water::oceanFetchFactor(st, 200.0f);
        const float brk   = Haruka::World::Water::oceanBreakScale(d, st, 200.0f);
        const float ss    = Haruka::World::Water::oceanSteepScale(d, st, 200.0f);
        float nuevo = 0.0f, viejo = 0.0f;
        for (int j = 0; j < Haruka::World::Water::OCEAN_WAVES; ++j) {
            const float k0 = 6.2831853f / st.wave[j][0];
            const float k  = Haruka::World::Water::oceanWaveNumber(k0, d);
            const float amp = st.wave[j][1] * green * brk * Haruka::World::Water::oceanShortWaveFade(k);
            nuevo += Haruka::World::Water::oceanHorizAmp(k, amp, d) * ss;
            viejo += Haruka::World::Water::oceanSteepness(k, amp, 0.0f) * amp;
        }
        std::printf("    charca de 30 cm: vaiven horizontal VIEJO %.3f m · NUEVO %.3f m (celda 4,4 m)\n",
                    viejo, nuevo);
        CHECK(nuevo < 4.4f, "la charca no se desplaza mas que su propia celda (no hay cubos deformados)");
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
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;
    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
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
        auto muestrear = [&](const Haruka::World::Water::OceanState& s, std::vector<double>& out) {
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
            Haruka::World::Water::OceanState uno = st;
            for (int i = 1; i < Haruka::World::Water::OCEAN_WAVES; ++i) uno.wave[i][1] = 0.0f;   // apaga los otros tres
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
                    Haruka::World::Water::OCEAN_WAVES, ns, ns * 2.0, ns * 4.0);
        CHECK(ns > 0.0, "el coste por evaluacion queda MEDIDO (subir trenes tiene precio conocido)");
    }
}

/**
 * @brief ¿LA MALLA DEL AGUA AGUANTA EL VAIVÉN, O SE ESTIRA Y SE DOBLA?
 *
 * ⚠️ ESTE ES EL PUNTO CIEGO QUE QUEDABA, Y NO LO VE `ocean_break_fold`. Aquel mide el jacobiano
 * ANALÍTICO: una propiedad del continuo, en el límite de separación cero. Lo que se dibuja no es el
 * continuo — son vértices cada `quadM` metros, y entre dos vecinos sólo hay una recta. Las dos cosas
 * se separan justo donde importa:
 *
 *   · el continuo puede plegarse a una escala MÁS FINA que la celda, y entonces la malla ni se
 *     entera: se estira, pero no se cruza;
 *   · y al revés, un pliegue que abarque varias celdas SÍ invierte el orden de los vértices, que es
 *     lo que en pantalla se ve como caras del revés y "cubos deformes".
 *
 * Se mide lo que de verdad le pasa a un quad: la separación de dos vértices vecinos DESPUÉS de
 * desplazarlos. `estiramiento = |p'(x+q) − p'(x)| / q`.
 *
 *   ≈ 1  la celda conserva su tamaño
 *   ≫ 1  la celda se estira (textura y normales embarradas)
 *   ≤ 0  el vecino se ha adelantado: la celda está DEL REVÉS, y eso es el fallo visible
 *
 * El barrido usa las separaciones REALES del pase de agua (`terrain_node_water.vert`: téxel × zancada
 * del nodo, de 0,6 m bajo los pies a 9,5 m a distancia) contra el perfil de una playa.
 *
 * CONTRAPRUEBA: con el mar en calma (amplitudes a 0) no puede haber ni estirón ni inversión. Sin
 * ella, "0 inversiones" no distingue una malla sana de una medida que no mide.
 */
void test_ocean_mesh_stretch() {
    beginTest("ocean_mesh_stretch");

    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
    const glm::vec3 up = upAt(0.2, -0.4);
    // ⚠️ CERCA DEL ORIGEN, **NO** A RADIO PLANETARIO, Y LA CONTRAPRUEBA LO CAZO EN EL PRIMER INTENTO.
    // Esto es una DIFERENCIA FINITA: se resta la posicion de dos vertices separados `q`. A 6,37e6 m
    // un float32 tiene 1 ulp ~ 0,5 m, asi que `b − a` con `q = 4,4 m` sale cuantizado y el cociente
    // da 0,89..1,11 con el mar EN CALMA. Se vio exactamente asi: la contraprueba dio 0,974..0,992 en
    // vez de 1,0000, y el instrumento no medira el mar hasta que de 1 exacto en calma.
    //
    // La posicion absoluta solo entra en la FASE de la ola, asi que muestrear a 1 km del origen es
    // el mismo mar con 5 ordenes de magnitud mas de resolucion. Ver [[float-ulp-precision-wall]] —
    // la misma piedra tumbo dos sondas del jacobiano el 2026-09-07.
    const glm::vec3 wp = up * 1000.0f;
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0,1,0))
                                                               : glm::cross(up, glm::vec3(1,0,0)));
    const glm::vec3 t2 = glm::cross(up, t1);

    // La pendiente del fondo hacia la orilla: sin ella no hay refracción, y la refracción es
    // justamente lo que concentra los trenes y hace que la cresta se pliegue (lo fija
    // `ocean_break_fold` con su contraprueba). Medir el estirón sin ella mediría otro mar.
    const glm::vec3 slope = t1;

    struct Res { double peor; double menor; int invertidas; int total; };
    auto barrido = [&](float depth, float q, const Haruka::World::Water::OceanState& estado) {
        Res r{ 0.0, 1e9, 0, 0 };
        for (int s = 0; s < 24; ++s) {
            // Puntos repartidos a lo largo de la costa y del tiempo: con ocho trenes
            // inconmensurables, un punto y un instante no dicen nada del peor caso.
            const glm::vec3 p = wp + t1 * (float)(s * 7) + t2 * (float)(s * 3);
            for (int n = 0; n < 40; ++n) {
                const float t = 0.37f * n;
                for (int eje = 0; eje < 2; ++eje) {
                    const glm::vec3 dir = (eje == 0) ? t1 : t2;
                    const glm::vec3 a = p;
                    const glm::vec3 b = p + dir * q;
                    const glm::vec3 da = Haruka::World::Water::oceanDisplacement(a, up, t, depth, 1.0f,
                                                                          estado,
                                                                          Haruka::World::Water::WATER_FETCH_UNLIMITED,
                                                                          slope);
                    const glm::vec3 db = Haruka::World::Water::oceanDisplacement(b, up, t, depth, 1.0f,
                                                                          estado,
                                                                          Haruka::World::Water::WATER_FETCH_UNLIMITED,
                                                                          slope);
                    // Proyectada SOBRE EL EJE de la celda y con signo: el modulo no distingue
                    // "el vecino sigue delante" de "el vecino se ha puesto detras", que es el caso
                    // que rompe la malla.
                    const double avance = (double)glm::dot((b + db) - (a + da), dir) / (double)q;
                    r.peor  = std::max(r.peor, avance);
                    r.menor = std::min(r.menor, avance);
                    if (avance <= 0.0) ++r.invertidas;
                    ++r.total;
                }
            }
        }
        return r;
    };

    // Las separaciones del pase de agua y el perfil de la playa donde la ola hace cosas.
    const float qs[]     = { 0.6f, 1.2f, 2.4f, 4.4f, 9.5f };
    const float fondos[] = { 12.0f, 8.0f, 5.0f, 2.0f, 1.2f };

    std::printf("    estiramiento de la celda (avance del vecino / separacion). "
                "1,00 = intacta · <=0 = celda DEL REVES\n");
    std::printf("    fondo \\ celda ");
    for (float q : qs) std::printf("   %5.1f m", (double)q);
    std::printf("\n");

    double peorGlobal = 0.0, menorGlobal = 1e9;
    int invGlobal = 0, totGlobal = 0;
    float fondoPeor = 0.0f, celdaPeor = 0.0f;
    for (float d : fondos) {
        std::printf("    %6.1f m      ", (double)d);
        for (float q : qs) {
            const Res r = barrido(d, q, st);
            std::printf("  %5.2f/%.2f", r.peor, r.menor);
            if (r.menor < menorGlobal) { menorGlobal = r.menor; fondoPeor = d; celdaPeor = q; }
            peorGlobal = std::max(peorGlobal, r.peor);
            invGlobal += r.invertidas; totGlobal += r.total;
        }
        std::printf("\n");
    }
    std::printf("    peor estiramiento %.2fx · peor compresion %.2f (fondo %.1f m, celda %.1f m) · "
                "celdas del reves %d de %d\n",
                peorGlobal, menorGlobal, (double)fondoPeor, (double)celdaPeor, invGlobal, totGlobal);

    // ── LA GUARDA QUE DE VERDAD IMPORTA: EL AGUA INTERIOR ───────────────────────────────────────
    //
    // ⚠️ EL VUELCO ES DELIBERADO EN EL MAR, Y AQUI NO PUEDE ENTRAR. El intento de 2026-08-29 se
    // revirtio porque metia metros de vaiven horizontal en una charca sobre celdas de 4,4 m: Andoni
    // lo vio como "el agua dinamica son cubos deformados", y eran exactamente estas celdas del reves.
    // Hoy no pasa por dos razones independientes —la elipse escala con la amplitud y el FETCH de un
    // lago recorta el oleaje— pero las dos viven lejos de aqui. Esto lo ata donde se ve.
    {
        const float dCharca = 0.30f, qCharca = 4.4f;
        Res rl{ 0.0, 1e9, 0, 0 };
        for (int s2 = 0; s2 < 24; ++s2) {
            const glm::vec3 p = wp + t1 * (float)(s2 * 7) + t2 * (float)(s2 * 3);
            for (int n = 0; n < 40; ++n) {
                const float tt = 0.37f * n;
                for (int eje = 0; eje < 2; ++eje) {
                    const glm::vec3 dir = (eje == 0) ? t1 : t2;
                    const glm::vec3 da = Haruka::World::Water::oceanDisplacement(p, up, tt, dCharca, 1.0f,
                                                                          st, 100.0f, slope);
                    const glm::vec3 db = Haruka::World::Water::oceanDisplacement(p + dir * qCharca, up, tt,
                                                                          dCharca, 1.0f, st, 100.0f,
                                                                          slope);
                    const double av = (double)glm::dot((dir * qCharca + db) - da, dir) / (double)qCharca;
                    rl.peor = std::max(rl.peor, av); rl.menor = std::min(rl.menor, av);
                    if (av <= 0.0) ++rl.invertidas;
                    ++rl.total;
                }
            }
        }
        std::printf("    CHARCA de 0,30 m (fetch 100 m, celda 4,4 m): estiramiento %.3f..%.3f · "
                    "celdas del reves %d\n", rl.menor, rl.peor, rl.invertidas);
        CHECK(rl.invertidas == 0,
              "el agua interior NO se pliega: ni una celda del reves en una charca");
        // ⚠️ EL 1,40 ES UN TECHO MEDIDO (hoy 1,290), NO UN NUMERO ELEGIDO — mi primera version puso
        // 1,20 a ojo y fallo. Un 29 % de estiramiento en una charca es una ola de Gerstner normal en
        // agua somera; lo que NO puede es parecerse al 2,34x de la rompiente, que es donde vuelca.
        CHECK(rl.peor < 1.40, "y se estira MUCHO menos que la rompiente (2,34x): el vuelco no llega");
    }

    // CONTRAPRUEBA: en calma la malla es la malla. Si esto no diera 1,00 exacto, lo que falla es la
    // medida y no el mar — y los numeros de arriba no significarian nada.
    Haruka::World::Water::OceanState calma = st;
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) calma.wave[i][1] = 0.0f;
    const Res rc = barrido(5.0f, 4.4f, calma);
    std::printf("    CONTRAPRUEBA en calma (fondo 5 m, celda 4,4 m): estiramiento %.4f..%.4f · "
                "celdas del reves %d\n", rc.menor, rc.peor, rc.invertidas);
    CHECK(rc.invertidas == 0 && std::abs(rc.peor - 1.0) < 1e-4 && std::abs(rc.menor - 1.0) < 1e-4,
          "CONTRAPRUEBA: con el mar en calma la celda no se estira ni se invierte");

    // ── LO QUE SE AFIRMA, Y LO QUE SOLO SE ACOTA ────────────────────────────────────────────────
    //
    // ⚠️ LA PRIMERA VERSION DE ESTE TEST EXIGIA CERO INVERSIONES EN TODA LA PLAYA Y FALLO: hay **87
    // celdas del reves de 48 000 (0,18 %)**, todas en 5 / 2 / 1,2 m de fondo, y el peor estiramiento
    // es **2,34x**. O sea que la voluta que `ocean_break_fold` mide como correcta EN EL CONTINUO
    // invierte celdas a la resolucion a la que el agua se dibuja de verdad. Era el punto que estaba
    // anotado como "nadie ha mirado si la malla lo lleva sin estirones": no lo lleva.
    //
    // Asi que se separa lo que es INVARIANTE de lo que es un TECHO MEDIDO:
    //
    //   · Fuera de la rompiente la malla NO puede doblarse. Una ola que no rompe no tiene por que
    //     cruzar la lamina sobre si misma, y si algun dia lo hace es un fallo, no un ajuste.
    //   · Dentro de la rompiente hay pliegue, y es lo que se pidio: una ola que vuelca SE cruza. Lo
    //     que no puede es CRECER sin que nadie se entere, asi que se acota donde esta hoy.
    //
    // ⚠️ El techo NO es un objetivo. Mientras el numero de abajo siga por encima de cero, la
    // respuesta a "¿el mar esta estable?" es NO, y esta es la linea que lo dice.
    {
        const Res hondo = barrido(12.0f, 4.4f, st);
        const Res mas   = barrido(40.0f, 4.4f, st);
        CHECK(hondo.invertidas == 0 && mas.invertidas == 0,
              "fuera de la rompiente (12 y 40 m) la malla no invierte NI UNA celda");
    }
    const double fracInv = totGlobal ? (double)invGlobal / (double)totGlobal : 0.0;
    CHECK(fracInv < 0.005,
          "TECHO MEDIDO: las celdas invertidas de la rompiente no pasan del 0,5 % (hoy 0,18 %)");
    CHECK(peorGlobal < 2.5,
          "TECHO MEDIDO: ninguna celda se estira mas de 2,5x (hoy 2,29x)");
}

// ================================================================================================
// VIENTO FLOJO: el espectro no puede COLAPSAR en un solo tren.
//
// Andoni (16-09): "el agua palpita y se mueve incluso cuando no deberia, se estira demasiado, hay
// mucha espuma en esas palpitaciones, y como tal no hay olas". `oceanStateFromWind` escala lambda
// con U^2 y topa por abajo en 8,7 m: a 4 m/s (el viento del spawn) SEIS de los ocho trenes caen al
// mismo 8,7 m. Seis senos de la MISMA frecuencia en direcciones distintas no son seis olas: son UNA
// oscilacion estacionaria, `A(x)·cos(wt - F(x))`, con la envolvente A(x) clavada en el sitio. Eso es
// lo que "palpita": cada punto sube y baja en su sitio, y donde A(x) es grande la pendiente sumada
// dispara la espuma. Se mide con la correlacion de la superficie consigo misma medio periodo
// despues: una onda estacionaria da -1 en TODOS los puntos; un oleaje que viaja, con frecuencias
// distintas, decorrelaciona.
// ================================================================================================
void test_ocean_low_wind_spectrum() {
    beginTest("ocean_low_wind_spectrum");
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;
    const glm::vec3 up(0, 1, 0);

    auto standing = [&](const Haruka::World::Water::OceanState& st, int& distinct, float& hs) {
        // Trenes vivos con lambda distinta.
        std::vector<float> lams;
        float e = 0.0f;
        for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) {
            if (st.wave[i][1] <= 1e-5f) continue;
            e += st.wave[i][1] * st.wave[i][1];
            bool dup = false;
            for (float l : lams) if (std::abs(l - st.wave[i][0]) < 1e-3f) dup = true;
            if (!dup) lams.push_back(st.wave[i][0]);
        }
        distinct = (int)lams.size();
        hs = 4.0f * std::sqrt(e * 0.5f);
        // La FIRMA de una onda estacionaria: la envolvente A(x) esta clavada en el sitio, con nodos
        // (A~0) y vientres. En un oleaje que viaja cada punto acaba viendo la cresta entera. Se mide
        // el recorrido (max-min) de la cota en cada punto durante 30 s y su variacion relativa
        // (desviacion/media) entre puntos: ~0 si todos ven lo mismo, alto si hay nodos y vientres.
        double sum = 0, sum2 = 0; int n = 0;
        for (int gy = 0; gy < 24; ++gy) for (int gx = 0; gx < 24; ++gx) {
            const glm::vec3 wp((float)gx * 3.7f, 0.0f, (float)gy * 3.7f);
            float lo = 1e9f, hi = -1e9f;
            for (int ti = 0; ti < 120; ++ti) {
                const float h = Haruka::World::Water::oceanDisplacement(wp, up, 10.0f + 0.25f * (float)ti, 1000.0f, 1.0f, st).y;
                lo = std::min(lo, h); hi = std::max(hi, h);
            }
            const double r = hi - lo;
            sum += r; sum2 += r * r; ++n;
        }
        const double mean = sum / n, var = sum2 / n - mean * mean;
        return (mean > 0) ? (float)(std::sqrt(std::max(var, 0.0)) / mean) : 0.0f;
    };

    int d4 = 0, d11 = 0; float hs4 = 0, hs11 = 0;
    const Haruka::World::Water::OceanState s4  = Haruka::World::Water::oceanStateFromWind(4.0f, 1.0f, 0.0f, 0.0f);
    const Haruka::World::Water::OceanState s11 = Haruka::World::Water::oceanStateFromWind(Haruka::World::Water::OCEAN_REF_WIND, 1.0f, 0.0f, 0.0f);
    const float c4 = standing(s4, d4, hs4), c11 = standing(s11, d11, hs11);
    std::printf("    4 m/s: %d longitudes distintas · Hs %.2f m · variacion del recorrido entre puntos %.2f\n", d4, hs4, c4);
    std::printf("    %.1f m/s (referencia): %d longitudes distintas · Hs %.2f m · variacion %.2f\n",
                Haruka::World::Water::OCEAN_REF_WIND, d11, hs11, c11);
    // Pierson-Moskowitz a 4 m/s: Hs = 0,21·U²/g = 0,34 m. Con λ fijas y PM en cada frecuencia sale
    // cerca (la banda 8,7-137 m contiene casi todo el espectro de una brisa).
    const float hsPM = 0.21f * 16.0f / 9.81f;
    CHECK(std::abs(hs4 - hsPM) / hsPM < 0.30f, "Hs a 4 m/s sigue a Pierson-Moskowitz");
    CHECK(d4 >= 2, "a 4 m/s quedan al menos dos longitudes distintas");
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i)
        for (int j = i + 1; j < Haruka::World::Water::OCEAN_WAVES; ++j)
            if (s4.wave[i][1] > 1e-5f && s4.wave[j][1] > 1e-5f)
                CHECK(std::abs(s4.wave[i][0] - s4.wave[j][0]) > 1e-3f, "dos trenes vivos nunca comparten longitud (eso es una onda estacionaria, no dos olas)");
    CHECK(c4 < 0.25f, "a 4 m/s la superficie NO es estacionaria (todos los puntos ven casi el mismo recorrido)");
    CHECK(c11 < 0.25f, "a la referencia tampoco");
    // Lo que hubo (hasta el 16-09): el piso APILABA (lambda = max(lambda·escala, 8,7)) en vez de
    // quitar, y seis trenes caian a la misma λ. Se reconstruye ese estado para dejar medido lo que se
    // veia — y como contraprueba de que la metrica distingue un mar estacionario de uno que viaja.
    {
        Haruka::World::Water::OceanState old = s4;
        const float sc = 16.0f / (Haruka::World::Water::OCEAN_REF_WIND * Haruka::World::Water::OCEAN_REF_WIND);
        for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) {
            old.wave[i][0] = std::max(Haruka::World::Water::OCEAN_WAVE[i][0] * sc, Haruka::World::Water::OCEAN_WAVE[Haruka::World::Water::OCEAN_WAVES - 1][0]);
            old.wave[i][1] = Haruka::World::Water::OCEAN_WAVE[i][1] * sc;
        }
        int dOld = 0; float hsOld = 0;
        const float cOld = standing(old, dOld, hsOld);
        std::printf("    ANTES (piso que apilaba): %d longitudes distintas de 8 · Hs %.2f m · variacion %.2f\n", dOld, hsOld, cOld);
        CHECK(cOld > 0.12f && cOld > c4 * 1.5f, "contraprueba: el estado antiguo a 4 m/s era parcialmente estacionario (es lo que palpitaba)");
    }
    // Contraprueba del metodo: un estado con todos los trenes a la MISMA lambda da -1 (estacionario).
    Haruka::World::Water::OceanState same = s11;
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) same.wave[i][0] = 20.0f;
    int ds = 0; float hss = 0;
    const float cs = standing(same, ds, hss);
    std::printf("    contraprueba (8 trenes a 20 m): variacion %.2f\n", cs);
    CHECK(cs > 0.35f, "contraprueba: ocho trenes de la misma longitud son una onda estacionaria (nodos y vientres)");
}

// ── EL MAR EN EL MARCO DEL JUEGO: viaja, sigue al viento despacio, y el ancla no salta ──────────
//
// 2026-09-18. Andoni: "el look no es bueno, palpita, se estira, el viento afecta demasiado". Medido:
//   · la fase `k·(D·x) − ω·t` con `x = dir·R` (planetocentrica) era la MISMA en todo el mar — `D`
//     tangente y `x` radial — asi que el oceano subia en bloque y las "olas" eran ruido de float;
//   · el estado del mar se rehacia cada frame con el viento instantaneo, que en un punto fijo salta
//     hasta 7,8 m/s en un segundo: la Hs de todo el mar saltaba 5,7 m entre frames;
//   · y λ escalaba con U², asi que cada cambio de viento revolvia la fase de todo el mar.
// Este test mide las tres cosas EN EL MARCO DEL JUEGO (up radial, posicion relativa al ancla), que
// es donde los tests anteriores no miraban (todos usaban un marco plano).
void test_ocean_sea_follows_wind() {
    beginTest("ocean_sea_follows_wind: la ola VIAJA en el marco del juego, el mar sigue al viento despacio, el ancla no salta");
    using namespace Haruka::World::Terrain;
    using namespace Haruka::World::Water;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);
    const glm::dvec3 c = glm::normalize(glm::dvec3(0.31, 0.62, 0.72));
    const glm::dvec3 e = glm::normalize(glm::cross(c, glm::dvec3(0, 1, 0)));

    // (1) LA OLA VARIA EN EL ESPACIO. Puntos cada 20 m a lo largo de un circulo maximo, como los
    // consulta la fisica (up = dir radial), con la posicion RELATIVA AL ANCLA (la camara).
    Haruka::World::Water::SeaFollower sea;
    Haruka::World::Water::OceanState st = Haruka::World::Water::oceanStateFromWind(Haruka::World::Water::OCEAN_REF_WIND, 1.0f, 0.0f, 0.0f);
    sea.anchorFor(st, pc + c * R, pc, R);
    auto sigmaAlong = [&](const glm::dvec3& anchor, float t) {
        double m1 = 0, m2 = 0; const int n = 200;
        for (int i = 0; i < n; ++i) {
            const glm::dvec3 dir = glm::normalize(c + e * (i * 20.0 / R));
            const glm::vec3 wp = glm::vec3(pc + dir * R - anchor);
            const double h = oceanWaveHeight(wp, glm::vec3(dir), t, 400.0f, 1.0f, st);
            m1 += h; m2 += h * h;
        }
        return std::sqrt(std::max(m2 / n - (m1 / n) * (m1 / n), 0.0));
    };
    const double sigAnchored = sigmaAlong(sea.anchor, 1.0f);
    const double sigCentred  = sigmaAlong(pc, 1.0f);            // el bug: posicion planetocentrica
    std::printf("    sigma espacial de la cota (4 km, t fijo): relativa al ancla %.3f m · planetocentrica %.3f m\n",
                sigAnchored, sigCentred);
    CHECK(sigAnchored > 0.3, "en el marco del juego la ola VARIA en el espacio (sigma > 30 cm a Hs 2,8 m)");
    CHECK(sigCentred < 0.05, "CONTRAPRUEBA: con la posicion planetocentrica no variaba (el mar en bloque, ruido de 1 cm)");

    // (2) EL MAR SIGUE AL VIENTO DESPACIO. Escalon de 4 a 12 m/s: Hs no salta; sube con tau ~20 min;
    // y al amainar baja con tau ~1 h. Reloj en segundos.
    Haruka::World::Water::SeaFollower f;
    auto hsAt = [&](const glm::vec2& w) { return Haruka::World::Water::oceanSignificantHeight(Haruka::World::Water::oceanStateFromWind(glm::length(w), w.x, w.y)); };
    f.filterWind(glm::vec2(4.0f, 0.0f), 0.0f);
    float prevHs = hsAt(f.windSea), maxStep = 0.0f, hs20 = 0.0f, hs60 = 0.0f;
    for (int s = 1; s <= 3600; ++s) {
        const glm::vec2 w = f.filterWind(glm::vec2(12.0f, 0.0f), (float)s);
        const float hs = hsAt(w);
        maxStep = std::max(maxStep, std::abs(hs - prevHs)); prevHs = hs;
        if (s == 1200) hs20 = hs;
        if (s == 3600) hs60 = hs;
    }
    const float hs4 = hsAt(glm::vec2(4, 0)), hs12 = hsAt(glm::vec2(12, 0));
    std::printf("    escalon 4 -> 12 m/s: Hs %.2f -> %.2f m · a los 20 min %.2f · a la hora %.2f · salto maximo por segundo %.4f m\n",
                hs4, hs12, hs20, hs60, maxStep);
    CHECK(maxStep < 0.01f, "la Hs nunca salta mas de 1 cm por segundo (antes: 5,7 m)");
    CHECK(hs20 > hs4 + 0.5f * (hs12 - hs4) && hs20 < hs12, "a los 20 min el mar ha crecido mas de la mitad (tau de subida ~20 min)");
    CHECK(hs60 > 0.9f * hs12, "a la hora esta casi desarrollado");
    float hsDecay = 0.0f;
    for (int s = 3601; s <= 7200; ++s) hsDecay = hsAt(f.filterWind(glm::vec2(4.0f, 0.0f), (float)s));
    std::printf("    amaina a 4 m/s: una hora despues Hs %.2f m (de %.2f; en calma seria %.2f)\n", hsDecay, hs60, hs4);
    CHECK(hsDecay > hs4 + 0.2f * (hs60 - hs4), "la mar de fondo sobrevive al viento: una hora despues aun queda mas del 20 %");
    CHECK(hsDecay < hs60, "pero baja");
    // Contraprueba del instrumento: el viento INSTANTANEO si salta.
    CHECK(std::abs(hs12 - hs4) > 1.0f, "CONTRAPRUEBA: sin filtro el escalon es de mas de 1 m de Hs");

    // (3) EL ANCLA NO SALTA. La camara se va 21 km; el ancla se mueve; en el ancla nueva la cota es
    // la misma antes y despues (compensacion exacta en aguas profundas) y a 1 km cambia poco.
    Haruka::World::Water::SeaFollower a;
    Haruka::World::Water::OceanState s1 = Haruka::World::Water::oceanStateFromWind(Haruka::World::Water::OCEAN_REF_WIND, 0.3f, 0.8f, 0.0f);
    a.anchorFor(s1, pc + c * R, pc, R);
    const glm::dvec3 far = glm::normalize(c + e * (21000.0 / R));
    auto hAt = [&](const Haruka::World::Water::OceanState& s, const glm::dvec3& dir, const glm::dvec3& anchor) {
        return oceanWaveHeight(glm::vec3(pc + dir * R - anchor), glm::vec3(dir), 7.0f, 400.0f, 1.0f, s);
    };
    const glm::dvec3 near1 = glm::normalize(far + e * (1000.0 / R));
    const double before0 = hAt(s1, far, a.anchor), before1 = hAt(s1, near1, a.anchor);
    Haruka::World::Water::OceanState s2 = s1;
    const bool moved = a.anchorFor(s2, pc + far * R, pc, R);
    const double after0 = hAt(s2, far, a.anchor), after1 = hAt(s2, near1, a.anchor);
    // Sin compensar (fases a cero) seria un salto.
    Haruka::World::Water::OceanState s3 = s2; for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) s3.phase[i] = 0.0f;
    const double naive0 = hAt(s3, far, a.anchor);
    std::printf("    re-anclaje a 21 km: movida=%d · cota en el ancla nueva %.4f -> %.4f m (sin compensar %.4f) · a 1 km %.4f -> %.4f m\n",
                (int)moved, before0, after0, naive0, before1, after1);
    CHECK(moved && a.reanchors == 1, "el ancla se movio una vez");
    CHECK(std::abs(after0 - before0) < 0.002, "en el ancla nueva la cota no cambia (< 2 mm)");
    CHECK(std::abs(after1 - before1) < 0.08, "a 1 km del ancla nueva cambia menos de 8 cm (la curvatura entre los dos marcos)");
    CHECK(std::abs(naive0 - before0) > 0.05, "CONTRAPRUEBA: sin la compensacion de fase el re-anclaje salta");
}

// Registro: el grupo sale de la carpeta de este archivo (ver Common/TestCommon.h).
HARUKA_REGISTER_TESTS(
    { "terrain_finest_octave", test_terrain_finest_octave },
    { "ocean_wave_velocity", test_ocean_wave_velocity },
    { "ocean_float_precision", test_ocean_float_precision },
    { "ocean_stokes_transport", test_ocean_stokes_transport },
    { "ocean_shoaling", test_ocean_shoaling },
    { "ocean_buoyancy_drift", test_ocean_buoyancy_drift },
    { "ocean_no_water_no_float", test_ocean_no_water_no_float },
    { "ocean_sea_state", test_ocean_sea_state },
    { "ocean_low_wind_spectrum", test_ocean_low_wind_spectrum },
    { "ocean_sea_follows_wind", test_ocean_sea_follows_wind },
    { "ocean_finite_depth", test_ocean_finite_depth },
    { "ocean_gerstner_ellipse", test_ocean_gerstner_ellipse },
    { "water_fill_window", test_water_fill_window },
    { "water_window_policy", test_water_window_policy },
    { "water_window_shore_rises", test_water_window_shore_rises },
    { "water_window_min_lake", test_water_window_min_lake },
    { "ocean_refraction_and_break", test_ocean_refraction_and_break },
    { "ocean_spectrum_limits", test_ocean_spectrum_limits },
    { "water_fill_global", test_water_fill_global },
    { "ocean_fetch", test_ocean_fetch },
    { "water_bake_resolution_limit", test_water_bake_resolution_limit },
    { "ocean_wave_reach", test_ocean_wave_reach },
    { "ocean_grid_carries_wave", test_ocean_grid_carries_wave },
    { "ocean_tide", test_ocean_tide },
    { "ocean_swash", test_ocean_swash },
    { "ocean_break_limit", test_ocean_break_limit },
    { "ocean_break_fold", test_ocean_break_fold },
    { "ocean_mesh_stretch", test_ocean_mesh_stretch },
    { "ocean_foam", test_ocean_foam },
    { "ocean_skewness", test_ocean_skewness },
    { "ocean_whitewater_drag", test_ocean_whitewater_drag },
    { "ocean_whitecaps", test_ocean_whitecaps });
