// ================================================================================================
// TIEMPO ADVERSO: la severidad sale de las CONDICIONES, y el embudo sale de la severidad.
//
// El problema que resuelve
// -----------------------
// Un tornado es la cosa más fácil de falsear de un motor: un temporizador, una partícula girando y
// un empujón al jugador. Eso se cae por tres sitios a la vez —no es determinista (y el DGS no puede
// validar a un jugador arrastrado por él), no responde al clima (sale con el cielo despejado, que es
// EXACTAMENTE el bug que este sistema nació para arreglar con la lluvia) y no es un campo (no se
// puede consultar desde el agua, ni desde un prop suelto, ni a media altura).
//
// Lo que este banco fija
// ----------------------
//  1. EL VIENTO DE FONDO ESTÁ ACOTADO. Medido en el juego valía 968,9 m/s. Con contraprueba: el
//     techo no puede estar haciendo todo el trabajo — si casi todas las muestras salen pegadas al
//     tope, el campo es degenerado y el test lo dice.
//  2. EL EMBUDO ES CONDICIONAL. Con aire frío y seco NO sale ni uno en todo el planeta y todas las
//     horas; con aire cálido y húmedo sí. CON CONTRAPRUEBA en los dos sentidos: un sistema "siempre"
//     pasa la segunda mitad y revienta la primera, y un `return 0` al revés.
//  3. ES RARO. Que pueda haber tornado no significa que haya siempre: se mide la fracción de tiempo.
//  4. EL CAMPO ES DE RANKINE, no un empujón. Máximo EN el núcleo (no en el eje, no en el infinito),
//     caída 1/r fuera, cero exacto más allá del alcance y por encima de la nube madre.
//     CON CONTRAPRUEBA: un campo lineal (el que sale de "empuja más cuanto más cerca del centro")
//     falla las tres.
//  5. SUCCIONA Y LEVANTA. La componente radial apunta HACIA DENTRO cerca del suelo y la vertical es
//     positiva en el núcleo. Sin esto un vórtice es un tiovivo: todo orbita y nada entra.
//  6. ES DETERMINISTA. Misma seed y mismo reloj ⇒ los mismos tornados, con el mismo id. Es el
//     requisito de multijugador y sale gratis por ser función pura. Con contraprueba: otra seed da
//     otros tornados (si no, el determinismo sería el de una constante).
//  7. LA TROMBA MARINA NO ES UN TORNADO PINTADO DE AZUL: se forma con menos energía y es más
//     pequeña y más débil. Las dos mitades se comprueban, porque sólo la primera sería un regalo.
// ================================================================================================
#include "test_common.h"
#include "core/weather_system.h"
#include "game/character.h"
#include "physics/physics_engine.h"

#include <cmath>
#include <cstdio>
#include <vector>

using Haruka::WeatherSample;
using Haruka::WeatherSystem;
using Vortex = WeatherSystem::Vortex;

namespace {

constexpr double kEarthR = 6371000.0;

/// Direcciones repartidas por la esfera (espiral áurea: uniforme de verdad, no amontona en polos).
std::vector<glm::dvec3> sphereDirs(int n) {
    std::vector<glm::dvec3> out;
    out.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        const double z   = 1.0 - 2.0 * (i + 0.5) / (double)n;
        const double r   = std::sqrt(std::max(0.0, 1.0 - z * z));
        const double phi = 2.39996322972865332 * i;
        out.push_back(glm::dvec3(r * std::cos(phi), r * std::sin(phi), z));
    }
    return out;
}

/// Un punto a `rM` metros del eje del vórtice, sobre el planeta. Se desplaza por un círculo máximo
/// en una dirección tangente cualquiera: la distancia al eje es exactamente el ángulo por el radio.
glm::dvec3 offsetFromAxis(const glm::dvec3& axisDir, double rM) {
    glm::dvec3 t = glm::cross(axisDir, glm::dvec3(0, 1, 0));
    if (glm::length(t) < 1e-6) t = glm::cross(axisDir, glm::dvec3(1, 0, 0));
    t = glm::normalize(t);
    const double a = rM / kEarthR;
    return glm::normalize(axisDir * std::cos(a) + t * std::sin(a));
}

/// Una muestra de tormenta fabricada a mano, para poder probar el CAMPO sin depender de que el
/// planeta tenga hoy una tormenta. No sustituye a las pruebas sobre el clima real (abajo): fija el
/// contrato de `vortexWindAt`, que es pura geometría del vórtice.
Vortex makeVortex(double coreM, float windMS, float topM, float spin) {
    Vortex v;
    v.dir = glm::normalize(glm::dvec3(0.3, 0.7, 0.2));
    v.coreRadiusM = coreM;
    v.windMS = windMS;
    v.topM = topM;
    v.spin = spin;
    v.age01 = 0.5f;
    return v;
}

} // namespace

