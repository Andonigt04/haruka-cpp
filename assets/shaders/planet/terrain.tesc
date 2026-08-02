#version 460 core
layout(vertices = 4) out;
in vec3 cpPos[]; in vec3 cpColor[]; in vec3 cpClimate[];
out vec3 ePos[]; out vec3 eColor[]; out vec3 eClimate[];
layout(std140, binding = 0) uniform SimplePlanetUBO {
    mat4 uMVP; vec4 uCenter; vec4 uLightDir; vec4 uLightColor; vec4 uAmbient; vec4 uExtra; vec4 uDebug;
};

// Factor de una arista, calculado SOLO a partir de la arista.
//
// Esta es la propiedad que sustituye a toda la maquinaria de cosido del sistema de chunks: dos
// parches vecinos comparten arista, le pasan los mismos dos extremos y obtienen el MISMO número,
// así que no puede haber grietas por construcción. Meter cualquier dato del parche aquí —su
// centro, su normal, su nivel— rompe la simetría y las grietas vuelven.
float edgeFactor(vec3 a, vec3 b) {
    // Las posiciones llegan relativas al centro del planeta; uCenter.xyz = centro − cámara.
    vec3  mid = (a + b) * 0.5 + uCenter.xyz;     // punto medio, relativo a la CÁMARA
    float d   = max(length(mid), 1.0);
    float arc = length(a - b);                   // longitud de la arista en metros
    // Objetivo: ~kTargetPx radianes por segmento. Antes 0.012: la costa quedaba en bloques de
    // kilómetros a media distancia; 0.004 dejaba triángulos de ~3 px en pantalla (se veían las
    // facetas); 0.002 los deja ~1.5 px, por debajo del umbral visible.
    // PERO con 0.002 el factor caía a 1 (la malla base de 256, bloques de ~38 km) nada más pasar
    // de ~1.2×radio: desde órbita la costa volvía a salir POLIGONAL. 0.0007 mantiene la
    // subdivisión cuando te acercas al planeta (LOD ~2-4 a 1.5-2×radio) SIN tocar la malla base.
    const float kTargetPx = 0.0007;               // radianes por segmento ≈ tamaño en pantalla
    return clamp(arc / (d * kTargetPx), 1.0, 64.0);
}

void main() {
    ePos[gl_InvocationID]     = cpPos[gl_InvocationID];
    eColor[gl_InvocationID]   = cpColor[gl_InvocationID];
    eClimate[gl_InvocationID] = cpClimate[gl_InvocationID];
    if (gl_InvocationID == 0) {
        // Orden de gl_TessLevelOuter en un quad: 0 = arista v0-v3, 1 = v0-v1, 2 = v1-v2, 3 = v2-v3.
        // Equivocarlo no da grietas —los factores siguen siendo simétricos— pero sí subdivide en la
        // dirección contraria, y el síntoma es "se ve mal" sin nada que lo explique.
        float e0 = edgeFactor(cpPos[3], cpPos[0]);
        float e1 = edgeFactor(cpPos[0], cpPos[1]);
        float e2 = edgeFactor(cpPos[1], cpPos[2]);
        float e3 = edgeFactor(cpPos[2], cpPos[3]);
        gl_TessLevelOuter[0] = e0; gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2; gl_TessLevelOuter[3] = e3;
        // El interior es el máximo de las aristas: si fuera menor, el parche se rompería por dentro
        // al no poder conectar aristas más finas que su relleno.
        gl_TessLevelInner[0] = max(e1, e3);
        gl_TessLevelInner[1] = max(e0, e2);
    }
}
