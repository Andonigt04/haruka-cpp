/**
 * @file planet.frag
 * @brief Fragment shader for planetary terrain chunks.
 *
 * Checkerboard pattern from per-face UVs. Both Procedural and Manual modes
 * use the same visual for now (different tint to distinguish them).
 * u_terrainMode: 0 = Procedural, 1 = Manual
 */
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec2 TexCoord;

layout(location = 11) uniform int u_terrainMode;
layout(location = 15) uniform int u_fogEnabled; // 1 = niebla on (config consola: fog)

// Texturas de bioma (Fase 4A). u_hasTex=0 → color procedural (fallback).
layout(location = 21) uniform sampler2D u_sandAlbedo;
layout(location = 22) uniform sampler2D u_sandNormal;
layout(location = 23) uniform int        u_hasTex;
layout(location = 24) uniform sampler2D u_grassAlbedo;
layout(location = 25) uniform sampler2D u_landAlbedo;
layout(location = 26) uniform sampler2D u_landNormal;
// Sombras del sol (depth map desde la luz). u_lightSpace ocupa locs 30..33.
layout(location = 30) uniform mat4      u_lightSpace;
layout(location = 34) uniform sampler2D u_shadowMap;
layout(location = 35) uniform int       u_shadowsOn;

// Only the fields actually used from the per-frame UBO.
// Must preserve std140 offsets: view(0), projection(64),
// cameraPos+pad(128), sunDirection+pad(144), sunLightColor+ambientStrength(160), enableHDR(176).
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

// Per-object UBO (binding 1). Para el terreno: u_planetRelCam.xyz = (cameraPos −
// planetCenter) en double→float (preciso) → relPos del fragmento = FragPos + eso.
// u_baseColorAndRadius.a = radio del planeta (m). Permite altura/pendiente/clima.
layout(std140, binding = 1) uniform PerObjectData {
    mat4 u_model;
    vec4 u_baseColorAndRadius;
    vec4 u_planetRelCam;
};

// --- Detalle fino 1–2 m como NORMAL MAP por píxel (Fase 4) ---
// El relieve de < ~8 m NO va en la geometría (aliasaría las normales del vértice
// = pinchos); aquí vive POR PÍXEL en la iluminación → detalle sin pinchos.
// DETAIL_STRENGTH: súbelo/bájalo si el relieve fino se ve flojo/exagerado.
const float DETAIL_STRENGTH = 0.0; // detalle procedural OFF (las texturas darán el relieve)