void test_weather_severe() {
    beginTest("weather_severe");

    WeatherSystem W;
    W.configure(4242u);
    const std::vector<glm::dvec3> dirs = sphereDirs(600);

    // ── 1. EL VIENTO DE FONDO ESTÁ ACOTADO ──────────────────────────────────────────────────────
    //
    // ⚠️ Este número se midió EN EL JUEGO antes de existir este test: `[Mar] viento 968.9 m/s`. La
    // causa era que el arrastre de cada frente valía `ω·R·0.35`, o sea su velocidad de avance sobre
    // el suelo — que con un frente que cruza el planeta en media hora son kilómetros por segundo. El
    // mar no se enteraba porque `oceanStateFromWind` topa la entrada a 22 m/s: el clamp de OTRO
    // sistema tapaba el bug, y por eso duró.
    {
        double maxWind = 0.0, sumWind = 0.0;
        int pinned = 0, samples = 0;
        for (int ti = 0; ti < 60; ++ti) {
            W.setTime(ti * 61.0);
            for (size_t i = 0; i < dirs.size(); ++i) {
                const float humid = 0.15f + 0.80f * ((i * 37) % 100) / 100.0f;
                const float tempC = -12.0f + 40.0f * ((i * 53) % 100) / 100.0f;
                const WeatherSample s = W.sampleAt(dirs[i], tempC, humid);
                const double sp = glm::length(s.wind);
                maxWind = std::max(maxWind, sp);
                sumWind += sp;
                if (sp > WeatherSystem::kMaxWindMS * 0.999) ++pinned;
                ++samples;
            }
        }
        const double pctPinned = 100.0 * pinned / samples;
        std::printf("    viento de fondo: max %.1f m/s · media %.1f m/s · %.2f%% pegadas al techo (%.0f m/s)\n",
                    maxWind, sumWind / samples, pctPinned, (double)WeatherSystem::kMaxWindMS);
        CHECK(maxWind <= (double)WeatherSystem::kMaxWindMS + 1e-6,
              "el viento de fondo NUNCA pasa del techo (eran 968,9 m/s medidos en el juego)");
        // CONTRAPRUEBA del techo: si casi todo sale pegado al tope, acotar habría APLANADO el campo
        // —un planeta entero soplando exactamente igual— y el punto 1 pasaría por degeneración.
        CHECK(pctPinned < 2.0, "el techo NO es el que manda: casi ninguna muestra llega a saturarlo");
        CHECK(maxWind > 20.0, "y sigue habiendo vendavales de verdad (el techo no ha matado el viento)");
    }

    // ── CUÁNTO DA DE SÍ EL PLANETA ──────────────────────────────────────────────────────────────
    //
    // Diagnóstico, no aserción, y está aquí porque sin él «no salen tornados» y «no puede haber
    // tornados» se ven igual desde fuera — que es el mismo par de síntomas indistinguibles que ya
    // costó una sesión con el mar. Si la torre máxima del planeta no llega al umbral, el problema
    // no está en el disparo sino en la nube.
    {
        auto techo = [&](const char* etiqueta, float tempC, float humid) {
            double maxThick = 0.0, maxPot = 0.0, maxPotW = 0.0, maxPrecip = 0.0, maxCover = 0.0;
            for (int ti = 0; ti < 120; ++ti) {
                W.setTime(ti * 37.0);
                for (const glm::dvec3& d : dirs) {
                    const WeatherSample s = W.sampleAt(d, tempC, humid);
                    maxThick  = std::max(maxThick, (double)s.cloudThicknessM());
                    maxPrecip = std::max(maxPrecip, (double)s.precip);
                    maxCover  = std::max(maxCover, (double)s.cloudCover);
                    maxPot    = std::max(maxPot, (double)WeatherSystem::vortexPotential(s, false));
                    maxPotW   = std::max(maxPotW, (double)WeatherSystem::vortexPotential(s, true));
                }
            }
            std::printf("    techo %-22s cobertura %.2f · precip %.2f · torre %5.0f m · potencial "
                        "tierra %.2f / agua %.2f\n",
                        etiqueta, maxCover, maxPrecip, maxThick, maxPot, maxPotW);
            return maxPot;
        };
        std::printf("    (umbral de torre %.0f m · torre plena %.0f m · disparo %.2f)\n",
                    (double)WeatherSystem::kVortexMinTowerM,
                    (double)WeatherSystem::kVortexFullTowerM,
                    (double)WeatherSystem::kVortexTrigger);
        const double potMax = techo("aire perfecto 28C/0.90:", 28.0f, 0.90f);
        const double potTrop = techo("cinturon humedo 26C/0.70:", 26.0f, 0.70f);
        techo("media del mundo 18C/0.29:", 18.0f, 0.286f);
        techo("desierto 30C/0.10:", 30.0f, 0.10f);
        CHECK(potMax > WeatherSystem::kVortexTrigger,
              "el planeta PUEDE hacer un embudo en su peor tormenta (si no, el disparo es inalcanzable)");
        CHECK(potTrop > WeatherSystem::kVortexTrigger,
              "y el cinturon humedo REAL del mundo tambien llega (si no, no habria tornados jamas)");
    }

    // ── 2/3. EL EMBUDO ES CONDICIONAL Y ES RARO ─────────────────────────────────────────────────
    //
    // Las dos mitades a la vez: en aire frío y seco no puede salir NI UNO, y en aire cálido y húmedo
    // tiene que salir alguno. Cualquiera de las dos sola se pasa con una implementación tonta
    // (`return 0` pasa la primera; "siempre uno" pasa la segunda).
    //
    // ⚠️ Y OJO CON LA PREGUNTA: los embudos se consultan POR VECINDAD (ver `kVortexCellRad`), así que
    // no existe «los del planeta». Para saber si el modelo PUEDE hacer uno se barre un puñado de
    // observadores repartidos por la esfera; para saber si te va a pasar por encima —que es otra
    // cosa completamente distinta— está el bloque de más abajo, desde un punto fijo.
    {
        Vortex buf[WeatherSystem::kMaxVortices];
        const double kBarrido = 400000.0;    // 400 km alrededor de cada observador
        const std::vector<glm::dvec3> obs = sphereDirs(40);
        auto contar = [&](float tC, float hu, bool water) {
            int n = 0;
            for (const glm::dvec3& o : obs)
                n += W.activeVortices(o, kBarrido, kEarthR, buf, WeatherSystem::kMaxVortices, tC, hu, water);
            return n;
        };

        int frio = 0, tornados = 0, trombas = 0, ticksConEmbudo = 0;
        const int ticks = 400;
        float maxViento = 0.0f; double maxCore = 0.0;

        for (int ti = 0; ti < ticks; ++ti) {
            W.setTime(ti * 37.0);
            frio += contar(-6.0f, 0.10f, false);

            // Mismo barrido, aire cálido y húmedo: sobre tierra (tornados) y sobre mar (trombas).
            int n = 0;
            for (const glm::dvec3& o : obs) {
                const int m = W.activeVortices(o, kBarrido, kEarthR, buf,
                                               WeatherSystem::kMaxVortices, 28.0f, 0.90f, false);
                n += m;
                for (int i = 0; i < m; ++i) {
                    maxViento = std::max(maxViento, buf[i].windMS);
                    maxCore   = std::max(maxCore, buf[i].coreRadiusM);
                    CHECK(buf[i].age01 >= 0.0f && buf[i].age01 <= 1.0f, "la edad del embudo esta en [0,1]");
                    CHECK(buf[i].topM > 100.0f, "la columna llega hasta su nube madre");
                    CHECK(std::fabs(glm::length(buf[i].dir) - 1.0) < 1e-9, "el eje es unitario");
                }
            }
            tornados += n;
            if (n > 0) ++ticksConEmbudo;
            trombas += contar(28.0f, 0.90f, true);
        }
        std::printf("    embudos (barrido de 40 observadores x 400 km): aire frio/seco %d · "
                    "aire calido/humedo %d en %d ticks (%.1f%% de los ticks con alguno) · "
                    "viento max %.0f m/s · nucleo max %.0f m\n",
                    frio, tornados, ticks, 100.0f * ticksConEmbudo / ticks, maxViento, maxCore);
        std::printf("    sobre agua %d vs sobre tierra %d en las mismas condiciones\n",
                    trombas, tornados);

        const int calido = tornados;
        CHECK(frio == 0, "con aire frio y seco NO se forma ni un embudo en ninguna parte");
        CHECK(calido > 0, "con aire calido y humedo SI se forman (el test no pasa por vacuidad)");
        // ⚠️ AQUI NO SE EXIGE QUE SEAN RAROS, y es a propósito: estas condiciones son las del aire
        // MAS favorable que admite el modelo, aplicadas al planeta ENTERO y a todas horas. Que en
        // ese mundo imposible haya casi siempre un embudo en alguna parte no dice nada malo. La
        // rareza se mide abajo, con la humedad que este mundo tiene de verdad.
        CHECK(maxViento > 60.0f, "un tornado sopla como un tornado");
        CHECK(maxViento <= 115.0f, "y no como un chiste (acotado por arriba)");
        // 7. La tromba se forma MÁS FÁCIL...
        CHECK(trombas >= tornados, "sobre agua el embudo se forma con menos energia (mas frecuente)");
    }

    // ── 3. CON LA HUMEDAD DE ESTE MUNDO, ¿CADA CUÁNTO? ──────────────────────────────────────────
    //
    // La medida que de verdad importa para jugar. La humedad MEDIA del planeta es 0,286 (la imprime
    // el motor al cargar el mundo: `clima del planeta: humedad [0.059, 0.882] media 0.286`), no el
    // 0,90 de arriba. Con ese aire el embudo tiene que ser un EVENTO: algo que se ve un par de veces
    // y se recuerda, no el tiempo de los martes.
    //
    // ⚠️ Y es una cota SUPERIOR de la frecuencia real, no una estimación: se aplica la misma humedad
    // a todo el planeta, cuando en el mundo la selva y el desierto no coexisten en cada punto.
    //
    // ⚠️ ES LA MEDIDA QUE MANDA SOBRE EL TAMAÑO DE CELDA Y LA VIDA DEL HUECO. Con la colocación vieja
    // (2 huecos por frente, 28 en todo el planeta) esta tabla salía **0,00 % a las tres distancias,
    // incluida la de 300 km**: el planeta casi siempre tenía tres embudos y ningún jugador iba a ver
    // uno jamás. «Hay un embudo en el planeta» y «hay un embudo cerca de mí» no son la misma cifra ni
    // se parecen, y confundirlas es como mirar la cobertura media del planeta para saber si llueve en
    // tu tejado.
    {
        Vortex buf[WeatherSystem::kMaxVortices];
        const int ticks = 2000;                 // 2000 · 37 s ≈ 20 horas de clima
        const glm::dvec3 obs = glm::normalize(glm::dvec3(0.4, 0.35, 0.85));

        auto desdeAqui = [&](float tC, float hu, double radioM) {
            int conEmbudo = 0;
            for (int ti = 0; ti < ticks; ++ti) {
                W.setTime(ti * 37.0);
                if (W.activeVortices(obs, radioM, kEarthR, buf,
                                     WeatherSystem::kMaxVortices, tC, hu, false) > 0) ++conEmbudo;
            }
            return 100.0 * conEmbudo / ticks;
        };

        const double cercaTrop = desdeAqui(26.0f, 0.70f, 2000.0);
        const double medioTrop = desdeAqui(26.0f, 0.70f, 30000.0);
        const double lejosTrop = desdeAqui(26.0f, 0.70f, 300000.0);
        const double medioMedia = desdeAqui(18.0f, 0.286f, 30000.0);

        std::printf("    DESDE UN PUNTO FIJO (20 h de clima) · cinturon humedo: embudo a < 2 km el "
                    "%.2f%% del tiempo · a < 30 km el %.2f%% · a < 300 km el %.2f%%\n",
                    cercaTrop, medioTrop, lejosTrop);
        std::printf("    el mismo punto con la humedad MEDIA del mundo (0.286): a < 30 km el %.2f%%\n",
                    medioMedia);

        CHECK(lejosTrop > 0.5, "en el cinturon humedo SE VEN: alguno a 300 km mas del 0,5 % del tiempo");
        CHECK(cercaTrop * 8.0 < 100.0, "pero que te pase uno POR ENCIMA sigue siendo raro (< 12,5 %)");
        CHECK(medioMedia < medioTrop,
              "y se concentran donde hay energia: menos con la humedad media que en el cinturon");
    }

    // ── 7. …Y ES MÁS PEQUEÑA Y MÁS DÉBIL ────────────────────────────────────────────────────────
    //
    // La otra mitad. Sólo con "más frecuente" habríamos hecho del mar el sitio más peligroso del
    // planeta, que es justo lo contrario de lo que pasa.
    {
        Vortex bufT[WeatherSystem::kMaxVortices], bufW[WeatherSystem::kMaxVortices];
        const std::vector<glm::dvec3> obs = sphereDirs(40);
        float pkT = 0.0f, pkW = 0.0f; double coreT = 0.0, coreW = 0.0;
        for (int ti = 0; ti < 400; ++ti) {
            W.setTime(ti * 37.0);
            for (const glm::dvec3& o : obs) {
                const int nt = W.activeVortices(o, 400000.0, kEarthR, bufT, WeatherSystem::kMaxVortices, 28.0f, 0.90f, false);
                const int nw = W.activeVortices(o, 400000.0, kEarthR, bufW, WeatherSystem::kMaxVortices, 28.0f, 0.90f, true);
                for (int i = 0; i < nt; ++i) { pkT = std::max(pkT, bufT[i].windMS); coreT = std::max(coreT, bufT[i].coreRadiusM); }
                for (int i = 0; i < nw; ++i) { pkW = std::max(pkW, bufW[i].windMS); coreW = std::max(coreW, bufW[i].coreRadiusM); }
            }
        }
        std::printf("    tornado: %.0f m/s / nucleo %.0f m   ·   tromba: %.0f m/s / nucleo %.0f m\n",
                    pkT, coreT, pkW, coreW);
        CHECK(pkW < pkT,     "la tromba marina sopla MENOS que el tornado");
        CHECK(coreW < coreT, "y su nucleo es MAS ESTRECHO");
    }

    // ── 6. DETERMINISMO ─────────────────────────────────────────────────────────────────────────
    {
        Vortex a[WeatherSystem::kMaxVortices], b[WeatherSystem::kMaxVortices];
        WeatherSystem W2; W2.configure(4242u);
        WeatherSystem W3; W3.configure(777u);
        // Dos observadores DISTINTOS mirando la misma zona: tienen que ver el mismo embudo. Es lo que
        // de verdad exige el multijugador —dos jugadores separados 20 km en la misma tormenta— y no
        // lo comprobaría un test que preguntara siempre desde el mismo sitio.
        const glm::dvec3 o1 = glm::normalize(glm::dvec3(0.4, 0.35, 0.85));
        const glm::dvec3 o2 = glm::normalize(o1 + glm::dvec3(0.0015, 0.0, 0.0));   // ~10 km al lado
        int comparados = 0, iguales = 0, distintosPorSeed = 0, vistosPorLosDos = 0;
        const std::vector<glm::dvec3> obs = sphereDirs(40);
        for (int ti = 0; ti < 200; ++ti) {
            const double t = ti * 37.0;
            W.setTime(t); W2.setTime(t); W3.setTime(t);
            for (const glm::dvec3& o : obs) {
                const int na = W.activeVortices(o, 400000.0, kEarthR, a, WeatherSystem::kMaxVortices, 28.0f, 0.90f, false);
                const int nb = W2.activeVortices(o, 400000.0, kEarthR, b, WeatherSystem::kMaxVortices, 28.0f, 0.90f, false);
                CHECK(na == nb, "misma seed y mismo reloj ⇒ el MISMO numero de embudos");
                for (int i = 0; i < na && i < nb; ++i) {
                    ++comparados;
                    if (a[i].id == b[i].id && glm::length(a[i].dir - b[i].dir) < 1e-12
                        && a[i].windMS == b[i].windMS) ++iguales;
                }
                const int nc = W3.activeVortices(o, 400000.0, kEarthR, b, WeatherSystem::kMaxVortices, 28.0f, 0.90f, false);
                if (nc != na) ++distintosPorSeed;
                else for (int i = 0; i < nc; ++i)
                    if (glm::length(a[i].dir - b[i].dir) > 1e-6) { ++distintosPorSeed; break; }
            }
            // Dos vecinos, el mismo instante: los embudos que caen en el solape han de ser LOS MISMOS.
            const int n1 = W.activeVortices(o1, 200000.0, kEarthR, a, WeatherSystem::kMaxVortices, 26.0f, 0.70f, false);
            const int n2 = W.activeVortices(o2, 200000.0, kEarthR, b, WeatherSystem::kMaxVortices, 26.0f, 0.70f, false);
            for (int i = 0; i < n1; ++i)
                for (int j = 0; j < n2; ++j)
                    if (a[i].id == b[j].id) {
                        ++vistosPorLosDos;
                        CHECK(glm::length(a[i].dir - b[j].dir) < 1e-12 && a[i].windMS == b[j].windMS,
                              "dos observadores vecinos ven EL MISMO embudo, no uno parecido");
                    }
        }
        std::printf("    determinismo: %d embudos comparados, %d identicos · otra seed difiere en %d "
                    "consultas · %d vistos por DOS observadores a la vez\n",
                    comparados, iguales, distintosPorSeed, vistosPorLosDos);
        CHECK(comparados > 0 && iguales == comparados,
              "dos clientes con el mismo reloj ven EL MISMO tornado (id, eje y fuerza)");
        // CONTRAPRUEBA: si el determinismo fuera el de una constante, otra seed daría lo mismo.
        CHECK(distintosPorSeed > 100, "otra seed da OTROS tornados (no es determinismo por constante)");
        CHECK(vistosPorLosDos > 0,
              "y el solape entre dos observadores no esta vacio (si no, la comprobacion es vacua)");
    }

    // ── 4/5. EL CAMPO ES DE RANKINE, SUCCIONA Y LEVANTA ─────────────────────────────────────────
    {
        const Vortex v = makeVortex(80.0, 90.0f, 1400.0f, +1.0f);
        const float alt = 2.0f;   // a ras de suelo, que es donde importa

        auto speedAt = [&](double rM) {
            return glm::length(WeatherSystem::vortexWindAt(v, offsetFromAxis(v.dir, rM), kEarthR, alt));
        };

        const double vEje  = speedAt(0.0);
        const double vCore = speedAt(v.coreRadiusM);
        const double v2    = speedAt(v.coreRadiusM * 2.0);
        const double v4    = speedAt(v.coreRadiusM * 4.0);
        const double vOut  = speedAt(v.coreRadiusM * WeatherSystem::kVortexReachCores * 1.01);

        // Barrido fino: ¿dónde está de verdad el máximo horizontal?
        double bestR = 0.0, bestH = 0.0;
        for (int i = 1; i <= 400; ++i) {
            const double r = v.coreRadiusM * WeatherSystem::kVortexReachCores * i / 400.0;
            const glm::dvec3 p = offsetFromAxis(v.dir, r);
            const glm::dvec3 w = WeatherSystem::vortexWindAt(v, p, kEarthR, alt);
            const double h = glm::length(w - p * glm::dot(w, p));   // sólo la parte horizontal
            if (h > bestH) { bestH = h; bestR = r; }
        }
        std::printf("    Rankine: |v| eje %.1f · nucleo %.1f · 2R %.1f · 4R %.1f · fuera %.1f m/s"
                    "  | max horizontal a %.0f m (nucleo %.0f m)\n",
                    vEje, vCore, v2, v4, vOut, bestR, v.coreRadiusM);

        CHECK(vOut == 0.0, "mas alla del alcance el campo es CERO EXACTO (no un 1/r infinito)");
        CHECK(std::fabs(bestR - v.coreRadiusM) < v.coreRadiusM * 0.20,
              "el viento horizontal es MAXIMO en el nucleo (el daño es un anillo, no un disco)");
        // CONTRAPRUEBA del perfil: un campo lineal ("empuja mas cuanto mas cerca") tendría el máximo
        // en el eje y no caería nunca. Las dos cosas se niegan aquí.
        CHECK(v2 < vCore && v4 < v2, "fuera del nucleo el viento CAE (1/r), no crece ni se queda");
        CHECK(v4 < vCore * 0.5, "y cae deprisa: a 4 nucleos queda menos de la mitad");

        // Por encima de la nube madre no hay vórtice: se puede volar por encima.
        const glm::dvec3 arriba = WeatherSystem::vortexWindAt(v, offsetFromAxis(v.dir, v.coreRadiusM),
                                                             kEarthR, v.topM + 1.0f);
        CHECK(glm::length(arriba) == 0.0, "por encima de su nube madre el vortice no existe");

        // 5. SUCCIÓN: la componente radial apunta HACIA DENTRO cerca del suelo.
        {
            const double r = v.coreRadiusM * 2.0;
            const glm::dvec3 p = offsetFromAxis(v.dir, r);
            const glm::dvec3 w = WeatherSystem::vortexWindAt(v, p, kEarthR, 2.0f);
            glm::dvec3 haciaEje = v.dir - p * glm::dot(p, v.dir);
            haciaEje = glm::normalize(haciaEje);
            const double radial = glm::dot(w, haciaEje);
            // 5. LEVANTA: la vertical es positiva en el núcleo.
            const glm::dvec3 wc = WeatherSystem::vortexWindAt(v, offsetFromAxis(v.dir, 5.0), kEarthR, 40.0f);
            const double vert = glm::dot(wc, glm::normalize(offsetFromAxis(v.dir, 5.0)));
            std::printf("    succion: radial %.1f m/s hacia el eje a 2R · ascendente %.1f m/s en el nucleo\n",
                        radial, vert);
            CHECK(radial > 1.0, "cerca del suelo el aire ENTRA hacia el eje (arrastra, no orbita)");
            CHECK(vert > 5.0,   "y en el nucleo SUBE (es lo que levanta la tromba de agua)");
        }

        // El giro tiene signo: el mismo vórtice al revés da el tangencial opuesto.
        const Vortex vN = makeVortex(80.0, 90.0f, 1400.0f, -1.0f);
        const glm::dvec3 p = offsetFromAxis(v.dir, v.coreRadiusM);
        const glm::dvec3 wA = WeatherSystem::vortexWindAt(v,  p, kEarthR, alt);
        const glm::dvec3 wB = WeatherSystem::vortexWindAt(vN, p, kEarthR, alt);
        glm::dvec3 haciaEje = glm::normalize(v.dir - p * glm::dot(p, v.dir));
        const glm::dvec3 tang = glm::cross(p, -haciaEje);
        CHECK(glm::dot(wA, tang) * glm::dot(wB, tang) < 0.0, "el signo del giro cambia el tangencial");
    }

    // ── SEVERIDAD: escalones ordenados y derivados de las condiciones ────────────────────────────
    {
        int hist[6] = {0, 0, 0, 0, 0, 0};
        int total = 0;
        for (int ti = 0; ti < 60; ++ti) {
            W.setTime(ti * 61.0);
            for (size_t i = 0; i < dirs.size(); ++i) {
                const float humid = 0.15f + 0.80f * ((i * 37) % 100) / 100.0f;
                const float tempC = -12.0f + 40.0f * ((i * 53) % 100) / 100.0f;
                const WeatherSample s = W.sampleAt(dirs[i], tempC, humid);
                const float sev = WeatherSystem::severityAt(s);
                CHECK(sev >= 0.0f && sev <= 1.0f, "la severidad esta acotada a [0,1]");
                ++hist[(int)WeatherSystem::severityClass(sev)];
                ++total;
            }
        }
        std::printf("    severidad:");
        for (int i = 0; i < 6; ++i)
            std::printf(" %s %.1f%% ·", WeatherSystem::severityName((WeatherSystem::Severity)i),
                        100.0f * hist[i] / total);
        std::printf("\n");
        CHECK(hist[0] > 0, "hay tiempo en calma");
        CHECK(hist[(int)WeatherSystem::Severity::Storm] + hist[(int)WeatherSystem::Severity::Severe] > 0,
              "y hay tormentas (el escalon alto es alcanzable)");
        CHECK(hist[(int)WeatherSystem::Severity::Severe] < total / 10,
              "pero la tormenta severa es RARA (menos del 10 % del planeta-hora)");

        // Monotonía: peor cielo ⇒ no menos severidad. Contra un umbral suelto, esto es lo que
        // impide que la escala se invierta en algún tramo por un signo cambiado.
        WeatherSample a; a.cloudCover = 0.1f; a.precip = 0.0f; a.cloudBaseM = 1200; a.cloudTopM = 1700;
        WeatherSample b = a; b.cloudCover = 0.95f; b.precip = 0.9f; b.cloudTopM = 10000; b.wind = glm::vec3(30, 0, 0);
        CHECK(WeatherSystem::severityAt(b) > WeatherSystem::severityAt(a),
              "la tormenta es MAS severa que el buen tiempo");
        CHECK(WeatherSystem::vortexPotential(a, false) == 0.0f,
              "sin torre no hay potencial de embudo, ni sobre tierra ni sobre agua");
    }
}

