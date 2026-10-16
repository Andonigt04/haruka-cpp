/**
 * @file triplanar.glsl
 * @brief Muestreo triplanar sobre una capa de un sampler2DArray (color y normal map).
 *
 * Sin dependencias a propósito: `vox.frag` lo necesita y no puede incluir `terrain_shade.glsl`
 * (arrastra `harukaSelectMaterial` y su UBO de materiales), así que tenía su propia copia.
 */
#ifndef HARUKA_TRIPLANAR_GLSL
#define HARUKA_TRIPLANAR_GLSL

// Triplanar sobre una CAPA del array: los tres planos pesados por la normal elevada a 4.
vec3 harukaTriplanarArr(sampler2DArray tex, float layer, vec3 wp, vec3 n, float s) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= max(bw.x + bw.y + bw.z, 1e-6);
    return texture(tex, vec3(wp.zy * s, layer)).rgb * bw.x
         + texture(tex, vec3(wp.xz * s, layer)).rgb * bw.y
         + texture(tex, vec3(wp.xy * s, layer)).rgb * bw.z;
}

// Normal map en "whiteout blend": se suman las desviaciones TANGENCIALES de las tres proyecciones,
// no los vectores ya girados. Mezclar normales de mundo las promedia hacia la normal geométrica y
// aplana el relieve justo en las caras diagonales, que son media esfera.
vec3 harukaTriplanarArrNrm(sampler2DArray tex, float layer, vec3 wp, vec3 n, float s) {
    vec3 bw = pow(abs(n), vec3(4.0));
    bw /= max(bw.x + bw.y + bw.z, 1e-6);
    vec3 tx = texture(tex, vec3(wp.zy * s, layer)).xyz * 2.0 - 1.0;
    vec3 ty = texture(tex, vec3(wp.xz * s, layer)).xyz * 2.0 - 1.0;
    vec3 tz = texture(tex, vec3(wp.xy * s, layer)).xyz * 2.0 - 1.0;
    return vec3(0.0, tx.y, tx.x) * sign(n.x) * bw.x
         + vec3(ty.x, 0.0, ty.y) * sign(n.y) * bw.y
         + vec3(tz.x, tz.y, 0.0) * sign(n.z) * bw.z;
}

#endif // HARUKA_TRIPLANAR_GLSL
