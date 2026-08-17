/**
 * @file terrain_deform.glsl
 * @brief Gemelo GPU de `core/planet/terrain_deform.h`: lo que el jugador ha excavado.
 *
 * ⚠️ MISMA CUENTA Y MISMO ORDEN que el .h. Si el render suma el delta y la física no —o lo suman
 * distinto— el jugador cava un hoyo y sigue caminando sobre el suelo que había. Falla en silencio,
 * que es peor que no poder cavar.
 *
 *     altura del suelo = bake(dir) + detalle(dir) + deform(dir)
 *
 * Y el delta NEGATIVO es además la PROFUNDIDAD de excavación: lo que hay que pasarle a
 * `harukaSelectMaterial` para que la pared del hoyo salga del estrato que toque.
 */
#ifndef HARUKA_TERRAIN_DEFORM_GLSL
#define HARUKA_TERRAIN_DEFORM_GLSL

layout(std140, binding = 26) uniform DeformParams {
    vec4 uDeformAnchor;   // xyz = ancla del parche relativa al OJO
    vec4 uDeformTanU;     // xyz = eje +X del parche
    vec4 uDeformTanV;     // xyz = eje +Z del parche
    vec4 uDeformMisc;     // x = lado (m) · y = celdas por lado · w = 1 si hay capa
};
layout(std430, binding = 27) readonly buffer TerrainDeformBuf { float uDeformDelta[]; };

/// @param posRelEye posición del punto RELATIVA AL OJO (mismo marco que el ancla).
/// @return delta de altura en metros. 0 fuera del parche o sin deformar.
float harukaTerrainDeform(vec3 posRelEye) {
    if (uDeformMisc.w < 0.5) return 0.0;
    int   n    = int(uDeformMisc.y);
    float span = uDeformMisc.x;
    if (n <= 1 || span <= 0.0) return 0.0;

    vec3  rel = posRelEye - uDeformAnchor.xyz;
    float u   = dot(rel, uDeformTanU.xyz);
    float v   = dot(rel, uDeformTanV.xyz);
    float fx  = (u + span * 0.5) / span * float(n - 1);
    float fy  = (v + span * 0.5) / span * float(n - 1);
    if (fx < 0.0 || fy < 0.0 || fx > float(n - 1) || fy > float(n - 1)) return 0.0;

    int   x0 = int(floor(fx)), y0 = int(floor(fy));
    int   x1 = min(x0 + 1, n - 1), y1 = min(y0 + 1, n - 1);
    float tx = fx - float(x0), ty = fy - float(y0);
    float d00 = uDeformDelta[y0 * n + x0], d10 = uDeformDelta[y0 * n + x1];
    float d01 = uDeformDelta[y1 * n + x0], d11 = uDeformDelta[y1 * n + x1];
    float a = d00 + (d10 - d00) * tx;
    float b = d01 + (d11 - d01) * tx;
    return a + (b - a) * ty;
}

#endif // HARUKA_TERRAIN_DEFORM_GLSL
