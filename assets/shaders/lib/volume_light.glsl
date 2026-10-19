/**
 * @file volume_light.glsl
 * @brief LUZ DE UN VOLUMEN QUE DISPERSA, sobre la superficie de una malla que lo representa. La malla
 *        da la SILUETA (nítida, como pintada); la luz sale de un volumen analítico que la envuelve
 *        (un elipsoide con un eje en la vertical): cuánto Sol atraviesa el cuerpo hasta este punto,
 *        cuánto cielo le llega desde arriba y cuánto se ve el Sol a través del borde fino.
 *
 * No sabe de nubes: un volumen es "envolvente (centro, semieje horizontal, semieje vertical) y
 * opacidad". Lo usan los cúmulos (bultos de `ShellScatter`), y valdría para humo, niebla en bancos o
 * espuma: cualquier cosa hecha de bultos que se ilumina como un medio y no como una piedra.
 *
 * ── POR QUÉ NO EL TOON ───────────────────────────────────────────────────────────────────────────
 * Con el toon cada bulto se ilumina SOLO por su normal: cada esfera tiene su lado claro y su lado
 * oscuro, todas iguales, y el racimo se lee como un montón de bolas. En una nube la luz depende de
 * CUÁNTO cuerpo hay entre el punto y el Sol: la cara al Sol del racimo entero es blanca, la contraria
 * se apaga en un degradado ancho hacia el azul del cielo, y la panza es gris azulado. La normal del
 * bulto solo modula eso (cada bulto conserva su volumen, como en una pintura).
 *
 * Todo en el espacio del pase (cámara-relativo, el ojo en el origen). Unidades del volumen: la
 * longitud de un camino se mide en el espacio ESCALADO del elipsoide (0..2 = cruzarlo entero).
 */
#ifndef HARUKA_VOLUME_LIGHT_GLSL
#define HARUKA_VOLUME_LIGHT_GLSL

#include "lib/sky_light.glsl"

/// La envolvente: centro (pase), semiejes horizontal y vertical (m), vertical `up`.
struct HarukaVolume {
    vec3  center;
    float rh;
    float rv;
    vec3  up;
};

/// Lo que cambia de un volumen a otro.
struct HarukaVolumeParams {
    float opacity;       ///< espesor óptico por unidad de camino escalado (cruzarlo entero = 2·opacity)
    float multi;         ///< parte de la luz que llega por dispersión múltiple (no se apaga tan rápido)
    float wrap;          ///< cuánto rodea la luz a cada bulto (0 = lambert, 1 = media esfera más)
    float translucency;  ///< el Sol a través del borde fino a contraluz (el ribete plateado)
    /// Cuánto manda la normal de CADA BULTO frente a la del volumen (0 = la luz solo sabe del cuerpo
    /// entero; 1 = cada bulto con su lado claro y su lado oscuro). Alta, el objeto se lee como un
    /// montón de globos; baja, como una sola masa.
    float shape;
};

/// Punto/dirección del pase → espacio escalado del elipsoide (esfera unidad).
vec3 harukaVolumeScale(HarukaVolume v, vec3 q) {
    const float y = dot(q, v.up);
    return (q - v.up * y) / v.rh + v.up * (y / v.rv);
}

/// Largo (escalado) del tramo del rayo p + t·d, t ≥ 0, dentro de la envolvente. `d` unitario.
float harukaVolumePath(HarukaVolume v, vec3 p, vec3 d) {
    const vec3 o = harukaVolumeScale(v, p - v.center);
    const vec3 r = harukaVolumeScale(v, d);
    const float a = dot(r, r), b = dot(o, r), c = dot(o, o) - 1.0;
    const float disc = b * b - a * c;
    if (disc <= 0.0) return 0.0;
    const float s = sqrt(disc);
    const float t0 = max((-b - s) / a, 0.0), t1 = (-b + s) / a;
    return max(t1 - t0, 0.0) * sqrt(a);   // largo en el espacio escalado
}

/// Transmitancia con dispersión múltiple barata: una exponencial dura más una larga (la luz que
/// rebota dentro y sale por donde la simple ya no llega). Sin la larga, la cara a la sombra es negra.
float harukaVolumeTransmit(float path, HarukaVolumeParams p) {
    const float tau = p.opacity * path;
    return mix(exp(-tau), exp(-0.25 * tau), p.multi);
}

/**
 * @brief Irradiancia (lineal, expuesta) que recibe el punto `p` de normal `N`, visto desde `V`.
 * Devuelve E para `harukaLit(albedo, E)`.
 */
vec3 harukaVolumeLight(HarukaVolume v, vec3 p, vec3 N, vec3 V, vec3 L, HarukaVolumeParams prm) {
    // EL SOL: lo que atraviesa el cuerpo hasta aquí, por lo que rodea al bulto.
    const float wrap = clamp((dot(N, L) + prm.wrap) / (1.0 + prm.wrap), 0.0, 1.0);
    const float body = harukaVolumeTransmit(harukaVolumePath(v, p, L), prm);
    const float lobe = wrap * wrap * (3.0 - 2.0 * wrap);
    vec3 E = uSkySunGround.rgb * body * mix(1.0, 0.35 + 0.65 * lobe, prm.shape);
    E += uSkyMoonGround.rgb * smoothstep(0.0, 0.6, dot(N, uSkyMoonDir.xyz)) * body;

    // EL CIELO: el de arriba entra por la cima y se apaga hacia la panza (la panza es GRIS AZULADO,
    // no negra: la ilumina el cielo de los lados y el suelo de abajo).
    const float sky = harukaVolumeTransmit(harukaVolumePath(v, p, v.up), prm);
    E += harukaSkyAmbient(normalize(mix(v.up, N, prm.shape)), v.up) * mix(0.45, 1.0, sky);

    // EL RIBETE: con el Sol detrás, el borde fino del racimo (poco cuerpo a lo largo de la vista)
    // deja pasar la luz hacia delante. Fino = el tramo de la VISTA por la envolvente, no la normal
    // del bulto: así brilla el borde de la nube, no el de cada bola.
    if (prm.translucency > 0.0) {
        const float fwd  = pow(max(dot(-V, L), 0.0), 6.0);
        const float thin = exp(-1.5 * prm.opacity * harukaVolumePath(v, p, -V));
        E += uSkySunGround.rgb * (prm.translucency * fwd * thin);
    }
    return E;
}

#endif // HARUKA_VOLUME_LIGHT_GLSL
