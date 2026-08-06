/**
 * @file precip.frag
 * @brief Gota / copo: la FORMA dentro del quad. Ver precip.vert para el porqué del pase.
 */
#version 450 core

layout(location = 0) in  vec3  vRel;
layout(location = 1) in  vec2  vQuadUV;
layout(location = 2) in  float vFade;
layout(location = 0) out vec4  FragColor;

layout(std140, binding = 5) uniform PrecipParams {
    vec4 u_up;        // xyz = cénit · w = celda (m)
    vec4 u_vel;       // xyz = caída+viento · w = intensidad [0,1]
    vec4 u_camCell;   // xyz = cámara mod celda · w = tiempo (s)
    vec4 u_look;      // xyz = vista · w = 1 si NIEVE
    vec4 u_tint;      // rgb = color · a = alcance de fundido (m)
    // ⚠️ El bloque debe declararse IDÉNTICO en vértice y fragmento (lo exige GLSL entre etapas del
    //    mismo programa). Añadir un campo solo en uno da "definitions of uniform block do not match"
    //    al LINKAR — no al compilar — y el pase se queda mudo.
    vec4 u_misc;      // x = alto del viewport (px) · y = 1 si hay máscara cenital
    mat4 u_skySpace;  // máscara de exposición al cielo (la usa el vértice)
};

void main() {
    const bool snow = u_look.w > 0.5;

    float a;
    if (snow) {
        // COPO: disco blando. Se ve como una mota, no como una raya — la nieve no cae, flota.
        a = 1.0 - smoothstep(0.35, 1.0, length(vQuadUV));
    } else {
        // GOTA: cápsula alargada. Bordes suaves en el ancho (si no, es un palo con aliasing) y
        // la cabeza más opaca que la cola (la estela se apaga hacia arriba).
        float w = 1.0 - smoothstep(0.25, 1.0, abs(vQuadUV.x));
        float h = 1.0 - smoothstep(0.55, 1.0, abs(vQuadUV.y));
        a = w * h * (0.55 + 0.45 * smoothstep(1.0, -1.0, vQuadUV.y));
    }

    a *= vFade * u_vel.w * (snow ? 0.85 : 0.55);
    if (a < 0.004) discard;                 // el grueso del quad es transparente: sale barato
    FragColor = vec4(u_tint.rgb, a);
}
