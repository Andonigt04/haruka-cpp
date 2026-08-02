/**
 * @file construction_inst.frag
 * @brief Fragment lit para piezas de construcción instanciadas. Fork de final.frag: el color base
 *        viene del varying InstanceColor (por instancia), NO del UBO per-objeto. Misma iluminación
 *        (ambiente + sol + luna, con los toggles), para que las piezas instanciadas se vean IGUAL
 *        que las de escena. Sin rama de estrella emisiva (una pieza nunca lo es).
 *
 * In:  Normal (0), FragPos (1), InstanceColor (2)
 * UBO: PerFrameData (binding 0) — MISMO layout que final.frag.
 */
#version 450 core

layout(location = 0) out vec4 FragColor;

layout(location = 0) in vec3 Normal;
layout(location = 1) in vec3 FragPos;
layout(location = 2) in vec4 InstanceColor;

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

// Cel suave + rim — MISMA iluminación que final.frag (mood cálido-aventura + crudeza mística), pero el
// color base viene por INSTANCIA. Así las piezas instanciadas se ven igual que la escena.
void main() {
    vec3 baseColor = InstanceColor.rgb;

    vec3 N = normalize(Normal);
    vec3 L = normalize(sunDirection);
    vec3 V = normalize(cameraPos - FragPos);
    // (sin especular: la obra es MATE — ver la nota de abajo)

    float ndl  = dot(N, L);
    float band = smoothstep(-0.03, 0.22, ndl);
    vec3  litCol    = baseColor * (0.80 + 0.25 * sunLightColor);
    vec3  shadowCol = baseColor * vec3(0.40, 0.46, 0.60);
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
