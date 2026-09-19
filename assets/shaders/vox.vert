/**
 * @file vox.vert
 * @brief Las paredes del campo volumétrico (cuevas, lo picado): un chunk por draw.
 *
 * Las posiciones vienen RELATIVAS AL ORIGEN DEL CHUNK (±100 m, exactas en float) y el origen llega
 * ya relativo a la cámara: la misma convención que todo el planeta para no operar a 6e6 m en float.
 */
#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

layout(std140, binding = 5) uniform VoxParams {
    mat4 uRotVP;        // proyección · rotación de la vista (sin traslación)
    vec4 uOriginRel;    // xyz = origen del chunk relativo a la cámara (m)
    vec4 uLightDir;     // xyz = hacia el sol
    vec4 uColor;        // rgb = roca · a = sin usar
    vec4 uUp;
    vec4 uTexOrigin;
    vec4 uTexInfo;
};

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vPosRel;
layout(location = 2) out vec3 vPosLocal;   // relativa al origen del chunk: para el triplanar

void main() {
    const vec3 p = uOriginRel.xyz + aPos;
    vNormal = aNormal;
    vPosRel = p;
    vPosLocal = aPos;
    gl_Position = uRotVP * vec4(p, 1.0);
}
