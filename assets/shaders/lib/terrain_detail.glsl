// ===========================================================================================
// DETALLE FINO DEL TERRENO — gemelo GLSL de `src/core/planet/terrain_detail.h`.
//
// La malla base tiene un vértice cada ~39 km. Todo lo que hay entre dos vértices lo pone esta
// función: la evalúa la GPU por vértice teselado y la CPU por consulta, y **tiene que dar
// exactamente lo mismo en los dos lados**. Si no, el suelo que se ve y el que se pisa son dos
// suelos distintos — el fallo que este motor ya pagó una vez.
//
// ⚠️ CUALQUIER CAMBIO VA EN LOS DOS FICHEROS A LA VEZ, y el orden de las operaciones importa: en
// float32 `(a*b)*c` y `a*(b*c)` no dan lo mismo. Las expresiones de aquí están escritas para
// coincidir literalmente con las del .h — no "simplificarlas".
// ===========================================================================================
#ifndef HARUKA_TERRAIN_DETAIL_GLSL
#define HARUKA_TERRAIN_DETAIL_GLSL

// Hash de una CELDA con aritmética ENTERA. Ver la nota larga del .h: la versión con `fract` sobre
// la coordenada en float NO puede funcionar a escala planetaria — en la octava fina la coordenada
// vale ~57 000 y `fract` conserva dos o tres dígitos, así que un ulp de diferencia daba un hash
// completamente distinto (medido: 39 saltos de hasta 41 m en 4096 muestras).
// `uint` envuelve módulo 2^32 igual que en C++, así que esto es bit-exacto en los dos lados.
float harukaDetailHash(ivec3 c) {
    uint h = uint(c.x) * 374761393u
           + uint(c.y) * 668265263u
           + uint(c.z) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return float(h >> 8) * (1.0 / 16777216.0);
}

// Celda separada en DOUBLE (para que el índice coincida en los bordes) y hasheada como ENTERO
// (para que el valor de esa celda sea el mismo bit). Hacen falta las dos cosas.
float harukaDetailNoise(dvec3 x) {
    dvec3 id = floor(x);
    ivec3 c  = ivec3(id);
    vec3  f  = vec3(x - id);
    vec3  u  = f * f * (3.0 - 2.0 * f);

    float a  = harukaDetailHash(c);
    float b  = harukaDetailHash(c + ivec3(1, 0, 0));
    float cc = harukaDetailHash(c + ivec3(0, 1, 0));
    float d  = harukaDetailHash(c + ivec3(1, 1, 0));
    float e  = harukaDetailHash(c + ivec3(0, 0, 1));
    float g  = harukaDetailHash(c + ivec3(1, 0, 1));
    float k  = harukaDetailHash(c + ivec3(0, 1, 1));
    float l  = harukaDetailHash(c + ivec3(1, 1, 1));

    // Escrito como `a + (b-a)*t`, NO como `mix`: el driver puede implementar mix con un FMA y eso
    // cambia el último bit respecto al C++.
    float x00 = a  + (b - a)  * u.x;
    float x10 = cc + (d - cc) * u.x;
    float x01 = e  + (g - e)  * u.x;
    float x11 = k  + (l - k)  * u.x;
    float y0  = x00 + (x10 - x00) * u.y;
    float y1  = x01 + (x11 - x01) * u.y;
    return y0 + (y1 - y0) * u.z;
}

// Cuánto detalle se deja pasar según la altura base. 1 = todo.
// Bajo el agua atenúa por profundidad (0 en la superficie → 1 a -200 m); ARRIBA, una banda de
// playa enciende el detalle de 0 en el nivel del mar a 1 a +5 m — sin ella la costa sería el borde
// recortado del ruido fino, una orilla pixeleada. Ver la nota larga del .h: la atenuación por
// |altura| dejaría el terreno plano, porque el campo de placas es casi binario.
float harukaSeaLevelAttenuation(float baseHeightM) {
    if (baseHeightM > 0.0) return clamp(baseHeightM * (1.0 / 5.0), 0.0, 1.0);
    return clamp(-baseHeightM * (1.0 / 200.0), 0.0, 1.0);
}

// Peso de una octava según lo fino que se vaya a dibujar. Criterio de Nyquist: hace falta al menos
// media longitud de onda por triángulo. Sin esto, una octava de 4,5 m evaluada en vértices separados
// 611 m —el mínimo de la malla del planeta— no se dibuja: se muestrea mal y HIERVE al mover la
// cámara. Ver la nota larga del .h.
float harukaOctaveWeight(float wavelengthM, float minFeatureM) {
    float t = wavelengthM / max(minFeatureM * 2.0, 1e-3);
    return clamp(t - 1.0, 0.0, 1.0);
}

// Detalle fino en METROS, sobre la altura que ya trae la malla base.
//   minFeatureM = tamaño del triángulo que va a llevar este vértice.
float harukaTerrainDetail(vec3 dir, float radius, float minFeatureM) {
    dvec3 p = dvec3(dir) * double(radius);
    float h = 0.0;
    h += (harukaDetailNoise(p * 0.00035LF) - 0.5) * 260.0 * harukaOctaveWeight(2857.0, minFeatureM);
    h += (harukaDetailNoise(p * 0.0016LF)  - 0.5) *  70.0 * harukaOctaveWeight( 625.0, minFeatureM);
    h += (harukaDetailNoise(p * 0.0090LF)  - 0.5) *  14.0 * harukaOctaveWeight( 111.0, minFeatureM);
    h += (harukaDetailNoise(p * 0.0450LF)  - 0.5) *   3.0 * harukaOctaveWeight(  22.0, minFeatureM);
    h += (harukaDetailNoise(p * 0.2200LF)  - 0.5) *   0.7 * harukaOctaveWeight(   4.5, minFeatureM);
    return h;
}

#endif // HARUKA_TERRAIN_DETAIL_GLSL
