/**
 * @file prop.frag
 * @brief Props (árboles/rocas): textura triplanar (corteza/follaje/roca) elegida por
 *        el color de vértice (marrón=corteza, verde=hoja, gris=roca). Sin UVs; el
 *        triplanar samplea por posición de mundo → la escala se mantiene al crecer.
 *        Fallback a color por vértice si no hay texturas. + lambert + niebla.
 */
#version 450 core

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec3 Color;
layout(location = 3) in vec3 LocalPos;

layout(location = 0) out vec4 FragColor;

layout(location = 15) uniform int       u_fogEnabled;
layout(location = 20) uniform sampler2D u_bark;
layout(location = 21) uniform sampler2D u_foliage;
layout(location = 22) uniform sampler2D u_rock;
layout(location = 23) uniform int       u_hasPropTex;

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;      float _pad0;
    vec3 sunDirection;    float _pad1;
    vec3 sunLightColor;   float ambientStrength;
    int  enableHDR;
};

vec3 triplanar(sampler2D tex, vec3 p, vec3 n, float scale) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= (bw.x + bw.y + bw.z + 1e-5);
    vec3 cx = texture(tex, p.yz * scale).rgb;
    vec3 cy = texture(tex, p.xz * scale).rgb;
    vec3 cz = texture(tex, p.xy * scale).rgb;
    return cx * bw.x + cy * bw.y + cz * bw.z;
}

void main() {
    vec3 N = normalize(Normal);
    vec3 albedo = Color; // fallback: color por vértice (con variación por instancia)

    // Cristal/mena: color MUY saturado → no es corteza/hoja/roca. Se muestra su color
    // directo (no textura) y brilla un poco para resaltar la mena entre las rocas.
    float mx = max(max(Color.r, Color.g), Color.b);
    float mn = min(min(Color.r, Color.g), Color.b);
    bool crystal = (mx - mn) > 0.28 || mx > 0.80; // saturado (gema) o muy brillante (metal)

    if (u_hasPropTex != 0 && !crystal) {
        bool leaf = (Color.g > Color.r && Color.g > Color.b);
        bool gray = (abs(Color.r - Color.g) < 0.08 && abs(Color.g - Color.b) < 0.08);
        vec3 t = leaf ? triplanar(u_foliage, LocalPos, N, 0.6)
               : gray ? triplanar(u_rock,    LocalPos, N, 0.5)
                      : triplanar(u_bark,    LocalPos, N, 0.8);
        // mantiene una pizca del tinte por instancia para que no sean clones
        albedo = t * (0.8 + 0.4 * dot(Color, vec3(0.333)) * 2.0);
    }

    // Iluminación con más FORMA (menos plano): difuso + relleno trasero tenue +
    // medio-ambiente hemisférico (cielo arriba / suelo abajo) + rim de borde.
    vec3 L = normalize(sunDirection);
    float ndl  = dot(N, L);
    float diff = max(ndl, 0.0);
    float fill = max(-ndl, 0.0) * 0.18;                 // luz de relleno por detrás
    // hemisférico: usa la componente vertical de la normal de mundo (FragPos es cam-rel,
    // pero su dirección sirve de "arriba" aproximado de la escena).
    float hemi = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3  amb  = sunLightColor * (ambientStrength * mix(0.65, 1.25, hemi));
    vec3 col = albedo * (amb + (diff + fill) * sunLightColor);
    float rim = pow(1.0 - max(dot(N, normalize(-FragPos)), 0.0), 3.0) * 0.18; // borde
    col += albedo * rim * sunLightColor;
    if (crystal) col += albedo * 0.45;   // glow del cristal

    const vec3 FOG_COLOR = vec3(0.60, 0.63, 0.68);
    float fog = (u_fogEnabled != 0) ? (1.0 - exp(-length(FragPos) * 0.00035)) : 0.0;
    col = mix(col, FOG_COLOR, fog);

    FragColor = vec4(col, 1.0);
}
