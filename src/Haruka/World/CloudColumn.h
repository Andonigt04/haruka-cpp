#pragma once
#include <glm/glm.hpp>
#include <cmath>
#include <algorithm>

namespace Haruka::World {

// ═══════════════════════════════════════════════════════════════════════════════════════════════
//  LA COLUMNA DE AIRE: de dónde sale una nube
//
//  ⚠️ ANTES HABÍA TRES CAPAS FIJAS (cúmulo en su losa, altocúmulo en 3,4–5,8 km, cirro en 6,8–10 km),
//  cada una con su función de densidad, su marcha y sus constantes. Se veían como lo que eran: tres
//  sábanas a tres cotas. Andoni: "no deberían de mostrarse nubes por capas".
//
//  Aquí una nube no es una capa: es donde el aire está SATURADO. Se describe la columna con cinco
//  números que el clima ya tiene o casi (suelo, temperatura y humedad en superficie, hasta dónde
//  llega la convección, y cuánto vapor hay en altura), y de ahí sale, para cualquier altura z:
//    · la humedad relativa RH(z) — subiendo, el aire se enfría y su RH sube hasta saturar (ahí está
//      la BASE: el nivel de condensación, que por eso es una cota plana), y por encima de la capa
//      convectiva el vapor se acaba (ahí está el TECHO);
//    · la FRACCIÓN de nube en esa altura, que es lo que el campo 3D reparte en nubes concretas.
//  El vapor en altura (frentes que adelantan, restos de convección) condensa donde el aire está
//  frío: alrededor de −10 °C sale un altocúmulo, alrededor de −40 °C (hielo) un cirro. La cota de
//  esas nubes NO es fija: es la de esas temperaturas, que en el trópico están a 11 km y en el polo
//  a 6. Y el aspecto (agua o hielo, denso o velo) sale de la temperatura del punto, no de "qué capa es".
//
//  Es la MISMA función en CPU (jugador dentro de nube, techo de la lluvia, horneado) y en GLSL
//  (`lib/cloud_column.glsl`); `test_cloud_formation` mide una contra la otra.
// ═══════════════════════════════════════════════════════════════════════════════════════════════

struct AirColumn {
    float groundM   = 0.0f;    ///< cota del suelo sobre el nivel del mar (m, ≥ 0)
    float tSurfC    = 15.0f;   ///< temperatura del aire en el suelo (°C)
    float rhSurf    = 0.5f;    ///< humedad relativa en el suelo [0,1]
    float cover     = 0.0f;    ///< fracción de cielo que los FRENTES cierran a nivel de la base [0,1]
    float blDepthM  = 1500.0f; ///< hasta dónde llega el aire de superficie (m sobre el suelo): 500 buen tiempo · 8000 tormenta
    float vaporMid  = 0.0f;    ///< vapor en altura media (condensa a ~−10 °C): fracción de nube [0,1]
    float vaporHigh = 0.0f;    ///< vapor en altura alta (condensa a ~−40 °C, hielo): fracción [0,1]
};

/// Gradiente térmico del aire (°C/m). El estándar de la troposfera.
constexpr float kLapseEnvC = 0.0065f;
/// Gradiente de una PARCELA que sube sin condensar (°C/m): adiabático seco. Es el que fija el LCL.
constexpr float kLapseDryC = 0.0098f;
/// Escala en que se acaba el vapor por encima de la capa convectiva (m): el techo se desfleca ahí.
constexpr float kBlTopFadeM = 400.0f;
/// Temperaturas de condensación del vapor en altura y anchura (en °C) de la banda que ocupan.
constexpr float kMidCloudC   = -10.0f, kMidCloudWidthC  = 9.0f;
constexpr float kHighCloudC  = -40.0f, kHighCloudWidthC = 10.0f;

/// Temperatura del aire a la cota z (m sobre el nivel del mar).
inline float airTempC(const AirColumn& c, float zM) {
    return c.tSurfC - kLapseEnvC * std::max(zM - c.groundM, 0.0f);
}

/// Presión de vapor de saturación (hPa) — Magnus. Vale ±0,1 % entre −40 y 50 °C.
inline float satVaporHPa(float tC) {
    return 6.112f * std::exp(17.67f * tC / (tC + 243.5f));
}

/// Nivel de condensación (m sobre el nivel del mar): la cota a la que una parcela de superficie,
/// subiendo a q constante y enfriándose 9,8 °C/km, satura. Es la BASE de la nube convectiva.
/// Cerrado: no hay que iterar. Con rh = 1 la base está en el suelo (niebla).
inline float condensationLevelM(const AirColumn& c) {
    const float rh = std::clamp(c.rhSurf, 0.02f, 1.0f);
    // Punto de rocío por Magnus invertido: T_d = b·γ/(a−γ), γ = ln(rh) + a·T/(b+T).
    const float a = 17.67f, b = 243.5f;
    const float g = std::log(rh) + a * c.tSurfC / (b + c.tSurfC);
    const float tDew = b * g / (a - g);
    // Espenshade: la parcela alcanza T_d subiendo (T − T_d)/(9,8 − 1,8) km ≈ 125 m/°C.
    return c.groundM + std::max(c.tSurfC - tDew, 0.0f) / (kLapseDryC - 0.0018f);
}

/// Base MÍNIMA sobre el suelo (m). Lo que se aprendió con la fórmula vieja: una base a 200-400 m se
/// lee como un techo encima de la cabeza ("la gris está muy cercana"). La niebla es otra cosa.
constexpr float kMinBaseAglM = 600.0f;

/**
 * @brief La nube de BUEN TIEMPO (sin frente) por unidad de humedad: `kFairWeatherCover · H`. Es
 * cúmulo entero; el vapor de nivel medio y el alto salen SOLO de los frentes.
 *
 * ⚠️ ERA 0,73 Y SE CALIBRÓ CONTRA EL 0,67 DE LA TIERRA, que es la cobertura de TODO tipo de nube
 * (estratos, velos, altocúmulos, cirros). La mitad iba a cúmulo y la otra mitad a vapor medio, así
 * que el cielo estaba cubierto casi siempre y en casi todas partes (Andoni, 18-10: "¿siempre hay
 * nubes o casi siempre?"; con la capa media dibujada, el cielo salía CERRADO). Los cúmulos de buen
 * tiempo cubren de 0,1 a 0,3 del cielo: esto deja 0,10 con la humedad media del mundo (0,286) y 0,32
 * en la selva, y lo que sube de ahí son los frentes, que van y vienen. Para más o menos nube, se
 * cambia aquí (el clima), no en el render.
 */
constexpr float kFairWeatherCover = 0.365f;

/**
 * @brief Qué parte de la cobertura de un frente queda como vapor de NIVEL MEDIO detrás de él.
 * ⚠️ Era la cobertura entera: un frente que pasaba dejaba 0,6 de altocúmulo y, dibujado como nube
 * (no como velo), el cielo salía cerrado (Andoni, 18-10: "hay muchas nubes"). El frente sigue
 * trayendo nube media; menos.
 */
constexpr float kFrontMidVaporShare = 0.4f;

/// La BASE de la nube convectiva (m sobre el nivel del mar): el nivel de condensación, nunca más
/// bajo que `kMinBaseAglM` sobre el suelo. Es la que usan la fracción, la lluvia y el horneado.
inline float cloudBaseM(const AirColumn& c) {
    return std::max(condensationLevelM(c), c.groundM + kMinBaseAglM);
}

/// Fracción de nube CONVECTIVA a la cota z: 0 bajo la base, `cover` dentro de la capa convectiva,
/// y se desfleca en `kBlTopFadeM` por encima de ella. La base entra en 60 m (es un plano).
inline float convectiveFractionAt(const AirColumn& c, float zM) {
    if (c.cover <= 0.0f) return 0.0f;
    const float base = cloudBaseM(c);
    const float top  = c.groundM + c.blDepthM;
    if (top <= base + 1.0f) return 0.0f;
    const float in   = glm::smoothstep(base, base + 60.0f, zM);
    const float out  = 1.0f - glm::smoothstep(top, top + kBlTopFadeM, zM);
    return c.cover * in * out;
}

/// Fracción de nube del vapor en ALTURA a la cota z: una campana en temperatura alrededor de la
/// de condensación (−10 °C el nivel medio, −40 °C el hielo). La cota sale de la temperatura.
inline float aloftFractionAt(const AirColumn& c, float zM) {
    const float t = airTempC(c, zM);
    const float dm = (t - kMidCloudC)  / kMidCloudWidthC;
    const float dh = (t - kHighCloudC) / kHighCloudWidthC;
    // Campanas CORTADAS a 2,5 anchuras: fuera de su banda el vapor da exactamente 0 (y no 1e-8),
    // que es lo que deja al jugador volar por aire limpio de verdad.
    const float gm = (std::fabs(dm) < 2.5f) ? std::exp(-dm * dm) : 0.0f;
    const float gh = (std::fabs(dh) < 2.5f) ? std::exp(-dh * dh) : 0.0f;
    return std::max(c.vaporMid * gm, c.vaporHigh * gh);
}

/// LA función: fracción de nube a la cota z (m sobre el nivel del mar) en esta columna.
inline float cloudFractionAt(const AirColumn& c, float zM) {
    return std::clamp(std::max(convectiveFractionAt(c, zM), aloftFractionAt(c, zM)), 0.0f, 1.0f);
}

/// Cota (m) donde condensa el vapor en altura de una banda: la de su temperatura.
inline float altitudeOfTempM(const AirColumn& c, float tC) {
    return c.groundM + std::max(c.tSurfC - tC, 0.0f) / kLapseEnvC;
}

/// Fase del agua a esa cota: 0 = agua líquida (cúmulo denso, coliflor) · 1 = hielo (velo, fibras).
/// Entre −15 y −38 °C está mezclada. Es lo que decide el ASPECTO, en vez de "qué capa es".
inline float iceFractionAt(const AirColumn& c, float zM) {
    return glm::smoothstep(-15.0f, -38.0f, airTempC(c, zM));
}

/// Techo (m) de lo convectivo: donde su fracción cae por debajo de la mitad. Para la lluvia y el
/// "estoy dentro de la nube" del jugador, que no marchan la columna entera.
inline float convectiveTopM(const AirColumn& c) { return c.groundM + c.blDepthM + kBlTopFadeM * 0.5f; }

} // namespace Haruka::World
