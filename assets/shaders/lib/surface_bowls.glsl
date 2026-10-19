/**
 * @file surface_bowls.glsl
 * @brief CUENCOS sobre una superficie: cuánto la bajan en un punto y con qué gradiente. Gemelo de
 *        `VoxWorld::surfaceDeltaM` (los cráteres): cada cuenco es −D·(1−s²)², s = distancia
 *        horizontal / radio, y se SUMAN.
 *
 * No es una textura: un hoyo de medio metro cabe entre dos téxeles de cualquier ventana razonable,
 * así que se evalúa en el propio vértice. Centros RELATIVOS AL OJO (la CPU resta en double): cerca de
 * la cámara el float da micras. Lo rellena `TerrainNodeRenderer::CraterUBO`.
 *
 * Lo usa quien se apoye en el suelo: el terreno (que se hunde) y el agua (cuyo fondo se hunde con
 * él — sin esto la profundidad que se VE no era la que nada la física, que sí suma el hoyo).
 */
#ifndef HARUKA_SURFACE_BOWLS_GLSL
#define HARUKA_SURFACE_BOWLS_GLSL

layout(std140, binding = 4) uniform SurfaceBowls {
    ivec4 uCraterInfo;      // x = cuántos
    vec4  uCraterC[64];     // xyz = centro relativo al ojo (m) · w = radio (m)
    vec4  uCraterU[64];     // xyz = vertical del cuenco · w = hondo en el centro (m)
};

/// @param posRelEye punto de la superficie, relativo al ojo (m).
/// @param grad  gradiente horizontal de la bajada (para inclinar la normal: n -= tangente(grad)).
/// @param cover 0..1: cuánto de cuenco hay aquí (1 ya a 0,35 del borde, que funde).
/// @return cuánto baja la superficie (m, ≤ 0).
float harukaSurfaceBowls(vec3 posRelEye, out vec3 grad, out float cover) {
    float dh = 0.0;
    grad  = vec3(0.0);
    cover = 0.0;
    for (int ci = 0; ci < uCraterInfo.x; ++ci) {
        const vec3  q  = posRelEye - uCraterC[ci].xyz;
        const vec3  cu = uCraterU[ci].xyz;
        const vec3  hq = q - cu * dot(q, cu);          // horizontal: el relieve no cuenta como distancia
        const float R2 = uCraterC[ci].w * uCraterC[ci].w;
        const float s2 = dot(hq, hq) / R2;
        if (s2 >= 1.0) continue;
        const float w = 1.0 - s2;
        dh   -= uCraterU[ci].w * w * w;
        grad += hq * (4.0 * uCraterU[ci].w * w / R2);   // d(−D·w²)/dx = 4·D·w·x/R²
        cover = max(cover, smoothstep(0.0, 0.35, w));
    }
    return dh;
}

#endif // HARUKA_SURFACE_BOWLS_GLSL
