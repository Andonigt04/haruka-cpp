/**
 * @file planet.vert
 * @brief Vertex shader for planetary terrain chunks.
 *
 * Vertices are stored relative to their chunk center (float-safe).
 * The per-draw offset = chunkCenter - cameraPos.
 *
 * CDLOD geomorphing: aMorphTarget is each vertex's position in the decimated
 * (parent-LOD) mesh. The morph factor is computed PER-CHUNK on the CPU from the
 * chunk-CENTER distance (height-independent), so the whole chunk morphs
 * coherently. Per-vertex morphing using the displaced position tears tall
 * terrain into stair-steps (a peak is kilometres nearer than its valley), so
 * a uniform per-chunk factor is used instead.
 *
 * In:  aPos (0), aNormal (1), aUV (2), aMorphTarget (3), aMorphNormal (4)
 */
#version 460 core   // gl_DrawID (core 4.6)

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aMorphTarget;
layout(location = 4) in vec3 aMorphNormal; // normal del LOD padre (CDLOD)

layout(location = 0) out vec3 Normal;
layout(location = 1) out vec3 FragPos;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) flat out int vMode; // modo → al frag (gl_DrawID no existe allí)

// UN SOLO camino para los datos por-draw: SSBO indexado por gl_DrawID, que vale el índice del
// comando en un multidraw indirecto y 0 en un draw suelto. Así el MISMO shader sirve a los chunks
// del POOL (drawIndexedIndirect, N items) y a los que no tienen slot de pool (drawIndexed, SSBO de
// 1 item) — sin uniforms por-draw, que no existen en Vulkan.
struct DrawItem { vec4 offMorph; ivec4 mode; }; // xyz = offset cámara-relativo, w = morph ; mode.x = terrainMode
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
    DrawItem d  = uItems[gl_DrawID];
    vec3  off   = d.offMorph.xyz;
    float morph = d.offMorph.w;
    vMode       = d.mode.x;

    vec3 morphedPos = mix(aPos, aMorphTarget, morph);
    vec3 camRelPos  = off + morphedPos;
    FragPos  = camRelPos;
    // La normal se morpha igual que la posición → el chunk aplanado hacia el padre usa la
    // normal del padre, así el sombreado casa con la geometría (sin escalón de normales).
    Normal   = normalize(mix(aNormal, aMorphNormal, morph));
    TexCoord = aUV;
    gl_Position = projection * mat4(mat3(view)) * vec4(camRelPos, 1.0);
}
