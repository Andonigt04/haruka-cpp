#version 460 core
#extension GL_GOOGLE_include_directive : require
/**
 * @file grass.frag
 * @brief Sombreado de una brizna: dos caras, sin textura. Raiz oscura (oclusion de la mata), luz
 *        del Sol envuelta (la hierba transmite: media Lambert), ambiente del cielo y la perspectiva
 *        aerea de siempre para que a 60 m se funda con el suelo en vez de recortarse.
 */
#include "lib/aerial.glsl"

layout(std140, binding = 0) uniform GrassDraw {
    mat4  uMVP;
    vec4  uWind;
    vec4  uSun;
    vec4  uPressA;
    vec4  uPressE;
    vec4  uPressN;
    vec4  uShade;       // x = ambiente · y = anchura · z = altura · w = (libre)
    vec4  uAerial;      // la misma que el terreno (ver lib/aerial.glsl)
    vec4  uUp;          // xyz = arriba en la camara (para la elevacion de la perspectiva aerea)
};

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vColor;
layout(location = 2) in float vT;
layout(location = 3) in vec3 vFragPos;
layout(location = 0) out vec4 fragColor;

void main() {
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;
    // Media Lambert: una hoja iluminada por detras no es negra.
    const float ndl  = dot(n, uSun.xyz) * 0.5 + 0.5;
    const float sun  = ndl * ndl * uSun.w;
    const float root = mix(0.35, 1.0, smoothstep(0.0, 0.6, vT));     // la mata se sombrea a si misma
    vec3 col = vColor * (uShade.x + sun) * root;
    // Un brillo frio en la punta contra el Sol (transmision).
    col += vec3(0.25, 0.30, 0.10) * pow(max(dot(normalize(-vFragPos), uSun.xyz), 0.0), 8.0) * vT * uSun.w * 0.4;
    const float dist = length(vFragPos);
    col = harukaAerial(col, dist, dot(vFragPos / max(dist, 1e-3), uUp.xyz), uAerial);
    fragColor = vec4(col, 1.0);
}
