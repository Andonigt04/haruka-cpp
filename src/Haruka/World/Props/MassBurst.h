#pragma once
/**
 * @file MassBurst.h
 * @brief LA CAPA MASIVA: un BROTE por celda en vez de N copias de una mata. GL-free.
 *
 * ── POR QUÉ ──────────────────────────────────────────────────────────────────────────────────────
 *
 * Las capas libres de relleno (hierba, flores, líquenes) emitían `perCell × halo` instancias por
 * celda — hasta 150 matas de hierba cerca de la cámara — y cada una costaba un `ScatteredProp`
 * (80 B) + un `InstancedObject` (56 B) + una instancia GPU (40 B), más el barrido, el cull y la
 * subida POR MATA y POR FRAME. Eso era el techo de "generar en masa".
 *
 * Pero el scatter ya decidía esas matas sin información propia: la cota de la PRIMERA para todas
 * (a 12 m la diferencia es la del quad) y la posición, escala, giro y tinte salían del HASH de la
 * celda. O sea, eran una función pura de (celda, k). Así que la CPU guarda UN registro por celda
 * (`MassBurst`, ~48 B en GPU) y la GPU reconstruye cada mata en el vertex shader con la MISMA
 * función. 150 matas: de ~26 KB a 48 B.
 *
 * ── GEMELO CPU ↔ GPU ─────────────────────────────────────────────────────────────────────────────
 *
 * `massElement` y `assets/shaders/lib/mass_burst.glsl` son la MISMA aritmética: hash entero de 32
 * bits (idéntico en los dos lados) y operaciones float simples. La CPU la necesita para lo que la
 * GPU no puede contestar: qué matas caen dentro de una explosión, de una pisada o de un corte — el
 * estado de cada mata se decide aquí y la GPU solo lo lee.
 *
 * ── ESTADO POR MATA ──────────────────────────────────────────────────────────────────────────────
 *
 * 2 bits por mata (`MassState`): viva, tendida (pisada/peso, efímero) o destruida (persistente y
 * sincronizable). Solo los brotes TOCADOS llevan palabras de estado: un prado intacto cuesta 0 B.
 * La destrucción puede ser por mata (`killElement`) o por zona (se resuelve contra las posiciones
 * de esta función): las dos acaban en los mismos bits.
 */
#include <cstdint>
#include <cmath>
#include <vector>
#include <glm/glm.hpp>

namespace Haruka::World::Props {

/// Tope de matas por brote: el vertex shader separa (brote, mata) con este número.
constexpr uint32_t kMassMaxCount = 1024;
/// `MassBurstGpu::c.z` cuando el brote no tiene estado (intacto).
constexpr uint32_t kMassNoState  = 0xFFFFFFFFu;

/// Estado de UNA mata (2 bits).
enum class MassState : uint32_t { Alive = 0, Flat = 1, Destroyed = 2 };

/// Un brote, en CPU: todo lo necesario para reconstruir sus matas.
struct MassBurst {
    int       proto    = -1;                   ///< prototipo de la mata (índice del registro)
    glm::vec3 dir      = glm::vec3(0, 1, 0);   ///< centro de la celda sobre la esfera
    float     heightM  = 0.0f;                 ///< cota del suelo en el centro (la de todas sus matas)
    float     cellM    = 12.0f;                ///< lado de la celda (m): las matas caen en el 90 %
    uint32_t  seed     = 0;                    ///< identidad estable (celda + capa)
    uint32_t  count    = 1;                    ///< matas del brote (≤ kMassMaxCount)
    float     scaleMin = 1.0f, scaleMax = 1.0f;
};

/// Un brote, en GPU (std430, 48 B). Lo lee `prop_burst.vert`.
struct MassBurstGpu {
    glm::vec4  a{0.0f};   ///< xyz = centro relativo al origen del pase · w = escala mínima
    glm::vec4  b{0.0f};   ///< xyz = vertical local (unitaria) · w = escala máxima
    glm::uvec4 c{0u};     ///< x = semilla · y = matas · z = 1ª palabra de estado (kMassNoState) · w = bits de cellM
};
static_assert(sizeof(MassBurstGpu) == 48, "MassBurstGpu: 48 B, gemelo de lib/mass_burst.glsl");

// ── La función compartida (gemela EXACTA de lib/mass_burst.glsl) ─────────────────────────────────

inline uint32_t massHash(uint32_t x) {          // = PG::hash32
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15;
    x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
/// [0,1] determinista. `float(uint)` y la división redondean igual en C++ y en GLSL (IEEE).
inline float massHashF(uint32_t h, uint32_t salt) {
    return (float)massHash(h ^ (salt * 0x85EBCA6Bu)) / 4294967295.0f;
}
inline uint32_t massElementHash(uint32_t seed, uint32_t k) {
    return k == 0u ? seed : massHash(seed ^ (k * 0x9E3779B9u));
}
/// Marco tangente del brote. El mismo convenio que `sphereSlopeWithHeightAt`.
inline void massTangents(const glm::vec3& up, glm::vec3& u, glm::vec3& v) {
    const glm::vec3 ref = (std::abs(up.y) < 0.99f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    u = glm::normalize(glm::cross(ref, up));
    v = glm::cross(up, u);
}

/// Una mata reconstruida.
struct MassElement {
    glm::vec2 offset;   ///< desplazamiento sobre el plano tangente (m): centro + u·x + v·y
    float     yaw;      ///< giro alrededor de la vertical (rad)
    float     scale;
    float     tint;     ///< gris del tinte (0,85..1,15, como el scatter)
};
inline MassElement massElement(uint32_t seed, uint32_t k, float cellM, float scaleMin, float scaleMax) {
    const uint32_t hk = massElementHash(seed, k);
    MassElement e;
    // La mata 0 en el centro EXACTO (donde se muestreó la cota); las demás en el 90 % de la celda.
    e.offset = (k == 0u) ? glm::vec2(0.0f)
                         : glm::vec2(massHashF(hk, 1u) - 0.5f, massHashF(hk, 2u) - 0.5f) * (0.9f * cellM);
    e.yaw    = massHashF(hk, 5u) * 6.2831853f;
    e.scale  = scaleMin + (scaleMax - scaleMin) * massHashF(hk, 3u);
    e.tint   = 0.85f + 0.30f * massHashF(hk, 4u);
    return e;
}

/// Palabras de estado de un brote de `count` matas (2 bits por mata).
inline uint32_t massStateWords(uint32_t count) { return (count * 2u + 31u) / 32u; }
inline MassState massGetState(const std::vector<uint32_t>& w, uint32_t k) {
    const uint32_t bit = k * 2u;
    return (MassState)((w[bit >> 5] >> (bit & 31u)) & 3u);
}
inline void massSetState(std::vector<uint32_t>& w, uint32_t k, MassState s) {
    const uint32_t bit = k * 2u;
    w[bit >> 5] = (w[bit >> 5] & ~(3u << (bit & 31u))) | ((uint32_t)s << (bit & 31u));
}

} // namespace Haruka::World::Props