float hash1(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// Value noise 3D con GRADIENTE analítico (iq). Devuelve (valor, ∂/∂x,∂/∂y,∂/∂z).
vec4 noised(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    vec3 u  = f * f * (3.0 - 2.0 * f);
    vec3 du = 6.0 * f * (1.0 - f);
    float a = hash1(i + vec3(0,0,0));
    float b = hash1(i + vec3(1,0,0));
    float c = hash1(i + vec3(0,1,0));
    float d = hash1(i + vec3(1,1,0));
    float e = hash1(i + vec3(0,0,1));
    float f1= hash1(i + vec3(1,0,1));
    float g = hash1(i + vec3(0,1,1));
    float h = hash1(i + vec3(1,1,1));
    float k0=a, k1=b-a, k2=c-a, k3=e-a, k4=a-b-c+d, k5=a-c-e+g, k6=a-b-e+f1,
          k7=-a+b+c-d+e-f1-g+h;
    vec3 grad = du * vec3(
        k1 + k4*u.y + k6*u.z + k7*u.y*u.z,
        k2 + k5*u.z + k4*u.x + k7*u.z*u.x,
        k3 + k6*u.x + k5*u.y + k7*u.x*u.y);
    float val = k0 + k1*u.x + k2*u.y + k3*u.z
              + k4*u.x*u.y + k5*u.y*u.z + k6*u.z*u.x + k7*u.x*u.y*u.z;
    return vec4(val, grad);
}

// Perturba la normal con relieve fino (2 octavas ~2 m y ~0.8 m). Mismo marco
// que el damero (FragPos en metros) → escala real, sin geometría.
vec3 applyDetailNormal(vec3 N, vec3 wp) {
    vec3 g = noised(wp * 0.5).yzw            // celdas de ~2 m
           + noised(wp * 1.25).yzw * 0.5;    // celdas de ~0.8 m (media amplitud)
    vec3 gTan = g - dot(g, N) * N;           // solo la parte tangencial inclina la normal
    return normalize(N - DETAIL_STRENGTH * gTan);
}

// --- Material de bioma (Etapa 4A) — paleta dieselpunk sombría (TERROSA) ---
// Para variante FRÍA: tira los verdes a grises/azules ceniza y sube la nieve.
const vec3 BIOME_SAND  = vec3(0.44, 0.39, 0.27); // costa / tierra batida
const vec3 BIOME_GRASS = vec3(0.26, 0.34, 0.16); // hierba (verde tierra, no gris)
const vec3 BIOME_DRY   = vec3(0.40, 0.35, 0.21); // hierba seca / tierra parda
const vec3 BIOME_ROCK  = vec3(0.33, 0.29, 0.25); // roca parda
const vec3 BIOME_SNOW  = vec3(0.74, 0.74, 0.71); // nieve sucia (no blanco puro)

// Color de superficie por altura (m s.n.m.), pendiente (0 llano…1 vertical) y
// clima (0 frío/polos…1 cálido/ecuador). La TIERRA es verde por defecto; roca y
// nieve solo aparecen MUY alto o en laderas empinadas → no un planeta gris.
vec3 biomeColor(float altitude, float slope, float climate, vec3 wp) {
    // Rompe las bandas de contorno: ruido grande sobre la altura usada en los biomas.
    float aN = altitude + (noised(wp * 0.006).x - 0.5) * 700.0;
    float coldness = clamp((1.0 - climate) * 0.6 + altitude / 6000.0, 0.0, 1.0);
    vec3 c = BIOME_GRASS;                                            // base = TIERRA verde
    c = mix(c, BIOME_DRY,  smoothstep(1100.0, 2600.0, aN));          // hierba seca en altura
    c = mix(c, BIOME_ROCK, smoothstep(2800.0, 4400.0, aN));          // roca solo muy alto
    c = mix(c, BIOME_ROCK, smoothstep(0.70,   0.90,   slope));       // roca SOLO en laderas muy empinadas
    float snow = smoothstep(0.74, 0.95, coldness) * (1.0 - smoothstep(0.86, 0.97, slope));
    c = mix(c, BIOME_SNOW, snow);                                    // nieve solo muy fría/alta
    float v = noised(wp * 0.4).x;
    return c * (0.94 + 0.10 * v);
}

// Triplanar: muestrea una textura tileable proyectando desde los 3 ejes del
// mundo y mezclando por la normal → sin estiramiento ni costuras en la esfera.
// scale = 1/metros_por_tile. (wp = FragPos; nota: aún relativo a cámara → "swim"
// al moverse; se cambiará por una UV estable del generador.)
vec3 triplanarAlbedo(sampler2D tex, vec3 wp, vec3 n, float scale) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= (bw.x + bw.y + bw.z + 1e-5);
    vec3 cx = texture(tex, wp.zy * scale).rgb;
    vec3 cy = texture(tex, wp.xz * scale).rgb;
    vec3 cz = texture(tex, wp.xy * scale).rgb;
    return cx * bw.x + cy * bw.y + cz * bw.z;
}

