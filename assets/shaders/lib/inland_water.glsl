/**
 * @file inland_water.glsl
 * @brief AGUA INTERIOR (ríos y lagos) como CAMPO, para que la dibuje la MISMA superficie del mar.
 *
 * ⚠️ ESTA ES LA LECCIÓN CARA. Había tres sistemas dibujando agua —la esfera del océano, una lámina
 * de 96×96 con celdas de 4,4 m y un composite de partículas en espacio de pantalla—, cada uno con su
 * malla, su shader y su propia respuesta a "¿aquí hay agua?". Ninguna coincidía con las otras, y
 * apagar una no quitaba el agua porque siempre quedaba otra. Se borraron las dos malas.
 *
 * Lo que queda es UNA superficie (la del mar) y UNA pregunta:
 *
 *     profundidad = nivel del agua − cota del suelo
 *
 * Para el mar el nivel es 0 (el campo de alturas ya viene desplazado para que el nivel del mar sea
 * la cota 0). Para un lago o un río, el nivel lo publica la simulación de aguas someras en el SSBO
 * de aquí. El resto —olas con bajío, rompiente, espuma, absorción, y dónde acaba el agua— sale igual
 * para los tres orígenes, porque es el mismo código leyendo el mismo campo.
 *
 * El parche de la sim es local (420 m alrededor del jugador): fuera de él no hay agua interior y el
 * campo devuelve su centinela, así que el mar se queda con su 0 y no cambia nada.
 */
#ifndef HARUKA_INLAND_WATER_GLSL
#define HARUKA_INLAND_WATER_GLSL

layout(std140, binding = 24) uniform InlandParams {
    vec4 uInlandAnchor;   // xyz = ancla del parche relativa al OJO
    vec4 uInlandTanU;     // xyz = eje +X del parche (unitario, en el plano tangente)
    vec4 uInlandTanV;     // xyz = eje +Z del parche
    vec4 uInlandMisc;     // x = lado del parche (m) · y = celdas por lado · w = 1 si hay campo
};
layout(std430, binding = 25) readonly buffer InlandWater { float uInlandSurface[]; };

/// Centinela de "sin agua interior aquí". Gemelo de `TerrestrialPlanet::kNoInlandWater`.
const float HARUKA_NO_INLAND = -1e9;

/**
 * @brief Cota de la superficie del agua interior en un punto, o el centinela si no hay.
 * @param posRelEye  posición del punto RELATIVA AL OJO (el mismo marco que el ancla).
 *
 * Bilineal a mano, como el resto de campos del terreno: el filtrado del hardware usa pesos de 8 bits
 * en varias GPU y aquí eso se vería como escalones en la lámina de un lago quieto.
 */
float harukaInlandWaterAt(vec3 posRelEye) {
    if (uInlandMisc.w < 0.5) return HARUKA_NO_INLAND;
    int   n    = int(uInlandMisc.y);
    float span = uInlandMisc.x;
    if (n <= 1 || span <= 0.0) return HARUKA_NO_INLAND;

    // Del mundo al parche: proyección sobre sus dos ejes tangentes, en metros desde el ancla.
    vec3  rel = posRelEye - uInlandAnchor.xyz;
    float u   = dot(rel, uInlandTanU.xyz);
    float v   = dot(rel, uInlandTanV.xyz);
    // Celdas: el parche va de -span/2 a +span/2 y su celda mide span/(n-1).
    float fx = (u + span * 0.5) / span * float(n - 1);
    float fy = (v + span * 0.5) / span * float(n - 1);
    if (fx < 0.0 || fy < 0.0 || fx > float(n - 1) || fy > float(n - 1)) return HARUKA_NO_INLAND;

    int   x0 = int(floor(fx)), y0 = int(floor(fy));
    int   x1 = min(x0 + 1, n - 1), y1 = min(y0 + 1, n - 1);
    float tx = fx - float(x0), ty = fy - float(y0);
    float s00 = uInlandSurface[y0 * n + x0], s10 = uInlandSurface[y0 * n + x1];
    float s01 = uInlandSurface[y1 * n + x0], s11 = uInlandSurface[y1 * n + x1];
    // ⚠️ Si CUALQUIER esquina está seca, no se interpola: mezclar una cota real con el centinela
    // daría una lámina hundiéndose hacia -1e9 en el borde del lago. El borde lo decide luego la
    // profundidad (agua donde el nivel supera al suelo), que es un criterio continuo de verdad.
    if (s00 <= HARUKA_NO_INLAND * 0.5 || s10 <= HARUKA_NO_INLAND * 0.5 ||
        s01 <= HARUKA_NO_INLAND * 0.5 || s11 <= HARUKA_NO_INLAND * 0.5) return HARUKA_NO_INLAND;
    float a = s00 + (s10 - s00) * tx;
    float b = s01 + (s11 - s01) * tx;
    return a + (b - a) * ty;
}

#endif // HARUKA_INLAND_WATER_GLSL
