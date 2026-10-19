#pragma once
/**
 * @file Atmosphere.h
 * @brief EL AIRE QUE DISPERSA LA LUZ: Rayleigh + Mie + ozono, dispersion SIMPLE integrada por el rayo.
 *
 * Andoni (17-10): "la atmosfera los colores estan harcodeados no por atmosfera" y "que la luz la haga
 * real con radiacion de luz etc no post añadido". El cielo eran cuatro colores (`sky_palette.glsl`) y
 * un naranja de atardecer fijo. Aqui el color sale de la fisica: el aire dispersa el azul ~6 veces mas
 * que el rojo (Rayleigh, λ^-4), los aerosoles dispersan hacia delante (Mie, el halo blanco del Sol) y
 * el ozono se come el naranja (el azul del cielo en el crepusculo). El cielo azul, el horizonte
 * blanquecino, el Sol rojo al ponerse y la sombra de la Tierra salen solos de la misma integral.
 *
 * GEMELO EXACTO de `assets/shaders/lib/atmosphere.glsl` (mismas constantes, mismo numero de pasos,
 * mismo orden de suma). `sky_atmo_*` carea los dos.
 *
 * ── UNIDADES ─────────────────────────────────────────────────────────────────────────────────────
 * Distancias en metros, posiciones relativas al CENTRO del planeta. La luz entra como IRRADIANCIA del
 * Sol en lo alto de la atmosfera (`sunE`, color RGB) y sale como RADIANCIA DEL MOTOR: `π·L`, la
 * convencion con la que una superficie lambertiana de albedo `a` bajo irradiancia `E` devuelve `a·E`
 * (es como sombrean el terreno, el agua y los objetos). Asi el cielo y el suelo estan en la misma
 * escala sin ningun factor elegido a ojo.
 *
 * Coeficientes: los del aire terrestre a nivel del mar (Bruneton 2017 / Hillaire 2020), en m^-1, para
 * las longitudes de onda de los tres canales (680, 550, 440 nm).
 */
#include <algorithm>
#include <cmath>
#include <vector>
#include <glm/glm.hpp>

