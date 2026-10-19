#pragma once
/**
 * @file PxLod.h
 * @brief LA LEY DE DETALLE POR PÍXELES para cualquier cosa FINA y repetida (hierba, cables, pelo,
 *        ramitas, piedrecillas): cuántos elementos se quedan a una distancia, decidido por TAMAÑO EN
 *        PANTALLA, no por metros. GL-free; gemelo EXACTO de `assets/shaders/lib/px_lod.glsl`.
 *
 * ── POR QUÉ NO HAY RADIOS ────────────────────────────────────────────────────────────────────────
 *
 * "Ralear desde 8 m hasta el 10 % a los 60 m" son dos metros fijos que valen para un monitor, una GPU
 * y un FOV. El motor decide el detalle por TAMAÑO EN PANTALLA y deja que un controlador por ms mueva
 * el umbral (props: `m_propDetail`; terreno: `errorPx`; aquí: `LoadController`). La única entrada es
 * `thrPx`, la anchura mínima en píxeles que se admite para un elemento.
 *
 * ── LA LEY ───────────────────────────────────────────────────────────────────────────────────────
 *
 * Un elemento de anchura `w` a distancia `d` mide `w·focalPx/d` píxeles. Mientras mida ≥ `thrPx` se
 * dibujan TODOS. Más lejos se quedan `keep = d1/d` de ellos (d1 = donde mide justo `thrPx`) y los que
 * quedan se ENSANCHAN `1/keep`, así que cada uno sigue midiendo `thrPx` y la cobertura en pantalla
 * se conserva: el conjunto no se aclara al alejarse, se hace más basto. Ensanchar tiene tope
 * (`widenMax`): pasado él el elemento ya no se parece a sí mismo, y ahí acaba —con un fundido en el
 * último cuarto, para que no haya borde—. Ese final, `reachM`, SALE de `thrPx`: no es un parámetro.
 *
 * Los que se quedan son un PREFIJO del orden del grupo (`k < N·keep`), no un dado: si el umbral baja,
 * los que había siguen y entran otros; si sube, se van los últimos. Sin parpadeo.
 */
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Haruka::World::Props {

struct PxLod {
    float focalPx  = 935.0f;   ///< píxeles por radián en el centro: (alto/2) / tan(fovY/2)
    float widthM   = 0.018f;   ///< anchura media del elemento (m); 0.018 = una brizna
    float thrPx    = 1.0f;     ///< anchura mínima admitida en pantalla (px): el mando del detalle
    float widenMax = 3.5f;     ///< cuánto puede ensancharse un elemento raleado antes de acabar
};

/// Distancia a la que un elemento sin ensanchar mide `thrPx` (m). Dentro, todos.
inline float pxFullM(const PxLod& L) {
    return L.widthM * L.focalPx / std::max(L.thrPx, 1e-3f);
}
/// Donde acaba el conjunto (m): el elemento ensanchado al tope mide `thrPx`.
inline float pxReachM(const PxLod& L) { return L.widenMax * pxFullM(L); }

/// Ensanche del elemento a distancia `d` (1 = el de siempre).
inline float pxWiden(const PxLod& L, float d) {
    return std::clamp(d / pxFullM(L), 1.0f, L.widenMax);
}
/// Fracción de elementos que se quedan a distancia `d`, fundido del final incluido. No crece con `d`.
inline float pxKeep(const PxLod& L, float d) {
    const float reach = pxReachM(L);
    if (d >= reach) return 0.0f;
    const float keep = 1.0f / pxWiden(L, d);
    // smoothstep(reach, 0,75·reach, d), escrito a mano para ser el MISMO que el GLSL
    const float t = std::clamp((d - reach) / (0.75f * reach - reach), 0.0f, 1.0f);
    return keep * (t * t * (3.0f - 2.0f * t));
}
/// Cuántos elementos de un grupo de `perGroup` lanza la CPU si su punto más cercano está a `dNear`.
/// Cota superior de lo que el shader se queda en cualquier punto del grupo (keep no crece con d).
inline uint32_t pxGroupThreads(const PxLod& L, uint32_t perGroup, float dNear) {
    const float keep = pxKeep(L, std::max(dNear, 0.0f));
    return (uint32_t)std::min((double)perGroup, std::ceil((double)perGroup * (double)keep));
}
/// ¿Se queda el elemento `k` de `perGroup` a distancia `d`? (el prefijo)
inline bool pxKeeps(const PxLod& L, uint32_t perGroup, uint32_t k, float d) {
    return (float)k < (float)perGroup * pxKeep(L, d);
}

/// El detalle del controlador (0..1) → umbral en píxeles: geométrico entre `thrMax` (detalle 0,
/// corto y basto) y `thrMin` (detalle 1, largo y fino). Geométrico porque la cuenta de elementos va
/// como 1/thr²: un paso del controlador cambia el coste en la misma proporción en todo el recorrido.
inline float pxThrPx(float detail, float thrMin, float thrMax) {
    const float t = std::clamp(detail, 0.0f, 1.0f);
    return thrMax * std::pow(thrMin / thrMax, t);
}

} // namespace Haruka::World::Props
