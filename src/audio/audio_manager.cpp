#include "audio/audio_manager.h"

#include "core/propagation.h"

#include <AL/alc.h>
#include <cstring>
#include <glm/glm.hpp>

namespace Haruka {

AudioManager& AudioManager::get() { static AudioManager m; return m; }

bool AudioManager::init(const std::string& outputDevice) {
    return m_sys.init(outputDevice);
}

std::vector<std::string> AudioManager::playbackDevices() {
    std::vector<std::string> out;
    const ALCchar* list = nullptr;
    if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT"))
        list = alcGetString(nullptr, ALC_ALL_DEVICES_SPECIFIER);   // todos los endpoints
    else if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT"))
        list = alcGetString(nullptr, ALC_DEVICE_SPECIFIER);
    for (const ALCchar* p = list; p && *p; p += std::strlen(p) + 1) // lista separada por \0
        out.emplace_back(p);
    return out;
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

// Cuánta señal llega de from a to: navegación por huecos (rodea muros + oclusión de terreno).
// Compartido con los conjuros serpenteantes vía Haruka::Propagation.
float AudioManager::propagation(const glm::dvec3& from, const glm::dvec3& to) const {
    return Haruka::Propagation::through(from, to);
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
    World w; w.pos = pos; w.voice = voice; w.gainBase = volume;
    w.gainCached = gain; w.listenerAtCalc = m_listenerPos; w.recalcIn = (int)(id % 24); // escalona
    m_world[id] = w;
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
    World w; w.pos = pos; w.voice = voice; w.gainBase = volume;
    w.gainCached = gain; w.listenerAtCalc = m_listenerPos; w.recalcIn = (int)(id % 24); // escalona
    m_world[id] = w;
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
        m_sys.voiceSetPos(w.voice, glm::vec3(w.pos - m_listenerPos));   // posición: SIEMPRE (barato)
        // Propagación (pathfind, CARO): recalcula a ratos o si el oyente se movió bastante.
        bool moved = glm::length(m_listenerPos - w.listenerAtCalc) > 3.0;
        if (w.recalcIn <= 0 || moved) {
            w.gainCached = w.gainBase * (0.15f + 0.85f * propagation(w.pos, m_listenerPos));
            w.listenerAtCalc = m_listenerPos;
            w.recalcIn = 24;                                  // ~cada 24 frames si no te mueves
        } else { --w.recalcIn; }
        m_sys.voiceSetGain(w.voice, w.gainCached);
    }
}

} // namespace Haruka
