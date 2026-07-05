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
#version 460 core   // gl_DrawID (core 4.6) para MultiDrawIndirect del agua

layout(location = 0) in vec3  aPos;        // sea-level position relative to chunk centre
layout(location = 1) in vec3  aNormal;     // radial outward (sphere normal at sea level)
layout(location = 2) in vec2 aWaterParam; // x = nivel (km, 0=océano) · y = profundidad (m)
layout(location = 3) in vec3  aMorphTarget; // CDLOD: posición en el LOD padre (decimado)

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out float WaveHeight; // signed crest height (m)
layout(location = 3) out float Foam;       // 0..1 from Gerstner Jacobian (crest pinching)
layout(location = 4) out float IsLake;     // 1 = lago, 0 = océano (para el fragment)
layout(location = 5) out float Depth;      // profundidad del agua (m): orilla≈0
layout(location = 6) out vec3  WorldDir;   // dirección RADIAL (planeta→vértice), PRECISA (para costa
                                           // per-píxel: evita el float32 de FragPos+planetRelCam)

layout(location = 10) uniform vec3  u_chunkOffset;   // chunkCentre - cameraPos
layout(location = 12) uniform float u_morphFactor;   // 0=detalle, 1=forma del padre (CDLOD, misma location que el terreno)
// MultiDrawIndirect del agua: por cada draw del batch, offset+morph vienen de este SSBO por gl_DrawID.
layout(location = 40) uniform int u_batched;         // 1 = leer offset/morph del SSBO
struct WDrawItem { vec4 offMorph; };                 // xyz=offset, w=morph
layout(std430, binding = 7) readonly buffer WDrawSSBO { WDrawItem uItems[]; };
layout(location = 14) uniform float u_time;          // seconds
layout(location = 15) uniform vec3  u_waveUp;        // radial up at camera surface point
layout(location = 16) uniform vec3  u_waveTangent;   // tangent basis (global per frame)
layout(location = 17) uniform vec3  u_waveBitangent;
layout(location = 18) uniform vec2  u_windDir;       // wind/current direction in tangent plane (unit)
layout(location = 19) uniform float u_windStrength;  // 0..N, scales amplitude/chop
layout(location = 21) uniform float u_tideHeight;    // F5.4: nivel de marea (m), desplazamiento radial del mar

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
// Amplitudes (m) — oleaje VIVO pero no embravecido (~1.6× vs el calmado previo, que a
// distancia/marea baja se veía casi plano). El sistema de mareas escala esto en runtime
// vía u_windStrength; con marea viva el oleaje sube más.
const float AMP[NUM_WAVES]     = float[](1.3,   0.8,  0.45, 0.25, 0.13, 0.08);
const float STEEP[NUM_WAVES]   = float[](0.75,  0.70, 0.65, 0.60, 0.50, 0.42);
const float ANGOFF[NUM_WAVES]  = float[](0.0,  0.55, -0.6,  1.1, -1.3,  0.9);

void main() {
    // CDLOD: la lámina se "aplana" hacia la forma del LOD padre antes del cambio de
    // nivel → sin salto de teselación (igual que el terreno). El faldón lleva su propio
    // morph = su posición, así no se mueve.
    // Datos por-draw: en batched (MultiDrawIndirect) vienen del SSBO por gl_DrawID.
    vec3  off   = u_chunkOffset;
    float morph = u_morphFactor;
    if (u_batched == 1) { WDrawItem d = uItems[gl_DrawID]; off = d.offMorph.xyz; morph = d.offMorph.w; }
    vec3 basePos   = mix(aPos, aMorphTarget, morph);
    vec3 camRelPos = off + basePos;

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
        float w   = sqrt(GRAV * k) * 0.55;         // deep-water dispersion, amansado (x0.55: se veía demasiado rápido)
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
    // Espuma SOLO en pliegues reales de cresta (detJ→0), no en toda la ondulación: antes
    // Foam=1-detJ disparaba espuma en cuanto detJ bajaba de 1 → rayas blancas rizadas por
    // TODO el mar abierto (se veía sucio). Gate duro: 0 hasta detJ~0.35, sube al doblarse.
    Foam = 1.0 - smoothstep(0.0, 0.35, detJ);

    // CLAMP DEL VALLE DE LA OLA: en agua fina el valle (disp.y<0) dipeaba por DEBAJO del lecho →
    // el fragmento de agua quedaba detrás del fondo → el z-test lo descartaba → HUECOS de agua (los
    // "círculos donde el agua no se renderiza") que se movían con la ola. Clampamos el valle a ~85%
    // de la profundidad (aWaterParam.y, m) → el agua NUNCA atraviesa el fondo (sin huecos) y las
    // CRESTAS siguen subiendo → el oleaje SIGUE VIÉNDOSE. En lo hondo no toca (profundidad >> ola).
    disp.y = max(disp.y, -aWaterParam.y * 0.85);

    // World-space displaced position.
    vec3 worldDisp = u_waveTangent * disp.x + u_waveUp * disp.y + u_waveBitangent * disp.z;
    // F5.4: NIVEL de marea → desplazamiento radial del MAR (no lagos), el océano "respira".
    worldDisp += u_waveUp * (u_tideHeight * (isLake ? 0.0 : 1.0));
    vec3 pos = camRelPos + worldDisp;

    // World-space normal from tangent-space accumulator.
    vec3 N = normalize(u_waveTangent * nrm.x + u_waveUp * nrm.y + u_waveBitangent * nrm.z);
    // Guard: keep it pointing outward (use sea-level radial normal as reference).
    if (dot(N, normalize(aNormal)) < 0.0) N = -N;
    // A DISTANCIA aplana la normal hacia el radial (plano): las olas sub-píxel hacían MOIRÉ de
    // especular (rizos circulares). Cerca = olas completas; >~10 km = mar liso, sin moiré.
    float flatten = smoothstep(6000.0, 22000.0, camDist);
    N = normalize(mix(N, normalize(aNormal), flatten));

    Normal     = N;
    FragPos    = pos;
    WaveHeight = crest;
    IsLake     = isLake ? 1.0 : 0.0;
    Depth      = aWaterParam.y;
    WorldDir   = aNormal;   // radial sea-level (planeta→vértice), precisa para la costa per-píxel
    gl_Position = projection * mat4(mat3(view)) * vec4(pos, 1.0);
}
