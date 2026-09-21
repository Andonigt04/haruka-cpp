#include "audio/sound_points.h"

#include <algorithm>
#include <cmath>

namespace Haruka::Audio {

// ── puntos ────────────────────────────────────────────────────────────────────────────────────

SoundPoints::Id SoundPoints::add(const glm::dvec3& pos, Sound sound, float power, float pitch) {
    const Id id = m_next++;
    m_points[id] = Point{ pos, sound, std::max(0.0f, power), pitch };
    return id;
}

void SoundPoints::set(Id id, const glm::dvec3& pos, float power, float pitch) {
    auto it = m_points.find(id);
    if (it == m_points.end()) return;
    it->second.pos = pos; it->second.power = std::max(0.0f, power); it->second.pitch = pitch;
}

void SoundPoints::setPower(Id id, float power) {
    auto it = m_points.find(id);
    if (it != m_points.end()) it->second.power = std::max(0.0f, power);
}

void SoundPoints::remove(Id id) { m_points.erase(id); }

void SoundPoints::emit(const glm::dvec3& pos, Sound sound, float power, float pitch) {
    if (power <= 0.0f) return;
    m_pending.push_back(Point{ pos, sound, power, pitch });
}

// ── geometria ─────────────────────────────────────────────────────────────────────────────────

void SoundPoints::tangentFrame(const glm::vec3& up, glm::vec3& e1, glm::vec3& e2) const {
    // Marco ESTABLE EN EL MUNDO (no gira con la cabeza): si los sectores girasen con la camara, un
    // punto quieto cambiaria de grupo al mirar a otro lado y su voz se reiniciaria.
    const glm::vec3 ref = (std::fabs(up.y) < 0.9f) ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    e1 = glm::normalize(glm::cross(up, ref));
    e2 = glm::cross(up, e1);
}

float SoundPoints::energyAt(float distM) const {
    const float a = m_cfg.refDistM / std::max(m_cfg.refDistM, distM);
    return a * a;
}

int SoundPoints::sectorOf(const glm::vec3& rel, const glm::vec3& up, const glm::vec3& e1, const glm::vec3& e2) const {
    const float d = glm::length(rel);
    // Pegado al oido no hay direccion que valga (el viento en la oreja, la propia pisada): sector
    // "centro", aparte, para que no caiga en un azimut al azar y cambie de grupo a cada paso.
    if (d < 1.0f) return 3 * m_cfg.sectors;
    int ring = -1;
    for (int r = 0; r < 3; ++r) if (d < m_cfg.ringEdgesM[r]) { ring = r; break; }
    if (ring < 0) return -1;
    (void)up;
    const float az = std::atan2(glm::dot(rel, e2), glm::dot(rel, e1));   // (−π, π]
    int s = (int)std::floor((az + 3.14159265f) / (2.0f * 3.14159265f) * (float)m_cfg.sectors);
    s = std::clamp(s, 0, m_cfg.sectors - 1);
    return ring * m_cfg.sectors + s;
}

void SoundPoints::cluster(const std::vector<Point>& pts, const glm::dvec3& listener, const glm::vec3& up,
                          std::vector<Group>& out) const {
    glm::vec3 e1, e2; tangentFrame(up, e1, e2);
    std::unordered_map<uint32_t, size_t> idx;
    for (const Point& p : pts) {
        if (p.power <= 0.0f) continue;
        const glm::vec3 rel = glm::vec3(p.pos - listener);
        const int sec = sectorOf(rel, up, e1, e2);
        if (sec < 0) continue;
        // Energia = (potencia · ref/d)²: la suma de N iguales da √N veces la amplitud de uno (suma
        // incoherente), que es lo que hace un bosque de verdad.
        const float amp = p.power * std::sqrt(energyAt(glm::length(rel)));
        const float e   = amp * amp;
        const uint32_t key = (uint32_t)p.sound * 256u + (uint32_t)sec;
        auto it = idx.find(key);
        if (it == idx.end()) { idx[key] = out.size(); out.push_back(Group{ key, p.sound }); it = idx.find(key); }
        Group& g = out[it->second];
        g.posE   += rel * e;
        g.energy += e;
        g.pitchE += p.pitch * e;
        g.count  += 1;
    }
}

// ── update ────────────────────────────────────────────────────────────────────────────────────

void SoundPoints::update(const glm::dvec3& listener, const glm::vec3& upIn, float dt) {
    const glm::vec3 up = glm::normalize(upIn);

    // 1. Disparos del frame: mezclados y listos; el llamador los dispara y se olvida.
    m_shots.clear();
    if (!m_pending.empty()) {
        std::vector<Group> g; cluster(m_pending, listener, up, g);
        for (const Group& gr : g) {
            if (gr.energy <= 0.0f) continue;
            Voice v; v.sound = gr.sound; v.key = gr.key; v.count = gr.count; v.active = true;
            v.gain   = std::min(std::sqrt(gr.energy), m_cfg.gainCap);
            v.relPos = gr.posE / gr.energy;
            v.pitch  = gr.pitchE / gr.energy;
            m_shots.push_back(v);
        }
        m_pending.clear();
    }

    // 2. Persistentes → grupos → presupuesto.
    std::vector<Point> pts; pts.reserve(m_points.size());
    for (const auto& [id, p] : m_points) pts.push_back(p);
    std::vector<Group> groups; cluster(pts, listener, up, groups);
    m_groupsBefore = (int)groups.size();
    struct Want { uint32_t key; Sound sound; glm::vec3 pos; float gain; float pitch; int count; };
    std::vector<Want> want; want.reserve(groups.size());
    for (const Group& gr : groups) {
        const float gain = std::min(std::sqrt(gr.energy), m_cfg.gainCap);
        if (gain < m_cfg.hearMinGain) continue;
        want.push_back({ gr.key, gr.sound, gr.posE / gr.energy, gain, gr.pitchE / gr.energy, gr.count });
    }
    std::sort(want.begin(), want.end(), [](const Want& a, const Want& b) { return a.gain > b.gain; });
    if ((int)want.size() > m_cfg.maxVoices) want.resize((size_t)m_cfg.maxVoices);

    // 3. Ranuras: un grupo que sigue conserva la suya; el que se va se apaga; el nuevo coge una libre.
    if ((int)m_slots.size() < m_cfg.maxVoices) m_slots.resize((size_t)m_cfg.maxVoices);
    std::vector<char> kept(m_slots.size(), 0), used(want.size(), 0);
    for (size_t s = 0; s < m_slots.size(); ++s) {
        if (!m_slots[s].active) continue;
        for (size_t w = 0; w < want.size(); ++w)
            if (!used[w] && want[w].key == m_slots[s].key) { used[w] = 1; kept[s] = 1; break; }
        if (!kept[s]) m_slots[s].active = false;             // se apaga (gain → 0)
    }
    const float kS = 1.0f - std::exp(-dt / std::max(m_cfg.smoothS, 1e-4f));
    const float kF = 1.0f - std::exp(-dt / std::max(m_cfg.fadeOutS, 1e-4f));
    for (size_t w = 0; w < want.size(); ++w) {
        size_t slot = (size_t)-1;
        if (used[w]) {
            for (size_t s = 0; s < m_slots.size(); ++s) if (kept[s] && m_slots[s].key == want[w].key) { slot = s; break; }
        } else {
            // Libre del todo; si no, la que se esta apagando mas baja (se reinicia con otro sonido).
            float best = 1e9f;
            for (size_t s = 0; s < m_slots.size(); ++s) {
                if (m_slots[s].active || kept[s]) continue;
                if (m_slots[s].gain < best) { best = m_slots[s].gain; slot = s; }
            }
            if (slot == (size_t)-1) continue;                // no deberia: want ≤ maxVoices
            Voice& nv = m_slots[slot];
            nv = Voice{};
            nv.sound = want[w].sound; nv.key = want[w].key; nv.active = true;
            nv.relPos = want[w].pos; nv.pitch = want[w].pitch; nv.gain = 0.0f;   // entra en fundido
            kept[slot] = 1;
        }
        Voice& v = m_slots[slot];
        v.count  = want[w].count;
        v.gain   += (want[w].gain  - v.gain)   * kS;
        v.relPos += (want[w].pos   - v.relPos) * kS;
        v.pitch  += (want[w].pitch - v.pitch)  * kS;
    }
    for (Voice& v : m_slots) {
        if (v.active) continue;
        v.gain -= v.gain * kF;
        if (v.gain < 1e-3f) v.gain = 0.0f;              // −60 dB: apagada
    }
}

} // namespace Haruka::Audio
