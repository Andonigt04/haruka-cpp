/**
 * @file prop_inst.vert
 * @brief Vertex shader INSTANCIADO para props del mundo (árboles, rocas, objetos). Los prototipos
 *        se hornean con el node material graph del IDE (malla + UVs + material PBR), y el modelo
 *        va CAMERA-RELATIVO en el stream de instancia (binding 1) con `view` solo rotación.
 *
 * In (per-vertex, binding 0):  aPos (loc 0), aNormal (loc 1), aColor (loc 2), aUv (loc 9)
 *                              — layout del prototipo prop (POS/NORMAL/COLOR/UV). El UV va a 9
 *                              para no chocar con el stream de instancia (loc 3..8, ver abajo).
 * In (per-instance, binding 1): model mat4 (loc 3..6), instanceColor (loc 7), instanceScale (loc 8)
 *                              — MISMO layout que GPUInstancing::appendInstanceLayout.
 * Out: Normal (0), FragPos (1), Color (2), TexCoord (3) → prop_inst.frag
 * UBOs: PerFrameData (binding 0, MISMO que construction_inst) + PropParams (binding 6).
 */
#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 9) in vec2 aUv;

// Stream de instancia (divisor=1): transformación completa + tinte por objeto.
layout(location = 3) in mat4 iModel;       // ocupa loc 3,4,5,6 — ya camera-relativo
layout(location = 7) in vec4 iColor;
layout(location = 8) in vec3 iScale;

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

// Parámetros del pase de props (binding 6): viento del clima + escalares del material del prototipo.
layout(std140, binding = 6) uniform PropParams {
    vec3  u_wind;   float u_time;      // viento del clima (mundo, m/s) · segundos
    vec4  u_matPBR;                    // x=metallic y=roughness z=ao w=máscara de texturas (bits)
};

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec3 Color;
layout(location = 3) out vec2 TexCoord;

void main() {
    // VIENTO: solo las HOJAS (verde) ondean, más cuanto más alto en el prototipo (aPos.y = arriba
    // local). Fase por posición del prop (columna de traslación de iModel) → no van todas a la vez.
    vec3  pos  = aPos;
    float leaf = (aColor.g > aColor.r + 0.02 && aColor.g > aColor.b + 0.02) ? 1.0 : 0.0;
    float h    = clamp(aPos.y, 0.0, 4.0);
    float ph   = iModel[3].x * 0.15 + iModel[3].z * 0.15;

    // El viento es el DEL CLIMA (el mismo que mueve nubes y lluvia): con tormenta encima el
    // follaje se dobla hacia donde van las nubes, y en calma apenas se mueve.
    vec3  wdir  = (dot(u_wind, u_wind) > 1e-4) ? normalize(u_wind) : vec3(1.0, 0.0, 0.0);
    float wspd  = clamp(length(u_wind) / 12.0, 0.15, 1.6);   // 12 m/s ≈ ondeo pleno
    float sway  = sin(u_time * 1.7 + ph) + 0.35 * sin(u_time * 3.3 + ph * 1.7);
    float flap  = cos(u_time * 1.4 + ph);                     // vaivén perpendicular (no un péndulo plano)
    vec3  wside = normalize(cross(vec3(0.0, 1.0, 0.0), wdir) + vec3(1e-5));
    pos += leaf * h * wspd * (wdir * (0.035 + 0.05 * sway) + wside * 0.03 * flap);

    vec3 camRel = (iModel * vec4(pos, 1.0)).xyz;   // prototipo → sitio del prop (camera-relativo)
    FragPos  = camRel;
    Normal   = normalize(transpose(inverse(mat3(iModel))) * aNormal);   // escala NO uniforme → traspuesta inversa
    Color    = aColor * iColor.rgb;
    TexCoord = aUv;
    gl_Position = projection * view * vec4(camRel, 1.0);
}
