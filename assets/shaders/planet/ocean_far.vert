#version 460 core
// EL MAR LEJANO: la esfera a nivel del mar, LISA. A partir del borde del clipmap una ola de 60 m es
// subpíxel, así que aquí no hay geometría de oleaje — solo la superficie. Comparte fragment con el
// mar cercano (`ocean.frag` espera las mismas varyings), con oleaje 0.
layout(location = 0) in vec3 aPos;    // vértice de la esfera de agua, relativo al CENTRO del planeta
layout(location = 1) in vec3 aNormal;
layout(location = 0) out vec3 vNorm;
layout(location = 1) out vec3 vFragPos;
layout(location = 2) out float vDepth;
layout(location = 3) out float vFoam;
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
    vec4 uTexAnchor;
};
void main() {
    vFragPos = aPos + uCenter.xyz;
    vNorm    = normalize(aNormal);
    vDepth   = 0.0;      // lo recalcula el fragment con el bake (es quien decide la orilla)
    vFoam    = 0.0;
    gl_Position = uMVP * vec4(vFragPos, 1.0);
}
