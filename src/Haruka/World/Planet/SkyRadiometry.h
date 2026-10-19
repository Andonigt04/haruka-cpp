#pragma once
/**
 * @file SkyRadiometry.h
 * @brief CUANTA LUZ Y DE QUE COLOR: el Sol por su radiacion, la Luna por lo que refleja, las estrellas
 *        por su magnitud, y la EXPOSICION de la camara que lo convierte en pantalla.
 *
 * Andoni (17-10): "que la luz la haga real con radiacion de luz etc no post añadido". La luz del Sol
 * era el `color` de un objeto de escena (1, 0,95, 0,85) puesto a mano, la Luna un 0,22 "estilizado" y
 * la noche un suelo de ambiente subido a ojo para que no fuera negra. Aqui:
 *
 *  · el COLOR del Sol es el de un cuerpo negro a 5772 K (su temperatura efectiva), integrado con las
 *    funciones de igualacion CIE 1931 y llevado a sRGB lineal;
 *  · su IRRADIANCIA sale de su luminosidad y la distancia: `E = L/(4π d²)`. La unidad del motor es la
 *    constante solar (la irradiancia a 1 UA), asi que un planeta a 1 UA recibe 1 y uno a 2 UA, 1/4;
 *  · la Luna es una esfera lambertiana de albedo 0,12 iluminada por ese mismo Sol; su luz y su fase
 *    salen de la geometria, no de un factor;
 *  · las estrellas son FUENTES PUNTUALES con magnitud visual: su irradiancia es la del Sol escalada
 *    por 10^(-0,4·(m - m_sol)).
 *
 * Y la EXPOSICION, que es lo unico que no esta en el cielo sino en el ojo: la noche de luna llena
 * recibe ~400 000 veces menos luz que el mediodia, y ningun monitor tiene ese rango. Una camara (y un
 * ojo) lo resuelve abriendo el diafragma, no inventando luz. `exposureTarget` es esa apertura: se
 * multiplica a TODA la luz por igual, asi que no cambia ningun color ni ninguna proporcion de la
 * escena, solo cuanto se abre. Lo que la luna no ilumina sigue siendo negro.
 */
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace Haruka::World::Planet {

inline constexpr double kSunLuminosityW = 3.828e26;       ///< luminosidad solar nominal (IAU 2015)
inline constexpr double kAstronomicalUnitM = 1.495978707e11;
inline constexpr double kSunTempK = 5772.0;               ///< temperatura efectiva del Sol
inline constexpr double kSunRadiusM = 6.957e8;
/// Constante solar: la irradiancia a 1 UA (≈ 1361 W/m²). Es la UNIDAD de luz del motor.
inline const double kSolarConstantWm2 = kSunLuminosityW / (4.0 * 3.14159265358979323846 *
                                                           kAstronomicalUnitM * kAstronomicalUnitM);
inline constexpr double kMoonAlbedo = 0.12;
/// Magnitud visual aparente del Sol a 1 UA: el cero de la escala de las estrellas.
inline constexpr double kSunApparentMag = -26.74;

/// Irradiancia del Sol (en constantes solares) a `distanceM` de su centro: `L/(4π d²)` / constante solar.
inline double solarIrradiance(double distanceM, double luminosityW = kSunLuminosityW) {
    const double d = std::max(distanceM, 1.0);
    return luminosityW / (4.0 * 3.14159265358979323846 * d * d) / kSolarConstantWm2;
}

namespace detail {
/// Gaussiana partida de Wyman, Sloan y Shirley (2013): el ajuste analitico de las CIE 1931.
inline double cieLobe(double x, double mu, double s1, double s2) {
    const double t = (x - mu) / (x < mu ? s1 : s2);
    return std::exp(-0.5 * t * t);
}
inline glm::dvec3 cieXYZ(double nm) {
    return { 1.056 * cieLobe(nm, 599.8, 37.9, 31.0) + 0.362 * cieLobe(nm, 442.0, 16.0, 26.7)
               - 0.065 * cieLobe(nm, 501.1, 20.4, 26.2),
             0.821 * cieLobe(nm, 568.8, 46.9, 40.5) + 0.286 * cieLobe(nm, 530.9, 16.3, 31.1),
             1.217 * cieLobe(nm, 437.0, 11.8, 36.0) + 0.681 * cieLobe(nm, 459.0, 26.0, 13.8) };
}
/// Radiancia espectral de Planck (forma relativa: las constantes globales se van al normalizar).
inline double planck(double nm, double tempK) {
    const double l = nm * 1.0e-9;
    const double c2 = 1.438776877e-2;   // hc/k (m·K)
    return 1.0 / (l * l * l * l * l * (std::exp(c2 / (l * tempK)) - 1.0));
}
} // namespace detail

/// Luminancia relativa de un color sRGB lineal (Rec. 709).
inline double luminance(const glm::dvec3& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }

/**
 * @brief Color de un cuerpo negro a `tempK`, en sRGB LINEAL, normalizado a luminancia 1.
 *
 * Se integra Planck × CIE de 380 a 780 nm y se pasa de XYZ a sRGB lineal (blanco D65). Con 5772 K da
 * un blanco ligeramente calido — es el color de la luz del Sol FUERA de la atmosfera; el amarillo y el
 * rojo del atardecer los pone el aire (`Atmosphere.h`), no este numero.
 */
inline glm::dvec3 blackbodyRGB(double tempK) {
    glm::dvec3 xyz(0.0);
    for (double nm = 380.0; nm <= 780.0; nm += 1.0) xyz += detail::cieXYZ(nm) * detail::planck(nm, tempK);
    const glm::dvec3 rgb(  3.2406 * xyz.x - 1.5372 * xyz.y - 0.4986 * xyz.z,
                          -0.9689 * xyz.x + 1.8758 * xyz.y + 0.0415 * xyz.z,
                           0.0557 * xyz.x - 0.2040 * xyz.y + 1.0570 * xyz.z);
    const glm::dvec3 pos = glm::max(rgb, glm::dvec3(0.0));
    const double Y = luminance(pos);
    return Y > 0.0 ? pos / Y : glm::dvec3(1.0);
}

/// El color del Sol (cuerpo negro a 5772 K), calculado una vez.
inline const glm::dvec3& solarColor() {
    static const glm::dvec3 c = blackbodyRGB(kSunTempK);
    return c;
}

/**
 * @brief Irradiancia que la Luna (esfera lambertiana) envia al observador.
 * @param sunAtMoon   irradiancia del Sol en la Luna (constantes solares).
 * @param moonRadiusM radio de la Luna.
 * @param distanceM   distancia observador-Luna.
 * @param phaseAngle  angulo Sol-Luna-observador (rad): 0 = llena, π = nueva.
 *
 * Integral de una esfera lambertiana: `E = (2/3)·A·E_sol·(r/d)²·Φ(α)` con
 * `Φ(α) = (sin α + (π−α)·cos α)/π`. Luna llena ≈ 1,6e-6 del Sol (la real, con el brillo de oposicion,
 * ronda 2,5e-6).
 */
inline double moonIrradiance(double sunAtMoon, double moonRadiusM, double distanceM, double phaseAngle,
                             double albedo = kMoonAlbedo) {
    const double pi = 3.14159265358979323846;
    const double a = std::clamp(phaseAngle, 0.0, pi);
    const double phi = (std::sin(a) + (pi - a) * std::cos(a)) / pi;
    const double k = moonRadiusM / std::max(distanceM, moonRadiusM);
    return (2.0 / 3.0) * albedo * sunAtMoon * k * k * std::max(phi, 0.0);
}

/// Irradiancia (constantes solares) de una estrella de magnitud visual `mag`.
inline double starIrradiance(double mag) { return std::pow(10.0, -0.4 * (mag - kSunApparentMag)); }

// ── EXPOSICION ───────────────────────────────────────────────────────────────────────────────────
/// Iluminacion horizontal de referencia (constantes solares): la que se ve "normal" sin abrir nada.
inline constexpr double kExposureRef = 1.0;
/// Cuanto compensa la camara: 1 = todo (la noche se veria como el dia), 0 = nada. Un ojo adaptado
/// compensa mucho pero no todo: la noche sigue siendo oscura y con menos color.
inline constexpr double kExposureComp = 0.80;
/// Limites de la apertura: lo que hay mas alla es negro (o blanco) de verdad.
inline constexpr double kExposureMin = 0.5;
inline constexpr double kExposureMax = 3.0e4;
/// Lo que ilumina una noche sin luna (resplandor del aire + estrellas, ~1 mlx).
inline constexpr double kNightFloorE = 1.0e-8;

/// Exposicion a la que tiende la camara con una iluminacion horizontal `horizontalE`.
inline double exposureTarget(double horizontalE) {
    const double E = std::max(horizontalE, kNightFloorE);
    return std::clamp(std::pow(kExposureRef / E, kExposureComp), kExposureMin, kExposureMax);
}

/**
 * @brief La adaptacion en el TIEMPO, en escala logaritmica. Abrir (de la luz a la oscuridad) es lento,
 *        cerrar es rapido — como el ojo y como una camara que no quiere quemar la imagen.
 */
inline double exposureAdapt(double current, double target, double dtS,
                            double tauOpenS = 4.0, double tauCloseS = 0.8) {
    if (!(current > 0.0)) return target;
    const double tau = (target > current) ? tauOpenS : tauCloseS;
    const double a = 1.0 - std::exp(-std::max(dtS, 0.0) / tau);
    return std::exp(std::log(current) + (std::log(target) - std::log(current)) * a);
}

} // namespace Haruka::World::Planet
