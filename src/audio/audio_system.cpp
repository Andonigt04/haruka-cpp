#include "audio/audio_system.h"

#include <AL/al.h>
#include <AL/alc.h>
#include <iostream>

namespace Haruka {

bool AudioSystem::init(const std::string& outputDevice) {
    ALCdevice* dev = nullptr;
    if (!outputDevice.empty()) dev = alcOpenDevice(outputDevice.c_str());  // intenta el elegido
    if (!dev) dev = alcOpenDevice(nullptr);                                // fallback: predeterminado
    if (!dev) { std::cerr << "[audio] OpenAL: ningún dispositivo\n"; return false; }
    ALCcontext* ctx = alcCreateContext(dev, nullptr);
    if (!ctx || !alcMakeContextCurrent(ctx)) {
        std::cerr << "[audio] OpenAL: contexto fallido\n";
        if (ctx) alcDestroyContext(ctx);
        alcCloseDevice(dev);
        return false;
    }
    m_device = dev; m_context = ctx;
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    ALuint pool[16]; alGenSources(16, pool);
    for (ALuint s : pool) m_pool.push_back(s);
    m_ok = true;
    return true;
}

void AudioSystem::shutdown() {
    if (!m_ok) return;
    for (uint32_t s : m_pool) { alSourceStop(s); ALuint a = s; alDeleteSources(1, &a); }
    m_pool.clear();
    alcMakeContextCurrent(nullptr);
    if (m_context) alcDestroyContext((ALCcontext*)m_context);
    if (m_device)  alcCloseDevice((ALCdevice*)m_device);
    m_context = m_device = nullptr;
    m_ok = false;
}

void AudioSystem::setListenerOrientation(const glm::vec3& fwd, const glm::vec3& up) {
    if (!m_ok) return;
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);   // oído en el origen
    float ori[6] = { fwd.x, fwd.y, fwd.z, up.x, up.y, up.z };
    alListenerfv(AL_ORIENTATION, ori);
}

void AudioSystem::playOneShot(uint32_t buffer, const glm::vec3& relPos, float gain, float refDist) {
    if (!m_ok || !buffer) return;
    ALuint v = 0;
    for (ALuint s : m_pool) {
        ALint st; alGetSourcei(s, AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING) { v = s; break; }
    }
    if (!v) return;   // pool lleno → descarta
    alSourcei(v, AL_LOOPING, AL_FALSE);
    alSourcei(v, AL_BUFFER, (ALint)buffer);
    alSource3f(v, AL_POSITION, relPos.x, relPos.y, relPos.z);
    alSourcef(v, AL_GAIN, gain);
    alSourcef(v, AL_REFERENCE_DISTANCE, refDist);
    alSourcePlay(v);
}

uint32_t AudioSystem::voiceCreate() {
    if (!m_ok) return 0;
    ALuint v; alGenSources(1, &v);
    return v;
}

void AudioSystem::voiceDestroy(uint32_t voice) {
    if (!m_ok || !voice) return;
    alSourceStop(voice);
    ALuint a = voice; alDeleteSources(1, &a);
}

void AudioSystem::voicePlay(uint32_t voice, uint32_t buffer, const glm::vec3& relPos,
                            float gain, bool loop, float refDist) {
    if (!m_ok || !voice) return;
    alSourcei(voice, AL_BUFFER, (ALint)buffer);
    alSourcei(voice, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
    alSource3f(voice, AL_POSITION, relPos.x, relPos.y, relPos.z);
    alSourcef(voice, AL_GAIN, gain);
    alSourcef(voice, AL_REFERENCE_DISTANCE, refDist);
    alSourcePlay(voice);
}

void AudioSystem::voiceSetPos(uint32_t voice, const glm::vec3& relPos) {
    if (m_ok && voice) alSource3f(voice, AL_POSITION, relPos.x, relPos.y, relPos.z);
}

void AudioSystem::voiceSetGain(uint32_t voice, float gain) {
    if (m_ok && voice) alSourcef(voice, AL_GAIN, gain);
}

} // namespace Haruka
