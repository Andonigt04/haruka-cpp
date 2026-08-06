/**
 * @file terrain_detail.h
 * @brief Detalle fino del terreno: la MISMA función que evalúa la GPU al teselar.
 *
 * La malla base del planeta tiene un vértice cada ~39 km. Todo lo que hay entre dos vértices —las
 * ondulaciones, las lomas, los badenes— lo pone esta función, evaluada por vértice teselado en la
 * GPU y por consulta en la CPU. **Tiene que dar exactamente lo mismo en los dos lados**: si no, el
 * suelo que se ve y el que se pisa son dos suelos distintos, que es el fallo que este motor ya
 * pagó una vez ("había TRES suelos y por eso se veían dos terrenos").
 *
 * El gemelo en GLSL es `assets/shaders/lib/terrain_detail.glsl`. **Cualquier cambio va en los dos
 * ficheros a la vez**, y el orden de las operaciones importa: en float32 `(a*b)*c` y `a*(b*c)` no
 * dan lo mismo.
 *
 * ## Reglas de paridad (pagadas ya una vez, ver docs/HISTORIAL.md)
 *
 * - **Todo en `float`, no en `double`.** GLSL calcula en float32; usar double aquí daría un
 *   resultado "mejor" que no es el que dibuja la GPU, y la diferencia se vería como terreno que
 *   flota o hunde al jugador.
 * - **Sin `-ffast-math`.** Release compila con él y autoriza reasociar y fundir en FMA, así que la
 *   paridad se rompería **solo en Release** con los tests en verde. El fichero que use esto debe
 *   llevar `-fno-fast-math -ffp-contract=off`.
 * - **Ni raíces ni divisiones en el camino común** si se puede evitar: el `sqrt` de double del
 *   driver (Mesa/ACO) es ~1e-8, no IEEE. Aquí no hay ninguna, y conviene que siga así.
 */
