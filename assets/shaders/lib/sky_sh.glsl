/**
 * @file sky_sh.glsl
 * @brief La CARA GPU de `SkySH`: irradiancia del cielo evaluada POR NORMAL.
 *
 * ── POR QUÉ EXISTE ─────────────────────────────────────────────────────────────────────────────
 * `sky_ambient.cpp` integra el cielo en 9 coeficientes de armónicos esféricos, y su cabecera pide
 * explícitamente que el fragment replique la fórmula. En la primera versión no se hizo: se evaluaba
 * UNA vez en la CPU para una normal hacia arriba y se pasaba como constante.
 *
 * ⚠️ Y eso es exactamente una SOMBRA PLANA. Con un ambiente constante, todo lo que no recibe sol
 * directo queda `albedo × constante`: una ladera que mira al cielo y una que mira al suelo se
 * iluminan igual, así que en sombra desaparece el relieve y los props se ven como recortes de color
 * liso. Los 9 coeficientes existen precisamente para que eso no pase.
 *
 * Con la evaluación por normal, una cara vuelta al cénit recibe el cielo entero y una vuelta al
 * suelo recibe el rebote — que es lo que da forma a lo que está en sombra.
 *
 * ⚠️ GEMELO EXACTO de `skyAmbientEval` (mismo triedro, misma base, mismo orden). Los coeficientes
 * ya vienen convolucionados con el kernel difuso desde la CPU: aquí solo hay un producto escalar.
 */
#ifndef HARUKA_SKY_SH_GLSL
#define HARUKA_SKY_SH_GLSL

layout(std140, binding = 28) uniform SkyAmbientSH {
    vec4 uSkySH[9];   // xyz = coeficiente (ya convolucionado) · w libre
};

/**
 * @param n   normal (mundo, unitaria).
 * @param up  cénit local (mundo, unitario) — el marco en el que se integró el SH.
 * @return irradiancia difusa. ⚠️ Es IRRADIANCIA: para usarla como multiplicador de albedo hay que
 *         dividirla por π (lo hace el consumidor, igual que la CPU).
 */
vec3 harukaSkySHEval(vec3 n, vec3 up) {
    // El eje auxiliar se elige ANTES de normalizar: si `up` fuese paralelo a él, el producto
    // vectorial daría 0 y `normalize` un NaN — no un vector corto que se pueda detectar después.
    vec3 aux = abs(up.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 e1  = normalize(cross(up, aux));
    vec3 e2  = cross(up, e1);

    float x = dot(n, e1), y = dot(n, e2), z = dot(n, up);

    return uSkySH[0].xyz * 0.2820947918
         + uSkySH[1].xyz * (0.4886025119 * y)
         + uSkySH[2].xyz * (0.4886025119 * z)
         + uSkySH[3].xyz * (0.4886025119 * x)
         + uSkySH[4].xyz * (1.0925484306 * x * y)
         + uSkySH[5].xyz * (1.0925484306 * y * z)
         + uSkySH[6].xyz * (0.3153915653 * (3.0 * z * z - 1.0))
         + uSkySH[7].xyz * (1.0925484306 * x * z)
         + uSkySH[8].xyz * (0.5462742153 * (x * x - y * y));
}

#endif // HARUKA_SKY_SH_GLSL
