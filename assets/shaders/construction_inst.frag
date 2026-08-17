/**
 * @file construction_inst.frag
 * @brief Fragment lit para piezas de construcción instanciadas. Fork de final.frag: el color base
 *        viene del varying InstanceColor (por instancia) MODULADO por las texturas del MATERIAL del
 *        grupo (albedo/normal/metallic/roughness/ao, los mismos slots que el pase de escena). Misma
 *        iluminación (ambiente + sol + luna, con los toggles), para que las piezas instanciadas se
 *        vean IGUAL que las de escena. Sin rama de estrella emisiva (una pieza nunca lo es).
 *
 * In:  Normal (0), FragPos (1), InstanceColor (2), TexCoord (3)
 * UBOs: PerFrameData (binding 0) + ConstParams (binding 6, escalares + máscara del material)
 * Texturas del material del GRUPO: bindings 0..4 (albedo/normal/metallic/roughness/ao)
 */
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec4 InstanceColor;
layout(location = 3) in vec2 TexCoord;

// --- Texturas del MATERIAL del grupo (compartidas por todas sus piezas) ------------------------
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

// Parámetros del material del GRUPO (binding 6): escalares + máscara de texturas (bits).
layout(std140, binding = 6) uniform ConstParams {
    vec4 u_matPBR;      // x=metallic y=roughness z=ao w=máscara de texturas
};

const int TEX_ALBEDO = 1, TEX_NORMAL = 2, TEX_METALLIC = 4, TEX_ROUGHNESS = 8, TEX_AO = 16;
bool hasTex(int bit) { return (int(u_matPBR.w) & bit) != 0; }

// TBN por DERIVADAS de pantalla, no por tangentes de vértice. Idéntico a final.frag.
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

// Cel suave + rim — MISMA iluminación que final.frag (mood cálido-aventura + crudeza mística). El
// color base viene por INSTANCIA (InstanceColor × albedo del material) y el AO modula las sombras.
void main() {
    vec3 baseColor = InstanceColor.rgb;
    // ALBEDO del material del grupo (los slots son los del editor de node graph). Se linealiza al
    // muestrear (el PNG viene en sRGB); sin eso las texturas salen lavadas.
    if (hasTex(TEX_ALBEDO)) {
        vec3 tex = texture(u_matAlbedo, TexCoord).rgb;
        baseColor *= pow(tex, vec3(2.2));
    }

    vec3 N = normalize(Normal);
    if (hasTex(TEX_NORMAL))
        N = applyNormalMap(N, normalize(texture(u_matNormal, TexCoord).xyz * 2.0 - 1.0));
    vec3 L = normalize(sunDirection);
    // ⚠️ EL OJO ESTÁ EN EL ORIGEN, no en `cameraPos`. Este pase dibuja CÁMARA-RELATIVO (`FragPos`
    // es `camRel`), pero `cameraPos` del UBO es la posición ABSOLUTA — ~1,5e8 en un sistema solar.
    // Restar una de otra daba un vector de vista casi constante apuntando al origen del sistema, o
    // sea un especular calculado contra una dirección que no existe: con normales por cara, eso se
    // ve como un parcheado de caras brillantes y negras, aspecto de cromo.
    vec3 V = normalize(-FragPos);

    float ao = u_matPBR.z;
    if (hasTex(TEX_AO)) ao = texture(u_matAO, TexCoord).r;

    float ndl  = dot(N, L);
    float band = smoothstep(-0.03, 0.22, ndl);
    vec3  litCol    = baseColor * (0.80 + 0.25 * sunLightColor);
    vec3  shadowCol = baseColor * vec3(0.40, 0.46, 0.60) * ao;
    if (enableShadows == 0) shadowCol *= 1.06;
    if (enableSSAO    == 0) shadowCol *= 1.03;
    vec3  diffuse   = mix(shadowCol, litCol, band);

    // El rim recorta la SILUETA de un objeto. Un muro son cientos de piezas y cada una traía su propio
    // borde brillante → ruido, no silueta. Aquí va discreto.
    float rim = pow(1.0 - max(dot(N, V), 0.0), 3.0) * 0.35;
    rim *= smoothstep(0.05, 0.55, band + 0.2);
    vec3  rimCol = mix(vec3(0.55, 0.74, 0.96), sunLightColor, 0.30) * (enableBloom != 0 ? 1.0 : 0.7);

    // Materiales de OBRA (madera, sillar, ladrillo, revoco) = RUGOSOS: apenas tienen lóbulo especular.
    // El realce toon fuerte (0.85 con un escalón de 0.35→0.45) se diseñó para superficies CURVAS, donde
    // sale un punto de luz pequeño. Sobre geometría PLANA la normal es constante en toda la cara, así
    // que la pieza ENTERA se encendía de blanco a la vez. Lóbulo ancho, tenue y sin escalón.
    // POR QUÉ RELUCE "POR ZONAS": todas las piezas de un mismo muro comparten la MISMA normal, así que el
    // muro entero entra y sale del lóbulo especular a la vez — se enciende una fachada completa mientras
    // las demás quedan mates. En una superficie curva eso sería un reflejo; en una fachada plana es un
    // parche. Un muro de ladrillo o sillar es MATE: se le quita el realce y basta con el difuso.
    // (Si algún día hay materiales pulidos —mármol, metal, vidrio— el brillo debe venir del MATERIAL,
    //  no de un valor global para toda la obra.)
    vec3 highlights = vec3(0.0);

    float ndlMoon   = max(dot(N, normalize(moonDirection)), 0.0);
    vec3  moonlight = moonLightColor * moonIntensity * smoothstep(0.0, 0.6, ndlMoon) * baseColor * 0.6;

    vec3 color = diffuse + rim * rimCol + highlights + moonlight;
    if (enableHDR != 0) {
        color = color / (color + vec3(1.0));
        color = pow(color, vec3(1.0 / 2.2));
    } else {
        color = clamp(color, 0.0, 1.0);
    }

    FragColor = vec4(color, 1.0);
}
