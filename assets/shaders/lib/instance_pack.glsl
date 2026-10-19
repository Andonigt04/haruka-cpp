/**
 * @file instance_pack.glsl
 * @brief ESCRIBIR una instancia del stream compacto (40 B) desde la GPU: gemelo de
 *        `InstanceDataFloat::make` / `packTint` (Renderer/GPUInstancing.h). Lo que un compute deja
 *        aquí lo lee `prop_inst.vert` tal cual por `lib/instance.glsl`, sin un vertex propio.
 *
 * El buffer es `uint[]` y cada instancia ocupa 10 palabras: en std430 un struct con vec3 se alinea a
 * 16 B y no casaría con los 40 B empaquetados del stream de vértices.
 *   0..2 posición · 3..4 cuaternión snorm16 (x,y | z,w) · 5..7 escala · 8 tinte RGBA8 · 9 partes rotas
 */
#ifndef HARUKA_INSTANCE_PACK_GLSL
#define HARUKA_INSTANCE_PACK_GLSL

#define HARUKA_INSTANCE_WORDS 10u

/// Tinte → RGBA8 con la escala en el alfa (hasta 4,0). La escala se cuantiza PRIMERO, hacia arriba.
uint harukaPackTint(vec3 c) {
    const vec3  cc   = max(c, vec3(0.0));
    const float want = clamp(max(max(cc.r, cc.g), max(cc.b, 1.0)), 1.0, 4.0);
    const float a    = ceil(want * 0.25 * 255.0 - 1e-4) / 255.0;
    return packUnorm4x8(vec4(min(cc / (a * 4.0), vec3(1.0)), a));
}

/// Cuaternión unitario de la base ortonormal (X, Y, Z) (columnas de la rotación).
vec4 harukaQuatFromBasis(vec3 X, vec3 Y, vec3 Z) {
    const float tr = X.x + Y.y + Z.z;
    vec4 q;
    if (tr > 0.0) {
        const float s = sqrt(tr + 1.0) * 2.0;
        q = vec4((Y.z - Z.y) / s, (Z.x - X.z) / s, (X.y - Y.x) / s, 0.25 * s);
    } else if (X.x > Y.y && X.x > Z.z) {
        const float s = sqrt(1.0 + X.x - Y.y - Z.z) * 2.0;
        q = vec4(0.25 * s, (Y.x + X.y) / s, (Z.x + X.z) / s, (Y.z - Z.y) / s);
    } else if (Y.y > Z.z) {
        const float s = sqrt(1.0 + Y.y - X.x - Z.z) * 2.0;
        q = vec4((Y.x + X.y) / s, 0.25 * s, (Z.y + Y.z) / s, (Z.x - X.z) / s);
    } else {
        const float s = sqrt(1.0 + Z.z - X.x - Y.y) * 2.0;
        q = vec4((Z.x + X.z) / s, (Z.y + Y.z) / s, 0.25 * s, (X.y - Y.x) / s);
    }
    return normalize(q);
}

/// a · b (primero b, luego a).
vec4 harukaQuatMul(vec4 a, vec4 b) {
    return vec4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz), a.w * b.w - dot(a.xyz, b.xyz));
}

/// Giro de `angle` rad alrededor del eje unitario `axis`.
vec4 harukaQuatAxisAngle(vec3 axis, float angle) {
    return vec4(axis * sin(angle * 0.5), cos(angle * 0.5));
}

#endif // HARUKA_INSTANCE_PACK_GLSL
