/**
 * @file final.frag
 * @brief Full-featured forward fragment shader (Final visualization mode).
 *
 * Reads per-frame and per-object data from UBOs instead of explicit-location
 * uniforms, eliminating the location conflict with the vertex shader.
 *
 * In:  Normal (loc 0), FragPos (loc 1), TexCoord (loc 2)
 * Out: FragColor (loc 0)
 * UBOs: PerFrameData (binding 0), PerObjectData (binding 1)
 * Texturas del MATERIAL: bindings 0..4 (albedo/normal/metallic/roughness/ao)
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

#include "lib/toon_shade.glsl"      // + frame_ubo + surface_shade
#include "lib/material_tex.glsl"   // MaterialComponent.textures, bindings 0..4

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec2 TexCoord;

// Idéntico a simple.vert (misma etapa de programa) — ver la nota de allí sobre el error de LINKADO.
layout(std140, binding = 1) uniform PerObjectData {
    mat4 model;
    vec4 baseColorAndPlanetRadius; // rgb = base color
    vec4 planetCenterAndFlag;
    vec4 materialPBR;              // x=metallic y=roughness z=ao w=máscara de texturas
    vec4 materialEmission;         // rgb = emisión
};

void main() {
    vec3 baseColor = harukaMatAlbedo(materialPBR, baseColorAndPlanetRadius.rgb, TexCoord);

    // Emissive stars: render at full brightness, no shading. Dejan de ser un disco plano:
    // suman la emisión del material (materialEmission), así un Sol con material EMISSIVE brilla.
    if (planetCenterAndFlag.w > 1.5) {
        FragColor = vec4(harukaOutputTonemap(baseColor + materialEmission.rgb), 1.0);
        return;
    }

    vec3 V = normalize(cameraPos - FragPos);
    vec3 L = normalize(sunDirection);

    HarukaToonParams tp = harukaToonDefaults();
    vec3 smoothN = normalize(Normal);
    vec3 N = harukaMatNormal(materialPBR, harukaLightingNormal(smoothN, FragPos, V, tp.facet),
                             FragPos, TexCoord);
    vec3 mra = harukaMatMRA(materialPBR, TexCoord);

    vec3 color = harukaToonShade(baseColor, N, smoothN, L, V, mra.x, mra.y, mra.z, tp)
               + materialEmission.rgb;
    FragColor = vec4(harukaOutputTonemap(color), 1.0);
}
