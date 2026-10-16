/**
 * @file toon_shade.glsl
 * @brief LA iluminación de malla del motor: low-poly facetado sin costuras + cel suave.
 *
 * Sustituye las tres copias de `toonShade` (final.frag, prop_inst.frag, construction_inst.frag).
 * Lo que de verdad diferenciaba a esas copias (rim al 35 % en obra, sin especular en obra) NO era
 * divergencia sino MATERIAL, y por eso viaja en `HarukaToonParams` en vez de en tres funciones.
 *
 * El estilo low-poly (normal de cara, banda ancha, rim con la suave) vive en lib/lowpoly.glsl,
 * compartido con el terreno. Aquí solo está lo propio de las mallas.
 *
 * Incluye: lib/frame_ubo.glsl (sol, luna, ambiente, toggles), surface_shade.glsl y lowpoly.glsl.
 */
#ifndef HARUKA_TOON_SHADE_GLSL
#define HARUKA_TOON_SHADE_GLSL

#include "lib/frame_ubo.glsl"
#include "lib/surface_shade.glsl"
#include "lib/lowpoly.glsl"

/// Lo que cambia de un material a otro. Todo en [0,1] salvo indicación.
struct HarukaToonParams {
    float facet;   ///< 1 = normal de cara (low-poly), 0 = normal interpolada.
    float rim;     ///< Ganancia del rim de silueta (obra: 0,35 — un muro son cientos de piezas).
    float spec;    ///< Ganancia del especular (obra: 0 — sillar y ladrillo son mates).
};

HarukaToonParams harukaToonDefaults() { return HarukaToonParams(1.0, 1.0, 1.0); }

/**
 * @brief Sombreado toon completo: difuso facetado + rim de silueta + especular + luna.
 *
 * Esto NO es un BRDF PBR (el modelo es toon a propósito): la rugosidad abre o cierra el lóbulo
 * especular, el metal tiñe ese brillo con el color base y la oclusión oscurece solo el lado en
 * sombra, que es donde se lee.
 *
 * @param N        normal de ILUMINACIÓN (de `harukaLightingNormal`, con normal map si lo hay).
 * @param smoothN  normal interpolada, solo para el rim.
 */
vec3 harukaToonShade(vec3 baseColor, vec3 N, vec3 smoothN, vec3 L, vec3 V,
                     float metallic, float roughness, float ao, HarukaToonParams p) {
    float ndl  = dot(N, L);
    float band = harukaFacetBand(harukaToonBand(ndl), ndl, p.facet);

    vec3 litCol    = baseColor * (0.80 + 0.25 * sunLightColor);
    vec3 shadowCol = baseColor * harukaToonShadowTint(ambientStrength, sunLightColor) * ao;
    if (enableShadows == 0) shadowCol *= 1.06;
    if (enableSSAO    == 0) shadowCol *= 1.03;
    vec3 color = mix(shadowCol, litCol, band);

    if (p.rim > 0.0) {
        float rim = pow(1.0 - max(dot(smoothN, V), 0.0), 3.0) * p.rim;
        rim *= smoothstep(0.05, 0.55, band + 0.2);
        color += rim * mix(vec3(0.55, 0.74, 0.96), sunLightColor, 0.30) * (enableBloom != 0 ? 1.0 : 0.7);
    }

    // Lóbulo ANCHO y sin escalón: sobre una cara plana la normal es constante y un escalón duro
    // encendería la cara entera de blanco.
    if (p.spec > 0.0) {
        float r     = clamp(roughness, 0.0, 1.0);
        float spec  = pow(max(dot(N, normalize(L + V)), 0.0), mix(96.0, 6.0, r));
        vec3  tint  = mix(vec3(1.0), baseColor, clamp(metallic, 0.0, 1.0));
        color += tint * (mix(0.16, 0.06, r) * p.spec) * smoothstep(0.25, 0.80, spec)
               * (enableBloom != 0 ? 1.0 : 0.55);
    }

    float ndlMoon = max(dot(N, normalize(moonDirection)), 0.0);
    color += moonLightColor * moonIntensity * smoothstep(0.0, 0.6, ndlMoon) * baseColor * 0.6;
    return color;
}

#endif // HARUKA_TOON_SHADE_GLSL
