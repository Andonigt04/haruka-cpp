// ================================================================================================
// EL CIELO Y LA LUZ POR FISICA (`World/Planet/Atmosphere.h`, `SkyRadiometry.h`), 2026-10-17.
//
// Andoni: "la atmosfera los colores estan harcodeados no por atmosfera" y "que la luz la haga real con
// radiacion de luz etc no post añadido". Lo que se mide aqui, sin motor ni GPU:
//  - el cielo es AZUL arriba y mas blanco en el horizonte, y nadie ha escrito esos colores;
//  - el Sol se pone ROJO: la luz que llega al suelo pierde el azul al cruzar mas aire;
//  - el crepusculo existe y se acaba: con el Sol a -18° el cielo es negro (sombra de la Tierra);
//  - desde orbita se ve la capa azul en el limbo y el espacio es negro;
//  - el color del Sol es el de un cuerpo negro a 5772 K, y su luz cae con 1/d²;
//  - la Luna llena da ~1e-6 de la luz del Sol, y la exposicion la hace visible sin inventar luz.
// ================================================================================================
#include "Common/TestCommon.h"
#include "Haruka/World/Planet/Atmosphere.h"
#include "Haruka/World/Planet/SkyRadiometry.h"
#include "Haruka/Renderer/SkyLight.h"

#include <cmath>
#include <cstdio>
#include <glm/glm.hpp>

namespace {

namespace P = Haruka::World::Planet;
constexpr double kPi = 3.14159265358979323846;
constexpr double kR  = 6371000.0;

glm::dvec3 dirAt(double elevDeg, double azDeg = 0.0) {   // marco local: +Y arriba
    const double e = elevDeg * kPi / 180.0, a = azDeg * kPi / 180.0;
    return { std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a) };
}

const P::AtmoMsLut& msLut() {   // la tabla de dispersion multiple, la misma que usa el motor
    static const P::AtmoMsLut lut = P::atmoBuildMsLut(kR);
    return lut;
}

glm::dvec3 sky(const glm::dvec3& eye, const glm::dvec3& view, const glm::dvec3& sun, bool multiple = true) {
    return P::atmoInscatter(eye, view, -1.0, kR, sun, glm::dvec3(1.0), sun, glm::dvec3(0.0), nullptr,
                            multiple ? &msLut() : nullptr);
}

double blueRed(const glm::dvec3& c) { return c.z / std::max(c.x, 1e-30); }

} // namespace

void test_sky_atmo_blue_sky() {
    beginTest("sky_atmo_blue_sky");
    const glm::dvec3 eye(0.0, kR + 2.0, 0.0);
    const glm::dvec3 sun = dirAt(60.0);
    const glm::dvec3 zen = sky(eye, dirAt(90.0), sun);
    const glm::dvec3 hor = sky(eye, dirAt(2.0, 180.0), sun);   // horizonte de espaldas al Sol
    std::printf("    Sol a 60°: cenit (%.4f, %.4f, %.4f) · horizonte (%.4f, %.4f, %.4f)\n",
                zen.x, zen.y, zen.z, hor.x, hor.y, hor.z);
    std::printf("    azul/rojo: cenit %.2f · horizonte %.2f · luminancia cenit %.3f horizonte %.3f\n",
                blueRed(zen), blueRed(hor), P::luminance(zen), P::luminance(hor));
    CHECK(blueRed(zen) > 2.0, "el cenit es AZUL (el aire dispersa el azul mucho mas que el rojo)");
    CHECK(blueRed(hor) < blueRed(zen), "el horizonte es mas BLANCO que el cenit (mas aire: se satura)");
    CHECK(P::luminance(hor) > P::luminance(zen), "y mas brillante");
    // Escala: radiancia del cielo frente a la del suelo blanco al sol (las dos en `π·L`). El cielo
    // despejado al cenit es varias veces mas oscuro que un papel blanco al sol, no mas brillante.
    CHECK(P::luminance(zen) > 0.03 && P::luminance(zen) < 0.5, "el cenit esta en la escala del suelo, sin factor a ojo");
}

