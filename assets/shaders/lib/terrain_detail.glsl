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

// Igual que `harukaDetailNoise`, pero devuelve ADEMÁS el gradiente ∂n/∂x. GEMELO de
// `detailNoiseGrad` (core/planet/terrain_detail.h), verificado por `terrain_detail_gradient`.
//
// El ruido es una trilineal de 8 esquinas con pesos smoothstep: su derivada es EXACTA y sale de los
// mismos 8 hashes que ya se leen. Por eso el gradiente es casi gratis, y por eso permite quitar las
// dos evaluaciones extra que el tess eval hacía por vértice solo para la normal (§8: 15 → 5 ruidos).
float harukaDetailNoiseGrad(dvec3 x, out vec3 grad) {
    dvec3 id = floor(x);
    ivec3 c  = ivec3(id);
    vec3  f  = vec3(x - id);
    vec3  u  = f * f * (3.0 - 2.0 * f);
    vec3  du = 6.0 * f * (1.0 - f);        // d(smoothstep)/df

    float a  = harukaDetailHash(c);
    float b  = harukaDetailHash(c + ivec3(1, 0, 0));
    float cc = harukaDetailHash(c + ivec3(0, 1, 0));
    float d  = harukaDetailHash(c + ivec3(1, 1, 0));
    float e  = harukaDetailHash(c + ivec3(0, 0, 1));
    float g  = harukaDetailHash(c + ivec3(1, 0, 1));
    float k  = harukaDetailHash(c + ivec3(0, 1, 1));
    float l  = harukaDetailHash(c + ivec3(1, 1, 1));

    float x00 = a  + (b - a)  * u.x;
    float x10 = cc + (d - cc) * u.x;
    float x01 = e  + (g - e)  * u.x;
    float x11 = k  + (l - k)  * u.x;
    float y0  = x00 + (x10 - x00) * u.y;
    float y1  = x01 + (x11 - x01) * u.y;

    float dnx0 = (b - a) + ((d - cc) - (b - a)) * u.y;
    float dnx1 = (g - e) + ((l - k)  - (g - e)) * u.y;
    grad.x = (dnx0 + (dnx1 - dnx0) * u.z) * du.x;
    float dny0 = x10 - x00;
    float dny1 = x11 - x01;
    grad.y = (dny0 + (dny1 - dny0) * u.z) * du.y;
    grad.z = (y1 - y0) * du.z;

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
/**
 * @brief Detalle con la dirección en DOBLE. Gemelo de `terrainDetail(dvec3,...)` en el .h.
 *
 * ⚠️ La sobrecarga de abajo toma `vec3` y sube a double DENTRO: subir después de redondear no
 * recupera nada. Da igual mientras quien llame tenga la dirección en float (el clipmap la
 * reconstruye así), pero el quadtree del v5 la calcula EXACTA desde enteros y pasarla por una firma
 * `vec3` desperdiciaría justo eso. No cuesta más: `harukaDetailNoise` ya trabaja en double.
 */
float harukaTerrainDetail(dvec3 dir, double radius, float minFeatureM) {
    if (minFeatureM >= 1428.5) return 0.0;
    precise dvec3 p = dir * radius;
    float h = 0.0;
    if (minFeatureM < 1428.5) h += (harukaDetailNoise(p * 0.00035LF) - 0.5) * 260.0 * harukaOctaveWeight(2857.0, minFeatureM);
    if (minFeatureM <  312.5) h += (harukaDetailNoise(p * 0.0016LF)  - 0.5) *  70.0 * harukaOctaveWeight( 625.0, minFeatureM);
    if (minFeatureM <   55.5) h += (harukaDetailNoise(p * 0.0090LF)  - 0.5) *  14.0 * harukaOctaveWeight( 111.0, minFeatureM);
    if (minFeatureM <   11.0) h += (harukaDetailNoise(p * 0.0450LF)  - 0.5) *   3.0 * harukaOctaveWeight(  22.0, minFeatureM);
    if (minFeatureM <   2.25) h += (harukaDetailNoise(p * 0.2200LF)  - 0.5) *   0.7 * harukaOctaveWeight(   4.5, minFeatureM);
    return h;
}

float harukaTerrainDetail(vec3 dir, float radius, float minFeatureM) {
    // Early-out ANTES de pagar el primer ruido: si ni la octava más GRUESA (λ=2857 m) tiene
    // triángulos para ella, ninguna octava puede contribuir. `harukaOctaveWeight(2857, minFeatureM)`
    // es > 0 ⟺ minFeatureM < 2857/2 = 1428.5, así que el corte es EXACTAMENTE equivalente a lo que
    // multiplicaban los pesos antes (y la CPU aplica el mismo corte). Es el camino de todo el planeta
    // visto de lejos u órbita: `triM = camD·0.012` supera 1428 m a ~119 km de la cámara, y antes cada
    // vértice pagaba 5 ruidos de detalle para multiplicarlos por peso 0.
    if (minFeatureM >= 1428.5) return 0.0;
    dvec3 p = dvec3(dir) * double(radius);
    float h = 0.0;
    // El resto de octavas con la misma equivalencia: cada guarda replica su `octaveWeight(λ/2) > 0`.
    if (minFeatureM < 1428.5) h += (harukaDetailNoise(p * 0.00035LF) - 0.5) * 260.0 * harukaOctaveWeight(2857.0, minFeatureM);
    if (minFeatureM <  312.5) h += (harukaDetailNoise(p * 0.0016LF)  - 0.5) *  70.0 * harukaOctaveWeight( 625.0, minFeatureM);
    if (minFeatureM <   55.5) h += (harukaDetailNoise(p * 0.0090LF)  - 0.5) *  14.0 * harukaOctaveWeight( 111.0, minFeatureM);
    if (minFeatureM <   11.0) h += (harukaDetailNoise(p * 0.0450LF)  - 0.5) *   3.0 * harukaOctaveWeight(  22.0, minFeatureM);
    if (minFeatureM <    2.25) h += (harukaDetailNoise(p * 0.2200LF) - 0.5) *   0.7 * harukaOctaveWeight(   4.5, minFeatureM);
    return h;
}

// `harukaTerrainDetail` + su GRADIENTE en coordenadas de MUNDO (m de altura por m recorrido).
// GEMELO de `terrainDetailGrad` (core/planet/terrain_detail.h).
//
// Sustituye a las diferencias finitas del tess eval: la normal salía de evaluar el detalle en
// `dir + t1·eps` y `dir + t2·eps` — 3 evaluaciones (15 ruidos) por vértice para un dato que la
// función ya conoce. Con esto es 1 evaluación (5 ruidos) y las derivadas tangenciales son
// `dot(grad, t1)` y `dot(grad, t2)`.
//
// ⚠️ La línea de `h` va escrita EXACTAMENTE igual que en `harukaTerrainDetail`, mismo orden de
// operaciones. Agrupar `amp·peso` en una variable cambia el redondeo y rompe la paridad de §5.
float harukaTerrainDetailGrad(vec3 dir, float radius, float minFeatureM, out vec3 grad) {
    grad = vec3(0.0);
    if (minFeatureM >= 1428.5) return 0.0;
    dvec3 p = dvec3(dir) * double(radius);
    float h = 0.0;
    vec3 g;
    if (minFeatureM < 1428.5) { h += (harukaDetailNoiseGrad(p * 0.00035LF, g) - 0.5) * 260.0 * harukaOctaveWeight(2857.0, minFeatureM);
        grad += g * (260.0 * harukaOctaveWeight(2857.0, minFeatureM) * 0.00035); }
    if (minFeatureM <  312.5) { h += (harukaDetailNoiseGrad(p * 0.0016LF,  g) - 0.5) *  70.0 * harukaOctaveWeight( 625.0, minFeatureM);
        grad += g * ( 70.0 * harukaOctaveWeight( 625.0, minFeatureM) * 0.0016); }
    if (minFeatureM <   55.5) { h += (harukaDetailNoiseGrad(p * 0.0090LF,  g) - 0.5) *  14.0 * harukaOctaveWeight( 111.0, minFeatureM);
        grad += g * ( 14.0 * harukaOctaveWeight( 111.0, minFeatureM) * 0.0090); }
    if (minFeatureM <   11.0) { h += (harukaDetailNoiseGrad(p * 0.0450LF,  g) - 0.5) *   3.0 * harukaOctaveWeight(  22.0, minFeatureM);
        grad += g * (  3.0 * harukaOctaveWeight(  22.0, minFeatureM) * 0.0450); }
    if (minFeatureM <   2.25) { h += (harukaDetailNoiseGrad(p * 0.2200LF, g) - 0.5) *   0.7 * harukaOctaveWeight(   4.5, minFeatureM);
        grad += g * (  0.7 * harukaOctaveWeight(   4.5, minFeatureM) * 0.2200); }
    return h;
}

// UV equirectangular de una dirección, en la convención de TODOS los bakes del motor: polo norte en
// la fila 0, longitud 0 en la columna central (gema de `equirectUV` de biome.frag y de la inversa
// con la que se hornean los mapas, ver tools/gen_planet_zones.py). Es la proyección que rellena los
// mapas, así que leer un mapa con ella devuelve el valor del punto correcto. La CPU tiene la gemela
// exacta en terrain_detail.h.
vec2 harukaEquirectUV(vec3 dir) {
    vec3 d = normalize(dir);
    return vec2(0.5 + atan(d.z, d.x) * 0.1591549,
                0.5 - asin(clamp(d.y, -1.0, 1.0)) * 0.3183099);
}

// Bilineal A MANO del campo de altura horneado (R32F), gemela de la CPU en terrain_detail.h.
// Envuelve la longitud (borde ±π) y abraza los polos (la fila del polo se repite). texelFetch +
// aritmética en el MISMO orden que C++, o el suelo que se pisa y el que se ve dejarían de coincidir
// en centímetros: el bilineal del hardware usa pesos de precisión limitada (8 bits en varias GPU).
float harukaSampleHeightField(sampler2D tex, ivec2 size, vec2 uv) {
    vec2  fxy = uv * vec2(size) - 0.5;
    ivec2 i0  = ivec2(floor(fxy));
    // Sin `%`: el módulo entero de una variable se compila en algunos drivers como división entera
    // (lenta). Aquí solo hace falta un ajuste por lado, y con ramas (que el GPU ejecuta en paralelo
    // sin coste de divergencia a nivel de warp) se va igual que el `%` para uv ∈ [0,1].
    int x0 = i0.x; if (x0 < 0) x0 += size.x; if (x0 >= size.x) x0 -= size.x;
    int y0 = i0.y; y0 = clamp(y0, 0, size.y - 1);
    int x1 = x0 + 1; if (x1 >= size.x) x1 = 0;
    int y1 = y0 + 1; if (y1 >= size.y) y1 = y0;
    vec2  t   = fxy - vec2(i0);
    float h00 = texelFetch(tex, ivec2(x0, y0), 0).r;
    float h10 = texelFetch(tex, ivec2(x1, y0), 0).r;
    float h01 = texelFetch(tex, ivec2(x0, y1), 0).r;
    float h11 = texelFetch(tex, ivec2(x1, y1), 0).r;
    float a = h00 + (h10 - h00) * t.x;
    float b = h01 + (h11 - h01) * t.x;
    return a + (b - a) * t.y;
}

#endif // HARUKA_TERRAIN_DETAIL_GLSL
