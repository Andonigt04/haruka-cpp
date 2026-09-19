/**
 * @file vox.frag
 * @brief Sombreado de las paredes del campo: roca del terreno en triplanar + luz de la boca.
 *
 * ⚠️ La normal del campo apunta hacia la ROCA (es el gradiente de "cuánta roca hay"); la cara que
 * el jugador ve desde dentro de la cueva mira hacia el AIRE. Se invierte aquí, y no en la malla,
 * para que la normal de la malla siga siendo la misma que empuja en la colisión.
 *
 * La textura es la MISMA capa de roca del array de materiales del terreno (`rockLayer`), en
 * triplanar por posición de mundo: `uTexOrigin` es el origen del chunk reducido módulo el tile en
 * doubles por la CPU, así que la posición que se muestrea es exacta y NO nada con la cámara.
 */
#version 450 core
// (No se incluye `lib/terrain_shade.glsl`: arrastra `harukaSelectMaterial` y su UBO de materiales.
//  El triplanar es una función de diez líneas; copiada, con la misma receta.)

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vPosRel;
layout(location = 2) in vec3 vPosLocal;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 5) uniform VoxParams {
    mat4 uRotVP;
    vec4 uOriginRel;
    vec4 uLightDir;
    vec4 uColor;        // rgb = roca (sin textura) · a = sin usar
    vec4 uUp;           // xyz = la vertical LOCAL (radial), no la Y del mundo
    vec4 uTexOrigin;    // xyz = origen del chunk mod tile (m) · w = capa de roca (<0 = sin textura)
    vec4 uTexInfo;      // x = metros por tile
};
layout(binding = 1) uniform sampler2DArray uAlbedo;

// Triplanar sobre una CAPA del array: los tres planos pesados por la normal elevada a 4. Gemela de
// `harukaTriplanarArr` (lib/terrain_shade.glsl).
vec3 voxTriplanar(sampler2DArray tex, float layer, vec3 wp, vec3 n, float s) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= max(bw.x + bw.y + bw.z, 1e-6);
    return texture(tex, vec3(wp.zy * s, layer)).rgb * bw.x
         + texture(tex, vec3(wp.xz * s, layer)).rgb * bw.y
         + texture(tex, vec3(wp.xy * s, layer)).rgb * bw.z;
}

void main() {
    const vec3 n   = -normalize(vNormal);
    const vec3 l   = normalize(uLightDir.xyz);
    const vec3 up  = normalize(uUp.xyz);
    vec3 base = uColor.rgb;
    if (uTexOrigin.w >= 0.0) {
        const vec3 wp = uTexOrigin.xyz + vPosLocal;
        base = voxTriplanar(uAlbedo, uTexOrigin.w, wp, n, 1.0 / max(uTexInfo.x, 0.01));
    }
    // Dentro de una cueva casi no llega sol directo. Lo que hay es luz de la boca, que viene de
    // ARRIBA: hemisferio (suelo claro, techo oscuro) sobre un ambiente de base, y el sol atenuado.
    const float hemi = 0.5 + 0.5 * dot(n, up);
    const float nl   = max(dot(n, l), 0.0);
    const float amb  = 0.16 + 0.34 * hemi;
    fragColor = vec4(base * (amb + 0.45 * nl), 1.0);
}