void test_sky_atmo_sunset() {
    beginTest("sky_atmo_sunset");
    const glm::dvec3 eye(0.0, kR + 2.0, 0.0);
    const glm::dvec3 T60 = P::atmoTransmittanceToSpace(eye, dirAt(60.0), kR);
    const glm::dvec3 T5  = P::atmoTransmittanceToSpace(eye, dirAt(5.0), kR);
    const glm::dvec3 T1  = P::atmoTransmittanceToSpace(eye, dirAt(1.0), kR);
    std::printf("    luz del Sol en el suelo: a 60° (%.3f, %.3f, %.3f) · a 5° (%.3f, %.3f, %.3f) · a 1° (%.4f, %.4f, %.4f)\n",
                T60.x, T60.y, T60.z, T5.x, T5.y, T5.z, T1.x, T1.y, T1.z);
    CHECK(P::luminance(T60) > 0.7 && P::luminance(T60) < 0.95, "a mediodia llega la mayor parte (~80 %)");
    CHECK(T5.x > T5.y && T5.y > T5.z, "a 5° la luz ya es AMARILLA-NARANJA (rojo > verde > azul)");
    CHECK(T1.x / std::max(T1.z, 1e-30) > 10.0, "a 1° el Sol es ROJO: el azul se ha perdido por el camino");
    // El cielo junto al Sol poniente se tiñe de naranja: la luz que dispersa ya ha perdido el azul.
    const glm::dvec3 sun = dirAt(1.0);
    const glm::dvec3 nearSun = sky(eye, dirAt(3.0), sun);
    const glm::dvec3 noonNear = sky(eye, dirAt(62.0), dirAt(60.0));
    std::printf("    cielo junto al Sol: poniente (%.4f, %.4f, %.4f) azul/rojo %.2f · mediodia azul/rojo %.2f\n",
                nearSun.x, nearSun.y, nearSun.z, blueRed(nearSun), blueRed(noonNear));
    CHECK(blueRed(nearSun) < 0.8, "junto al Sol poniente el cielo es calido (nadie ha escrito ese naranja)");
}

void test_sky_atmo_twilight() {
    beginTest("sky_atmo_twilight");
    const glm::dvec3 eye(0.0, kR + 2.0, 0.0);
    const double noon = P::luminance(sky(eye, dirAt(90.0), dirAt(60.0)));
    const double civil = P::luminance(sky(eye, dirAt(90.0), dirAt(-6.0)));
    const double astro = P::luminance(sky(eye, dirAt(90.0), dirAt(-18.0)));
    std::printf("    cenit: mediodia %.3e · Sol a -6° %.3e (%.1e del mediodia) · a -18° %.3e\n",
                noon, civil, civil / noon, astro);
    CHECK(civil > 1e-5 * noon, "en el crepusculo civil el cielo aun brilla (la alta atmosfera ve el Sol)");
    CHECK(civil < 0.05 * noon, "pero mucho menos que de dia");
    CHECK(astro < 1e-5 * noon, "con el Sol a -18° el cielo es NEGRO: todo el aire esta en la sombra de la Tierra");
    // ⚠️ EL CREPUSCULO ES AZUL, NO VIOLETA. Con dispersion simple el cenit a -6° salia (0,86, 0,50,
    // 1,0): la luz que rebota mas de una vez es casi todo el cielo del crepusculo, y es azul.
    const glm::dvec3 c6 = sky(eye, dirAt(90.0), dirAt(-6.0)), c6s = sky(eye, dirAt(90.0), dirAt(-6.0), false);
    std::printf("    cenit con el Sol a -6°: con dispersion multiple rojo/azul %.2f · solo simple %.2f\n",
                c6.x / c6.z, c6s.x / c6s.z);
    CHECK(c6.x / c6.z < 0.6, "el cenit del crepusculo es AZUL profundo (rojo < 0,6 del azul)");
    CHECK(c6s.x / c6s.z > c6.x / c6.z, "CONTRAPRUEBA: sin la dispersion multiple sale mas violeta");
}