// ================================================================================================
// EL VIENTO EMPUJA AL PERSONAJE (game/character.cpp).
//
// El problema que resuelve
// -----------------------
// Un campo de velocidades perfecto que no consulta nadie es exactamente igual de útil que no tener
// tornados. Y hay dos formas silenciosas de que no llegue a notarse, las dos reales en este código:
//   · `Character::move()` REESCRIBE la velocidad tangencial cada frame con tecla pulsada → andar
//     borraría el empujón justo cuando el jugador intenta escapar;
//   · sin rozamiento, CUALQUIER brisa desplaza al personaje sin parar, porque nada frena la deriva.
//
// Lo que este banco fija
//  1. Sin viento no se mueve nada (si no, todo lo demás pasa por accidente).
//  2. Una brisa NO te mueve estando de pie: el rozamiento le gana. CONTRAPRUEBA del punto 3.
//  3. Un tornado SÍ, y con la aceleración que dice la fórmula del arrastre.
//  4. ANDAR NO CANCELA EL VIENTO: con `move()` llamado cada frame, la deriva sobrevive.
// ================================================================================================
void test_wind_pushes_character() {
    beginTest("wind_pushes_character");

    // Personaje a 100 m del centro de un "planeta" en el origen: el "arriba" es radial, como en el
    // juego. Da igual el radio para lo que se mide (arrastre y rozamiento son locales).
    auto hacerJugador = [](const glm::dvec3& vAire, bool enSuelo) {
        auto ch = std::make_unique<Haruka::Character>(glm::dvec3(0, 100, 0), "");
        auto body = std::make_shared<Haruka::RigidBody>();
        body->radius = 0.4; body->mass = 70.0;
        body->position = glm::dvec3(0, 100.4, 0);
        body->isCharacter = true;
        ch->setPhysicsDriven(true);
        ch->setPhysicsBody(body);
        ch->setUpDirection(glm::dvec3(0, 1, 0));
        ch->setGrounded(enSuelo);
        ch->setWindProvider([vAire](const glm::dvec3&) { return vAire; });
        return std::make_pair(std::move(ch), body);
    };

    const float dt = 1.0f / 60.0f;
    auto correr = [&](const glm::dvec3& vAire, bool enSuelo, int frames, bool andando) {
        auto [ch, body] = hacerJugador(vAire, enSuelo);
        for (int i = 0; i < frames; ++i) {
            if (andando) ch->move(glm::vec2(0, 1), dt);   // "hacia delante", como una tecla pulsada
            ch->setGrounded(enSuelo);                      // el juego lo fija cada frame
            ch->update(dt);
        }
        return glm::length(ch->getWindDrift());
    };

    const double sinViento = correr(glm::dvec3(0.0), true, 120, false);
    const double brisa     = correr(glm::dvec3(8.0, 0, 0), true, 120, false);   // 8 m/s = brisa fuerte
    const double vendaval  = correr(glm::dvec3(35.0, 0, 0), true, 120, false);  // 35 m/s = temporal
    const double tornado   = correr(glm::dvec3(90.0, 0, 0), true, 120, false);  // 90 m/s = núcleo
    const double tornadoAnd= correr(glm::dvec3(90.0, 0, 0), true, 120, true);   // …y andando

    std::printf("    deriva tras 2 s de pie: sin viento %.2f · brisa 8 m/s %.2f · temporal 35 m/s %.2f "
                "· tornado 90 m/s %.2f m/s\n", sinViento, brisa, vendaval, tornado);
    std::printf("    con el tornado ANDANDO a la vez: %.2f m/s (si fuera 0, andar cancelaria el viento)\n",
                tornadoAnd);

    CHECK(sinViento == 0.0, "sin viento el personaje no deriva (el test no mide su propio ruido)");
    // 2. CONTRAPRUEBA del arrastre: si bastara con "hay viento", esto saldría distinto de cero.
    CHECK(brisa == 0.0, "una brisa de 8 m/s NO te arrastra de pie: el rozamiento le gana");
    CHECK(vendaval > 0.5, "un temporal de 35 m/s SI empieza a moverte");
    CHECK(tornado > 10.0, "y el nucleo de un tornado te arrastra de verdad");
    CHECK(tornado > vendaval * 2.0, "el arrastre va con el CUADRADO: el salto no es proporcional");
    CHECK(tornadoAnd > 10.0, "ANDAR NO CANCELA EL VIENTO (move() reescribe la tangencial cada frame)");

    // En el AIRE no hay rozamiento que valga: la misma brisa sí acaba llevándote.
    const double brisaAire = correr(glm::dvec3(8.0, 0, 0), false, 120, false);
    std::printf("    la misma brisa de 8 m/s EN EL AIRE: %.2f m/s\n", brisaAire);
    CHECK(brisaAire > 0.3, "en el aire no hay rozamiento: la misma brisa si te lleva");
}

