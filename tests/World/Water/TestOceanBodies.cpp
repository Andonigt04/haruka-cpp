// Lo que flota: transporte de Stokes, deriva y flotacion, arrastre de la espuma, y que sin agua no se flota.
// (Partido de TestOcean.cpp; el registro de todos sigue allí, en el orden de siempre.)
#include "World/Water/OceanTestCommon.h"

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
    const glm::vec3 D0 = glm::normalize(t1 * Haruka::World::Water::OCEAN_WAVE[0][2]
                                      + t2 * Haruka::World::Water::OCEAN_WAVE[0][3]);

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
