/**
 * @file px_lod.glsl
 * @brief LA LEY DE DETALLE POR PÍXELES en la GPU: gemelo EXACTO de `World/Props/PxLod.h` (léelo
 *        allí: por qué no hay radios y cómo se conserva la cobertura al ralear).
 *
 * `L` = (focalPx, widthM, thrPx, widenMax), el mismo orden que el struct de C++, para subirlo como
 * un vec4 tal cual.
 */
#ifndef HARUKA_PX_LOD_GLSL
#define HARUKA_PX_LOD_GLSL

float pxFullM (vec4 L)          { return L.y * L.x / max(L.z, 1e-3); }
float pxReachM(vec4 L)          { return L.w * pxFullM(L); }
float pxWiden (vec4 L, float d) { return clamp(d / pxFullM(L), 1.0, L.w); }
float pxKeep  (vec4 L, float d) {
    const float reach = pxReachM(L);
    if (d >= reach) return 0.0;
    const float t = clamp((d - reach) / (0.75 * reach - reach), 0.0, 1.0);
    return (1.0 / pxWiden(L, d)) * (t * t * (3.0 - 2.0 * t));
}
bool  pxKeeps (vec4 L, uint perGroup, uint k, float d) { return float(k) < float(perGroup) * pxKeep(L, d); }

#endif // HARUKA_PX_LOD_GLSL
