/**
 * @file paint.glsl
 * @brief ACABADO PINTADO (estilo Arcane / fondos de anime): trazos de pincel pegados al objeto, que
 *        mueven la luz (el que llama la desplaza con el trazo), y el borde DESCUIDADO (la silueta se
 *        deshilacha donde acaban los trazos). No sabe qué pinta: el que llama da el punto
 *        en el marco del objeto, la dirección del trazo y el tamaño del pincel.
 *
 * ── POR QUÉ ──────────────────────────────────────────────────────────────────────────────────────
 * Una malla con luz continua se lee como plástico (globos), por buena que sea la luz. Un pintor no
 * hace degradados limpios: pinta con trazos que se montan unos sobre otros, y el borde de una nube
 * es donde el último trazo se quedó corto. (Se probó además posterizar la luz en 3-4 tonos: sobre una
 * superficie que cambia suave y en círculo, como una panza, cada frontera era un anillo.) Eso es lo que hace esto, en el espacio del OBJETO: los
 * trazos no nadan por la pantalla al moverse la cámara.
 */
#ifndef HARUKA_PAINT_GLSL
#define HARUKA_PAINT_GLSL

uint harukaPaintHash(ivec3 c, uint seed) {
    uint x = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ seed * 2654435761u;
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float harukaPaintValue(vec3 p, uint seed) {
    const vec3 i = floor(p), f = p - i;
    const vec3 w = f * f * (3.0 - 2.0 * f);
    const ivec3 c = ivec3(i);
    #define HV(dx, dy, dz) (float(harukaPaintHash(c + ivec3(dx, dy, dz), seed) & 0xFFFFu) / 65535.0)
    const float a = mix(mix(HV(0,0,0), HV(1,0,0), w.x), mix(HV(0,1,0), HV(1,1,0), w.x), w.y);
    const float b = mix(mix(HV(0,0,1), HV(1,0,1), w.x), mix(HV(0,1,1), HV(1,1,1), w.x), w.y);
    #undef HV
    return mix(a, b, w.z);
}

/**
 * @brief Trazos de pincel en [0,1]: ruido 3D ESTIRADO a lo largo de `along` (un trazo es 8 veces más
 * largo que ancho). UNA pasada: con dos cruzadas y trazos cortos (5:1) la nube parecía de pelo o de
 * moqueta, y costaba el doble por píxel.
 * El marco es FIJO por objeto (`along`, `upv` y su perpendicular): ⚠️ con el marco de la normal de la
 * superficie, la coordenada "a lo largo de la normal" crece en círculo sobre una cara plana y los
 * trazos salían en ANILLOS concéntricos (la panza de una nube parecía una ágata).
 * @param p      punto en el marco del objeto (m)
 * @param along  dirección del trazo (unitaria, se hace perpendicular a `upv`)
 * @param upv    la vertical del objeto (unitaria)
 * @param width  ancho del pincel (m)
 */
float harukaBrushRatio(vec3 p, vec3 along, vec3 upv, float width, float ratio, uint seed) {
    vec3 t = along - upv * dot(along, upv);
    t = dot(t, t) > 1e-6 ? normalize(t) : normalize(cross(upv, vec3(0.31, 0.0, 0.95)));
    const vec3 c = cross(t, upv);
    const vec3 q = vec3(dot(p, t) / (ratio * width), dot(p, upv) / width, dot(p, c) / width);
    return harukaPaintValue(q, seed);
}
float harukaBrush(vec3 p, vec3 along, vec3 upv, float width, uint seed) {
    return harukaBrushRatio(p, along, upv, width, 8.0, seed);
}

/**
 * @brief DÓNDE PINTA EL PINCEL (0 = color liso, 1 = trazo de lleno). Un pintor no texturiza el objeto
 * entero: pone trazos donde pasa algo y deja liso el resto. ⚠️ Con el trazo en toda la superficie por
 * igual se leía como una textura de la nube entera (Andoni: "los trazos se ven globales de la nube
 * entera, no localizados").
 * ⚠️ Se probaron también toques sueltos (un ruido de baja frecuencia del objeto): salían manchas
 * redondas moteadas, lamparones, no pinceladas.
 * @param l      la luz continua (0 sombra honda · 1 luz plena): el trazo vive en la FRONTERA luz/sombra
 * @param edge   0 de cara · 1 en la silueta: el contorno también se pinta a trazos
 */
float harukaPaintWhere(float l, float edge) {
    const float terminator = 1.0 - smoothstep(0.08, 0.30, abs(l - 0.5));
    const float rim = smoothstep(0.55, 0.90, edge);
    return clamp(max(terminator, 0.8 * rim), 0.0, 1.0);
}

/**
 * @brief ¿Se queda corto el pincel aquí? `edge` = 0 en el centro de la cara, 1 en la silueta. Devuelve
 * true si el píxel no se pinta: cerca del borde, donde el trazo es flojo.
 */
bool harukaPaintRagged(float edge, float brush, float amount) {
    return edge > 1.0 - amount * (0.25 + 0.75 * brush);
}

#endif // HARUKA_PAINT_GLSL
