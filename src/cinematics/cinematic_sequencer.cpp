#include "cinematics/cinematic_sequencer.h"

#include "core/camera.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>   // glm::quatLookAt
#include <glm/gtc/quaternion.hpp>   // glm::slerp

namespace Haruka {

static glm::dvec3 readVec3(const nlohmann::json& a, const glm::dvec3& fallback) {
    if (a.is_array() && a.size() >= 3)
        return glm::dvec3(a[0].get<double>(), a[1].get<double>(), a[2].get<double>());
    return fallback;
}

bool CinematicSequencer::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    nlohmann::json j;
    try { f >> j; } catch (...) { return false; }

    m_keys.clear();
    const auto& cam = j.contains("camera") ? j["camera"] : nlohmann::json::array();
    if (!cam.is_array() || cam.empty()) return false;
    for (const auto& k : cam) {
        Key key;
        key.t    = k.value("t", 0.0);
        key.pos  = readVec3(k.value("pos",  nlohmann::json::array()), glm::dvec3(0.0));
        key.look = readVec3(k.value("look", nlohmann::json::array()), key.pos + glm::dvec3(0, 0, -1));
        key.fov  = k.value("fov", 45.0f);
        key.anchor     = k.value("anchor", std::string{});
        key.lookAnchor = k.value("lookAnchor", std::string{});
        m_keys.push_back(key);
    }
    // Keyframes must be ordered by time for the segment search.
    std::sort(m_keys.begin(), m_keys.end(), [](const Key& a, const Key& b){ return a.t < b.t; });

    m_duration  = m_keys.back().t;
    m_letterbox = j.value("letterbox", true);
    m_blendIn   = j.value("blendIn",  0.5f);
    m_blendOut  = j.value("blendOut", 0.5f);
    m_relative  = j.value("relative", false);
    m_loop      = j.value("loop", false);
    return true;
}

void CinematicSequencer::play() {
    if (m_keys.empty()) return;
    m_time = 0.0;
    m_playing = true;
    m_haveStart = false;   // captured on the first update (live gameplay camera)
}

void CinematicSequencer::stop() {
    m_playing = false;
}

// Catmull-Rom over (possibly non-uniform) time keys: finds the bounding segment
// and interpolates with the two neighbour keys clamped at the ends.
static glm::dvec3 catmull(const glm::dvec3& p0, const glm::dvec3& p1,
                          const glm::dvec3& p2, const glm::dvec3& p3, double u) {
    double u2 = u * u, u3 = u2 * u;
    return 0.5 * ((2.0 * p1) +
                  (-p0 + p2) * u +
                  (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * u2 +
                  (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * u3);
}

glm::dvec3 CinematicSequencer::anchorBase(const std::string& name) const {
    if (!name.empty()) {
        auto it = m_anchors.find(name);
        if (it != m_anchors.end()) return it->second;
    }
    return m_relative ? m_startPos : glm::dvec3(0.0);   // fallback: relative→player, else absolute
}

// Bake each raw key into a world-space key: pos/look = anchor (or relative/absolute base) + offset.
// Run once on the first update, when both the anchors and the relative origin are known.
void CinematicSequencer::buildResolved() {
    m_resolved.clear();
    m_resolved.reserve(m_keys.size());
    for (const auto& k : m_keys) {
        Key r; r.t = k.t; r.fov = k.fov;
        const std::string& la = k.lookAnchor.empty() ? k.anchor : k.lookAnchor;
        r.pos  = anchorBase(k.anchor) + k.pos;
        r.look = anchorBase(la)       + k.look;
        glm::dvec3 d = r.look - r.pos;                          // orientation baked from pos→look
        r.orient = (glm::length(d) > 1e-9)
                 ? glm::quatLookAt(glm::normalize(d), glm::dvec3(0.0, 1.0, 0.0))
                 : glm::dquat(1.0, 0.0, 0.0, 0.0);
        m_resolved.push_back(r);
    }
}

void CinematicSequencer::sample(double time, glm::dvec3& pos, glm::dquat& orient, float& fov) const {
    const int n = (int)m_resolved.size();
    if (n == 0) { return; }
    if (n == 1) { pos = m_resolved[0].pos; orient = m_resolved[0].orient; fov = m_resolved[0].fov; return; }

    // Find segment i such that keys[i].t <= time <= keys[i+1].t.
    int i = 0;
    while (i < n - 2 && time > m_resolved[i + 1].t) ++i;
    const Key& k1 = m_resolved[i];
    const Key& k2 = m_resolved[i + 1];
    double span = std::max(1e-9, k2.t - k1.t);
    double u  = std::clamp((time - k1.t) / span, 0.0, 1.0);
    double us = u * u * (3.0 - 2.0 * u);   // smoothstep (used for orientation + fov)

    const Key& k0 = m_resolved[std::max(0, i - 1)];
    const Key& k3 = m_resolved[std::min(n - 1, i + 2)];

    pos    = catmull(k0.pos, k1.pos, k2.pos, k3.pos, u);            // smooth flight path
    orient = glm::normalize(glm::slerp(k1.orient, k2.orient, us));  // SLERP → no swing/bounce
    fov    = (float)glm::mix((double)k1.fov, (double)k2.fov, us);
}

bool CinematicSequencer::update(double dt, Camera& out, const Camera& gameplayCam) {
    if (!m_playing) return false;

    // Capture the live gameplay camera on the first frame, then bake anchors/relative
    // offsets into world-space keys (anchors are set by the game before play()).
    if (!m_haveStart) {
        m_startPos    = gameplayCam.position;
        m_startOrient = gameplayCam.orientation;
        m_startFov    = gameplayCam.zoom;
        m_haveStart   = true;
        buildResolved();
    }

    m_time += dt;
    if (m_time >= m_duration) {
        if (m_loop) { m_time = (m_duration > 1e-9) ? std::fmod(m_time, m_duration) : 0.0; } // demo-reel
        else        { m_time = m_duration; m_playing = false; }
    }

    glm::dvec3 pos; glm::dquat orient; float fov;
    sample(m_time, pos, orient, fov);

    // Blend in from the gameplay camera over the first blendIn seconds (smoothstep).
    if (m_blendIn > 1e-4f && m_time < (double)m_blendIn) {
        double a = m_time / (double)m_blendIn;
        a = a * a * (3.0 - 2.0 * a);
        pos    = glm::mix(m_startPos, pos, a);
        orient = glm::normalize(glm::slerp(m_startOrient, orient, a));
        fov    = (float)glm::mix((double)m_startFov, (double)fov, a);
    }

    out.position    = pos;
    out.orientation = orient;
    out.zoom        = fov;
    return true;
}

float CinematicSequencer::letterbox() const {
    if (!m_playing || !m_letterbox) return 0.0f;
    // Ease the bars in at the start and out near the end.
    float in  = (m_blendIn  > 1e-4f) ? std::clamp((float)(m_time / m_blendIn), 0.0f, 1.0f) : 1.0f;
    if (m_loop) return in;   // looping demo-reel: keep the bars up (no end fade)
    float rem = (float)(m_duration - m_time);
    float out = (m_blendOut > 1e-4f) ? std::clamp(rem / m_blendOut, 0.0f, 1.0f) : 1.0f;
    return std::min(in, out);
}

} // namespace Haruka
