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
#include "lib/sky_light.glsl"   // la luz del frame por fisica (binding 31)

/// Lo que cambia de un material a otro. Todo en [0,1] salvo indicación.
struct HarukaToonParams {
    float facet;   ///< 1 = normal de cara (low-poly), 0 = normal interpolada.
    float rim;     ///< Ganancia del rim de silueta (obra: 0,35 — un muro son cientos de piezas).
    float spec;    ///< Ganancia del especular (obra: 0 — sillar y ladrillo son mates).
    /// CONTRALUZ: la luz del Sol que atraviesa los bordes FINOS de algo translúcido cuando el Sol
    /// está detrás (el borde plateado de una nube, una hoja al trasluz). 0 = opaco.
    float translucency;
    /// DIFUSIÓN: la parte del Sol que llega a la cara a la SOMBRA después de atravesar el cuerpo
    /// dispersándose (una nube de 1 km deja pasar ~0,1-0,3 de la luz: la panza no es negra). 0 = opaco.
    float diffusion;
};

HarukaToonParams harukaToonDefaults() { return HarukaToonParams(1.0, 1.0, 1.0, 0.0, 0.0); }

/**
 * @brief Sombreado toon completo: difuso facetado + rim de silueta + especular, con la luz del frame.
 *
 * Esto NO es un BRDF PBR (el modelo es toon a propósito): la banda decide DÓNDE hay luz y sombra; la
 * CANTIDAD y el COLOR de esa luz son los del frame (`lib/sky_light.glsl`): el Sol tras el aire, la
 * Luna, el cielo y el suelo. ⚠️ Aquí ponía `baseColor * (0.80 + 0.25 * sunLightColor)` y un tinte de
 * sombra elegido: un objeto brillaba casi igual a mediodía que a medianoche.
 *
 * Devuelve color de PANTALLA (ver `harukaLit`): `baseColor` es un color de pantalla, como en todo el
 * motor.
 *
 * @param N        normal de ILUMINACIÓN (de `harukaLightingNormal`, con normal map si lo hay).
 * @param smoothN  normal interpolada, para el rim y el ambiente.
 */
vec3 harukaToonShade(vec3 baseColor, vec3 N, vec3 smoothN, vec3 L, vec3 V,
                     float metallic, float roughness, float ao, HarukaToonParams p) {
    float ndl  = dot(N, L);
    float band = harukaFacetBand(harukaToonBand(ndl), ndl, p.facet);

    const vec3 up  = uSkyUp.xyz;
    const vec3 amb = harukaSkyAmbient(smoothN, up) * ao;
    vec3 E = uSkySunGround.rgb * band
           + uSkyMoonGround.rgb * smoothstep(0.0, 0.6, dot(N, uSkyMoonDir.xyz))
           + amb;
    // CONTRALUZ: lo que pasa del Sol por un borde fino hacia el ojo. Es dispersión hacia delante (el
    // lóbulo de las gotas, `dot(-V, L)` cerca de 1) por un camino corto (la silueta: N ⟂ V). Se suma
    // a la LUZ, no al color: con el Sol de frente o un objeto grueso no aporta nada.
    if (p.translucency > 0.0) {
        const float fwd  = pow(max(dot(-V, L), 0.0), 4.0);
        const float thin = pow(1.0 - max(dot(smoothN, V), 0.0), 2.0);
        E += uSkySunGround.rgb * (p.translucency * fwd * thin);
    }
    // DIFUSIÓN: lo que no recibe el Sol directo (1 − banda) recibe la fracción que atraviesa el cuerpo.
    if (p.diffusion > 0.0) E += uSkySunGround.rgb * (p.diffusion * (1.0 - band));
    vec3 color = harukaLit(baseColor, E);

    // El rim es la luz del CIELO rozando la silueta (estilo del material, con la luz que hay).
    if (p.rim > 0.0) {
        float rim = pow(1.0 - max(dot(smoothN, V), 0.0), 3.0) * p.rim;
        rim *= smoothstep(0.05, 0.55, band + 0.2);
        color += rim * 0.5 * harukaLit(vec3(1.0), uSkyAmbientUp.rgb + 0.3 * uSkySunGround.rgb);
    }

    // Lóbulo ANCHO y sin escalón: sobre una cara plana la normal es constante y un escalón duro
    // encendería la cara entera de blanco. Su brillo es el del Sol que llega.
    if (p.spec > 0.0) {
        float r     = clamp(roughness, 0.0, 1.0);
        float spec  = pow(max(dot(N, normalize(L + V)), 0.0), mix(96.0, 6.0, r));
        vec3  tint  = mix(vec3(1.0), baseColor, clamp(metallic, 0.0, 1.0));
        color += tint * (mix(0.16, 0.06, r) * p.spec) * smoothstep(0.25, 0.80, spec)
               * harukaLit(vec3(1.0), uSkySunGround.rgb * band);
    }
    // El rim y el especular se SUMAN después del hombro de `harukaLit` y volvían a pasar de 1: un
    // halo blanco con bloom alrededor de lo claro (el hacha). El total pasa por el mismo hombro.
    return harukaShoulder(color);
}

#endif // HARUKA_TOON_SHADE_GLSL
