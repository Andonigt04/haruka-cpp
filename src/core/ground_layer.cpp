#include "core/ground_layer.h"

#include <algorithm>
#include <cmath>

namespace Haruka {

float GroundLayer::lifetimeFor(GroundMaterial m) {
    switch (m) {
        case GroundMaterial::Snow: return 180.0f;  // aguanta: en nieve se ve tu camino de vuelta
        case GroundMaterial::Mud:  return 120.0f;  // el barro conserva la forma hasta que se seca
        case GroundMaterial::Sand: return 35.0f;   // el borde se desmorona solo
        case GroundMaterial::Ash:  return 45.0f;
        default:                   return 0.0f;
    }
}

float GroundLayer::sinkDepthFor(GroundMaterial m) {
    switch (m) {
        case GroundMaterial::Snow: return 0.14f;   // nieve profunda: hasta el tobillo largo
        case GroundMaterial::Mud:  return 0.07f;
        case GroundMaterial::Sand: return 0.05f;
        case GroundMaterial::Ash:  return 0.06f;
        default:                   return 0.0f;
    }
}

float GroundLayer::speedFactorFor(GroundMaterial m) {
    switch (m) {
        case GroundMaterial::Snow: return 0.62f;   // andar por nieve cuesta, y se nota
        case GroundMaterial::Mud:  return 0.72f;
        case GroundMaterial::Sand: return 0.80f;
        case GroundMaterial::Ash:  return 0.85f;
        default:                   return 1.0f;
    }
}

float GroundLayer::fade(const Stamp& s, double now) {
    const float life = lifetimeFor(s.material);
    if (life <= 0.0f) return 0.0f;
    const float t = (float)((now - s.bornAt) / (double)life);
    if (t >= 1.0f) return 0.0f;
    if (t <= 0.0f) return s.depth;
    // Curvas distintas por material, y se ven: la ARENA pierde la forma casi enseguida (el borde cae
    // sobre la huella) mientras que la NIEVE se mantiene y desaparece al final, cuando la tapa lo que
    // sigue cayendo. Una sola curva lineal haría que las dos se comportaran igual.
    const float k = (s.material == GroundMaterial::Sand || s.material == GroundMaterial::Ash)
                  ? (1.0f - t) * (1.0f - t)          // desmoronamiento rápido
                  : 1.0f - t * t * t;                // persistente y luego cae
    return s.depth * std::max(0.0f, k);
}

void GroundLayer::stamp(const Stamp& s) {
    if (s.material == GroundMaterial::None || s.depth <= 0.0f) return;
    m_stamps.push_back(s);
    // Cola acotada: la huella más vieja cede el sitio. Sin esto, cruzar el mundo andando haría crecer
    // la lista sin techo y el pase de huellas se volvería más caro cuanto más tiempo llevaras jugando.
    while (m_stamps.size() > kMaxStamps) m_stamps.pop_front();
}

void GroundLayer::update(double now) {
    // ⚠️ NO basta con tirar por delante hasta encontrar una viva, aunque estén ordenadas por
    // antigüedad: cada MATERIAL tiene su propia vida (arena 35 s, nieve 180 s), así que una huella
    // reciente en arena caduca ANTES que una vieja en nieve. Con el atajo del frente, cruzar de la
    // duna a la nieve dejaba huellas de arena muertas atrapadas detrás de una de nieve viva — se
    // seguían proyectando cada frame y ocupaban sitio en la cola acotada. n ≤ kMaxStamps y esto va
    // una vez por frame: recorrerla entera no es el coste que hay que optimizar.
    for (auto it = m_stamps.begin(); it != m_stamps.end(); ) {
        if (fade(*it, now) <= 0.0f) it = m_stamps.erase(it);
        else                        ++it;
    }
}

float GroundLayer::trampledAt(const glm::dvec3& worldPos, double now) const {
    float acc = 0.0f;
    for (const Stamp& s : m_stamps) {
        const glm::dvec3 d = worldPos - s.pos;
        const double d2 = glm::dot(d, d);   // (no `length2`: vive en glm/gtx, que exige GLM_ENABLE_EXPERIMENTAL)
        const double r  = (double)s.radius;
        if (d2 > r * r) continue;
        const float t = 1.0f - (float)(std::sqrt(d2) / r);
        acc = std::max(acc, fade(s, now) * t * t * (3.0f - 2.0f * t));
    }
    return std::min(acc, 1.0f);
}

} // namespace Haruka
