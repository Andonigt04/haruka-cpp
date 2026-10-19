// ================================================================================================
// EL SOL QUIETO Y EL PLANETA QUE SE MUEVE (`World/Planet/SkyEphemeris.h`), 2026-10-17.
//
// Andoni: "haz que el sol este quieto y que el planeta se mueva y la luna". Lo que se demuestra, con el
// reloj del juego (dia de 24 h) y sin motor:
//  - el Sol sale por el este y se pone por el oeste, y vuelve al mismo sitio cada dia SOLAR;
//  - las ESTACIONES: el Sol de mediodia recorre de +23,4° a -23,4° de latitud en un año;
//  - la Luna sale y se pone cada dia (antes no: no la afectaba el giro), unos 50 min mas tarde cada
//    dia, y sus fases van al ritmo del mes SINODICO (29,5 dias), no del sidereo;
//  - la MAREA con las masas reales: dos pleamares por dia lunar, y viva/muerta segun la fase.
// ================================================================================================
#include "Common/TestCommon.h"
#include "Haruka/World/Planet/SkyEphemeris.h"
#include "Haruka/World/Water/OceanWave.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <glm/glm.hpp>

namespace {

namespace P = Haruka::World::Planet;
namespace W = Haruka::World::Water;
constexpr double kPi = 3.14159265358979323846;

// Angulo hacia el OESTE QUE SE VE: +Z (ejes de mano derecha; ver la cabecera de SkyEphemeris.h, el mapa
// del juego esta en espejo y su "longitud" crece hacia aqui).
double lonOf(const glm::dvec3& d) { return std::atan2(d.z, d.x); }
double latOf(const glm::dvec3& d) { return std::asin(std::clamp(d.y, -1.0, 1.0)); }
double wrap(double a) { while (a > kPi) a -= 2 * kPi; while (a < -kPi) a += 2 * kPi; return a; }

// Instantes en que la longitud de `dirAt` pasa por `lon0` hacia el oeste, entre t0 y t1.
template <class F>
int transits(F dirAt, double lon0, double t0, double t1, double step, double* first, double* last) {
    int n = 0; double prev = wrap(lonOf(dirAt(t0)) - lon0);
    for (double t = t0 + step; t <= t1; t += step) {
        const double cur = wrap(lonOf(dirAt(t)) - lon0);
        if (prev < 0.0 && cur >= 0.0 && cur - prev < kPi) {   // cruza hacia el oeste (+Z: crece)
            if (n == 0 && first) *first = t;
            if (last) *last = t;
            ++n;
        }
        prev = cur;
    }
    return n;
}

}  // namespace

// --------------------------------------------------- EL SOL: SALE POR EL ESTE Y VUELVE CADA DIA
void test_sky_sun_daily() {
    beginTest("sky_sun_daily");
    const P::SkyClock c;
    const double t = 1234.5;
    const double dLon = wrap(lonOf(P::skyAt(c, t + 1.0).sunDir) - lonOf(P::skyAt(c, t).sunDir));
    std::printf("    el Sol se mueve %.3f grados de longitud por segundo de juego\n", dLon * 180.0 / kPi);
    CHECK(dLon > 0.0, "el Sol va hacia el OESTE que se ve (+Z): sale por la derecha mirando al norte");
    // LO QUE VE EL JUGADOR: en el ecuador (lon 0), mirando al norte, el Sol sale por la DERECHA. Con
    // ejes de mano derecha y la camara de `Character` (derecha = frente x arriba), la derecha es -Z.
    {
        const glm::dvec3 up(1, 0, 0), north(0, 1, 0), right = glm::cross(north, up);
        double tRise = -1.0, prevAlt = glm::dot(P::skyAt(c, 0.0).sunDir, up);
        for (double tt = 0.5; tt < 2.0 * c.dayS && tRise < 0.0; tt += 0.5) {
            const double alt = glm::dot(P::skyAt(c, tt).sunDir, up);
            if (prevAlt < 0.0 && alt >= 0.0) tRise = tt;
            prevAlt = alt;
        }
        const double side = (tRise >= 0.0) ? glm::dot(P::skyAt(c, tRise).sunDir, right) : 0.0;
        std::printf("    al salir, el Sol esta %s (componente %+.2f hacia la derecha mirando al norte)\n",
                    side > 0.0 ? "a la DERECHA" : "a la IZQUIERDA", side);
        CHECK(tRise >= 0.0 && side > 0.5, "mirando al norte, el Sol sale por la derecha (el este que se ve)");
    }
    // La HORA (longitud del Sol), no la direccion: cerca del equinoccio la latitud del Sol cambia
    // 0,4 grados al dia, que son las estaciones, no un error del dia.
    const glm::dvec3 a = P::skyAt(c, t).sunDir, b = P::skyAt(c, t + c.dayS).sunDir;
    const double ang = std::abs(wrap(lonOf(b) - lonOf(a))) * 180.0 / kPi;
    std::printf("    un dia solar despues el Sol esta a %.4f grados de longitud de donde estaba\n", ang);
    CHECK(ang < 0.2, "un dia solar despues el Sol vuelve a la misma hora (< 0,2 grados de longitud)");
    // Y no es un dia sidereo: con el planeta orbitando, en un dia sidereo el Sol se ha atrasado ~1 grado.
    const glm::dvec3 s = P::skyAt(c, t + P::siderealDayS(c)).sunDir;
    const double angS = std::abs(wrap(lonOf(s) - lonOf(a))) * 180.0 / kPi;
    std::printf("    un dia SIDEREO despues: %.3f grados (lo que el planeta avanzo en su orbita)\n", angS);
    CHECK(angS > 0.5 && angS < 1.5, "en un dia sidereo el Sol se atrasa ~1 grado: el planeta ORBITA");
}

