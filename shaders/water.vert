/**
 * @file water.vert
 * @brief Planetary ocean — Fase 2 (Gerstner waves).
 *
 * Gerstner waves give real fluid motion (water particles move in circles →
 * sharp crests, broad troughs that travel). The wave bank is evaluated in a
 * tangent plane anchored at the CAMERA's surface point (u_waveTangent/Bitangent/
 * Up, supplied per frame), which is:
 *   - seam-free: the basis is global per frame, identical across all chunks;
 *   - precise: phase uses camera-relative positions (small floats), avoiding the
 *     catastrophic precision loss of phase = k * |absolutePos| at planetary scale.
 * Curvature error over the visible ocean patch (a few km vs 6371 km radius) is
 * negligible.
 *
 * Wind hook (future storm system): u_windDir / u_windStrength bias the wave
 * directions and amplitude. A sparse storm/wind system can drive these per
 * region without touching this shader.
 */
#version 450 core

layout(location = 0) in vec3  aPos;        // sea-level position relative to chunk centre
layout(location = 1) in vec3  aNormal;     // radial outward (sphere normal at sea level)
layout(location = 2) in vec2 aWaterParam; // x = nivel (km, 0=océano) · y = profundidad (m)

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out float WaveHeight; // signed crest height (m)
layout(location = 3) out float Foam;       // 0..1 from Gerstner Jacobian (crest pinching)
layout(location = 4) out float IsLake;     // 1 = lago, 0 = océano (para el fragment)
layout(location = 5) out float Depth;      // profundidad del agua (m): orilla≈0

layout(location = 10) uniform vec3  u_chunkOffset;   // chunkCentre - cameraPos
layout(location = 14) uniform float u_time;          // seconds
layout(location = 15) uniform vec3  u_waveUp;        // radial up at camera surface point
layout(location = 16) uniform vec3  u_waveTangent;   // tangent basis (global per frame)
layout(location = 17) uniform vec3  u_waveBitangent;
layout(location = 18) uniform vec2  u_windDir;       // wind direction in tangent plane (unit)
layout(location = 19) uniform float u_windStrength;  // 0..N, scales amplitude/chop

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;      float _pad0;
    vec3 sunDirection;    float _pad1;
    vec3 sunLightColor;   float ambientStrength;
    int  enableHDR;
    int  _enableBloom; int _enableSSAO; int _enableIBL; int _enableShadows;
    int  _pad3a; int _pad3b; int _pad3c;
    vec3 moonDirection;  float moonIntensity;
    vec3 moonLightColor; float _pad4;
};

const int   NUM_WAVES = 6;
const float GRAV      = 9.81;

// Per-wave: wavelength (m), amplitude (m), steepness, direction angle offset (rad).
const float WAVELEN[NUM_WAVES] = float[](120.0, 73.0, 41.0, 23.0, 13.0, 7.0);
// Amplitudes (m) — oleaje calmado (~mitad) para un mar realista, no embravecido.
// El sistema de mareas (fase/posición de la luna) escalará esto en runtime vía u_windStrength.
const float AMP[NUM_WAVES]     = float[](0.8,   0.5,  0.28, 0.15, 0.08, 0.05);
const float STEEP[NUM_WAVES]   = float[](0.75,  0.70, 0.65, 0.60, 0.50, 0.42);
const float ANGOFF[NUM_WAVES]  = float[](0.0,  0.55, -0.6,  1.1, -1.3,  0.9);

void main() {
    vec3 camRelPos = u_chunkOffset + aPos;

    // 2D position in the camera-anchored tangent plane.
    vec2 horiz = vec2(dot(camRelPos, u_waveTangent), dot(camRelPos, u_waveBitangent));

    float baseAng = atan(u_windDir.y, u_windDir.x);
    float windAmp = clamp(u_windStrength, 0.05, 4.0);

    // Lago = agua calmada: oleaje casi nulo (rizos mm) frente al oleaje oceánico.
    bool  isLake    = (aWaterParam.x > 0.0);
    float waveScale = isLake ? 0.04 : 1.0;

    vec3  disp = vec3(0.0);              // tangent-space displacement (x=T, y=up, z=B)
    vec3  nrm  = vec3(0.0, 1.0, 0.0);    // tangent-space normal accumulator
    float crest = 0.0;
    // Jacobian of horizontal displacement (xx, zz, xz) for foam where the
    // surface folds/pinches at sharp crests (det J < 0 ⇒ foam).
    float Jxx = 1.0, Jzz = 1.0, Jxz = 0.0;

    // Wave LOD: fade out a wave once its wavelength shrinks below a few pixels on
    // screen, i.e. once camera distance exceeds ~ wavelength * factor. This kills
    // shimmer/aliasing and saves nothing on vertex count but a lot on visual noise.
    float camDist = length(camRelPos);

    for (int i = 0; i < NUM_WAVES; ++i) {
        float ang = baseAng + ANGOFF[i];
        vec2  D   = vec2(cos(ang), sin(ang));
        float k   = 6.28318530718 / WAVELEN[i];   // spatial frequency
        // Distance fade: full amplitude until 400× wavelength, gone by 1200×.
        float fade = 1.0 - smoothstep(WAVELEN[i] * 400.0, WAVELEN[i] * 1200.0, camDist);
        float A   = AMP[i] * windAmp * fade * waveScale;
        if (A < 1e-4) continue;
        float w   = sqrt(GRAV * k);                // deep-water dispersion
        float Q   = STEEP[i] / (k * A * float(NUM_WAVES)); // keep crests from looping

        float phase = k * dot(D, horiz) - w * u_time;
        float c = cos(phase), s = sin(phase);

        // Gerstner displacement (horizontal in tangent plane, vertical along up).
        disp.x += Q * A * D.x * c;
        disp.z += Q * A * D.y * c;
        disp.y += A * s;
        crest  += A * s;

        // Analytic normal accumulation (GPU Gems 1).
        float WA = k * A;
        nrm.x -= D.x * WA * c;
        nrm.z -= D.y * WA * c;
        nrm.y -= Q   * WA * s;

        // Jacobian terms: ∂disp_h/∂h = -Q A k D⊗D sin(phase).
        float QAk = Q * A * k * s;
        Jxx -= QAk * D.x * D.x;
        Jzz -= QAk * D.y * D.y;
        Jxz -= QAk * D.x * D.y;
    }

    float detJ = Jxx * Jzz - Jxz * Jxz;       // <1 where crests pinch, <0 = fold
    Foam = clamp(1.0 - detJ, 0.0, 1.0);

    // World-space displaced position.
    vec3 worldDisp = u_waveTangent * disp.x + u_waveUp * disp.y + u_waveBitangent * disp.z;
    vec3 pos = camRelPos + worldDisp;

    // World-space normal from tangent-space accumulator.
    vec3 N = normalize(u_waveTangent * nrm.x + u_waveUp * nrm.y + u_waveBitangent * nrm.z);
    // Guard: keep it pointing outward (use sea-level radial normal as reference).
    if (dot(N, normalize(aNormal)) < 0.0) N = -N;

    Normal     = N;
    FragPos    = pos;
    WaveHeight = crest;
    IsLake     = isLake ? 1.0 : 0.0;
    Depth      = aWaterParam.y;
    gl_Position = projection * mat4(mat3(view)) * vec4(pos, 1.0);
}
