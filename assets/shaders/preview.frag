#version 450 core

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
    vec3 moonDirection;  float moonIntensity;
    vec3 moonLightColor; float _pad4;
};

// Idéntico a simple.vert, con el que linka (ver la nota de allí): declararlo con menos campos da
// un error de LINKADO, no de compilación, y el modo Preview se quedaría mudo.
layout(std140, binding = 1) uniform PerObjectData {
    mat4 model;
    vec4 baseColorAndPlanetRadius;
    vec4 planetCenterAndFlag;
    vec4 materialPBR;
    vec4 materialEmission;
};

// El modo PREVIEW muestra el material sin el look final: solo el albedo, para juzgar la textura
// (y el grafo que la horneó) sin que el toon, el rim ni el tonemap la disfracen.
layout(binding = 0) uniform sampler2D u_matAlbedo;

void main() {
    vec3 baseColor = baseColorAndPlanetRadius.rgb;
    if (dot(baseColor, baseColor) < 0.001)
        baseColor = vec3(0.72, 0.74, 0.78);
    if ((int(materialPBR.w) & 1) != 0)
        baseColor *= pow(texture(u_matAlbedo, TexCoord).rgb, vec3(2.2));

    // Emissive stars: render at full brightness, no shading
    if (planetCenterAndFlag.w > 1.5) {
        FragColor = vec4(baseColor, 1.0);
        return;
    }

    vec3 n = normalize(Normal);
    vec3 L = normalize(sunDirection);
    float ndl = max(dot(n, L), 0.0);

    float ambient = max(ambientStrength, 0.15);
    vec3 color = ambient * baseColor
               + (1.0 - ambient) * ndl * sunLightColor * baseColor;

    FragColor = vec4(color, 1.0);
}