void main() {
    // Posición del fragmento RELATIVA al centro del planeta (precisa) → altura,
    // dirección radial (clima/pendiente). u_planetRelCam.xyz = cameraPos−planetCenter.
    vec3  relPos   = FragPos + u_planetRelCam.xyz;
    float radius   = u_baseColorAndRadius.a;
    float altitude = length(relPos) - radius;             // m sobre el nivel del mar
    vec3  up       = normalize(relPos);                    // radial (planeta sin rotación)
    vec3  Ngeo     = normalize(Normal);                    // normal geométrica (para pendiente)
    float slope    = clamp(1.0 - dot(Ngeo, up), 0.0, 1.0); // 0 llano … 1 vertical
    float climate  = clamp(1.0 - abs(up.y), 0.0, 1.0);     // 0 polos … 1 ecuador

    float bodyProfile = u_planetRelCam.w;  // 0 terran · 1 luna · 2 gas
    vec3 baseColor;
    if (bodyProfile > 1.5) {
        // GAS (Júpiter): bandas horizontales por latitud, tonos cálidos.
        float n    = noised(FragPos * 0.0015).x;
        float band = sin(up.y * 16.0 + n * 4.0) * 0.5 + 0.5;
        baseColor  = mix(vec3(0.60, 0.46, 0.34), vec3(0.86, 0.77, 0.61), band);
    } else if (bodyProfile > 0.5) {
        // LUNA: regolito gris; suelos de cráter (poca pendiente) más oscuros y
        // bordes/laderas algo más claros → relieve legible.
        float g   = 0.52 + 0.05 * noised(FragPos * 0.02).x;
        g        *= mix(0.78, 1.06, slope);
        baseColor = vec3(g, g, g * 1.03);
    } else if (u_terrainMode == 1) {
        baseColor = vec3(0.30, 0.30, 0.38);                // modo manual: tinte neutro
    } else if (u_hasTex == 1) {
        // Bioma TEXTURIZADO (triplanar): hierba → tierra seca → roca → nieve,
        // + arena en TODA orilla (uv.x = shoreFactor del generador).
        float aN = altitude + (noised(FragPos * 0.006).x - 0.5) * 700.0; // rompe bandas
        float coldness = clamp((1.0 - climate) * 0.6 + altitude / 6000.0, 0.0, 1.0);
        vec3 col = triplanarAlbedo(u_grassAlbedo, FragPos, Ngeo, 0.5);                 // hierba (base)
        col = mix(col, triplanarAlbedo(u_landAlbedo, FragPos, Ngeo, 0.5),
                  smoothstep(900.0, 2400.0, aN));                                       // tierra seca en altura
        col = mix(col, BIOME_ROCK, smoothstep(0.55, 0.82, slope));                     // roca en laderas
        col = mix(col, BIOME_ROCK, smoothstep(2800.0, 4400.0, aN));                    // roca muy alto
        float snow = smoothstep(0.74, 0.95, coldness) * (1.0 - smoothstep(0.86, 0.97, slope));
        col = mix(col, BIOME_SNOW, snow);                                              // nieve
        float sandW = TexCoord.x * (1.0 - smoothstep(0.45, 0.7, slope));               // arena en orillas
        col = mix(col, triplanarAlbedo(u_sandAlbedo, FragPos, Ngeo, 0.7), sandW);
        baseColor = col;
    } else {
        baseColor = biomeColor(altitude, slope, climate, FragPos);   // procedural (sin texturas)
        float sandW = TexCoord.x * (1.0 - smoothstep(0.45, 0.7, slope));
        baseColor = mix(baseColor, BIOME_SAND, sandW);
    }

    // Normal del vértice (analítica, sin pinchos) + detalle fino 1–2 m por píxel.
    vec3 N = applyDetailNormal(Ngeo, FragPos);
    vec3 L = normalize(sunDirection);
    vec3 V = normalize(-FragPos);
    vec3 H = normalize(L + V);

    float ndl  = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 32.0);

    // Sombra del sol (PCF 3×3) — los props proyectan en u_shadowMap.
    float shadow = 0.0;
    if (u_shadowsOn != 0) {
        vec4 lp = u_lightSpace * vec4(FragPos, 1.0);
        vec3 pc = lp.xyz / lp.w * 0.5 + 0.5;     // a [0,1]
        if (pc.z <= 1.0 && pc.x > 0.0 && pc.x < 1.0 && pc.y > 0.0 && pc.y < 1.0) {
            float bias = max(0.0025 * (1.0 - ndl), 0.0008);
            vec2 texel = 1.0 / vec2(textureSize(u_shadowMap, 0));
            for (int x = -2; x <= 2; ++x)      // PCF 5×5 → bordes más suaves
            for (int y = -2; y <= 2; ++y) {
                float d = texture(u_shadowMap, pc.xy + vec2(x, y) * texel).r;
                shadow += (pc.z - bias > d) ? 1.0 : 0.0;
            }
            shadow /= 25.0;
        }
    }

    // Luz de luna (2ª luz): difusa tenue azulada → la noche no es negra cuando la
    // Luna está alta. Sin sombras (es luz suave de relleno).
    float ndlMoon = max(dot(N, normalize(moonDirection)), 0.0);

    vec3 color = ambientStrength * baseColor
               + 0.7 * ndl * sunLightColor * baseColor * (1.0 - 0.85 * shadow)
               + sunLightColor * 0.12 * spec * (1.0 - shadow)
               + moonLightColor * moonIntensity * ndlMoon * baseColor;

    // Niebla atmosférica: da profundidad y un horizonte claro, y disimula el LOD
    // lejano. Color grisáceo (dieselpunk sombrío). FOG_DENSITY = qué tan pronto cierra.
    const vec3  FOG_COLOR   = vec3(0.60, 0.63, 0.68);
    const float FOG_DENSITY = 0.00035; // conocido-bueno: oculta el horizonte de bajo LOD (tiras)
    float fog = (u_fogEnabled != 0) ? (1.0 - exp(-length(FragPos) * FOG_DENSITY)) : 0.0;
    color = mix(color, FOG_COLOR, fog);

    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    FragColor = vec4(color, 1.0);
}
