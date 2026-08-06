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

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec3 Color;
layout(location = 3) in vec2 TexCoord;

// --- Texturas del MATERIAL del PROTOTIPO (compartidas por todas sus instancias) -------------
layout(binding = 0) uniform sampler2D u_matAlbedo;
layout(binding = 1) uniform sampler2D u_matNormal;
layout(binding = 2) uniform sampler2D u_matMetallic;
layout(binding = 3) uniform sampler2D u_matRoughness;
layout(binding = 4) uniform sampler2D u_matAO;

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
    int  _pad3a; int _pad3b; int _pad3c;
    vec3 moonDirection;  float moonIntensity;
    vec3 moonLightColor; float _pad4;
};

// Parámetros del pase de props + escalares del material del PROTOTIPO (compartidos).
layout(std140, binding = 6) uniform PropParams {
    vec3  u_wind;   float u_time;
    vec4  u_matPBR;     // x=metallic y=roughness z=ao w=máscara de texturas (bits)
};

const int TEX_ALBEDO = 1, TEX_NORMAL = 2, TEX_METALLIC = 4, TEX_ROUGHNESS = 8, TEX_AO = 16;
bool hasTex(int bit) { return (int(u_matPBR.w) & bit) != 0; }

// TBN por DERIVADAS de pantalla, no por tangentes de vértice (las mallas procedurales no las
// generan). Idéntico a final.frag.
vec3 applyNormalMap(vec3 N, vec3 texN) {
    vec3 dp1 = dFdx(FragPos), dp2 = dFdy(FragPos);
    vec2 du1 = dFdx(TexCoord), du2 = dFdy(TexCoord);
    float det = du1.x * du2.y - du2.x * du1.y;
    if (abs(det) < 1e-12) return N;
    vec3 T = normalize((dp1 * du2.y - dp2 * du1.y) / det);
    T = normalize(T - N * dot(N, T));
    vec3 B = cross(N, T);
    return normalize(mat3(T, B, N) * texN);
}

vec3 toonShade(vec3 baseColor, vec3 N, vec3 L, vec3 V, float metallic, float roughness, float ao) {
    vec3  H   = normalize(L + V);
    float ndl = dot(N, L);

    float band = smoothstep(-0.03, 0.22, ndl);
    vec3  litCol    = baseColor * (0.80 + 0.25 * sunLightColor);
    vec3  shadowCol = baseColor * vec3(0.40, 0.46, 0.60) * ao;
    if (enableShadows == 0) shadowCol *= 1.06;
    if (enableSSAO    == 0) shadowCol *= 1.03;
    vec3  diffuse   = mix(shadowCol, litCol, band);

    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0);
    rim *= smoothstep(0.05, 0.55, band + 0.2);
    vec3  rimCol = mix(vec3(0.55, 0.74, 0.96), sunLightColor, 0.30) * (enableBloom != 0 ? 1.0 : 0.7);

    float shininess = mix(96.0, 6.0, clamp(roughness, 0.0, 1.0));
    float spec = pow(max(dot(N, H), 0.0), shininess);
    vec3  specTint   = mix(vec3(1.0), baseColor, clamp(metallic, 0.0, 1.0));
    float specGain   = mix(0.16, 0.06, clamp(roughness, 0.0, 1.0));
    vec3  highlights = specTint * specGain * smoothstep(0.25, 0.80, spec) * (enableBloom != 0 ? 1.0 : 0.55);

    float ndlMoon   = max(dot(N, normalize(moonDirection)), 0.0);
    vec3  moonlight = moonLightColor * moonIntensity * smoothstep(0.0, 0.6, ndlMoon) * baseColor * 0.6;

    return diffuse + rim * rimCol + highlights + moonlight;
}

void main() {
    vec3 baseColor = Color;   // color por vértice × tinte por instancia (variedad sin geometría)

    // ALBEDO del material del prototipo (horneado con el node material graph). Se linealiza al
    // muestrear (el PNG viene en sRGB); sin eso las texturas salen lavadas.
    if (hasTex(TEX_ALBEDO)) {
        vec3 tex = texture(u_matAlbedo, TexCoord).rgb;
        baseColor *= pow(tex, vec3(2.2));
    }

    vec3 N = normalize(Normal);
    if (hasTex(TEX_NORMAL))
        N = applyNormalMap(N, normalize(texture(u_matNormal, TexCoord).xyz * 2.0 - 1.0));
    vec3 L = normalize(sunDirection);
    vec3 V = normalize(cameraPos - FragPos);

    float metallic  = u_matPBR.x;
    float roughness = u_matPBR.y;
    float ao        = u_matPBR.z;
    if (hasTex(TEX_METALLIC))  metallic  = texture(u_matMetallic,  TexCoord).r;
    if (hasTex(TEX_ROUGHNESS)) roughness = texture(u_matRoughness, TexCoord).r;
    if (hasTex(TEX_AO))        ao        = texture(u_matAO,        TexCoord).r;

    vec3 color = toonShade(baseColor, N, L, V, metallic, roughness, ao);
    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    FragColor = vec4(color, 1.0);
}
