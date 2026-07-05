/**
 * @file planet.vert
 * @brief Vertex shader for planetary terrain chunks.
 *
 * Vertices are stored relative to their chunk center (float-safe).
 * u_chunkOffset = chunkCenter - cameraPos, supplied per draw call.
 *
 * CDLOD geomorphing: aMorphTarget is each vertex's position in the decimated
 * (parent-LOD) mesh. u_morphFactor is computed PER-CHUNK on the CPU from the
 * chunk-CENTER distance (height-independent), so the whole chunk morphs
 * coherently. Per-vertex morphing using the displaced position tears tall
 * terrain into stair-steps (a peak is kilometres nearer than its valley), so
 * a uniform per-chunk factor is used instead.
 *
 * In:  aPos (0), aNormal (1), aUV (2), aMorphTarget (3)
 */
#version 460 core   // gl_DrawID (core 4.6) para MultiDrawIndirect

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aMorphTarget;
layout(location = 4) in vec3 aMorphNormal; // normal del LOD padre (CDLOD)

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) flat out int vMode; // modo → al frag (draw normal: uniform; batched: SSBO)

layout(location = 10) uniform vec3  u_chunkOffset;
layout(location = 12) uniform float u_morphFactor; // 0=full detail, 1=parent shape
layout(location = 11) uniform int   u_terrainMode; // 0=Procedural 1=Manual (draw normal)

// MultiDrawIndirect: por cada draw del batch, sus datos (offset/morph/modo) vienen de este SSBO
// indexado por gl_DrawID → sin uniforms por chunk → 1 sola llamada glMultiDrawElementsIndirect.
layout(location = 40) uniform int u_batched; // 1 = leer del SSBO por gl_DrawID
struct DrawItem { vec4 offMorph; ivec4 mode; }; // xyz=offset, w=morph ; mode.x=terrainMode
layout(std430, binding = 7) readonly buffer DrawDataSSBO { DrawItem uItems[]; };

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
    // Datos por-draw: en batched vienen del SSBO (gl_DrawID), si no de los uniforms por chunk.
    vec3 off; float morph; int mode;
    if (u_batched == 1) {
        DrawItem d = uItems[gl_DrawID];
        off = d.offMorph.xyz; morph = d.offMorph.w; mode = d.mode.x;
    } else {
        off = u_chunkOffset; morph = u_morphFactor; mode = u_terrainMode;
    }
    vMode = mode;
    vec3 morphedPos = mix(aPos, aMorphTarget, morph);
    vec3 camRelPos  = off + morphedPos;
    FragPos  = camRelPos;
    // La normal se morpha igual que la posición → el chunk aplanado hacia el padre usa la
    // normal del padre, así el sombreado casa con la geometría (sin escalón de normales).
    Normal   = normalize(mix(aNormal, aMorphNormal, morph));
    TexCoord = aUV;
    gl_Position = projection * mat4(mat3(view)) * vec4(camRelPos, 1.0);
}