namespace Haruka::World::Planet {

/// Espesor de la capa de aire que se integra (m). Por encima de 100 km la densidad es < 1e-5.
inline constexpr double kAtmoTopM      = 100000.0;
/// Rayleigh: dispersion a nivel del mar y escala de altura.
inline const glm::dvec3 kAtmoBetaR      { 5.802e-6, 13.558e-6, 33.100e-6 };
inline constexpr double kAtmoHR         = 8000.0;
/// Mie (aerosoles de un dia claro): dispersion, absorcion, escala de altura y asimetria.
inline constexpr double kAtmoBetaMs     = 3.996e-6;
inline constexpr double kAtmoBetaMa     = 4.400e-6;
inline constexpr double kAtmoHM         = 1200.0;
inline constexpr double kAtmoMieG       = 0.80;
/// Ozono: solo ABSORBE (no dispersa). Perfil en tienda centrado a 25 km, 15 km de semiancho.
inline const glm::dvec3 kAtmoBetaO      { 0.650e-6, 1.881e-6, 0.085e-6 };
inline constexpr double kAtmoOzoneC     = 25000.0;
inline constexpr double kAtmoOzoneW     = 15000.0;
/// Pasos de la integral: por el rayo de vista y por el rayo hacia la fuente de luz.
inline constexpr int    kAtmoViewSteps  = 16;
inline constexpr int    kAtmoLightSteps = 6;
inline constexpr double kAtmoPi         = 3.14159265358979323846;

/// Densidades relativas (1 = nivel del mar) de las tres especies a la altitud `h` (m).
struct AtmoDensity { double r, m, o; };
inline AtmoDensity atmoDensity(double h) {
    h = std::max(h, 0.0);
    return { std::exp(-h / kAtmoHR), std::exp(-h / kAtmoHM),
             std::max(0.0, 1.0 - std::abs(h - kAtmoOzoneC) / kAtmoOzoneW) };
}

/// Extincion (m^-1) con esas densidades: lo que se pierde por metro, dispersado o absorbido.
inline glm::dvec3 atmoExtinction(const AtmoDensity& d) {
    return kAtmoBetaR * d.r + glm::dvec3(kAtmoBetaMs + kAtmoBetaMa) * d.m + kAtmoBetaO * d.o;
}

/// Rayo-esfera centrada en el origen. Devuelve false si no la corta; `t0 <= t1`.
inline bool atmoRaySphere(const glm::dvec3& ro, const glm::dvec3& rd, double rad, double& t0, double& t1) {
    const double b = glm::dot(ro, rd);
    const double c = glm::dot(ro, ro) - rad * rad;
    const double h = b * b - c;
    if (h < 0.0) { t0 = t1 = 0.0; return false; }
    const double s = std::sqrt(h);
    t0 = -b - s; t1 = -b + s;
    return true;
}

/// Funciones de fase, normalizadas sobre la esfera (∫ p dΩ = 1).
inline double atmoPhaseR(double mu) { return 3.0 / (16.0 * kAtmoPi) * (1.0 + mu * mu); }
inline double atmoPhaseM(double mu) {   // Cornette-Shanks
    const double g = kAtmoMieG, g2 = g * g;
    return 3.0 / (8.0 * kAtmoPi) * (1.0 - g2) * (1.0 + mu * mu)
         / ((2.0 + g2) * std::pow(1.0 + g2 - 2.0 * g * mu, 1.5));
}

/**
 * @brief Profundidad optica desde `p` hasta salir de la atmosfera en la direccion `d`.
 * @return τ por canal; si el rayo choca con el planeta, la luz no llega: τ = 1e9 (sombra de la Tierra).
 */
inline glm::dvec3 atmoOpticalDepthToSpace(const glm::dvec3& p, const glm::dvec3& d, double R,
                                          int steps = kAtmoLightSteps) {
    double g0, g1;
    if (atmoRaySphere(p, d, R, g0, g1) && g0 > 0.0) return glm::dvec3(1.0e9);
    double a0, a1;
    if (!atmoRaySphere(p, d, R + kAtmoTopM, a0, a1) || a1 <= 0.0) return glm::dvec3(0.0);
    const double t0 = std::max(a0, 0.0), dt = (a1 - t0) / steps;
    glm::dvec3 tau(0.0);
    for (int i = 0; i < steps; ++i) {
        const glm::dvec3 x = p + d * (t0 + (i + 0.5) * dt);
        tau += atmoExtinction(atmoDensity(glm::length(x) - R)) * dt;
    }
    return tau;
}

/// Transmitancia desde `p` hasta el espacio en la direccion `d` (la luz del Sol que llega a `p`).
inline glm::dvec3 atmoTransmittanceToSpace(const glm::dvec3& p, const glm::dvec3& d, double R,
                                           int steps = kAtmoLightSteps) {
    const glm::dvec3 tau = atmoOpticalDepthToSpace(p, d, R, steps);
    return glm::exp(-tau);
}

// ── DISPERSION MULTIPLE (Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere") ──────
//
// ⚠️ SIN ESTO EL CREPUSCULO SALIA VIOLETA y el cielo de dia apagado (medido: cenit con el Sol a -6°
// = (0,86, 0,50, 1,0)). La luz que rebota MAS de una vez en el aire es la mitad del cielo de dia y
// casi todo el del crepusculo, y es la que lo pinta de azul profundo. Se aproxima como Hillaire: en
// cada punto, la luz de 2º orden que llega de todas las direcciones con fase isotropa, y los ordenes
// siguientes como serie geometrica `1/(1 - f_ms)`. Depende solo de la altitud y del angulo del Sol,
// asi que es una tabla de 32 x 32 que se calcula UNA vez; la GPU lee LA MISMA (ver `SkyLight`).
inline constexpr int    kAtmoMsSize    = 32;    ///< tabla de kAtmoMsSize (cos del Sol) x kAtmoMsSize (altitud)
inline constexpr int    kAtmoMsDirs    = 8;     ///< direcciones por eje en la esfera (8 x 16)
inline constexpr int    kAtmoMsSteps   = 20;    ///< pasos por rayo
inline constexpr double kAtmoGroundAlbedo = 0.2;

/// Coordenadas de la tabla: u = cos del Sol en [-1, 1], v = sqrt(altitud / techo) (mas filas abajo).
inline double atmoMsU(double muS) { return std::clamp(0.5 * (muS + 1.0), 0.0, 1.0); }
inline double atmoMsV(double h)   { return std::sqrt(std::clamp(h / kAtmoTopM, 0.0, 1.0)); }

struct AtmoMsLut {
    std::vector<glm::vec3> data;   ///< [iv * kAtmoMsSize + iu], radiancia por unidad de irradiancia solar
    double R = 0.0;
};

/// Ψ_ms(h, μs): la radiancia isotropa de 2º orden en adelante en un punto, por unidad de irradiancia.
inline glm::dvec3 atmoMsEntry(double R, double h, double muS) {
    const glm::dvec3 x(0.0, R + std::max(h, 1.0), 0.0);
    const glm::dvec3 sun(std::sqrt(std::max(0.0, 1.0 - muS * muS)), muS, 0.0);
    const double pU = 1.0 / (4.0 * kAtmoPi);
    glm::dvec3 L2(0.0), fms(0.0);
    const int NT = kAtmoMsDirs, NP = 2 * kAtmoMsDirs;
    const double dT = kAtmoPi / NT, dP = 2.0 * kAtmoPi / NP;
    for (int i = 0; i < NT; ++i) {
        const double th = (i + 0.5) * dT;
        const double dOmega = std::sin(th) * dT * dP;
        for (int j = 0; j < NP; ++j) {
            const double ph = (j + 0.5) * dP;
            const glm::dvec3 w(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
            double a0, a1, g0, g1;
            if (!atmoRaySphere(x, w, R + kAtmoTopM, a0, a1)) continue;
            double tEnd = a1;
            const bool ground = atmoRaySphere(x, w, R, g0, g1) && g0 > 0.0;
            if (ground) tEnd = std::min(tEnd, g0);
            const double dt = tEnd / kAtmoMsSteps;
            glm::dvec3 tau(0.0), l(0.0), f(0.0);
            for (int k = 0; k < kAtmoMsSteps; ++k) {
                const glm::dvec3 y = x + w * ((k + 0.5) * dt);
                const AtmoDensity den = atmoDensity(glm::length(y) - R);
                const glm::dvec3 ext = atmoExtinction(den);
                const glm::dvec3 sc = kAtmoBetaR * den.r + glm::dvec3(kAtmoBetaMs * den.m);
                const glm::dvec3 T = glm::exp(-(tau + ext * (0.5 * dt)));
                l += T * sc * atmoTransmittanceToSpace(y, sun, R) * pU * dt;
                f += T * sc * dt;
                tau += ext * dt;
            }
            if (ground) {   // el suelo lambertiano devuelve la luz que le llega del Sol
                const glm::dvec3 gp = x + w * tEnd;
                const glm::dvec3 n = glm::normalize(gp);
                l += glm::exp(-tau) * atmoTransmittanceToSpace(gp, sun, R) *
                     std::max(glm::dot(n, sun), 0.0) * (kAtmoGroundAlbedo / kAtmoPi);
            }
            L2 += l * pU * dOmega;
            fms += f * pU * dOmega;
        }
    }
    return L2 / (glm::dvec3(1.0) - fms);
}

/// La tabla entera para un planeta de radio `R` (~1 s de CPU: se hace una vez y se guarda).
inline AtmoMsLut atmoBuildMsLut(double R) {
    AtmoMsLut lut; lut.R = R;
    lut.data.resize((size_t)kAtmoMsSize * kAtmoMsSize);
    for (int iv = 0; iv < kAtmoMsSize; ++iv)
        for (int iu = 0; iu < kAtmoMsSize; ++iu) {
            const double v = (double)iv / (kAtmoMsSize - 1), u = (double)iu / (kAtmoMsSize - 1);
            lut.data[(size_t)iv * kAtmoMsSize + iu] = glm::vec3(atmoMsEntry(R, v * v * kAtmoTopM, 2.0 * u - 1.0));
        }
    return lut;
}

/// Lectura bilineal A MANO (gemela de `harukaAtmoMs` en GLSL: la misma cuenta, no el filtro del driver).
inline glm::dvec3 atmoMsSample(const AtmoMsLut& lut, double h, double muS) {
    if (lut.data.empty()) return glm::dvec3(0.0);
    const double fu = atmoMsU(muS) * (kAtmoMsSize - 1), fv = atmoMsV(h) * (kAtmoMsSize - 1);
    const int u0 = std::min((int)fu, kAtmoMsSize - 2), v0 = std::min((int)fv, kAtmoMsSize - 2);
    const double au = fu - u0, av = fv - v0;
    auto at = [&](int iu, int iv) { return glm::dvec3(lut.data[(size_t)iv * kAtmoMsSize + iu]); };
    return glm::mix(glm::mix(at(u0, v0), at(u0 + 1, v0), au), glm::mix(at(u0, v0 + 1), at(u0 + 1, v0 + 1), au), av);
}

/**
 * @brief Luz que el aire dispersa hacia el ojo a lo largo de un rayo (dispersion SIMPLE + MULTIPLE).
 *
 * @param ro     ojo, relativo al centro del planeta (m).
 * @param rd     direccion de la vista (unitaria).
 * @param tMax   hasta donde se integra (m): un objeto a esa distancia; < 0 = hasta el suelo o el espacio.
 * @param R      radio del planeta (m).
 * @param sunDir, sunE    hacia el Sol (unitaria) e irradiancia solar en lo alto de la atmosfera.
 * @param moonDir, moonE  idem para la Luna (luz del Sol reflejada; la misma integral).
 * @param outT   transmitancia del tramo integrado (lo que queda de lo que hay detras).
 * @return radiancia del motor (`π·L`) que entra en el ojo por esa direccion.
 *
 * Cada paso: cuanto aire hay (densidades), cuanta luz del Sol le llega (transmitancia hacia el Sol,
 * que es lo que enrojece el atardecer y deja a oscuras lo que esta en la sombra de la Tierra), cuanto
 * de eso se desvia hacia el ojo (fases de Rayleigh y Mie) y cuanto sobrevive hasta el ojo.
 */
inline glm::dvec3 atmoInscatter(const glm::dvec3& ro, const glm::dvec3& rd, double tMax, double R,
                                const glm::dvec3& sunDir, const glm::dvec3& sunE,
                                const glm::dvec3& moonDir, const glm::dvec3& moonE,
                                glm::dvec3* outT = nullptr, const AtmoMsLut* ms = nullptr) {
    if (outT) *outT = glm::dvec3(1.0);
    double a0, a1;
    if (!atmoRaySphere(ro, rd, R + kAtmoTopM, a0, a1) || a1 <= 0.0) return glm::dvec3(0.0);
    double tEnd = a1;
    double g0, g1;
    if (atmoRaySphere(ro, rd, R, g0, g1) && g0 > 0.0) tEnd = std::min(tEnd, g0);
    if (tMax >= 0.0) tEnd = std::min(tEnd, tMax);
    const double tStart = std::max(a0, 0.0);
    if (tEnd <= tStart) return glm::dvec3(0.0);

    const double muS = glm::dot(rd, sunDir), muM = glm::dot(rd, moonDir);
    const double pRs = atmoPhaseR(muS), pMs = atmoPhaseM(muS);
    const double pRm = atmoPhaseR(muM), pMm = atmoPhaseM(muM);
    const double dt = (tEnd - tStart) / kAtmoViewSteps;
    glm::dvec3 tauView(0.0), L(0.0);
    for (int i = 0; i < kAtmoViewSteps; ++i) {
        const glm::dvec3 x = ro + rd * (tStart + (i + 0.5) * dt);
        const AtmoDensity den = atmoDensity(glm::length(x) - R);
        const glm::dvec3 ext = atmoExtinction(den);
        // El punto medio del paso: la mitad de su propia extincion ya cuenta para el camino al ojo.
        const glm::dvec3 Tv = glm::exp(-(tauView + ext * (0.5 * dt)));
        const glm::dvec3 scR = kAtmoBetaR * den.r, scM = glm::dvec3(kAtmoBetaMs * den.m);
        const double hx = glm::length(x) - R;
        const glm::dvec3 up = x / (hx + R);
        if (sunE.x + sunE.y + sunE.z > 0.0) {
            const glm::dvec3 Ts = atmoTransmittanceToSpace(x, sunDir, R);
            L += Tv * Ts * sunE * (scR * pRs + scM * pMs) * dt;
            if (ms) L += Tv * (scR + scM) * atmoMsSample(*ms, hx, glm::dot(up, sunDir)) * sunE * dt;
        }
        if (moonE.x + moonE.y + moonE.z > 0.0) {
            const glm::dvec3 Tm = atmoTransmittanceToSpace(x, moonDir, R);
            L += Tv * Tm * moonE * (scR * pRm + scM * pMm) * dt;
            if (ms) L += Tv * (scR + scM) * atmoMsSample(*ms, hx, glm::dot(up, moonDir)) * moonE * dt;
        }
        tauView += ext * dt;
    }
    if (outT) *outT = glm::exp(-tauView);
    return L * kAtmoPi;   // radiancia del motor: π·L
}

} // namespace Haruka::World::Planet
