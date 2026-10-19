/**
 * @file prop_inst.frag
 * @brief Fragment toon de TODA malla instanciada: props del mundo (`prop_inst.vert`), la capa
 *        masiva (`prop_burst.vert`) y mallas de escena repetidas como la obra (`mesh_inst.vert`).
 *        El material (albedo/normal/metallic/roughness/ao) se muestrea por UV y se TIÑE por
 *        instancia. Misma iluminación que la escena para que se vea igual que terreno y objetos.
 *
 * Lo que cambia entre un árbol y un muro es MATERIAL, y llega por el UBO (`u_flags.yz`):
 *  - rim (obra: 0,35): el rim recorta la SILUETA de un objeto, y un muro son cientos de piezas —
 *    cada una con su propio borde brillante era ruido, no silueta.
 *  - especular (obra: 0): todas las piezas de un muro comparten la MISMA normal y el muro entero
 *    entraba y salía del lóbulo a la vez. Sillar, ladrillo y revoco son mates.
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
layout(location = 4) in vec3 AerialT;   // el aire, interpolado del vértice (lib/aerial.glsl)
layout(location = 5) in vec3 AerialS;

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

    HarukaToonParams tp = HarukaToonParams(u_material.x, u_flags.y, u_flags.z, u_material.y, u_material.z);   // facet · rim · especular · contraluz · difusión
    vec3 smoothN = normalize(Normal);
    vec3 N = harukaMatNormal(u_matPBR, harukaLightingNormal(smoothN, FragPos, V, tp.facet),
                             FragPos, TexCoord);
    vec3 mra = harukaMatMRA(u_matPBR, TexCoord);

    vec3 color = harukaOutputTonemap(harukaToonShade(baseColor, N, smoothN, L, V,
                                                     mra.x, mra.y, mra.z, tp));

    // El mismo aire que el terreno y las nubes, por física, integrado POR VÉRTICE (ver `harukaAerialTS`).
    color = harukaAerialApply(color, AerialT, AerialS);
    FragColor = vec4(color, 1.0);
}
