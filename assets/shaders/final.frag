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

#include "lib/surface_shade.glsl"

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec2 TexCoord;

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 cameraPos;      float _pad0;
    vec3 sunDirection;   float _pad1;
    vec3 sunLightColor;  float ambientStrength;
    int  enableHDR;
    int  enableBloom;
    int  enableSSAO;
    int  enableIBL;
    int  enableShadows;
    int  _pad3a; int _pad3b; int _pad3c; // 3 ints sueltos (no array std140 → moon alineada a 208)
    vec3 moonDirection;  float moonIntensity;   // 2ª luz (luna)
    vec3 moonLightColor; float _pad4;
};

// Idéntico a simple.vert (misma etapa de programa) — ver la nota de allí sobre el error de LINKADO.
layout(std140, binding = 1) uniform PerObjectData {
    mat4 model;
    vec4 baseColorAndPlanetRadius; // rgb = base color
    vec4 planetCenterAndFlag;
    vec4 materialPBR;              // x=metallic y=roughness z=ao w=máscara de texturas
    vec4 materialEmission;         // rgb = emisión
};

// --- Texturas del MATERIAL del objeto (MaterialComponent.textures) ---------------------------
// El binding = la unidad que ata el RHI (Context::bindTexture), sin glUniform1i de sampler.
// Un sampler SIN atar lee negro, que es un color válido: por eso la presencia de cada slot viaja
// en la máscara de bits de `materialPBR.w` y no se deduce del propio muestreo.
layout(binding = 0) uniform sampler2D u_matAlbedo;
layout(binding = 1) uniform sampler2D u_matNormal;
layout(binding = 2) uniform sampler2D u_matMetallic;
layout(binding = 3) uniform sampler2D u_matRoughness;
layout(binding = 4) uniform sampler2D u_matAO;

const int TEX_ALBEDO = 1, TEX_NORMAL = 2, TEX_METALLIC = 4, TEX_ROUGHNESS = 8, TEX_AO = 16;
bool hasTex(int bit) { return (int(materialPBR.w) & bit) != 0; }


// --- Iluminación TOON (cel suave + rim), estilo Genshin/SAO ---
// Mood: cálido-aventura con crudeza mística → luz cálida, sombra FRÍA y algo desaturada, terminador
// SUAVE (no duro), rim-light frío que recorta la silueta, y specular en mancha nítida.
// metallic/roughness/ao llegan del MATERIAL. Esto NO es un BRDF PBR (el modelo es toon a
// propósito): la rugosidad abre o cierra el lóbulo especular, el metal tiñe ese brillo con el
// color base en vez de dejarlo blanco, y la oclusión oscurece el lado en sombra —que es donde
// se lee— sin tocar el lado iluminado.
vec3 toonShade(vec3 baseColor, vec3 N, vec3 L, vec3 V, float metallic, float roughness, float ao) {
    vec3  H   = normalize(L + V);
    float ndl = dot(N, L);

    // Terminador suave (2 tonos con el corte suavizado, no cel duro).
    float band = harukaToonBand(ndl);
    vec3  litCol    = baseColor * (0.80 + 0.25 * sunLightColor);      // lado iluminado (cálido)
    vec3  shadowCol = baseColor * harukaToonShadowTint(ambientStrength, sunLightColor) * ao;
    if (enableShadows == 0) shadowCol *= 1.06;
    if (enableSSAO    == 0) shadowCol *= 1.03;
    vec3  diffuse   = mix(shadowCol, litCol, band);

    // Rim / fresnel: borde luminoso frío (lo que hace que la silueta "salte" contra el fondo).
    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0);
    rim *= smoothstep(0.05, 0.55, band + 0.2);                       // sobre todo donde llega algo de luz
    vec3  rimCol = mix(vec3(0.55, 0.74, 0.96), sunLightColor, 0.30) * (enableBloom != 0 ? 1.0 : 0.7);

    // Specular toon. OJO con la amplitud: el escalón duro (0.85 entre 0.35 y 0.45) funciona en
    // superficies CURVAS —da una mancha pequeña— pero sobre una cara PLANA la normal es constante y
    // se enciende la cara ENTERA de blanco. La escena tiene mucha caja, así que: lóbulo más ancho,
    // tenue y sin escalón. La mancha nítida de pelo/agua/metal irá por material cuando haga falta.
    // La rugosidad mueve el exponente: liso (0) = mancha apretada y brillante, rugoso (1) = lóbulo
    // ancho y apagado. El METAL no tiene brillo blanco: lo tiñe con su color base.
    float shininess = mix(96.0, 6.0, clamp(roughness, 0.0, 1.0));
    float spec = pow(max(dot(N, H), 0.0), shininess);
    vec3  specTint   = mix(vec3(1.0), baseColor, clamp(metallic, 0.0, 1.0));
    float specGain   = mix(0.16, 0.06, clamp(roughness, 0.0, 1.0));
    vec3  highlights = specTint * specGain * smoothstep(0.25, 0.80, spec) * (enableBloom != 0 ? 1.0 : 0.55);

    // Luna (2ª luz, tenue, lado noche).
    float ndlMoon   = max(dot(N, normalize(moonDirection)), 0.0);
    vec3  moonlight = moonLightColor * moonIntensity * smoothstep(0.0, 0.6, ndlMoon) * baseColor * 0.6;

    return diffuse + rim * rimCol + highlights + moonlight;
}

void main() {
    vec3 baseColor = baseColorAndPlanetRadius.rgb;

    // ALBEDO del material. El PNG viene en sRGB y aquí se trabaja en lineal (el tonemap de abajo
    // devuelve el gamma), así que se linealiza al muestrear; sin eso las texturas salen lavadas.
    if (hasTex(TEX_ALBEDO)) {
        vec3 tex = texture(u_matAlbedo, TexCoord).rgb;
        baseColor *= pow(tex, vec3(2.2));
    }

    // Emissive stars: render at full brightness, no shading. Dejan de ser un disco plano:
    // suman la emisión del material (materialEmission), así un Sol con material EMISSIVE brilla.
    if (planetCenterAndFlag.w > 1.5) {
        vec3 star = baseColor + materialEmission.rgb;
        if (enableHDR != 0) {
            star = star / (star + vec3(1.0));
            star = pow(star, vec3(1.0 / 2.2));
        } else {
            star = clamp(star, 0.0, 1.0);
        }
        FragColor = vec4(star, 1.0);
        return;
    }

    vec3 N = normalize(Normal);
    if (hasTex(TEX_NORMAL))
        N = harukaApplyNormalMap(N, normalize(texture(u_matNormal, TexCoord).xyz * 2.0 - 1.0),
                             FragPos, TexCoord);
    vec3 L = normalize(sunDirection);
    vec3 V = normalize(cameraPos - FragPos);

    // Los escalares del material son el valor por defecto; la textura, cuando existe, MANDA
    // (es lo que espera quien acaba de hornear un grafo a PNG: ve su mapa, no el slider).
    float metallic  = materialPBR.x;
    float roughness = materialPBR.y;
    float ao        = materialPBR.z;
    if (hasTex(TEX_METALLIC))  metallic  = texture(u_matMetallic,  TexCoord).r;
    if (hasTex(TEX_ROUGHNESS)) roughness = texture(u_matRoughness, TexCoord).r;
    if (hasTex(TEX_AO))        ao        = texture(u_matAO,        TexCoord).r;

    vec3 color = toonShade(baseColor, N, L, V, metallic, roughness, ao) + materialEmission.rgb;
    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    FragColor = vec4(color, 1.0);
}