#pragma once
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace Haruka { namespace Planet {

/**
 * @brief Hash de una CELDA a [0,1), con aritmética ENTERA.
 *
 * Entero y no float a propósito, y esta fue la lección cara de esta etapa. La versión anterior era
 * `fract(p * 0.3183099 + 0.1)` sobre la coordenada en float, y **no puede funcionar a escala
 * planetaria**: en la octava fina la coordenada vale ~57 000, y el ulp de float32 ahí es ~0,004, así
 * que `fract` de ese producto conserva dos o tres dígitos. Un ulp de diferencia entre CPU y GPU —que
 * lo hay siempre— daba un hash COMPLETAMENTE distinto. Medido: media 1,4 cm pero **39 saltos de
 * hasta 41 m** en 4096 muestras.
 *
 * ⚠️ Reducir la coordenada en double NO lo arreglaba, y se probó: el problema no era en qué celda
 * cae el punto, sino que el hash de una celda ya no era estable. La media y el peor caso salieron
 * IDÉNTICOS antes y después, que es lo que delató la hipótesis equivocada.
 *
 * Con enteros no hay redondeo: `uint` envuelve módulo 2³² igual en GLSL y en C++, así que las dos
 * implementaciones dan el mismo bit. El `>> 8` deja 24 bits, que es justo lo que float32 representa
 * exacto.
 */
inline float detailHash(const glm::ivec3& c) {
    uint32_t h = (uint32_t)c.x * 374761393u
               + (uint32_t)c.y * 668265263u
               + (uint32_t)c.z * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return (float)(h >> 8) * (1.0f / 16777216.0f);
}

/**
 * @brief Value noise 3D trilineal. La celda se separa en DOUBLE y se hashea como ENTERO.
 *
 * El double es para que el índice de celda coincida cuando el punto cae justo en el borde (1 ulp de
 * `dir` son ~0,38 m sobre la Tierra). El entero es para que el hash de esa celda sea el mismo bit
 * en los dos lados. Hacen falta las dos cosas: la primera sola no basta —está medido— y la segunda
 * sola dejaría saltos en las fronteras de celda.
 */
inline float detailNoise(const glm::dvec3& x) {
    const glm::dvec3 id = glm::floor(x);
    const glm::ivec3 c  = glm::ivec3(id);
    const glm::vec3  f  = glm::vec3(x - id);
    const glm::vec3  u  = f * f * (3.0f - 2.0f * f);

    const float a = detailHash(c);
    const float b = detailHash(c + glm::ivec3(1, 0, 0));
    const float cc= detailHash(c + glm::ivec3(0, 1, 0));
    const float d = detailHash(c + glm::ivec3(1, 1, 0));
    const float e = detailHash(c + glm::ivec3(0, 0, 1));
    const float g = detailHash(c + glm::ivec3(1, 0, 1));
    const float k = detailHash(c + glm::ivec3(0, 1, 1));
    const float l = detailHash(c + glm::ivec3(1, 1, 1));

    const float x00 = a  + (b - a)  * u.x;
    const float x10 = cc + (d - cc) * u.x;
    const float x01 = e  + (g - e)  * u.x;
    const float x11 = k  + (l - k)  * u.x;
    const float y0  = x00 + (x10 - x00) * u.y;
    const float y1  = x01 + (x11 - x01) * u.y;
    return y0 + (y1 - y0) * u.z;
}


/**
 * @brief Detalle fino en metros, con las octavas ATENUADAS por lo fino que se vaya a dibujar.
 *
 * @param dir          dirección UNITARIA desde el centro del planeta.
 * @param radius       radio al que se evalúa (base + altura de la malla), en metros.
 * @param minFeatureM  tamaño del triángulo que va a llevar este vértice, en metros.
 *
 * `minFeatureM` es lo que evita el aliasing. Una octava de 4,5 m evaluada en vértices separados
 * 611 m —el mínimo que puede dar la malla del planeta— no se dibuja: se muestrea mal y produce
 * ruido que hierve al mover la cámara. Cada octava entra solo cuando hay triángulos para ella, con
 * el criterio de Nyquist: hace falta al menos media longitud de onda por triángulo.
 *
 * ⚠️ La CPU pasa SIEMPRE el valor más fino (el del clipmap). La física solo importa donde está el
 * jugador, y ahí el clipmap da 2 m; si la CPU atenuara por distancia como el render, el suelo que
 * se pisa cambiaría según dónde mire la cámara.
 */
inline float octaveWeight(float wavelengthM, float minFeatureM) {
    // 1 cuando el triángulo es mucho más fino que la onda, 0 cuando no llega a media onda.
    const float t = wavelengthM / glm::max(minFeatureM * 2.0f, 1e-3f);
    return glm::clamp(t - 1.0f, 0.0f, 1.0f);
}

inline float terrainDetail(const glm::vec3& dir, float radius, float minFeatureM) {
    // Early-out idéntico al gemelo GLSL: si ni la octava más gruesa (λ=2857 m) tiene triángulos para
    // ella —`octaveWeight` es > 0 ⟺ minFeatureM < 1428.5— ninguna octava contribuye y el resultado es
    // 0 (lo mismo que sumar los términos con peso 0, sin pagar el ruido). La CPU de la física siempre
    // pasa minFeatureM=2.0, así que este camino no lo toca; es el render de lejos/orbita el que ahorra.
    if (minFeatureM >= 1428.5f) return 0.0f;
    const glm::dvec3 p = glm::dvec3(dir) * (double)radius;
    float h = 0.0f;
    // Guardas con la MISMA equivalencia que las de octaveWeight (λ/2), mismo corte que el .glsl.
    if (minFeatureM < 1428.5f) h += (detailNoise(p * 0.00035) - 0.5f) * 260.0f * octaveWeight(2857.0f, minFeatureM);
    if (minFeatureM <  312.5f) h += (detailNoise(p * 0.0016)  - 0.5f) *  70.0f * octaveWeight( 625.0f, minFeatureM);
    if (minFeatureM <   55.5f) h += (detailNoise(p * 0.0090)  - 0.5f) *  14.0f * octaveWeight( 111.0f, minFeatureM);
    if (minFeatureM <   11.0f) h += (detailNoise(p * 0.0450)  - 0.5f) *   3.0f * octaveWeight(  22.0f, minFeatureM);
    if (minFeatureM <    2.25f) h += (detailNoise(p * 0.2200) - 0.5f) *   0.7f * octaveWeight(   4.5f, minFeatureM);
    return h;
}

/** @brief Compatibilidad: todo el detalle, como lo evalúa la física. */
inline float terrainDetail(const glm::vec3& dir, float radius) {
    return terrainDetail(dir, radius, 2.0f);
}

/**
 * @brief Cuánto detalle se deja pasar según la altura base. 1 = todo.
 *
 * Bajo el agua atenúa por profundidad (0 en la superficie → 1 a -200 m). Y ARRIBA, una banda de
 * playa: el detalle se enciende de 0 en el nivel del mar a 1 a +5 m. Sin esa banda, la costa es el
 * borde recortado del ruido fino (octavas de 4,5 y 22 m) — una orilla "pixeleada" que el agua
 * transparente recorta en escalones. Con la banda, la línea de costa sigue la retícula base (curva
 * limpia) y la orilla se ve como arena mojada plana.
 *
 * El detalle base vale ±151 m mientras el recorte de zona separa mar y tierra por solo ±40 m: sin
 * atenuar bajo el agua, en una zona pintada de agua asomaba terreno por encima del mar — el mapa
 * decía una cosa y la geometría otra.
 */
inline float seaLevelAttenuation(float baseHeightM) {
    if (baseHeightM > 0.0f) return glm::clamp(baseHeightM * (1.0f / 5.0f), 0.0f, 1.0f);
    return glm::clamp(-baseHeightM * (1.0f / 200.0f), 0.0f, 1.0f);     // agua: por profundidad
}

/**
 * @brief UV equirectangular de una dirección: convención de TODOS los bakes (norte en la fila 0).
 *
 * Gemela exacta de `harukaEquirectUV` del .glsl (mismos literales, mismo orden). Es la proyección
 * que rellena los mapas horneados — lee un mapa con ella y obtienes el valor del punto correcto.
 */
inline glm::vec2 equirectUV(const glm::vec3& dir) {
    const glm::vec3 d = glm::normalize(dir);
    return glm::vec2(0.5f + std::atan2(d.z, d.x) * 0.1591549f,
                     0.5f - std::asin(glm::clamp(d.y, -1.0f, 1.0f)) * 0.3183099f);
}

/**
 * @brief Bilineal a mano de un campo de altura horneado (R32F), gemela de la GPU.
 *
 * Envuelve la longitud (borde ±π) y abraza los polos (la fila del polo se repite). El orden de las
 * operaciones es el MISMO que `harukaSampleHeightField` del .glsl: la física muestrea este campo con
 * la misma cuenta que el render, o el suelo que se pisa y el que se ve divergirían en centímetros.
 */
inline float sampleHeightField(const glm::vec2& uv, int w, int h, const float* field) {
    const glm::vec2  fxy = uv * glm::vec2((float)w, (float)h) - 0.5f;
    const glm::ivec2 i0  = glm::ivec2(glm::floor(fxy));
    // Sin `%`: mismo ajuste por lado que el gemelo GLSL (que evita la división entera por drivers).
    // Para uv ∈ [0,1] el resultado es idéntico al módulo.
    int x0 = i0.x; if (x0 < 0) x0 += w; if (x0 >= w) x0 -= w;
    int y0 = i0.y; y0 = glm::clamp(y0, 0, h - 1);
    int x1 = x0 + 1; if (x1 >= w) x1 = 0;
    int y1 = y0 + 1; if (y1 >= h) y1 = y0;
    const glm::vec2 t = fxy - glm::vec2(i0);
    const float h00 = field[(size_t)y0 * (size_t)w + (size_t)x0];
    const float h10 = field[(size_t)y0 * (size_t)w + (size_t)x1];
    const float h01 = field[(size_t)y1 * (size_t)w + (size_t)x0];
    const float h11 = field[(size_t)y1 * (size_t)w + (size_t)x1];
    const float a = h00 + (h10 - h00) * t.x;
    const float b = h01 + (h11 - h01) * t.x;
    return a + (b - a) * t.y;
}

}} // namespace Haruka::Planet
