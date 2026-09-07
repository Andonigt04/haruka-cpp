/**
 * @file lib/sky_clouds.glsl
 * @brief El CAMPO de las nubes pintadas del cielo: hash, value-noise, fBm y `cloudField`.
 *
 * ⚠️ ESTO VIVIA DENTRO DE `sky.frag` Y POR ESO NO SE PODIA MEDIR. El cielo se tapa muy por debajo de
 * lo que pide el clima —medido: cobertura pedida 0,80 -> **6,6 %** de cielo tapado al cenit— y el
 * umbral que lo decide (`lo = mix(0.70, 0.32, cloudiness)`) solo tiene sentido contra la DISTRIBUCION
 * de `cloudField`. Con la funcion encerrada en un fragment con `main`, ningun banco podia llamarla:
 * la unica salida era estimar el rango del fBm a ojo, que es adivinar.
 *
 * Sacado a una lib para que `cloud_field_probe.comp` evalue EXACTAMENTE la misma funcion que dibuja.
 */
#ifndef HARUKA_SKY_CLOUDS_GLSL
#define HARUKA_SKY_CLOUDS_GLSL

// Hash 3D barato para el campo de estrellas.
float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

// Value-noise + fBm para las NUBES pintadas (anime).
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash13(vec3(i, 0.0));
    float b = hash13(vec3(i + vec2(1.0, 0.0), 0.0));
    float c = hash13(vec3(i + vec2(0.0, 1.0), 0.0));
    float d = hash13(vec3(i + vec2(1.0, 1.0), 0.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) { v += a * vnoise(p); p *= 2.0; a *= 0.5; }
    return v;
}

/**
 * @brief El mismo fBm, pero SIN las octavas que el pixel no puede resolver.
 *
 * ⚠️ ESTO HIZO FALTA AL SUBIR `scale`. `vnoise` es value-noise sobre una reticula ENTERA: cuando una
 * octava mide menos de un par de pixeles, lo que se ve no es ruido sino la RETICULA — manchas
 * rectangulares. Al poner el tamaño de nube por su escala fisica (el altocumulo paso de elementos de
 * 4,8 km a 1,2 km) la frecuencia subio 4x y cerca del horizonte, donde ademas la coordenada se
 * comprime, el cielo salio con "cuadrados deformes". Reportado mirando la pantalla, no por el banco.
 *
 * Es el mismo argumento de Nyquist que ya aplican `oceanShortWaveFade` a los trenes de ola y
 * `octaveWeight` al relieve: una octava que no se puede muestrear no aporta detalle, aporta alias.
 *
 * ⚠️ Y SE DESVANECE HACIA SU MEDIA, NO HACIA CERO. Apagar la octava a cero le baja la media al campo,
 * y la media es justo lo que calibra el umbral (`cloudField` tiene media 0,398, medida con
 * `cloud_field_probe`): el cielo lejano se quedaria sin nubes por un cambio que solo pretendia quitar
 * el aliasing. Llevandola a su media (0,5) la varianza cae —que es lo que se quiere, lejos se ve
 * liso— y la media no se mueve ni un bit.
 *
 * @param px  lado del pixel MEDIDO EN LAS UNIDADES DE `p`. Cero = sin dato (una sonda de compute,
 *            donde no hay derivadas de pantalla): entonces no se apaga nada.
 */
float fbmAA(vec2 p, float px) {
    float v = 0.0, a = 0.5, f = 1.0;
    for (int i = 0; i < 5; ++i) {
        // La octava `i` tiene longitud `1/f`. Se desvanece cuando esa longitud baja de ~3 pixeles.
        const float w = clamp(1.5 - 3.0 * px * f, 0.0, 1.0);
        v += a * mix(0.5, vnoise(p * f), w);
        a *= 0.5; f *= 2.0;
    }
    return v;
}

// Densidad de nube: DOMAIN WARP (deforma las coords con otro ruido → formas orgánicas billowy, no blobs) +
// erosión de detalle en los bordes (desgarrados). `wind` = deriva temporal.
float cloudField(vec2 p, vec2 wind, float px) {
    // ⚠️ EL FOOTPRINT SE ESCALA CON CADA LLAMADA. Las tres capas de ruido miran `p` a 0,6, 1,3 y 4,0:
    // si se les pasara el mismo `px`, la de detalle (x4) seguiria aliaseando con las otras dos ya
    // limpias — y es justo la que mas alias produce.
    vec2  warp = vec2(fbmAA(p * 0.6 + wind * 0.5,       px * 0.6),
                      fbmAA(p * 0.6 + 5.2 - wind * 0.5, px * 0.6));
    float d    = fbmAA(p * 1.3 + warp * 1.4 + wind,     px * 1.3);
    d -= 0.20 * fbmAA(p * 4.0 - wind * 2.0,             px * 4.0);
    return d;
}
/// Sobrecarga sin footprint: para las sondas de compute, donde no hay derivadas de pantalla.
float cloudField(vec2 p, vec2 wind) { return cloudField(p, wind, 0.0); }


#endif // HARUKA_SKY_CLOUDS_GLSL
