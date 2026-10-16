/**
 * @file construction_inst.frag
 * @brief Fragment lit para piezas de construcción instanciadas. Fork de final.frag: el color base
 *        viene del varying InstanceColor (por instancia) MODULADO por las texturas del MATERIAL del
 *        grupo (albedo/normal/metallic/roughness/ao, los mismos slots que el pase de escena). Misma
 *        iluminación (ambiente + sol + luna, con los toggles), para que las piezas instanciadas se
 *        vean IGUAL que las de escena. Sin rama de estrella emisiva (una pieza nunca lo es).
 *
 * In:  Normal (0), FragPos (1), InstanceColor (2), TexCoord (3)
 * UBOs: PerFrameData (binding 0) + ConstParams (binding 6, escalares + máscara del material)
 * Texturas del material del GRUPO: bindings 0..4 (albedo/normal/metallic/roughness/ao)
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

#include "lib/toon_shade.glsl"      // + frame_ubo + surface_shade
#include "lib/material_tex.glsl"

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec4 InstanceColor;
layout(location = 3) in vec2 TexCoord;

// Parámetros del material del GRUPO (binding 6): escalares + máscara de texturas (bits).
layout(std140, binding = 6) uniform ConstParams {
    vec4 u_matPBR;      // x=metallic y=roughness z=ao w=máscara de texturas
};

void main() {
    vec3 baseColor = harukaMatAlbedo(u_matPBR, InstanceColor.rgb, TexCoord);

    // ⚠️ EL OJO ESTÁ EN EL ORIGEN, no en `cameraPos`: este pase dibuja CÁMARA-RELATIVO (ver
    // prop_inst.frag para el porqué).
    vec3 V = normalize(-FragPos);
    vec3 L = normalize(sunDirection);

    // Material de OBRA:
    //  - rim al 35 %: el rim recorta la SILUETA de un objeto, y un muro son cientos de piezas —
    //    cada una con su propio borde brillante era ruido, no silueta.
    //  - sin especular: todas las piezas de un muro comparten la MISMA normal, así que el muro
    //    entero entraba y salía del lóbulo a la vez — una fachada encendida y las demás mates.
    //    Madera, sillar, ladrillo y revoco son mates. Si algún día hay obra pulida (mármol, metal,
    //    vidrio), el brillo debe venir del MATERIAL, no de aquí.
    HarukaToonParams tp = HarukaToonParams(1.0, 0.35, 0.0);
    vec3 smoothN = normalize(Normal);
    vec3 N = harukaMatNormal(u_matPBR, harukaLightingNormal(smoothN, FragPos, V, tp.facet),
                             FragPos, TexCoord);
    vec3 mra = harukaMatMRA(u_matPBR, TexCoord);

    FragColor = vec4(harukaOutputTonemap(harukaToonShade(baseColor, N, smoothN, L, V,
                                                         mra.x, mra.y, mra.z, tp)), 1.0);
}
