/**
 * @file leaf.frag
 * @brief Hojas: textura (tu leaf.png) × tinte por instancia + lambert (doble cara)
 *        + niebla. Alpha-test por si la textura recorta la forma.
 */
#version 450 core

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec3 Tint;

layout(location = 0) out vec4 FragColor;

layout(location = 15) uniform int       u_fogEnabled;
layout(location = 20) uniform sampler2D u_leaf;
layout(location = 23) uniform int       u_hasTex;

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;      float _pad0;
    vec3 sunDirection;    float _pad1;
    vec3 sunLightColor;   float ambientStrength;
    int  enableHDR;
};

void main() {
    vec4 t = (u_hasTex != 0) ? texture(u_leaf, TexCoord) : vec4(0.28, 0.50, 0.20, 1.0);
    if (t.a < 0.4) discard;                  // recorte por alpha (si la textura lo trae)

    vec3 N = normalize(Normal);
    if (!gl_FrontFacing) N = -N;             // hoja vista por ambas caras
    vec3 L = normalize(sunDirection);
    float ndl  = dot(N, L);
    float diff = max(ndl, 0.0);
    float fill = max(-ndl, 0.0) * 0.25;      // translucidez (la hoja deja pasar luz)
    float hemi = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    float amb  = ambientStrength * mix(0.6, 1.3, hemi);
    vec3 col = t.rgb * Tint * (amb + diff + fill) * sunLightColor;

    const vec3 FOG_COLOR = vec3(0.60, 0.63, 0.68);
    float fog = (u_fogEnabled != 0) ? (1.0 - exp(-length(FragPos) * 0.00035)) : 0.0;
    col = mix(col, FOG_COLOR, fog);

    FragColor = vec4(col, 1.0);
}
