/**
 * @file planet_pooled.vert
 * @brief Variante GPU-driven de planet.vert (LOD v2 — F4.1).
 *
 * Igual que planet.vert PERO los datos por-chunk (offset cámara-relativo y factor
 * de morph) NO vienen como uniforms por draw call: se leen de un SSBO indexado por
 * gl_DrawID, de modo que TODOS los chunks visibles se dibujan con UNA sola llamada
 * (glMultiDrawElementsIndirect) en vez de N. Usa el MISMO planet.frag (terrainMode y
 * profile siguen siendo uniforms por-planeta, iguales para todos los chunks del cuerpo).
 *
 * Vértices interleaved en el pool: pos(0) | normal(1) | uv(2) | morph(3).
 */
#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aMorphTarget;

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec2 TexCoord;

// Datos por-chunk, indexados por gl_DrawID (orden del multidraw). xyz = chunkOffset
// (chunkCenter + planetCenter − cameraPos), w = morphFactor (0=detalle, 1=padre).
struct ChunkDraw { vec4 offsetMorph; };
layout(std430, binding = 5) readonly buffer ChunkDrawBuf { ChunkDraw chunks[]; };

layout(std140, binding = 0) uniform PerFrameData {
    mat4 view;
    mat4 projection;
    vec3 _cameraPos;      float _pad0;
    vec3 sunDirection;    float _pad1;
    vec3 sunLightColor;   float ambientStrength;
    int  enableHDR;
    int  _enableBloom; int _enableSSAO; int _enableIBL; int _enableShadows;
    int  _pad3a; int _pad3b; int _pad3c;
    vec3 moonDirection;  float moonIntensity;
    vec3 moonLightColor; float _pad4;
};

void main() {
    ChunkDraw c = chunks[gl_DrawID];
    vec3  chunkOffset = c.offsetMorph.xyz;
    float morphFactor = c.offsetMorph.w;

    vec3 morphedPos = mix(aPos, aMorphTarget, morphFactor);
    vec3 camRelPos  = chunkOffset + morphedPos;
    FragPos  = camRelPos;
    Normal   = normalize(aNormal);
    TexCoord = aUV;
    gl_Position = projection * mat4(mat3(view)) * vec4(camRelPos, 1.0);
}
