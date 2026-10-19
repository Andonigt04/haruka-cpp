// ================================================================================================
// FORMACIÓN DE LA NUBE: el único test de "cómo se forma".
//
// Andoni: "la formación de nubes solo debería de tener un test de cómo se forma, no X" y "no
// deberían de mostrarse nubes por capas". Este test mide el MODELO (`CloudColumn.h`): dada una
// columna de aire —suelo, temperatura y humedad en superficie, hasta dónde llega la convección y
// cuánto vapor hay en altura— dónde condensa. Todo lo que el render hace después (campo 3D, marcha,
// luz) reparte ESTA fracción en nubes concretas; si esto está mal, ninguna imagen puede estar bien.
//
// Cada medida lleva su contraprueba: aire seco no forma nube, y cambiar la entrada mueve la salida
// en la dirección física (más seco = base más alta; más frío = cirro más bajo).
// ================================================================================================
#include "Common/TestCommon.h"
#include "Haruka/World/CloudColumn.h"
#include "Haruka/World/WeatherSystem.h"

#include <cmath>
#include <cstdio>

void test_cloud_formation() {
    using namespace Haruka;
    std::printf("\n== nubes: FORMACION (una columna de aire, donde condensa) ==\n");

    // ── 1. LA BASE ES EL NIVEL DE CONDENSACIÓN ──────────────────────────────────────────────────
    // Espenshade: LCL ≈ 125 m por °C de depresión del punto de rocío. A 15 °C y 50 % de humedad el
    // punto de rocío es 4,65 °C → 10,35 °C → 1 294 m. Es el número de los libros, no uno elegido.
    {
        Haruka::World::AirColumn c; c.tSurfC = 15.0f; c.rhSurf = 0.5f; c.cover = 0.5f; c.blDepthM = 2500.0f;
        const float base = Haruka::World::condensationLevelM(c);
        std::printf("    base (15 C, 50 %%): %.0f m  (libro: ~1 300)\n", base);
        CHECK(std::fabs(base - 1294.0f) < 80.0f, "la base de la nube es el LCL de Espenshade (1 294 m a 15 C / 50 %, ±80)");

        Haruka::World::AirColumn seco = c; seco.rhSurf = 0.3f;
        Haruka::World::AirColumn humedo = c; humedo.rhSurf = 0.8f;
        std::printf("    base al 30 %%: %.0f m · al 80 %%: %.0f m\n", Haruka::World::condensationLevelM(seco), Haruka::World::condensationLevelM(humedo));
        CHECK(Haruka::World::condensationLevelM(seco) > base + 500.0f, "aire mas seco = base mas alta (contraprueba direccional)");
        CHECK(Haruka::World::condensationLevelM(humedo) < base - 500.0f, "aire mas humedo = base mas baja");

        Haruka::World::AirColumn niebla = c; niebla.rhSurf = 1.0f;
        CHECK(std::fabs(Haruka::World::condensationLevelM(niebla) - c.groundM) < 1.0f, "al 100 % el nivel de condensacion esta en el suelo (niebla)");
        CHECK(std::fabs(Haruka::World::cloudBaseM(niebla) - (c.groundM + Haruka::World::kMinBaseAglM)) < 1.0f, "...pero la BASE de la nube no baja de 600 m sobre el suelo (la niebla es otra cosa)");

        // ⚠️ El fallo que motivó esto: sobre una meseta la base iba sobre el nivel del MAR y la nube
        // quedaba dentro del terreno. Aquí la base va sobre el suelo.
        Haruka::World::AirColumn meseta = c; meseta.groundM = 1000.0f;
        CHECK(std::fabs(Haruka::World::condensationLevelM(meseta) - (base + 1000.0f)) < 1.0f, "sobre una meseta de 1 000 m la base sube 1 000 m (la nube nunca nace bajo tierra)");
    }

    // ── 2. LA NUBE CONVECTIVA VA DE LA BASE AL TECHO DE LA CAPA CONVECTIVA, Y NADA MÁS ──────────
    {
        Haruka::World::AirColumn c; c.tSurfC = 15.0f; c.rhSurf = 0.5f; c.cover = 0.6f; c.blDepthM = 2500.0f;
        const float base = Haruka::World::cloudBaseM(c), top = c.groundM + c.blDepthM;
        CHECK(Haruka::World::cloudFractionAt(c, base - 100.0f) == 0.0f,               "bajo la base no hay nube");
        CHECK(std::fabs(Haruka::World::cloudFractionAt(c, base + 300.0f) - 0.6f) < 1e-4f, "dentro, la fraccion es la cobertura que piden los frentes");
        CHECK(Haruka::World::cloudFractionAt(c, top + 2.0f * Haruka::World::kBlTopFadeM) < 1e-3f,    "por encima del techo (+2 escalas de desfleque) no hay nube");
        CHECK(Haruka::World::cloudFractionAt(c, top + 0.5f * Haruka::World::kBlTopFadeM) > 0.05f && Haruka::World::cloudFractionAt(c, top + 0.5f * Haruka::World::kBlTopFadeM) < 0.55f,
              "el techo se desfleca, no se corta (a media escala queda entre el 5 y el 55 %)");
        // Contraprueba: sin frentes no hay nube convectiva, este el aire como este.
        Haruka::World::AirColumn sinFrente = c; sinFrente.cover = 0.0f;
        float maxF = 0.0f; for (float z = 0.0f; z < 12000.0f; z += 50.0f) maxF = std::max(maxF, Haruka::World::cloudFractionAt(sinFrente, z));
        CHECK(maxF == 0.0f, "CONTRAPRUEBA: sin cobertura de frentes ni vapor en altura la columna esta limpia de 0 a 12 km");
        // La tormenta es la misma nube con la capa convectiva alta: una columna, no otra capa.
        Haruka::World::AirColumn tormenta = c; tormenta.blDepthM = 8000.0f; tormenta.cover = 1.0f;
        CHECK(Haruka::World::cloudFractionAt(tormenta, 6000.0f) > 0.99f, "una tormenta es la MISMA nube con la conveccion a 8 km: a 6 km sigue llena");
        CHECK(Haruka::World::cloudFractionAt(c, 6000.0f) == 0.0f,        "...y el cumulo de buen tiempo a 6 km no tiene nada");
    }

    // ── 3. EL VAPOR EN ALTURA CONDENSA A SU TEMPERATURA, NO A UNA COTA FIJA ─────────────────────
    {
        auto peakZ = [](const Haruka::World::AirColumn& c) {
            float best = 0.0f, bz = 0.0f;
            for (float z = 0.0f; z < 14000.0f; z += 25.0f) { const float f = Haruka::World::aloftFractionAt(c, z); if (f > best) { best = f; bz = z; } }
            return bz;
        };
        Haruka::World::AirColumn tropico; tropico.tSurfC = 28.0f; tropico.vaporHigh = 1.0f;
        Haruka::World::AirColumn polar;   polar.tSurfC   = -5.0f; polar.vaporHigh   = 1.0f;
        const float zT = peakZ(tropico), zP = peakZ(polar);
        std::printf("    cirro (hielo, -40 C): tropico %.0f m · polar %.0f m\n", zT, zP);
        CHECK(std::fabs(zT - Haruka::World::altitudeOfTempM(tropico, Haruka::World::kHighCloudC)) < 30.0f, "el cirro esta donde el aire tiene -40 C");
        CHECK(zT > zP + 3000.0f, "en el tropico el cirro esta mas de 3 km mas alto que en el polo (la cota sale de la temperatura)");
        CHECK(zT > 9500.0f && zT < 11500.0f, "tropico: cirro a 10-11 km (medido en la atmosfera real)");

        Haruka::World::AirColumn medio; medio.tSurfC = 15.0f; medio.vaporMid = 1.0f;
        const float zM = peakZ(medio);
        std::printf("    altocumulo (-10 C) a 15 C de superficie: %.0f m\n", zM);
        CHECK(zM > 3300.0f && zM < 4400.0f, "altocumulo a ~3,8 km con 15 C en superficie (donde el aire esta a -10 C)");
        CHECK(Haruka::World::aloftFractionAt(medio, zM) > 0.99f && Haruka::World::aloftFractionAt(medio, zM + 3000.0f) < 0.05f, "y es una banda de temperatura, no toda la columna");
        // Contraprueba: sin vapor en altura, nada en altura, aunque haya cumulo.
        Haruka::World::AirColumn soloCumulo; soloCumulo.cover = 1.0f; soloCumulo.blDepthM = 1500.0f;
        CHECK(Haruka::World::aloftFractionAt(soloCumulo, 8000.0f) == 0.0f && Haruka::World::cloudFractionAt(soloCumulo, 8000.0f) == 0.0f, "CONTRAPRUEBA: sin vapor en altura el cielo alto esta limpio");
    }

    // ── 4. EL ASPECTO SALE DE LA FASE DEL AGUA, NO DE "QUÉ CAPA ES" ─────────────────────────────
    {
        Haruka::World::AirColumn c; c.tSurfC = 15.0f;
        CHECK(Haruka::World::iceFractionAt(c, Haruka::World::condensationLevelM(c) + 500.0f) == 0.0f, "un cumulo de buen tiempo es agua (0 de hielo)");
        CHECK(Haruka::World::iceFractionAt(c, Haruka::World::altitudeOfTempM(c, -40.0f)) > 0.99f,     "a -40 C es hielo entero: el velo del cirro es la fase, no una capa");
        const float mixed = Haruka::World::iceFractionAt(c, Haruka::World::altitudeOfTempM(c, -26.0f));
        CHECK(mixed > 0.2f && mixed < 0.8f, "entre -15 y -38 esta mezclada (el altocumulo tiene de las dos)");
    }

    // ── 5. TEMPERATURA Y SATURACIÓN: la física de apoyo con sus números de tabla ───────────────
    {
        CHECK(std::fabs(Haruka::World::satVaporHPa(20.0f) - 23.4f) < 0.3f, "Magnus: 23,4 hPa a 20 C (tabla)");
        CHECK(std::fabs(Haruka::World::satVaporHPa(0.0f) - 6.11f) < 0.05f, "Magnus: 6,11 hPa a 0 C (tabla)");
        Haruka::World::AirColumn c; c.tSurfC = 15.0f;
        CHECK(std::fabs(Haruka::World::airTempC(c, 5000.0f) - (15.0f - 32.5f)) < 0.01f, "gradiente estandar: -17,5 C a 5 km con 15 C en el suelo");
    }

    // ── 6. EL BUEN TIEMPO ES CÚMULO Y POCO; EL VAPOR EN ALTURA ES DE LOS FRENTES (2026-10-18) ───
    // El fondo de buen tiempo (por humedad, sin frente) es `kFairWeatherCover·H` y es cumulo entero;
    // sin frente NO hay vapor de nivel medio (antes la mitad del fondo iba a velo medio y, dibujado,
    // el cielo salia cerrado siempre). Lo que anade un frente es convectivo entero.
    {
        Haruka::World::WeatherSystem w; w.configure(1234u); w.setTime(30.0);
        // Se buscan un punto SIN frente (cobertura == fondo) y el punto MAS cubierto (frente).
        const float hum = 0.5f, bg = Haruka::World::kFairWeatherCover * hum;
        Haruka::World::WeatherSample calm{}, storm{}; bool haveCalm = false; float bestCover = -1.0f;
        for (int i = 0; i < 4000 && !(haveCalm && bestCover > 0.9f); ++i) {
            const double a = i * 2.399963, z = 1.0 - 2.0 * ((i + 0.5) / 4000.0);
            const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
            const glm::dvec3 d(r * std::cos(a), z, r * std::sin(a));
            const Haruka::World::WeatherSample ws = w.sampleAt(d, 15.0f, hum);
            if (!haveCalm && std::fabs(ws.cloudCover - bg) < 1e-3f) { calm = ws; haveCalm = true; }
            if (ws.cloudCover > bestCover) { bestCover = ws.cloudCover; storm = ws; }
        }
        std::printf("    sin frente: cobertura %.3f -> cumulo %.3f · velo medio %.3f  |  frente: cobertura %.3f -> cumulo %.3f\n",
                    calm.cloudCover, calm.column.cover, calm.column.vaporMid, storm.cloudCover, storm.column.cover);
        CHECK(haveCalm, "hay un punto sin frente (cobertura = fondo por humedad)");
        CHECK(std::fabs(calm.column.cover - bg) < 1e-3f, "sin frente, el cumulo es el fondo entero");
        CHECK(calm.column.vaporMid < 1e-3f, "y sin frente no hay velo de nivel medio: el cielo puede estar despejado");
        CHECK(storm.column.cover >= storm.cloudCover - 1e-3f, "con frente, toda la cobertura es convectiva");
        CHECK(storm.column.cover > calm.column.cover + 0.2f, "CONTRAPRUEBA: el frente sigue teniendo mucho mas cumulo que el buen tiempo");
    }
}

// Registro: el grupo sale de la carpeta de este archivo (ver Common/TestCommon.h).
HARUKA_REGISTER_TESTS(
    { "cloud_formation", test_cloud_formation });
