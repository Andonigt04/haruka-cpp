#include "audio/audio_loader.h"

#include <AL/al.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace Haruka {

static uint64_t toneKey(float freq, float dur, const glm::vec3& t, bool loop) {
    auto q = [](float v, float s) { return (uint64_t)(v * s); };
    uint64_t k = q(freq, 10) * 1000003ull + q(dur, 100) * 9176ull + (loop ? 1ull : 0ull);
    k = k * 131 + q(t.x, 16) + q(t.y, 16) * 7 + q(t.z, 16) * 53;
    return k;
}

uint32_t AudioLoader::tone(float freq, float dur, const glm::vec3& timbre, bool loop) {
    uint64_t key = toneKey(freq, dur, timbre, loop);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return it->second;        // reutiliza

    const int rate = 44100;
    int n = (int)(rate * dur);
    std::vector<short> pcm(n);
    float norm = timbre.x + 0.5f * timbre.y + 0.3f * timbre.z + 1e-3f;
    for (int i = 0; i < n; ++i) {
        float ti  = (float)i / rate;
        float env = loop ? 1.0f : std::exp(-3.5f * ti);             // loop: continuo
        // Mezcla de armónicos según "timbre" (da cuerpo distinto a fuego/rayo/etc.).
        float s = std::sin(6.2831853f * freq * ti) * timbre.x
                + std::sin(6.2831853f * freq * 2.0f * ti) * 0.5f * timbre.y
                + std::sin(6.2831853f * freq * 3.0f * ti) * 0.3f * timbre.z;
        s *= env / norm;
        pcm[i] = (short)(std::max(-1.0f, std::min(1.0f, s)) * 30000.0f);
    }
    ALuint buf; alGenBuffers(1, &buf);
    alBufferData(buf, AL_FORMAT_MONO16, pcm.data(), (ALsizei)(n * sizeof(short)), rate);
    m_cache[key] = buf;
    return buf;
}

uint32_t AudioLoader::file(const std::string& path) {
    auto it = m_files.find(path);
    if (it != m_files.end()) return it->second;            // ya cargado

    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "[audio] no se abre " << path << "\n"; return 0; }
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) {
        std::cerr << "[audio] no es WAV PCM: " << path << "\n"; return 0;
    }

    uint16_t channels = 0, bits = 0; uint32_t rate = 0;
    const char* pcm = nullptr; uint32_t pcmLen = 0;
    size_t p = 12;                                          // tras "RIFF<size>WAVE"
    while (p + 8 <= d.size()) {
        uint32_t sz; std::memcpy(&sz, d.data() + p + 4, 4);
        const char* id = d.data() + p; const char* body = d.data() + p + 8;
        if (!std::memcmp(id, "fmt ", 4) && sz >= 16) {
            std::memcpy(&channels, body + 2, 2);
            std::memcpy(&rate,     body + 4, 4);
            std::memcpy(&bits,     body + 14, 2);
        } else if (!std::memcmp(id, "data", 4)) {
            pcm = body; pcmLen = std::min(sz, (uint32_t)(d.size() - (p + 8)));
        }
        p += 8 + sz + (sz & 1);                            // chunks alineados a par
    }
    if (!pcm || !channels || !bits) { std::cerr << "[audio] WAV incompleto: " << path << "\n"; return 0; }

    ALenum fmt = (channels == 1) ? (bits == 8 ? AL_FORMAT_MONO8   : AL_FORMAT_MONO16)
                                 : (bits == 8 ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16);
    ALuint buf; alGenBuffers(1, &buf);
    alBufferData(buf, fmt, pcm, (ALsizei)pcmLen, (ALsizei)rate);
    m_files[path] = buf;
    return buf;
}

void AudioLoader::clear() {
    for (auto& [k, b] : m_cache) { ALuint a = b; alDeleteBuffers(1, &a); }
    for (auto& [k, b] : m_files) { ALuint a = b; alDeleteBuffers(1, &a); }
    m_cache.clear();
    m_files.clear();
}

} // namespace Haruka
