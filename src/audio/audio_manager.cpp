#include "audio/audio_manager.h"

#include "core/application.h"
#include "game/planetary_system.h"
#include "renderer/motor_instance.h"

#include <glm/glm.hpp>

namespace Haruka {

AudioManager& AudioManager::get() { static AudioManager m; return m; }

bool AudioManager::init(const std::string& outputDevice) {
    return m_sys.init(outputDevice);
}

void AudioManager::shutdown() {
    for (auto& [id, w] : m_world) m_sys.voiceDestroy(w.voice);
    m_world.clear();
    m_loader.clear();
    m_sys.shutdown();
}

void AudioManager::setListener(const glm::dvec3& pos, const glm::dvec3& fwd, const glm::dvec3& up) {
    m_listenerPos = pos;   // las fuentes se colocan RELATIVAS (sin jitter float a 1 UA)
    m_sys.setListenerOrientation(glm::normalize(glm::vec3(fwd)), glm::normalize(glm::vec3(up)));
}

// 0 = tapado (terreno en medio), 1 = línea libre. Muestrea el segmento from↔to bajo la
// superficie del planeta. Playback Y consulta lógica (IA/sigilo). Aquí entrarán los portales.
float AudioManager::propagation(const glm::dvec3& from, const glm::dvec3& to) const {
    Application* app = MotorInstance::getInstance().getApplication();
    auto* ps = app ? app->getPlanetarySystem() : nullptr;
    if (!app || !ps) return 1.0f;
    glm::dvec3 center; double radius; uint32_t sd; float rl;
    if (!ps->getActivePlanet(center, radius, sd, rl)) return 1.0f;

    const int N = 8;
    int blocked = 0;
    for (int i = 1; i < N; ++i) {
        glm::dvec3 p = glm::mix(from, to, (double)i / N);
        double surf = radius + app->getTerrainHeightAt(p);
        if (glm::length(p - center) < surf - 0.5) ++blocked;
    }
    return 1.0f - (float)blocked / (float)(N - 1);
}

void AudioManager::playTone(const glm::dvec3& pos, float freq, float dur, float volume,
                            const glm::vec3& timbre) {
    if (!m_sys.ok()) return;
    uint32_t buf = m_loader.tone(freq, dur, timbre, /*loop*/false);
    float gain = volume * (0.15f + 0.85f * propagation(pos, m_listenerPos));  // nunca 0 (difracción)
    m_sys.playOneShot(buf, glm::vec3(pos - m_listenerPos), gain);
}

void AudioManager::playSound(const glm::dvec3& pos, const std::string& path, float volume) {
    if (!m_sys.ok()) return;
    uint32_t buf = m_loader.file(path);
    if (!buf) return;
    float gain = volume * (0.15f + 0.85f * propagation(pos, m_listenerPos));
    m_sys.playOneShot(buf, glm::vec3(pos - m_listenerPos), gain);
}

AudioManager::SourceId AudioManager::addSourceFile(const glm::dvec3& pos, const std::string& path,
                                                   float volume) {
    if (!m_sys.ok()) return 0;
    uint32_t buf = m_loader.file(path);
    if (!buf) return 0;
    uint32_t voice = m_sys.voiceCreate();
    if (!voice) return 0;
    float gain = volume * (0.15f + 0.85f * propagation(pos, m_listenerPos));
    m_sys.voicePlay(voice, buf, glm::vec3(pos - m_listenerPos), gain, /*loop*/true);
    SourceId id = m_nextId++;
    m_world[id] = { pos, voice, volume };
    return id;
}

AudioManager::SourceId AudioManager::addSource(const glm::dvec3& pos, float freq, float volume,
                                               const glm::vec3& timbre) {
    if (!m_sys.ok()) return 0;
    uint32_t buf   = m_loader.tone(freq, 0.6f, timbre, /*loop*/true);
    uint32_t voice = m_sys.voiceCreate();
    if (!voice) return 0;
    float gain = volume * (0.15f + 0.85f * propagation(pos, m_listenerPos));
    m_sys.voicePlay(voice, buf, glm::vec3(pos - m_listenerPos), gain, /*loop*/true);
    SourceId id = m_nextId++;
    m_world[id] = { pos, voice, volume };
    return id;
}

void AudioManager::setSourcePosition(SourceId id, const glm::dvec3& pos) {
    auto it = m_world.find(id);
    if (it != m_world.end()) it->second.pos = pos;
}

void AudioManager::removeSource(SourceId id) {
    auto it = m_world.find(id);
    if (it == m_world.end()) return;
    m_sys.voiceDestroy(it->second.voice);
    m_world.erase(it);
}

void AudioManager::update() {
    if (!m_sys.ok()) return;
    // Fuentes persistentes: recoloca (relativo al oído) y reatenúa por propagación cada frame.
    for (auto& [id, w] : m_world) {
        m_sys.voiceSetPos(w.voice, glm::vec3(w.pos - m_listenerPos));
        m_sys.voiceSetGain(w.voice, w.gain * (0.15f + 0.85f * propagation(w.pos, m_listenerPos)));
    }
}

} // namespace Haruka