// --------------------------------------------------------------------------- LAS ESTACIONES
void test_sky_seasons() {
    beginTest("sky_seasons");
    const P::SkyClock c;
    const double year = c.yearDays * c.dayS;
    double latMax = -9, latMin = 9;
    for (double t = 0.0; t < year; t += c.dayS * 0.5) {
        const double l = latOf(P::skyAt(c, t).sunDir);
        latMax = std::max(latMax, l); latMin = std::min(latMin, l);
    }
    std::printf("    latitud del Sol en un año: de %.2f a %.2f grados (eje inclinado %.2f)\n",
                latMin * 180 / kPi, latMax * 180 / kPi, c.tiltRad * 180 / kPi);
    CHECK(std::abs(latMax - c.tiltRad) < 0.01 && std::abs(latMin + c.tiltRad) < 0.01,
          "el Sol de mediodia va de +inclinacion a -inclinacion en un año (estaciones)");
    // Medio año despues, el hemisferio iluminado es el otro.
    const double l0 = latOf(P::skyAt(c, year * 0.25).sunDir), l1 = latOf(P::skyAt(c, year * 0.75).sunDir);
    CHECK(l0 * l1 < 0.0, "medio año despues el Sol esta en el otro hemisferio");
}

// ------------------------------------------- LA LUNA SALE Y SE PONE, MAS TARDE CADA DIA, CON FASES
void test_sky_moon() {
    beginTest("sky_moon");
    const P::SkyClock c;
    auto moonAt = [&](double t) { return P::skyAt(c, t).moonDir; };
    auto sunAt  = [&](double t) { return P::skyAt(c, t).sunDir; };
    double first = 0, last = 0;
    const int nM = transits(moonAt, 0.0, 0.0, 30 * c.dayS, 0.5, &first, &last);
    const double lunarDay = (last - first) / std::max(1, nM - 1);
    std::printf("    la Luna pasa por el meridiano %d veces en 30 dias: un 'dia lunar' de %.3f dias (%.0f min de mas)\n",
                nM, lunarDay / c.dayS, (lunarDay / c.dayS - 1.0) * 24 * 60);
    CHECK(nM >= 27 && nM <= 30, "la Luna sale y se pone cada dia (cruza el meridiano ~29 veces en 30 dias)");
    CHECK(std::abs(lunarDay / c.dayS - 1.035) < 0.01, "y cada dia pasa ~50 min mas tarde (dia lunar 1,035)");

    // Fases: luna nueva = Luna y Sol en la misma direccion. Entre dos nuevas, el mes SINODICO.
    double prevE = 9, t1 = -1, t2 = -1; bool falling = false;
    for (double t = 0.0; t < 70 * c.dayS; t += c.dayS * 0.01) {
        const double e = std::acos(std::clamp(glm::dot(moonAt(t), sunAt(t)), -1.0, 1.0));
        if (falling && e > prevE && prevE < 0.3) { if (t1 < 0) t1 = t; else if (t2 < 0) t2 = t; }
        falling = e < prevE; prevE = e;
    }
    const double syn = (t2 - t1) / c.dayS;
    std::printf("    de luna nueva a luna nueva: %.2f dias (sidereo %.2f)\n", syn, c.monthDays);
    CHECK(t2 > 0.0 && std::abs(syn - 29.53) < 0.1, "las fases van al ritmo del mes sinodico (29,5 dias)");

    // La Luna sube y baja de latitud con la inclinacion del eje MAS la de su orbita.
    double decMax = 0;
    for (double t = 0.0; t < c.nodeDays * c.dayS; t += c.dayS * 0.37) decMax = std::max(decMax, std::abs(latOf(moonAt(t))));
    std::printf("    latitud maxima de la Luna en un ciclo de nodos: %.2f grados\n", decMax * 180 / kPi);
    CHECK(decMax <= c.tiltRad + c.moonInclRad + 1e-3 && decMax > c.tiltRad + c.moonInclRad - 0.01,
          "la Luna llega a inclinacion del eje + la de su orbita (28,6 grados), nunca mas");
}

