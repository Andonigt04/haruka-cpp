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

layout(location = 20) uniform int u_waterQuality;

layout(std140, binding = 0) uniform PerFrameData {
    mat4  view;
    mat4  projection;
    vec3  _cameraPos;   float _pad0;
    vec3  sunDirection; float _pad1;
    vec3  sunLightColor; float ambientStrength;
    int   enableHDR;
};

const vec3 DEEP_COLOR    = vec3(0.015, 0.07, 0.14);
const vec3 SHALLOW_COLOR = vec3(0.05, 0.28, 0.40);

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
    vec3 body = mix(DEEP_COLOR, SHALLOW_COLOR, ndv);

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

    // Sun specular glint.
    float spec = pow(max(dot(N, H), 0.0), 256.0);
    color += sunLightColor * spec * 1.5;

    color += ambientStrength * body;

    // Foam: Gerstner-Jacobian (crest folds) + tall crests.
    float crestFoam = smoothstep(1.2, 2.2, WaveHeight);
    float foam = clamp(max(Foam, crestFoam), 0.0, 1.0);
    color = mix(color, vec3(0.85, 0.92, 0.97), foam * 0.7);

    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    float alpha = mix(0.80, 1.0, max(fresnel, foam));
    FragColor = vec4(color, alpha);
}
