#ifndef HARUKA_BASE_FIELD_GLSL
#define HARUKA_BASE_FIELD_GLSL
/**
 * @file base_field.glsl
 * @brief El BAKE del planeta (elevación, temperatura, humedad) como array de 6 caras.
 *
 * ── LA BILINEAL VA A MANO, Y NO ES UNA MANÍA ────────────────────────────────────────────────────
 *
 * ⚠️ NO se usa el filtrado del hardware. El bilineal de GL usa pesos de precisión limitada (8 bits
 * en varias GPU), y eso basta para que el suelo del clipmap y el de la malla difieran en centímetros
 * — que es justo lo que hay que evitar, porque los dos tienen que describir UN solo suelo. Esta
 * bilineal replica exactamente la que hace la CPU.
 *
 * ── QUIÉN LO LEE ────────────────────────────────────────────────────────────────────────────────
 *
 * `clipmap.tese`, `terrain_node.comp` (la altura del nodo) y `terrain_node.vert` (el clima para
 * seleccionar material). Tenía DOS copias literales antes de existir este archivo; la tercera es la
 * que lo hizo insostenible. Ver el inventario de shaders: cinco copias divergidas del toon ya
 * costaron caro una vez.
 *
 * El que incluya esto declara `uBaseField` en su binding (15 por convención del motor) e incluye
 * antes `lib/cube_face.glsl`, de donde sale `harukaDirToCubeFace`.
 *
 * Devuelve (elevación en METROS, temperatura, humedad).
 */

vec3 harukaSampleBaseField(sampler2DArray fld, vec3 dir) {
    int f; vec2 uv;
    harukaDirToCubeFace(dir, f, uv);
    int N = textureSize(fld, 0).x - 1;          // lado de la retícula = faceRes
    vec2 fxy = (uv * 0.5 + 0.5) * float(N);
    ivec2 i0 = clamp(ivec2(floor(fxy)), ivec2(0), ivec2(N - 1));
    vec2 t = fxy - vec2(i0);
    vec3 h00 = texelFetch(fld, ivec3(i0 + ivec2(0,0), f), 0).rgb;
    vec3 h10 = texelFetch(fld, ivec3(i0 + ivec2(1,0), f), 0).rgb;
    vec3 h01 = texelFetch(fld, ivec3(i0 + ivec2(0,1), f), 0).rgb;
    vec3 h11 = texelFetch(fld, ivec3(i0 + ivec2(1,1), f), 0).rgb;
    vec3 a = h00 + (h10 - h00) * t.x;
    vec3 b = h01 + (h11 - h01) * t.x;
    return a + (b - a) * t.y;
}

#endif // HARUKA_BASE_FIELD_GLSL