// ------------------------------------------------------ LA MAREA CON EL SOL Y LA LUNA REALES
void test_sky_tide() {
    beginTest("sky_tide");
    const P::SkyClock c;
    const double R = 6371000.0, dSun = 1.495978707e11, dMoon = 3.844e8;
    // Marea en un punto del ecuador a lo largo de los dias: rango por dia y por fase.
    const glm::dvec3 here(1.0, 0.0, 0.0);
    auto tideAt = [&](double t) {
        const P::SkyState s = P::skyAt(c, t);
        const glm::dvec3 pos[2] = { s.sunDir * dSun, s.moonDir * dMoon };
        const double gm[2] = { P::kGmSun, P::kGmMoon };
        return W::oceanTideAt(here, R, pos, gm, 2);
    };
    // Pleamares en un dia lunar (maximos locales).
    int highs = 0; double prev2 = tideAt(0), prev1 = tideAt(1);
    double hi = -1e9, lo = 1e9;
    for (double t = 2.0; t < 1.035 * c.dayS; t += 1.0) {
        const double h = tideAt(t);
        if (prev1 > prev2 && prev1 > h) ++highs;
        prev2 = prev1; prev1 = h; hi = std::max(hi, h); lo = std::min(lo, h);
    }
    std::printf("    %d pleamares en un dia lunar · rango del dia %.2f m\n", highs, hi - lo);
    CHECK(highs == 2, "dos pleamares por dia lunar");
    // Viva y muerta: el rango diario a lo largo de un mes.
    double rMax = 0, rMin = 1e9;
    for (int d = 0; d < 30; ++d) {
        double h0 = 1e9, h1 = -1e9;
        for (double t = d * c.dayS; t < (d + 1.035) * c.dayS; t += 2.0) { const double h = tideAt(t); h0 = std::min(h0, h); h1 = std::max(h1, h); }
        rMax = std::max(rMax, h1 - h0); rMin = std::min(rMin, h1 - h0);
    }
    std::printf("    rango diario en un mes: marea viva %.2f m · muerta %.2f m (cociente %.1f)\n", rMax, rMin, rMax / rMin);
    CHECK(rMax / rMin > 1.8, "marea viva claramente mayor que la muerta (Sol y Luna alineados o no)");
    CHECK(rMin > 0.3, "hasta en marea muerta el mar sube y baja (> 30 cm): se ve en la orilla");
    CHECK(rMax < 4.0, "y en marea viva no pasa de 4 m: no se traga las playas (antes, con x15, 12 m)");
}

HARUKA_REGISTER_TESTS(
    { "sky_sun_daily", test_sky_sun_daily },
    { "sky_seasons", test_sky_seasons },
    { "sky_moon", test_sky_moon },
    { "sky_tide", test_sky_tide });
