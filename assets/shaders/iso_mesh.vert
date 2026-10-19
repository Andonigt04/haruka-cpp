#version 460 core
/**
 * @file iso_mesh.vert
 * @brief Dibuja la malla que escribe `iso_surface.comp`, leída del SSBO por `gl_VertexID` (sin
 *        vertex buffer: el índice es la identidad). Genérico: el marco local del campo (origen y ejes
 *        relativos al OJO) lo da quien dibuja; el material lo pone el fragment.
 */

layout(std140, binding = 0) uniform IsoMeshDraw {
    mat4 uViewProj;     // vista·proyección CAMARA-RELATIVA (sin traslación)
    vec4 uOriginEye;    // xyz = origen del marco local relativo al ojo (m)
    vec4 uAxisX;        // xyz = eje x local en el mundo
    vec4 uAxisY;        // xyz = eje y local (arriba)
    vec4 uAxisZ;        // xyz = eje z local
    vec4 uMisc;         // libre para el material (water_surface: x = profundidad para el color)
};

struct IsoVert { vec4 p; vec4 n; };
layout(std430, binding = 4) readonly buffer IsoVerts { IsoVert uVerts[]; };

layout(location = 0) out vec3 vFragPos;   // relativa al ojo (m)
layout(location = 1) out vec3 vNormal;

void main() {
    const IsoVert v = uVerts[gl_VertexID];
    const vec3 rel = uOriginEye.xyz + uAxisX.xyz * v.p.x + uAxisY.xyz * v.p.y + uAxisZ.xyz * v.p.z;
    vFragPos = rel;
    vNormal  = uAxisX.xyz * v.n.x + uAxisY.xyz * v.n.y + uAxisZ.xyz * v.n.z;
    gl_Position = uViewProj * vec4(rel, 1.0);
}
