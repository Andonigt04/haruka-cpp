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
#include "lib/sky_light.glsl"      // Sol y Luna por radiometria (binding 31)

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

    // ── CUERPOS DEL CIELO: su luz sale de la RADIOMETRIA, no de un color de objeto ──────────────
    // Se SUMAN al cielo ya pintado (PSO aditivo): lo que hay delante es el aire, y su luz es la del
    // cielo de esa direccion. `FragPos` es relativo al ojo.
    if (planetCenterAndFlag.w > 2.5) {
        const vec3 N = normalize(Normal);
        vec3 R;
        if (planetCenterAndFlag.w > 3.5) {
            // EL SOL: un disco de radiancia `E/Ω` (π·L = E/sin²ρ), visto a traves del aire (el
            // `uSkySunGround` ya lleva la transmitancia: al ponerse sale rojo). Oscurecimiento al
            // borde de la fotosfera (u = 0,6), normalizado para que el disco entero sume E.
            const float sr = sin(uSkySunDir.w);
            const float mu = max(dot(N, normalize(-FragPos)), 0.0);
            const float u  = 0.6;
            R = uSkySunGround.rgb / max(sr * sr, 1.0e-12) * (1.0 - u * (1.0 - mu)) / (1.0 - u / 3.0);
        } else {
            // LA LUNA: roca de albedo 0,12 (gemelo de `kMoonAlbedo`) bajo el Sol, sin aire que la
            // ilumine por la cara oscura. La fase sale de la geometria.
            R = 0.12 * uSkySunAtMoon.rgb * max(dot(N, uSkySunDir.xyz), 0.0);
        }
        FragColor = vec4(harukaDisplay(R), 1.0);
        return;
    }

    // Emissive stars: render at full brightness, no shading. Dejan de ser un disco plano:
    // suman la emisión del material (materialEmission), así un Sol con material EMISSIVE brilla.
    if (planetCenterAndFlag.w > 1.5) {
        FragColor = vec4(harukaOutputTonemap(baseColor + materialEmission.rgb), 1.0);
        return;
    }

    // ⚠️ EL OJO ESTÁ EN EL ORIGEN: la escena se dibuja CÁMARA-RELATIVA (`getTransformMatrix(obj,
    // camera->position)`) y `cameraPos` es la posición ABSOLUTA (~1,5e8 m). Con `cameraPos - FragPos`
    // la vista apuntaba siempre al origen del sistema y el especular encendía caras ENTERAS: el
    // hacha en la mano salía blanca con el Sol de lado (mismo fallo que ya tuvo prop_inst.frag).
    vec3 V = normalize(-FragPos);
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
