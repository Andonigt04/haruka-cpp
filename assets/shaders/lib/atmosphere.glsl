/**
 * @file atmosphere.glsl
 * @brief EL AIRE QUE DISPERSA LA LUZ: Rayleigh + Mie + ozono, dispersion simple por el rayo.
 *
 * GEMELO EXACTO de `src/Haruka/World/Planet/Atmosphere.h` (mismas constantes, mismos pasos, mismo orden).
 * Funciones PURAS: todo entra por parametro (posiciones relativas al CENTRO del planeta, en metros), asi
 * que la usan el cielo, la perspectiva aerea, las nubes y las sondas del banco sin depender de un UBO.
 * Ver la cabecera del gemelo para las unidades: la luz entra como irradiancia y sale como radiancia
 * del motor (`π·L`).
 *
 * ⚠️ En float: las posiciones respecto al centro son de ~6,4e6 m (ulp 0,5 m). Para el aire, que cambia
 * en kilometros, sobra; no se usa para nada que necesite el centimetro.
 */
#ifndef HARUKA_ATMOSPHERE_GLSL
#define HARUKA_ATMOSPHERE_GLSL

const float HARUKA_ATMO_TOP_M   = 100000.0;
const vec3  HARUKA_ATMO_BETA_R  = vec3(5.802e-6, 13.558e-6, 33.100e-6);
const float HARUKA_ATMO_HR      = 8000.0;
const float HARUKA_ATMO_BETA_MS = 3.996e-6;
const float HARUKA_ATMO_BETA_MA = 4.400e-6;
const float HARUKA_ATMO_HM      = 1200.0;
const float HARUKA_ATMO_MIE_G   = 0.80;
const vec3  HARUKA_ATMO_BETA_O  = vec3(0.650e-6, 1.881e-6, 0.085e-6);
const float HARUKA_ATMO_OZONE_C = 25000.0;
const float HARUKA_ATMO_OZONE_W = 15000.0;
const int   HARUKA_ATMO_VIEW_STEPS  = 16;
const int   HARUKA_ATMO_LIGHT_STEPS = 6;
const float HARUKA_ATMO_PI = 3.14159265358979323846;

/// Densidades relativas (Rayleigh, Mie, ozono) a la altitud `h` (m).
vec3 harukaAtmoDensity(float h) {
    h = max(h, 0.0);
    return vec3(exp(-h / HARUKA_ATMO_HR), exp(-h / HARUKA_ATMO_HM),
                max(0.0, 1.0 - abs(h - HARUKA_ATMO_OZONE_C) / HARUKA_ATMO_OZONE_W));
}

vec3 harukaAtmoExtinction(vec3 den) {
    return HARUKA_ATMO_BETA_R * den.x + vec3(HARUKA_ATMO_BETA_MS + HARUKA_ATMO_BETA_MA) * den.y
         + HARUKA_ATMO_BETA_O * den.z;
}

bool harukaAtmoRaySphere(vec3 ro, vec3 rd, float rad, out float t0, out float t1) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - rad * rad;
    float h = b * b - c;
    if (h < 0.0) { t0 = t1 = 0.0; return false; }
    float s = sqrt(h);
    t0 = -b - s; t1 = -b + s;
    return true;
}

float harukaAtmoPhaseR(float mu) { return 3.0 / (16.0 * HARUKA_ATMO_PI) * (1.0 + mu * mu); }
float harukaAtmoPhaseM(float mu) {
    const float g = HARUKA_ATMO_MIE_G, g2 = g * g;
    return 3.0 / (8.0 * HARUKA_ATMO_PI) * (1.0 - g2) * (1.0 + mu * mu)
         / ((2.0 + g2) * pow(1.0 + g2 - 2.0 * g * mu, 1.5));
}

// ── DISPERSION MULTIPLE: la tabla Ψ_ms(altitud, cos del Sol) de Hillaire ─────────────────────────
// La calcula la CPU (`atmoBuildMsLut`) y la sube `SkyLight` al binding 20: la MISMA tabla en los dos
// lados. Se lee con `texelFetch` y bilineal a mano, como `atmoMsSample`, para no depender de que el
// driver filtre texturas de 32 bits.
const int HARUKA_ATMO_MS_SIZE = 32;
layout(binding = 20) uniform sampler2D uAtmoMsLut;

vec3 harukaAtmoMs(float h, float muS) {
    float fu = clamp(0.5 * (muS + 1.0), 0.0, 1.0) * float(HARUKA_ATMO_MS_SIZE - 1);
    float fv = sqrt(clamp(h / HARUKA_ATMO_TOP_M, 0.0, 1.0)) * float(HARUKA_ATMO_MS_SIZE - 1);
    int u0 = min(int(fu), HARUKA_ATMO_MS_SIZE - 2), v0 = min(int(fv), HARUKA_ATMO_MS_SIZE - 2);
    float au = fu - float(u0), av = fv - float(v0);
    vec3 a = texelFetch(uAtmoMsLut, ivec2(u0, v0), 0).rgb, b = texelFetch(uAtmoMsLut, ivec2(u0 + 1, v0), 0).rgb;
    vec3 c = texelFetch(uAtmoMsLut, ivec2(u0, v0 + 1), 0).rgb, d = texelFetch(uAtmoMsLut, ivec2(u0 + 1, v0 + 1), 0).rgb;
    return mix(mix(a, b, au), mix(c, d, au), av);
}

