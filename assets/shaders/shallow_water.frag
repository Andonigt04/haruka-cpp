/**
 * @file shallow_water.frag
 * @brief Shallow-water heightfield shading: two-sided Lambert + sun specular,
 *        with foam blended over the steep (breaking) crests.
 */
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in float Alpha;
layout(location = 3) in float Foam;

// UBO (binding 6) — std140: vec3 (12) + float(4) = 16, x2 = 32 bytes.
layout(std140, binding = 6) uniform ShallowWaterParams {
    vec3  u_color;         // base water colour
    float u_alphaMode;     // 1 = per-vertex Alpha; 0 = opaque
    vec3  u_foamColor;     // breaking-crest foam
    float u_foamStrength;  // 0..1 how much foam overrides the base colour
};

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;     float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
};

void main() {
    vec3 N = normalize(Normal);
    vec3 V = normalize(-FragPos);
    if (dot(N, V) < 0.0) N = -N;   // two-sided

    vec3 L = normalize(sunDirection);
    vec3 H = normalize(L + V);

    float ndl  = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 48.0);

    // La respuesta a la luz, aparte: la comparten el agua y la espuma. Tenerla en una variable es
    // lo que impide que se separen — que es justo lo que había pasado.
    vec3 lit = vec3(ambientStrength) + 0.85 * ndl * sunLightColor;

    vec3 color = lit * u_color
               + sunLightColor * 0.25 * spec;

    // Espuma de cresta rompiente.
    //
    // ⚠️ VA MULTIPLICADA POR LA LUZ. Antes era `mix(color, u_foamColor, f)` con `u_foamColor` EN
    // CRUDO, más un `color += u_foamColor * 0.15 * f` explícitamente autoiluminado. O sea que donde
    // había espuma la superficie valía un blanco fijo, idéntico a mediodía y a medianoche: los lagos
    // y los ríos brillaban en la oscuridad y "la luz no les afectaba" — no como efecto secundario,
    // sino porque literalmente no entraba en la cuenta.
    //
    // Y saturaba fácil: `u_foamStrength` vale 1,2, así que `f` llega a 1 con `Foam ≥ 0,83` y láminas
    // enteras quedaban en blanco puro, no solo las crestas.
    //
    // La espuma sigue destacando sobre el agua, que era la intención del término autoiluminado, pero
    // por la razón correcta: es más CLARA bajo la misma luz. De noche se apaga con todo lo demás.
    float f = clamp(u_foamStrength * Foam, 0.0, 1.0);
    color = mix(color, u_foamColor * lit, f);

    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }
    float a = (u_alphaMode > 0.5) ? Alpha : 1.0;
    FragColor = vec4(color, a);
}