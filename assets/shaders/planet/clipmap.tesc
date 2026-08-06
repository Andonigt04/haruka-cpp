#version 460 core
layout(vertices = 4) out;
in vec2 cLocal[]; out vec2 eLocal[];
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
};
layout(std140, binding = 13) uniform ClipParams {
    vec4 uClipOrigin;   // xyz = dirección del centro de la rejilla, w = tamaño de parche (m)
    vec4 uClipTanU;     // xyz = tangente este
    vec4 uClipTanV;     // xyz = tangente norte
    vec4 uClipCover;    // x = semi-lado (m) del clipmap, y/z = inicio/fin del anillo de mezcla
};
// Mismo criterio que el de la malla del planeta: factor a partir de la ARISTA. Aquí las aristas son
// mucho más cortas, así que el factor satura a 32 cerca y baja solo en el borde de la rejilla.
float edgeFactor(vec2 a, vec2 b) {
    // OJO: `patch` es palabra RESERVADA en GLSL (el calificador `patch out` de teselación). Usarla
    // como nombre de variable da "syntax error, unexpected PATCH" sin más pista.
    vec2  m = (a + b) * 0.5;
    // Distancia aproximada a la cámara EN EL PLANO: la cámara está sobre el origen local.
    float d = max(length(m), 1.0);
    float arc = length(a - b);
    // Tope 32 (antes 64): los parches cercanos a la cámara saturaban a 64 → quads de 2 m en los
    // primeros ~500 m, que es el grueso del coste de teselación (cada vértice evalúa ~15 ruidos de
    // detalle). Con 32 los quads cercanos son de 4 m — la octava de 22 m sigue con 5,5 muestras por
    // onda, el suelo se ve igualmente suave, y la teselación cuesta 4× menos. El fragmento (relleno)
    // es por píxel, no cambia con esto.
    return clamp(arc / (d * 0.004), 1.0, 32.0);
}
void main() {
    eLocal[gl_InvocationID] = cLocal[gl_InvocationID];
    if (gl_InvocationID == 0) {
        float e0 = edgeFactor(cLocal[3], cLocal[0]);
        float e1 = edgeFactor(cLocal[0], cLocal[1]);
        float e2 = edgeFactor(cLocal[1], cLocal[2]);
        float e3 = edgeFactor(cLocal[2], cLocal[3]);
        gl_TessLevelOuter[0]=e0; gl_TessLevelOuter[1]=e1;
        gl_TessLevelOuter[2]=e2; gl_TessLevelOuter[3]=e3;
        gl_TessLevelInner[0]=max(e1,e3); gl_TessLevelInner[1]=max(e0,e2);
    }
}
