/**
 * @file prop_inst.frag
 * @brief Fragment toon para props del mundo INSTANCIADOS. Fork de final.frag: el material del
 *        prototipo (albedo/normal/metallic/roughness/ao, horneado con el node material graph del
 *        IDE) se muestrea por UV y se TIÑE por instancia (iColor). Misma iluminación que la
 *        escena para que los props instanciados se vean igual que el terreno y los objetos.
 *
 * In:  Normal (0), FragPos (1), Color (2), TexCoord (3)
 * UBOs: PerFrameData (binding 0) + PropParams (binding 6, con los escalares del material)
 * Texturas del MATERIAL del prototipo: bindings 0..4 (albedo/normal/metallic/roughness/ao)
 */
#version 450 core
#extension GL_GOOGLE_include_directive : require

#include "lib/toon_shade.glsl"      // + frame_ubo + surface_shade
#include "lib/material_tex.glsl"
#include "lib/aerial.glsl"

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec3 Color;
layout(location = 3) in vec2 TexCoord;

#include "lib/prop_params.glsl"   // PropParams (binding 6): material, origen, aire

void main() {
    // Color por vértice × tinte por instancia (variedad sin geometría) × albedo del prototipo.
    vec3 baseColor = harukaMatAlbedo(u_matPBR, Color, TexCoord);

    // ⚠️ EL OJO ESTÁ EN EL ORIGEN, no en `cameraPos`. Este pase dibuja CÁMARA-RELATIVO (`FragPos`
    // es `camRel`), pero `cameraPos` del UBO es la posición ABSOLUTA — ~1,5e8 en un sistema solar.
    // Restar una de otra daba un vector de vista casi constante apuntando al origen del sistema, o
    // sea un especular calculado contra una dirección que no existe: con normales por cara, eso se
    // ve como un parcheado de caras brillantes y negras, aspecto de cromo.
    vec3 V = normalize(-FragPos);
    vec3 L = normalize(sunDirection);

    HarukaToonParams tp = harukaToonDefaults();
    vec3 smoothN = normalize(Normal);
    vec3 N = harukaMatNormal(u_matPBR, harukaLightingNormal(smoothN, FragPos, V, tp.facet),
                             FragPos, TexCoord);
    vec3 mra = harukaMatMRA(u_matPBR, TexCoord);

    vec3 color = harukaOutputTonemap(harukaToonShade(baseColor, N, smoothN, L, V,
                                                     mra.x, mra.y, mra.z, tp));

    // El mismo aire que el terreno y las nubes. Un prop nunca está a más de 6 km, así que la
    // elevación de la vista se toma 0 (color de horizonte): la diferencia es inapreciable.
    color = harukaAerial(color, length(FragPos), 0.0, u_aerial);
    FragColor = vec4(color, 1.0);
}