/// Profundidad optica desde `p` hasta el espacio en `d`; 1e9 si el planeta la tapa.
vec3 harukaAtmoOpticalDepthToSpace(vec3 p, vec3 d, float R, int steps) {
    float g0, g1;
    if (harukaAtmoRaySphere(p, d, R, g0, g1) && g0 > 0.0) return vec3(1.0e9);
    float a0, a1;
    if (!harukaAtmoRaySphere(p, d, R + HARUKA_ATMO_TOP_M, a0, a1) || a1 <= 0.0) return vec3(0.0);
    float t0 = max(a0, 0.0), dt = (a1 - t0) / float(steps);
    vec3 tau = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        vec3 x = p + d * (t0 + (float(i) + 0.5) * dt);
        tau += harukaAtmoExtinction(harukaAtmoDensity(length(x) - R)) * dt;
    }
    return tau;
}

vec3 harukaAtmoTransmittanceToSpace(vec3 p, vec3 d, float R, int steps) {
    return exp(-harukaAtmoOpticalDepthToSpace(p, d, R, steps));
}

/**
 * @brief Luz dispersada hacia el ojo por un rayo (ver `atmoInscatter` en el gemelo).
 * @param tMax  < 0 = hasta el suelo o el espacio; si no, hasta el objeto.
 * @param viewSteps, lightSteps  la sonda y el cielo usan los del gemelo; la perspectiva aerea, menos.
 */
vec3 harukaAtmoInscatterN(vec3 ro, vec3 rd, float tMax, float R, vec3 sunDir, vec3 sunE,
                          vec3 moonDir, vec3 moonE, int viewSteps, int lightSteps, out vec3 outT) {
    outT = vec3(1.0);
    float a0, a1;
    if (!harukaAtmoRaySphere(ro, rd, R + HARUKA_ATMO_TOP_M, a0, a1) || a1 <= 0.0) return vec3(0.0);
    float tEnd = a1;
    float g0, g1;
    if (harukaAtmoRaySphere(ro, rd, R, g0, g1) && g0 > 0.0) tEnd = min(tEnd, g0);
    if (tMax >= 0.0) tEnd = min(tEnd, tMax);
    float tStart = max(a0, 0.0);
    if (tEnd <= tStart) return vec3(0.0);

    float muS = dot(rd, sunDir), muM = dot(rd, moonDir);
    float pRs = harukaAtmoPhaseR(muS), pMs = harukaAtmoPhaseM(muS);
    float pRm = harukaAtmoPhaseR(muM), pMm = harukaAtmoPhaseM(muM);
    bool sunOn  = (sunE.x + sunE.y + sunE.z) > 0.0;
    bool moonOn = (moonE.x + moonE.y + moonE.z) > 0.0;
    float dt = (tEnd - tStart) / float(viewSteps);
    vec3 tauView = vec3(0.0), L = vec3(0.0);
    for (int i = 0; i < viewSteps; ++i) {
        vec3 x = ro + rd * (tStart + (float(i) + 0.5) * dt);
        vec3 den = harukaAtmoDensity(length(x) - R);
        vec3 ext = harukaAtmoExtinction(den);
        vec3 Tv = exp(-(tauView + ext * (0.5 * dt)));
        vec3 scR = HARUKA_ATMO_BETA_R * den.x, scM = vec3(HARUKA_ATMO_BETA_MS * den.y);
        float hx = length(x) - R;
        vec3 up = x / (hx + R);
        if (sunOn) {
            vec3 Ts = harukaAtmoTransmittanceToSpace(x, sunDir, R, lightSteps);
            L += Tv * Ts * sunE * (scR * pRs + scM * pMs) * dt;
            L += Tv * (scR + scM) * harukaAtmoMs(hx, dot(up, sunDir)) * sunE * dt;
        }
        if (moonOn) {
            vec3 Tm = harukaAtmoTransmittanceToSpace(x, moonDir, R, lightSteps);
            L += Tv * Tm * moonE * (scR * pRm + scM * pMm) * dt;
            L += Tv * (scR + scM) * harukaAtmoMs(hx, dot(up, moonDir)) * moonE * dt;
        }
        tauView += ext * dt;
    }
    outT = exp(-tauView);
    return L * HARUKA_ATMO_PI;
}

vec3 harukaAtmoInscatter(vec3 ro, vec3 rd, float tMax, float R, vec3 sunDir, vec3 sunE,
                         vec3 moonDir, vec3 moonE, out vec3 outT) {
    return harukaAtmoInscatterN(ro, rd, tMax, R, sunDir, sunE, moonDir, moonE,
                                HARUKA_ATMO_VIEW_STEPS, HARUKA_ATMO_LIGHT_STEPS, outT);
}

#endif // HARUKA_ATMOSPHERE_GLSL
