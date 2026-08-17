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
// ⚠️ `vLoc` LO EXIGE `ocean.frag`, que es COMPARTIDO con el mar cercano. Sin declararlo aquí el
// pipeline lejano no enlaza —"vLoc not declared as input from previous stage"— y la esfera del
// océano desaparece sin más aviso que una línea de log. Compartir fragment entre dos vertex obliga
// a que los dos emitan el mismo juego de varyings.
layout(location = 4) out vec2  vLoc;
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
    vec4 uTexAnchor;
};
void main() {
    vFragPos = aPos + uCenter.xyz;
    vNorm    = normalize(aNormal);
    vDepth   = 0.0;      // lo recalcula el fragment con el bake (es quien decide la orilla)
    vFoam    = 0.0;
    // Un valor enorme: `vLoc` solo alimenta el recorte del hueco del anillo (`max(|vLoc|) < hole`),
    // y la esfera lejana no está dentro de ningún anillo. Con esto el test nunca la descarta —
    // aunque el `uClipCover` que quede atado sea el del último anillo dibujado.
    vLoc     = vec2(1e9);
    gl_Position = uMVP * vec4(vFragPos, 1.0);
}