void test_sky_atmo_from_space() {
    beginTest("sky_atmo_from_space");
    const glm::dvec3 eye(0.0, kR + 400000.0, 0.0);
    const glm::dvec3 sun = glm::normalize(glm::dvec3(1.0, 0.3, 0.0));
    // Rayo rasante que pasa a 20 km del suelo: el limbo.
    const double rTan = kR + 20000.0, re = glm::length(eye);
    const double ang = std::asin(rTan / re);   // angulo con el nadir
    const glm::dvec3 limb(std::sin(ang), -std::cos(ang), 0.0);
    const glm::dvec3 L = sky(eye, limb, sun);
    const glm::dvec3 up = sky(eye, glm::dvec3(0, 1, 0), sun);
    std::printf("    desde 400 km: limbo (%.4f, %.4f, %.4f) · mirando al espacio (%.2e)\n",
                L.x, L.y, L.z, P::luminance(up));
    CHECK(P::luminance(L) > 0.01 && blueRed(L) > 1.0, "desde orbita se ve la capa AZUL del limbo");
    CHECK(P::luminance(up) == 0.0, "y hacia fuera, espacio negro (no hay aire que dispersar)");
}

void test_sky_light_sun() {
    beginTest("sky_light_sun");
    const glm::dvec3 c = P::solarColor();
    const glm::dvec3 cold = P::blackbodyRGB(3000.0), hot = P::blackbodyRGB(12000.0);
    std::printf("    cuerpo negro 5772 K: (%.3f, %.3f, %.3f) · 3000 K (%.3f, %.3f, %.3f) · 12000 K (%.3f, %.3f, %.3f)\n",
                c.x, c.y, c.z, cold.x, cold.y, cold.z, hot.x, hot.y, hot.z);
    CHECK(std::abs(P::luminance(c) - 1.0) < 1e-9, "normalizado a luminancia 1 (el brillo lo pone la distancia)");
    CHECK(c.x > c.z && c.z / c.x > 0.75, "el Sol es blanco CALIDO, casi neutro (fuera de la atmosfera)");
    CHECK(cold.z / cold.x < 0.4 && hot.z > hot.x, "la temperatura manda: 3000 K anaranjado, 12000 K azulado");
    const double e1 = P::solarIrradiance(P::kAstronomicalUnitM), e2 = P::solarIrradiance(2.0 * P::kAstronomicalUnitM);
    std::printf("    constante solar %.1f W/m² · irradiancia a 1 UA %.6f · a 2 UA %.6f\n", P::kSolarConstantWm2, e1, e2);
    CHECK(std::abs(P::kSolarConstantWm2 - 1361.0) < 2.0, "la luminosidad y la UA reales dan la constante solar (1361 W/m²)");
    CHECK(std::abs(e1 - 1.0) < 1e-12 && std::abs(e2 - 0.25) < 1e-12, "y la luz cae con 1/d²");
    const double s0 = P::starIrradiance(0.0);
    std::printf("    estrella de magnitud 0: %.2e del Sol\n", s0);
    CHECK(s0 > 1e-11 && s0 < 4e-11, "una estrella de magnitud 0 es ~2e-11 del Sol");
}

