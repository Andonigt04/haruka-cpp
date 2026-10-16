/**
 * @file material_tex.glsl
 * @brief Los 5 slots de textura del MATERIAL (bindings 0..4) y su muestreo condicionado por máscara.
 *
 * Estaba copiado en final.frag, prop_inst.frag y construction_inst.frag. La presencia de cada slot
 * viaja en bits (`pbr.w`) y no se deduce del muestreo: un sampler sin atar lee negro, que es un
 * color válido. Los escalares de `pbr` son el valor por defecto; la textura, si existe, MANDA.
 *
 * Requiere: lib/surface_shade.glsl (harukaApplyNormalMap).
 */
#ifndef HARUKA_MATERIAL_TEX_GLSL
#define HARUKA_MATERIAL_TEX_GLSL

layout(binding = 0) uniform sampler2D u_matAlbedo;
layout(binding = 1) uniform sampler2D u_matNormal;
layout(binding = 2) uniform sampler2D u_matMetallic;
layout(binding = 3) uniform sampler2D u_matRoughness;
layout(binding = 4) uniform sampler2D u_matAO;

const int TEX_ALBEDO = 1, TEX_NORMAL = 2, TEX_METALLIC = 4, TEX_ROUGHNESS = 8, TEX_AO = 16;
bool harukaHasTex(vec4 pbr, int bit) { return (int(pbr.w) & bit) != 0; }

/// Color base × albedo. El PNG viene en sRGB y se trabaja en lineal: sin linealizar sale lavado.
vec3 harukaMatAlbedo(vec4 pbr, vec3 baseColor, vec2 uv) {
    if (harukaHasTex(pbr, TEX_ALBEDO)) baseColor *= pow(texture(u_matAlbedo, uv).rgb, vec3(2.2));
    return baseColor;
}

/// Normal map sobre `N` (si el material lo trae). `pos` y `uv` alimentan el TBN por derivadas.
vec3 harukaMatNormal(vec4 pbr, vec3 N, vec3 pos, vec2 uv) {
    if (!harukaHasTex(pbr, TEX_NORMAL)) return N;
    return harukaApplyNormalMap(N, normalize(texture(u_matNormal, uv).xyz * 2.0 - 1.0), pos, uv);
}

/// (metallic, roughness, ao): escalar del material o textura si existe.
vec3 harukaMatMRA(vec4 pbr, vec2 uv) {
    vec3 mra = pbr.xyz;
    if (harukaHasTex(pbr, TEX_METALLIC))  mra.x = texture(u_matMetallic,  uv).r;
    if (harukaHasTex(pbr, TEX_ROUGHNESS)) mra.y = texture(u_matRoughness, uv).r;
    if (harukaHasTex(pbr, TEX_AO))        mra.z = texture(u_matAO,        uv).r;
    return mra;
}

#endif // HARUKA_MATERIAL_TEX_GLSL
