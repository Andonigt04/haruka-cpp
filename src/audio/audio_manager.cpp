#include "audio/audio_manager.h"

#include "core/propagation.h"

#include <AL/alc.h>
#include <chrono>
#include <algorithm>
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
    for (auto& sv : m_slotVoices) if (sv.voice) m_sys.voiceDestroy(sv.voice);
    m_slotVoices.clear();
    m_loader.clear();
    m_sys.shutdown();
}

void AudioManager::setListener(const glm::dvec3& pos, const glm::dvec3& fwd, const glm::dvec3& up) {
    m_listenerPos = pos;   // las fuentes se colocan RELATIVAS (sin jitter float a 1 UA)
    m_listenerUp  = up;
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
    updatePoints();
}

void AudioManager::setVolumes(float master, float ambient, float sfx) {
    m_sys.setMasterGain(master);
    m_volAmbient = ambient;
    m_volSfx     = sfx;
}

// Los grupos de SoundPoints → voces AL. Cada ranura de SoundPoints tiene su voz aqui, con el mismo
// indice; si la ranura cambia de grupo (otra `key`) se vuelve a lanzar con el buffer que toque.
void AudioManager::updatePoints() {
    using clock = std::chrono::steady_clock;
    const double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    const float dt = (m_lastUpdateS < 0.0) ? (1.0f / 60.0f) : (float)std::min(0.1, now - m_lastUpdateS);
    m_lastUpdateS = now;

    m_points.update(m_listenerPos, glm::vec3(m_listenerUp), dt);

    // Bajo el agua todo lo de fuera llega amortiguado; el bucle "sumergido" es el unico que no.
    const float duck = m_submerged ? 0.15f : 1.0f;
    auto volumeOf = [&](Audio::Sound snd) {
        const float v = Audio::isLoop(snd) ? m_volAmbient : m_volSfx;
        return (snd == Audio::Sound::Underwater) ? v : v * duck;
    };

    const auto& slots = m_points.voices();
    if (m_slotVoices.size() < slots.size()) m_slotVoices.resize(slots.size());
    for (size_t i = 0; i < slots.size(); ++i) {
        const auto& sl = slots[i];
        SlotVoice& sv = m_slotVoices[i];
        const bool wantPlaying = sl.active || sl.gain > 0.0f;
        if (!wantPlaying) {
            if (sv.playing) { m_sys.voiceDestroy(sv.voice); sv = SlotVoice{}; }
            continue;
        }
        if (!sv.playing || sv.key != sl.key) {
            if (!sv.voice) sv.voice = m_sys.voiceCreate();
            if (!sv.voice) continue;
            const uint32_t buf = m_loader.synth(sl.sound);
            m_sys.voiceSetRolloff(sv.voice, 0.0f);            // atenua SoundPoints, no la distancia AL
            m_sys.voicePlay(sv.voice, buf, sl.relPos, 0.0f, /*loop*/Audio::isLoop(sl.sound));
            sv.key = sl.key; sv.playing = true;
        }
        m_sys.voiceSetPos(sv.voice, sl.relPos);
        m_sys.voiceSetGain(sv.voice, sl.gain * volumeOf(sl.sound));
        m_sys.voiceSetPitch(sv.voice, sl.pitch);
    }
    for (const auto& sh : m_points.oneShots())
        m_sys.playOneShotFlat(m_loader.synth(sh.sound), sh.relPos, sh.gain * volumeOf(sh.sound), sh.pitch);
}

} // namespace Haruka
