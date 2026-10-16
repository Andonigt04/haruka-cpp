/**
 * @file mass_burst.glsl
 * @brief La capa masiva en GPU: reconstruye la mata `k` de un brote. GEMELO EXACTO de
 *        `src/Haruka/World/Props/MassBurst.h` (`massElement`, `massTangents`, el estado de 2 bits).
 *
 * Si se toca una constante aquí, se toca allí: la CPU usa la misma función para decidir qué matas
 * destruye una explosión o tumba una pisada, y si las dos divergen se destruye una mata y desaparece
 * otra.
 */
#ifndef HARUKA_MASS_BURST_GLSL
#define HARUKA_MASS_BURST_GLSL

#define HARUKA_MASS_MAX_COUNT 1024u
#define HARUKA_MASS_NO_STATE  0xFFFFFFFFu

struct HarukaMassBurst {
    vec4  a;   // xyz = centro relativo al origen del pase · w = escala mínima
    vec4  b;   // xyz = vertical local · w = escala máxima
    uvec4 c;   // x = semilla · y = matas · z = 1ª palabra de estado · w = bits de cellM
};

uint harukaMassHash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15;
    x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float harukaMassHashF(uint h, uint salt) {
    return float(harukaMassHash(h ^ (salt * 0x85EBCA6Bu))) / 4294967295.0;
}
uint harukaMassElementHash(uint seed, uint k) {
    return k == 0u ? seed : harukaMassHash(seed ^ (k * 0x9E3779B9u));
}
void harukaMassTangents(vec3 up, out vec3 u, out vec3 v) {
    vec3 ref = (abs(up.y) < 0.99) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    u = normalize(cross(ref, up));
    v = cross(up, u);
}

struct HarukaMassElement { vec2 offset; float yaw; float scale; float tint; };

HarukaMassElement harukaMassElement(uint seed, uint k, float cellM, float scaleMin, float scaleMax) {
    uint hk = harukaMassElementHash(seed, k);
    HarukaMassElement e;
    e.offset = (k == 0u) ? vec2(0.0)
             : vec2(harukaMassHashF(hk, 1u) - 0.5, harukaMassHashF(hk, 2u) - 0.5) * (0.9 * cellM);
    e.yaw   = harukaMassHashF(hk, 5u) * 6.2831853;
    e.scale = scaleMin + (scaleMax - scaleMin) * harukaMassHashF(hk, 3u);
    e.tint  = 0.85 + 0.30 * harukaMassHashF(hk, 4u);
    return e;
}

#endif // HARUKA_MASS_BURST_GLSL