void test_sky_light_moon_exposure() {
    beginTest("sky_light_moon_exposure");
    const double R = 1737400.0, d = 384400000.0;
    const double full = P::moonIrradiance(1.0, R, d, 0.0);
    const double quarter = P::moonIrradiance(1.0, R, d, kPi / 2.0);
    const double newM = P::moonIrradiance(1.0, R, d, kPi);
    std::printf("    Luna: llena %.2e · cuarto %.2e (%.3f de la llena) · nueva %.1e\n",
                full, quarter, quarter / full, newM);
    CHECK(full > 1.0e-6 && full < 3.0e-6, "la Luna llena da ~1e-6 de la luz del Sol (no un 0,22)");
    CHECK(std::abs(quarter / full - 1.0 / kPi) < 1e-9, "en cuarto, 1/π de la llena (esfera lambertiana)");
    CHECK(newM < 1e-12, "y la nueva no ilumina");

    // EXPOSICION: abre el diafragma, no inventa luz.
    const double kNoon = P::exposureTarget(0.95), kDusk = P::exposureTarget(0.02);
    const double kMoon = P::exposureTarget(full), kDark = P::exposureTarget(0.0);
    std::printf("    exposicion: mediodia %.2f · ocaso %.1f · luna llena %.0f · sin luna %.0f\n",
                kNoon, kDusk, kMoon, kDark);
    CHECK(kNoon < 1.5 && kDusk > kNoon && kMoon > kDusk && kDark >= kMoon, "cuanta menos luz, mas abre");
    const double moonlit = 0.3 * full * kMoon, noonLit = 0.3 * 0.95 * kNoon;
    std::printf("    suelo de albedo 0,3 en pantalla: mediodia %.3f · luna llena %.3f · sin luna %.5f\n",
                noonLit, moonlit, 0.3 * P::kNightFloorE * kDark);
    CHECK(moonlit > 0.01 && moonlit < 0.3 * noonLit, "con luna llena se ve el suelo, pero OSCURO: sigue siendo de noche");
    CHECK(0.3 * P::kNightFloorE * kDark < 0.01, "sin luna, casi negro");
    // Adaptacion: abrir es lento, cerrar es rapido.
    const double open1 = P::exposureAdapt(1.0, 1000.0, 1.0), close1 = P::exposureAdapt(1000.0, 1.0, 1.0);
    std::printf("    en 1 s: de 1 hacia 1000 -> %.1f · de 1000 hacia 1 -> %.2f\n", open1, close1);
    // En escala logaritmica (lo que ve el ojo): que fraccion del camino recorre en 1 s.
    const double fOpen = std::log(open1) / std::log(1000.0), fClose = 1.0 - std::log(close1) / std::log(1000.0);
    std::printf("    camino recorrido en 1 s: abriendo %.0f %% · cerrando %.0f %%\n", 100.0 * fOpen, 100.0 * fClose);
    CHECK(fOpen > 0.0 && fOpen < 0.3, "abrir a la noche lleva segundos");
    CHECK(fClose > 0.7, "cerrar al salir al sol es casi inmediato");
}

// LA CAMARA DEL MOTOR (`Renderer/SkyLight`): exposicion por la luz que llega y balance "luz de dia".
// El Sol a 60° sale BLANCO (el preset de una camara), el del ocaso sigue NARANJA (el preset no lo
// corrige), y la exposicion deja la luz que llega de la fuente en la escala de referencia.
void test_sky_light_camera() {
    beginTest("sky_light_camera");
    auto sunAt = [&](double elevDeg) {
        Haruka::Renderer::SkyLight sl;
        Haruka::Renderer::SkyLight::Input in;
        in.eye = glm::dvec3(0.0); in.planetCenter = glm::dvec3(0.0, -(kR + 2.0), 0.0); in.planetRadius = kR;
        in.hasSun = true; in.sunPos = dirAt(elevDeg) * P::kAstronomicalUnitM;
        in.dtS = 100.0;   // adaptada del todo
        sl.update(in);
        return std::make_pair(glm::dvec3(sl.data().sunGround), sl.exposure());
    };
    const auto [s60, k60] = sunAt(60.0);
    const auto [s3,  k3]  = sunAt(3.0);
    std::printf("    Sol a 60°: luz que llega (%.3f, %.3f, %.3f), exposicion %.2f · a 3°: (%.3f, %.3f, %.3f), exposicion %.2f\n",
                s60.x, s60.y, s60.z, k60, s3.x, s3.y, s3.z, k3);
    CHECK(std::abs(s60.x / s60.z - 1.0) < 0.01 && std::abs(s60.y / s60.z - 1.0) < 0.01,
          "con el Sol a 60° la luz sale BLANCA (balance de blancos 'luz de dia')");
    CHECK(s3.x / s3.z > 3.0, "y con el Sol a 3° sigue NARANJA: el preset no corrige el ocaso");
    CHECK(k3 > k60, "con menos luz la camara abre mas");
    CHECK(P::luminance(s60) > 0.7 && P::luminance(s60) < 1.3, "a mediodia la luz del Sol queda en la escala de referencia (~1)");
}

HARUKA_REGISTER_TESTS(
    { "sky_light_camera", test_sky_light_camera },
    { "sky_atmo_blue_sky", test_sky_atmo_blue_sky },
    { "sky_atmo_sunset", test_sky_atmo_sunset },
    { "sky_atmo_twilight", test_sky_atmo_twilight },
    { "sky_atmo_from_space", test_sky_atmo_from_space },
    { "sky_light_sun", test_sky_light_sun },
    { "sky_light_moon_exposure", test_sky_light_moon_exposure });
