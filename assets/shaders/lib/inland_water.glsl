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

/// ── LOS LAGOS DEL MUNDO, HORNEADOS CON EL PLANETA (2026-09-01) ─────────────────────────────────
///
/// Cota de la lámina por téxel equirect, misma retícula que `uHeightTex`. Sale de `bakeWaterMap`,
/// que rellena las cuencas del planeta ENTERO hasta su punto de derrame una sola vez.
///
/// ⚠️ ESTO ES LO QUE ARREGLA LAS TRES LIMITACIONES DEL PARCHE, y la cabecera de arriba las cuenta
/// como si fueran inevitables: *"el parche de la sim es local (420 m alrededor del jugador): fuera
/// de él no hay agua interior"*. Ya no. Fuera del parche manda este campo, que existe en todo el
/// planeta, no depende del observador —así que dos clientes ven los mismos lagos— y no tiene un
/// BORDE por el que el agua se fugue, que era lo que hacía aparecer un lago al caminar.
///
/// El parche NO desaparece: sigue siendo quien manda donde está, porque es el que lleva lo DINÁMICO
/// (lluvia, caudal, una presa que revienta). Este campo es el suelo sobre el que se apoya.
/// R = cota de la lámina · G = FETCH (diámetro equivalente de la masa de agua, m). El fetch va en la
/// MISMA textura y no en otra porque los dos salen del mismo relleno y se leen en el mismo téxel:
/// separarlos abriría la puerta a que un punto tuviera el nivel de un lago y el fetch de otro.
layout(binding = 18) uniform sampler2D uLakeTex;
/// Gemelo de `Haruka::Planet::WATER_FILL_DRY`. Cualquier cosa por debajo es "aquí no hay lago".
const float HARUKA_LAKE_DRY = -1.0e30;

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

/**
 * @brief Cota de la lámina del LAGO HORNEADO en esa dirección, o el centinela si aquí no hay.
 *
 * ⚠️ NEAREST, NO BILINEAL, y es lo contrario de lo que hace el resto de campos de este motor. La
 * lámina de un lago es PLANA y su borde es un ESCALÓN contra la orilla: interpolar entre "hay lago a
 * 40 m" y "no hay lago" inventaría una rampa de agua subiendo por la ladera. Gemelo exacto de
 * `TerrestrialPlanet::lakeLevelAt`, que lo muestrea igual por la misma razón.
 */
float harukaBakedLakeAt(vec3 dir) {
    ivec2 sz = textureSize(uLakeTex, 0);
    if (sz.x <= 1 || sz.y <= 1) return HARUKA_NO_INLAND;
    vec2  uv = harukaEquirectUV(dir);
    ivec2 t  = ivec2(floor(uv * vec2(sz)));
    t.x = ((t.x % sz.x) + sz.x) % sz.x;          // longitud envuelve
    t.y = clamp(t.y, 0, sz.y - 1);               // latitud no
    float lv = texelFetch(uLakeTex, t, 0).r;
    return (lv > HARUKA_LAKE_DRY) ? lv : HARUKA_NO_INLAND;
}

/**
 * @brief FETCH en ese punto: cuánto recorrido de agua abierta hay para que el viento levante ola.
 *
 * En el océano devuelve el centinela de "ilimitado" y el oleaje no se toca. En un lago devuelve su
 * diámetro equivalente, y con él `harukaFetchFactor` baja la ola a lo que ese lago puede sostener —
 * sin esto, un lago de 400 m tiene el swell del Atlántico. Ver `water_fill.h`.
 */
float harukaBakedFetchAt(vec3 dir) {
    ivec2 sz = textureSize(uLakeTex, 0);
    if (sz.x <= 1 || sz.y <= 1) return HARUKA_FETCH_UNLIMITED;
    vec2  uv = harukaEquirectUV(dir);
    ivec2 t  = ivec2(floor(uv * vec2(sz)));
    t.x = ((t.x % sz.x) + sz.x) % sz.x;
    t.y = clamp(t.y, 0, sz.y - 1);
    float f = texelFetch(uLakeTex, t, 0).g;
    return (f > 0.0) ? f : HARUKA_FETCH_UNLIMITED;
}

