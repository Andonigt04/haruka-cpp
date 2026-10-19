#version 460 core
#extension GL_GOOGLE_include_directive : require
/**
 * @file water_surface.frag
 * @brief El MATERIAL agua sobre cualquier geometría que traiga posición relativa al ojo y normal
 *        (hoy: la superficie del líquido 3D, `iso_mesh.vert`). La luz es la del mar
 *        (`harukaOceanShade`), con la del frame y el aire de en medio: el agua se ve igual la
 *        dibuje quien la dibuje. No calcula olas: la forma ya viene de la física.
 */
#include "lib/aerial.glsl"
#include "lib/ocean_shade.glsl"

layout(std140, binding = 0) uniform IsoMeshDraw {
    mat4 uViewProj; vec4 uOriginEye; vec4 uAxisX; vec4 uAxisY; vec4 uAxisZ;
    vec4 uMisc;         // x = profundidad (m) para el color y la opacidad · y = 1: vista de depuración
};

layout(location = 0) in vec3 vFragPos;
layout(location = 1) in vec3 vNormal;
layout(location = 0) out vec4 fragColor;

void main() {
    // Depuración (`HARUKA_LIQUID_DEBUG=1`): la superficie en magenta opaco, con su normal como luz,
    // para ver DÓNDE está y qué forma tiene sin el agua de siempre por medio.
    if (uMisc.y > 0.5) {
        const vec3 nd = normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0);
        fragColor = vec4(vec3(1.0, 0.0, 1.0) * (0.35 + 0.65 * max(dot(nd, normalize(uSkySunDir.xyz)), 0.0)), 1.0);
        return;
    }
    const vec3 wp = vFragPos - uSkyPlanet.xyz;     // relativa al centro del planeta
    const vec3 up = normalize(wp);
    vec3 n = normalize(vNormal);
    // Cara trasera (se mira el agua desde dentro, o el envés del labio): la normal hacia el ojo.
    if (!gl_FrontFacing) n = -n;
    const vec3 L = normalize(uSkySunDir.xyz);
    const vec3 ambE = harukaSkyAmbient(n, up) + uSkyMoonGround.rgb * max(dot(n, uSkyMoonDir.xyz), 0.0);
    const vec3 Rv = reflect(normalize(vFragPos), n);
    vec4 c = harukaOceanShade(wp, vFragPos, n, n, L, uSkySunGround.rgb, ambE, harukaSkyRadiance(Rv, up),
                              uMisc.x, 0.0, 0.0);
    c.rgb = harukaAerialSkyLinear(c.rgb, vFragPos);
    fragColor = vec4(harukaDisplay(c.rgb), c.a);
}
