/**
 * @file surface_shade.glsl
 * @brief Lo que comparten los fragment shaders de MALLA (props, construcción, mallas del juego).
 *
 * ⚠️ NACE DE UN INVENTARIO, NO DE UNA IDEA. Al cruzar los 105 shaders del proyecto salieron cinco
 * copias del mismo modelo de iluminación toon, y las copias habían DIVERGIDO: cuatro anchos de
 * terminador distintos (de 0,25 a 0,70) y cuatro colores de sombra distintos, tres de ellos
 * constantes escritas a mano que no miran ni el cielo ni la hora del día. El mismo objeto, con la
 * misma luz, se ve distinto según qué shader lo dibuje.
 *
 * Esto es el primer paso de esa unificación, y a propósito el que NO cambia ni un píxel:
 * `applyNormalMap` estaba duplicada BYTE A BYTE en `final.frag`, `prop_inst.frag` y
 * `construction_inst.frag` (0 diferencias, medido). Aquí solo se muda.
 *
 * Lo que falta y va aquí cuando se decida el criterio: `toonShade`. No se ha traído todavía porque
 * unificarlo SÍ cambia el aspecto —hay que elegir qué terminador y qué color de sombra son los
 * buenos— y eso es una decisión de arte, no una refactorización.
 */
#ifndef HARUKA_SURFACE_SHADE_GLSL
#define HARUKA_SURFACE_SHADE_GLSL

/**
 * @brief Aplica un normal map construyendo el marco TBN por DERIVADAS DE PANTALLA.
 *
 * Las mallas procedurales de este motor no traen tangentes de vértice, así que el marco se deduce
 * de cómo varían posición y UV entre píxeles vecinos. Es la razón por la que esto vive en el
 * fragment y no en el vertex.
 *
 * ⚠️ `worldPos` y `uv` se pasan como PARÁMETROS y no se leen de las varyings por su nombre. Las tres
 * copias que esto sustituye usaban `FragPos`/`TexCoord` como globales, lo que ataba la función a que
 * cada shader llamara igual a sus varyings — y no lo hacían: en `final.frag` la UV entra por
 * location 2 y en los otros dos por la 3. Con parámetros, el cálculo es idéntico (`dFdx` de un
 * argumento es el mismo que el de la varying) y la dependencia deja de ser implícita.
 *
 * @param N        normal geométrica normalizada.
 * @param texN     normal del mapa, ya en [-1,1].
 * @param worldPos posición del fragmento en el espacio en el que se quiere el marco.
 * @param uv       coordenada de textura con la que se muestreó `texN`.
 */
vec3 harukaApplyNormalMap(vec3 N, vec3 texN, vec3 worldPos, vec2 uv) {
    vec3 dp1 = dFdx(worldPos), dp2 = dFdy(worldPos);
    vec2 du1 = dFdx(uv),       du2 = dFdy(uv);
    float det = du1.x * du2.y - du2.x * du1.y;
    // Sin área en UV no hay marco que deducir: devolver la geométrica es lo correcto, y evita
    // dividir por cero (que daría NaN y un píxel negro o blanco, no un fallo visible como tal).
    if (abs(det) < 1e-12) return N;
    vec3 T = normalize((dp1 * du2.y - dp2 * du1.y) / det);
    T = normalize(T - N * dot(N, T));            // Gram-Schmidt: T perpendicular a N
    vec3 B = cross(N, T);
    return normalize(mat3(T, B, N) * texN);
}

// =================================================================================================
// EL TERMINADOR Y EL COLOR DE SOMBRA: UNA SOLA DECISIÓN, CINCO SHADERS
// =================================================================================================
//
// ⚠️ ESTABAN COPIADOS CINCO VECES Y HABÍAN DIVERGIDO. Medido sobre los 105 shaders del proyecto:
//
//   final.frag              smoothstep(-0.03, 0.22)   sombra = vec3(0.40,0.46,0.60)  CONSTANTE
//   construction_inst.frag  smoothstep(-0.03, 0.22)   sombra = vec3(0.40,0.46,0.60)  CONSTANTE
//   planet.frag             smoothstep(-0.04, 0.24)   sombra = vec3(0.42,0.48,0.60)  CONSTANTE
//   Survival/prop.frag      smoothstep(-0.06, 0.26)   sombra = dos constantes por hemisferio
//   prop_inst.frag          smoothstep(-0.15, 0.55)   sombra = ambiente REAL
//
// Cuatro de cinco pintaban la sombra con un RGB fijo: no miraban el cielo ni la hora. Al atardecer
// todo lo sombreado seguía siendo el mismo gris azulado frío, y ese es exactamente el "color de
// sombra plano" que se veía. Y el terminador iba de 0,25 de ancho a 0,70: el mismo objeto con la
// misma luz daba un borde duro o suave según quién lo dibujara.
//
// Lo que NO se unifica, a propósito: el rim, el especular y el SSS de las hojas. Ahí las diferencias
// son de MATERIAL, no copias que se separaron — un muro de obra quiere el rim al 35 % y el terreno no
// tiene especular. Unificar eso sería borrar decisiones, no deduplicar.

/**
 * @brief El terminador cel: 0 en sombra, 1 a plena luz.
 *
 * Ancho 0,30 — el TÉRMINO MEDIO entre los cinco que había (0,25 · 0,28 · 0,32 · 0,70), y elegido
 * como tal: 0,25 daba un borde casi duro y 0,70 disolvía la banda hasta perder el estilo cel.
 *
 * @param ndl  dot(N, L) SIN saturar. El borde inferior es negativo a propósito: la banda empieza un
 *             poco antes del terminador geométrico, que es lo que evita la línea dura.
 */
float harukaToonBand(float ndl) {
    return smoothstep(-0.05, 0.25, ndl);
}

/**
 * @brief El tinte de la zona en sombra, derivado de la LUZ REAL en vez de una constante.
 *
 * Es la misma expresión que alimenta `uAmbient` en el terreno (`ambientStrength * (0.55,0.65,0.85)`),
 * más una fracción del sol que representa el rebote. Que las dos salgan de aquí es lo que hace que
 * suelo y props respondan IGUAL a la hora del día, en vez de que uno cambie y el otro no.
 *
 * @param ambientStrength  intensidad del ambiente del frame.
 * @param sunColor         color del sol del frame.
 */
vec3 harukaToonShadowTint(float ambientStrength, vec3 sunColor) {
    return ambientStrength * vec3(0.55, 0.65, 0.85) + 0.18 * sunColor;
}

#endif // HARUKA_SURFACE_SHADE_GLSL