/// ── LA VENTANA FINA DE LAGOS (2026-09-01) ──────────────────────────────────────────────────────
///
/// ⚠️ `uLakeTex` NO PUEDE TENER LAGOS y esta medido: es una equirect de 512, o sea **78,2 km por
/// texel** en la Tierra. Una cuenca menor que eso no existe en el — da mares interiores. Esta es la
/// segunda mitad: un recuadro de ±8 km alrededor del observador, inundado sobre el terreno de verdad
/// a 62 m/texel, con el campo global de condicion de contorno (ver `core/planet/water_fill.h`).
///
/// ⚠️ ESTA AQUI Y NO SOLO EN LA FISICA POR UNA RAZON QUE ESTE MOTOR YA HA PAGADO TRES VECES: si la
/// fisica nadara en un lago que el render no dibuja, seria otra respuesta mas a "¿aqui hay agua?".
/// `TerrestrialPlanet::lakeWindowLevelAt` es el gemelo EXACTO de lo de abajo.
layout(binding = 19) uniform sampler2D uLakeWinTex;
layout(std140, binding = 26) uniform LakeWindowParams {
    /// xy = centro (lon, lat) en radianes · zw = 1/(2·medio lado) para pasar a UV.
    vec4 uLakeWin;
};

/// UV del punto dentro del recuadro, o fuera de [0,1) si no cae en el.
vec2 harukaLakeWinUV(vec3 dir) {
    vec3  n   = normalize(dir);
    float lat = asin(clamp(n.y, -1.0, 1.0));
    float lon = atan(n.z, n.x);
    float dLon = lon - uLakeWin.x;
    // La longitud envuelve: sin esto, un recuadro a caballo de ±π rechaza la mitad de si mismo.
    dLon -= 6.28318530717958647692 * floor(dLon / 6.28318530717958647692 + 0.5);
    return vec2(dLon * uLakeWin.z + 0.5, (lat - uLakeWin.y) * uLakeWin.w + 0.5);
}

float harukaWindowLakeAt(vec3 dir) {
    ivec2 sz = textureSize(uLakeWinTex, 0);
    if (sz.x <= 1 || sz.y <= 1) return HARUKA_NO_INLAND;   // 1x1 seco = no hay ventana
    vec2 uv = harukaLakeWinUV(dir);
    if (uv.x < 0.0 || uv.x >= 1.0 || uv.y < 0.0 || uv.y >= 1.0) return HARUKA_NO_INLAND;
    ivec2 t = clamp(ivec2(floor(uv * vec2(sz))), ivec2(0), sz - 1);
    float lv = texelFetch(uLakeWinTex, t, 0).r;
    return (lv > HARUKA_LAKE_DRY) ? lv : HARUKA_NO_INLAND;
}

float harukaWindowFetchAt(vec3 dir) {
    ivec2 sz = textureSize(uLakeWinTex, 0);
    if (sz.x <= 1 || sz.y <= 1) return HARUKA_FETCH_UNLIMITED;
    vec2 uv = harukaLakeWinUV(dir);
    if (uv.x < 0.0 || uv.x >= 1.0 || uv.y < 0.0 || uv.y >= 1.0) return HARUKA_FETCH_UNLIMITED;
    ivec2 t = clamp(ivec2(floor(uv * vec2(sz))), ivec2(0), sz - 1);
    float f = texelFetch(uLakeWinTex, t, 0).g;
    return (f > 0.0) ? f : HARUKA_FETCH_UNLIMITED;
}

/// EL FETCH de un punto, con la ventana por delante. Gemelo de `TerrestrialPlanet::lakeFetchAt`:
/// donde la ventana ve lamina, su fetch es el bueno — mide el lago DE VERDAD y no el texel de 78 km
/// que el campo grueso confunde con un mar interior.
float harukaWaterFetchAt(vec3 dir) {
    if (harukaWindowLakeAt(dir) > HARUKA_NO_INLAND) return harukaWindowFetchAt(dir);
    return harukaBakedFetchAt(dir);
}

/**
 * @brief LA cota del agua en un punto: mar, lago horneado, ventana fina o parche dinámico. Una sola
 *        respuesta.
 *
 * El orden importa y es el de "quién sabe más": el parche gana donde existe porque lleva lo dinámico
 * (una crecida, una presa), la ventana fina sabe más que el campo global del mismo sitio (62 m/téxel
 * contra 78 km), el campo global cubre el resto del planeta, y el mar es el suelo. `max` de los
 * cuatro, con centinelas por debajo de todo, hace exactamente eso sin ramas.
 */
float harukaWaterLevelAt(vec3 posRelEye, vec3 dir, float seaLevelM) {
    return max(max(seaLevelM, harukaInlandWaterAt(posRelEye)),
               max(harukaBakedLakeAt(dir), harukaWindowLakeAt(dir)));
}

#endif // HARUKA_INLAND_WATER_GLSL