// ================================================================================================
// EL CIELO HORNEADO NO ES UNA LOSA (WeatherSystem::bakeSky).
//
// El pase de nubes recibia la cobertura por direccion pero base, techo y lluvia de UN punto: el de
// la camara. Todo el planeta con la misma losa, y las capas altas a alturas fijas para disimularlo.
// Aqui se fija que el horneado lleve la nube DE CADA PUNTO: la base y el techo varian por el planeta
// y la torre es mas alta donde llueve. CONTRAPRUEBA: la losa vieja (una muestra para todo) tiene
// varianza exactamente cero, asi que el punto 1 no puede pasar por accidente.
// ================================================================================================
void test_sky_bake() {
    beginTest("sky_bake");
    WeatherSystem W;
    W.configure(4242u);
    W.setTime(9137.5);

    const int CW = 128, CH = 64;
    std::vector<float> temp((size_t)CW * CH), hum((size_t)CW * CH), sky((size_t)CW * CH * 4);
    for (int y = 0; y < CH; ++y)
        for (int x = 0; x < CW; ++x) {
            const size_t i = (size_t)y * CW + x;
            const double lat = (0.5 - (y + 0.5) / (double)CH) * 3.14159265358979;
            temp[i] = (float)(28.0 - 40.0 * std::fabs(lat) / 1.5708);   // tropico calido, polos helados
            hum[i]  = 0.15f + 0.75f * (float)((x * 37 + y * 11) % 100) / 100.0f;
        }
    float baseMin = 0, topMax = 0;
    const float cmax = W.bakeSky(temp.data(), hum.data(), CW, CH, sky.data(), &baseMin, &topMax);

    double sumB = 0, sumB2 = 0, sumT = 0, sumT2 = 0;
    double towerWet = 0, towerDry = 0; int nWet = 0, nDry = 0;
    int nan = 0;
    for (size_t i = 0; i < (size_t)CW * CH; ++i) {
        const float c = sky[i * 4], b = sky[i * 4 + 1], t = sky[i * 4 + 2], p = sky[i * 4 + 3];
        if (!(c == c && b == b && t == t && p == p)) ++nan;
        sumB += b; sumB2 += (double)b * b; sumT += t; sumT2 += (double)t * t;
        if (p > 0.3f) { towerWet += t - b; ++nWet; } else if (p == 0.0f) { towerDry += t - b; ++nDry; }
        CHECK(t > b, "el techo esta por encima de la base en todo texel");
        CHECK(c >= 0.0f && c <= 1.0f, "cobertura en [0,1]");
    }
    const double N = (double)CW * CH;
    const double sdB = std::sqrt(std::max(0.0, sumB2 / N - (sumB / N) * (sumB / N)));
    const double sdT = std::sqrt(std::max(0.0, sumT2 / N - (sumT / N) * (sumT / N)));
    std::printf("    cielo %dx%d: cobertura max %.2f · base %.0f..(sd %.0f) m · techo ..%.0f (sd %.0f) m "
                "· torre con lluvia %.0f m (n=%d) vs sin lluvia %.0f m (n=%d)\n",
                CW, CH, cmax, baseMin, sdB, topMax, sdT,
                nWet ? towerWet / nWet : 0.0, nWet, nDry ? towerDry / nDry : 0.0, nDry);
    CHECK(nan == 0, "ningun NaN en el horneado");
    CHECK(sdB > 100.0, "la BASE varia por el planeta (no es una losa: la vieja tenia sd = 0)");
    CHECK(sdT > 500.0, "y el TECHO tambien");
    CHECK(nWet > 0 && nDry > 0, "hay texeles con y sin lluvia (si no, la comparacion es vacua)");
    CHECK(towerWet / std::max(nWet, 1) > 2.0 * towerDry / std::max(nDry, 1),
          "donde llueve la torre es al menos el DOBLE de alta: la tormenta se distingue del claro");
    CHECK(topMax > baseMin + 3000.0f, "la banda global tiene sitio para una torre");

    // ── EL MAR NO SALE CUBIERTO, Y LAS CAPAS ALTAS SON OTRO CAMPO ───────────────────────────────
    //
    // "El mar totalmente cubierto de nubes y solo existen en una capa". Dos medidas:
    //  (a) el MISMO planeta con humedad 1,0 (la que tiene el mar en el campo) como TIERRA sale
    //      cerrado; como MAR (`skyHumidity`) tiene que quedar con claros. CONTRAPRUEBA: sin la
    //      correccion las dos medias son iguales por construccion.
    //  (b) el cirro no es una fraccion de la cobertura del cumulo: es otro campo (frentes
    //      adelantados). Si fuera una copia, la correlacion punto a punto seria 1.
    {
        std::vector<float> hum1((size_t)CW * CH, 1.0f), land((size_t)CW * CH, 0.0f), sea((size_t)CW * CH, 1.0f);
        std::vector<float> skyL((size_t)CW * CH * 4), skyS((size_t)CW * CH * 4), hi((size_t)CW * CH * 4);
        W.bakeSky(temp.data(), hum1.data(), CW, CH, skyL.data(), nullptr, nullptr, land.data(), nullptr);
        W.bakeSky(temp.data(), hum1.data(), CW, CH, skyS.data(), nullptr, nullptr, sea.data(), hi.data());
        double mL = 0, mS = 0, clearS = 0, rainL = 0, rainS = 0;
        double sxy = 0, sx = 0, sy = 0, sxx = 0, syy = 0, cirMax = 0, cirMean = 0;
        for (size_t i = 0; i < (size_t)CW * CH; ++i) {
            mL += skyL[i * 4]; mS += skyS[i * 4];
            if (skyS[i * 4] < 0.35f) clearS += 1.0;
            if (skyL[i * 4 + 3] > 0.0f) rainL += 1.0;
            if (skyS[i * 4 + 3] > 0.0f) rainS += 1.0;
            const double x = skyS[i * 4], yv = hi[i * 4];
            sxy += x * yv; sx += x; sy += yv; sxx += x * x; syy += yv * yv;
            cirMax = std::max(cirMax, yv); cirMean += yv;
        }
        mL /= N; mS /= N; clearS /= N; rainL /= N; rainS /= N; cirMean /= N;
        const double corr = (sxy - sx * sy / N) / std::sqrt(std::max(1e-12, (sxx - sx * sx / N) * (syy - sy * sy / N)));
        std::printf("    humedad 1.0 como TIERRA: cobertura media %.3f, llueve en %.1f%% · como MAR: %.3f, "
                    "claros (<0.35) %.1f%%, llueve en %.1f%%\n", mL, 100 * rainL, mS, 100 * clearS, 100 * rainS);
        std::printf("    cirro: media %.3f · max %.3f · correlacion con el cumulo %.3f\n", cirMean, cirMax, corr);
        CHECK(mL > 0.8, "con humedad 1.0 tratada como tierra el cielo esta cerrado (es el sintoma)");
        CHECK(mS < 0.75, "tratada como MAR queda por debajo de 0.75 de media");
        CHECK(clearS > 0.10, "y con claros de verdad (mas del 10 % del mar por debajo de 0.35)");
        CHECK(rainS < rainL, "y llueve MENOS sobre el mar que con la humedad cruda");
        CHECK(cirMax > 0.3 && cirMean > 0.01, "hay cirro en alguna parte (si no, la capa no existe)");
        CHECK(corr < 0.85, "el cirro NO es una copia del cumulo: va por delante del frente");
    }
}
