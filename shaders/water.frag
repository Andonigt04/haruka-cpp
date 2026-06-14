/**
 * @file water.frag
 * @brief Planetary ocean shading — Fase 3.
 *
 * Procedural sky reflection (no cubemap dependency: the engine's IBL is not
 * populated) + Fresnel + sun specular + Gerstner-Jacobian foam.
 *
 * u_waterQuality (settings hook, default 1):
 *   0 = sun specular only (cheapest)
 *   1 = + procedural sky reflection (default)
 *   2 = reserved for screen-space reflections (future)
 */
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in float WaveHeight;
layout(location = 3) in float Foam;
layout(location = 4) in float IsLake;   // 1 = lago (agua dulce), 0 = océano
layout(location = 5) in float Depth;    // profundidad del agua (m): orilla≈0

layout(location = 20) uniform int u_waterQuality;

layout(std140, binding = 0) uniform PerFrameData {
    mat4  view;
    mat4  projection;
    vec3  _cameraPos;   float _pad0;
    vec3  sunDirection; float _pad1;
    vec3  sunLightColor; float ambientStrength;
    int   enableHDR;
    int   _enableBloom; int _enableSSAO; int _enableIBL; int _enableShadows;
    int   _pad3a; int _pad3b; int _pad3c;
    vec3  moonDirection;  float moonIntensity;   // 2ª luz (luna)
    vec3  moonLightColor; float _pad4;
};

const vec3 DEEP_COLOR    = vec3(0.015, 0.07, 0.14);
const vec3 SHALLOW_COLOR = vec3(0.05, 0.28, 0.40);
// Lago (agua dulce): más turquesa/verde y menos profundo que el océano.
const vec3 LAKE_DEEP     = vec3(0.04, 0.16, 0.18);
const vec3 LAKE_SHALLOW  = vec3(0.12, 0.40, 0.42);

// Cheap analytic sky: horizon haze → zenith blue, plus a soft sun halo.
vec3 skyColor(vec3 dir, vec3 sunDir) {
    float up   = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3  zen  = vec3(0.25, 0.45, 0.80);
    vec3  hor  = vec3(0.70, 0.80, 0.92);
    vec3  base = mix(hor, zen, pow(up, 0.6));
    float sun  = pow(max(dot(dir, sunDir), 0.0), 64.0);
    return base + sunLightColor * sun * 0.6;
}

void main() {
    vec3 N = normalize(Normal);
    vec3 V = normalize(-FragPos);   // camera at origin in cam-relative space
    vec3 L = normalize(sunDirection);
    vec3 H = normalize(L + V);

    float ndv = max(dot(N, V), 0.0);

    // Fresnel (Schlick) — more reflective at grazing angles.
    float fresnel = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);

    // Body colour: deeper looking straight down, lighter at grazing angle.
    // Lago vs océano: paleta de agua dulce (turquesa) si IsLake.
    vec3 deep    = mix(DEEP_COLOR,    LAKE_DEEP,    IsLake);
    vec3 shallow = mix(SHALLOW_COLOR, LAKE_SHALLOW, IsLake);
    // Color por PROFUNDIDAD real: turquesa en el bajío → azul oscuro en lo hondo.
    // (+ un poco de aclarado por ángulo rasante para el brillo del horizonte.)
    float shallowF = exp(-Depth * 0.10);            // 1 en la orilla, →0 a ~20-30 m
    vec3 body = mix(deep, shallow, max(shallowF, ndv * 0.35));

    vec3 color = body;

    // Reflection (quality-gated).
    if (u_waterQuality >= 1) {
        vec3 R = reflect(-V, N);
        vec3 sky = skyColor(R, L);
        color = mix(body, sky, fresnel);
    } else {
        // Low: just a flat sky tint via fresnel.
        color = mix(body, vec3(0.45, 0.62, 0.85), fresnel);
    }

    // Diffuse lift so the night side isn't pure black.
    float ndl = max(dot(N, L), 0.0);
    color += sunLightColor * body * ndl * 0.25;

    // Luz de luna: aporte difuso azulado + un brillo especular suave (reflejo lunar
    // en el agua) → el mar nocturno no queda negro y tiene un destello plateado.
    vec3  ML       = normalize(moonDirection);
    float ndlMoon  = max(dot(N, ML), 0.0);
    vec3  HM       = normalize(ML + V);
    float specMoon = pow(max(dot(N, HM), 0.0), 300.0);
    color += moonLightColor * moonIntensity * (body * ndlMoon + vec3(specMoon) * 0.6);

    // Sun specular glint.
    // Sun specular glint — más cerrado y tenue (el brillo anterior era demasiado
    // drástico/irreal): reflejo de sol puntual, no un fogonazo en todo el mar.
    float spec = pow(max(dot(N, H), 0.0), 400.0);
    color += sunLightColor * spec * 0.45;

    color += ambientStrength * body;

    // Espuma de ORILLA: banda blanca donde el agua es muy somera (Depth→0),
    // modulada por el oleaje (las crestas empujan la espuma). Vale para mar y lago.
    float shoreFoam = (1.0 - smoothstep(0.0, 2.2, Depth))
                    * (0.55 + 0.45 * smoothstep(-0.4, 1.0, WaveHeight));
    // Foam de cresta (Gerstner-Jacobian + crestas altas): casi nula en lagos calmos.
    float crestFoam = smoothstep(1.2, 2.2, WaveHeight) * (1.0 - 0.9 * IsLake);
    float foam = clamp(max(max(Foam * (1.0 - 0.9 * IsLake), crestFoam), shoreFoam), 0.0, 1.0);
    color = mix(color, vec3(0.85, 0.92, 0.97), foam * 0.75);

    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    float alpha = mix(0.80, 1.0, max(fresnel, foam));
    FragColor = vec4(color, alpha);
}
