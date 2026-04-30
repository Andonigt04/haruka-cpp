#include "audio_manager.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include "core/error_reporter.h"

namespace Haruka {

// ============ AudioBuffer ============
AudioBuffer::AudioBuffer(const std::string& filepath) {
    if (filepath.find(".wav") != std::string::npos) {
        loadWAV(filepath);
    } else if (filepath.find(".ogg") != std::string::npos) {
        loadOGG(filepath);
    } else {
        HARUKA_AUDIO_ERROR(ErrorCode::ASSET_FORMAT_INVALID, "Unsupported audio format: " + filepath);
    }
}

AudioBuffer::~AudioBuffer() {
    if (bufferId) {
        alDeleteBuffers(1, &bufferId);
    }
}

bool AudioBuffer::loadWAV(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        HARUKA_AUDIO_ERROR(ErrorCode::AUDIO_LOAD_FAILED, "Cannot open audio file: " + filepath);
        return false;
    }
    
    // Parse WAV header
    char chunkId[4];
    uint32_t chunkSize;
    char format[4];
    
    file.read(chunkId, 4);
    if (std::strncmp(chunkId, "RIFF", 4) != 0) {
        HARUKA_AUDIO_ERROR(ErrorCode::ASSET_FORMAT_INVALID, "Not a WAV file (missing RIFF): " + filepath);
        return false;
    }
    
    file.read((char*)&chunkSize, 4);
    file.read(format, 4);
    
    if (std::strncmp(format, "WAVE", 4) != 0) {
        HARUKA_AUDIO_ERROR(ErrorCode::ASSET_FORMAT_INVALID, "Not a WAVE format: " + filepath);
        return false;
    }
    
    // Find fmt chunk
    char subchunkId[4];
    uint32_t subchunkSize;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    
    file.read(subchunkId, 4);
    file.read((char*)&subchunkSize, 4);
    file.read((char*)&audioFormat, 2);
    file.read((char*)&numChannels, 2);
    file.read((char*)&sampleRate, 4);
    file.read((char*)&byteRate, 4);
    file.read((char*)&blockAlign, 2);
    file.read((char*)&bitsPerSample, 2);
    
    // Find data chunk
    file.read(subchunkId, 4);
    while (std::strncmp(subchunkId, "data", 4) != 0 && !file.eof()) {
        file.read((char*)&subchunkSize, 4);
        file.seekg(subchunkSize, std::ios::cur);
        file.read(subchunkId, 4);
    }
    
    if (file.eof()) {
        HARUKA_AUDIO_ERROR(ErrorCode::AUDIO_LOAD_FAILED, "No data chunk found in: " + filepath);
        return false;
    }
    
    uint32_t dataSize;
    file.read((char*)&dataSize, 4);
    
    std::vector<char> data(dataSize);
    file.read(data.data(), dataSize);
    file.close();
    
    // Determine format
    ALenum alFormat;
    if (numChannels == 1 && bitsPerSample == 8) alFormat = AL_FORMAT_MONO8;
    else if (numChannels == 1 && bitsPerSample == 16) alFormat = AL_FORMAT_MONO16;
    else if (numChannels == 2 && bitsPerSample == 8) alFormat = AL_FORMAT_STEREO8;
    else if (numChannels == 2 && bitsPerSample == 16) alFormat = AL_FORMAT_STEREO16;
    else {
        HARUKA_AUDIO_ERROR(ErrorCode::ASSET_FORMAT_INVALID,
            "Unsupported WAV format: " + std::to_string(numChannels) + " channels, "
            + std::to_string(bitsPerSample) + " bits");
        return false;
    }
    
    // Create buffer
    alGenBuffers(1, &bufferId);
    alBufferData(bufferId, alFormat, data.data(), dataSize, sampleRate);
    
    ALenum error = alGetError();
    if (error != AL_NO_ERROR) {
        HARUKA_AUDIO_ERROR(ErrorCode::AUDIO_LOAD_FAILED,
            "OpenAL error loading '" + filepath + "': " + std::to_string(error));
        return false;
    }

    std::cout << "[Audio] Loaded: " << filepath << std::endl;
    return true;
}

bool AudioBuffer::loadOGG(const std::string& filepath) {
    HARUKA_AUDIO_ERROR(ErrorCode::ASSET_FORMAT_INVALID, "OGG loading not implemented: " + filepath);
    return false;
}

// ============ AudioManager ============
AudioManager::AudioManager() {}

AudioManager::~AudioManager() {
    shutdown();
}

bool AudioManager::init() {
    device = alcOpenDevice(nullptr);
    if (!device) {
        HARUKA_AUDIO_ERROR(ErrorCode::AUDIO_INIT_FAILED, "Failed to open OpenAL device");
        return false;
    }

    context = alcCreateContext(device, nullptr);
    if (!context || !alcMakeContextCurrent(context)) {
        HARUKA_AUDIO_ERROR(ErrorCode::AUDIO_INIT_FAILED, "Failed to create OpenAL context");
        alcCloseDevice(device);
        return false;
    }
    
    // Preallocate sources
    sources.resize(32);
    for (int i = 0; i < 32; i++) {
        alGenSources(1, &sources[i].sourceId);
        freeSources.push_back(i);
    }
    
    std::cout << "[Audio] Initialized (" << sources.size() << " sources)" << std::endl;
    return true;
}

void AudioManager::shutdown() {
    for (auto& source : sources) {
        if (source.sourceId) {
            alDeleteSources(1, &source.sourceId);
        }
    }
    sources.clear();
    buffers.clear();
    
    if (context) {
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(context);
    }
    if (device) {
        alcCloseDevice(device);
    }
    
    std::cout << "[Audio] Shutdown" << std::endl;
}

void AudioManager::setListenerPosition(const glm::vec3& pos) {
    alListener3f(AL_POSITION, pos.x, pos.y, pos.z);
}

void AudioManager::setListenerVelocity(const glm::vec3& vel) {
    alListener3f(AL_VELOCITY, vel.x, vel.y, vel.z);
}

void AudioManager::setListenerOrientation(const glm::vec3& forward, const glm::vec3& up) {
    ALfloat orientation[] = { forward.x, forward.y, forward.z, up.x, up.y, up.z };
    alListenerfv(AL_ORIENTATION, orientation);
}

bool AudioManager::loadSound(const std::string& name, const std::string& filepath) {
    auto buffer = std::make_shared<AudioBuffer>(filepath);
    if (buffer->isLoaded()) {
        buffers[name] = buffer;
        return true;
    }
    return false;
}

int AudioManager::allocateSource() {
    if (freeSources.empty()) {
        HARUKA_AUDIO_ERROR(ErrorCode::AUDIO_LOAD_FAILED, "No free audio sources available");
        return -1;
    }
    
    int sourceId = freeSources.back();
    freeSources.pop_back();
    return sourceId;
}

void AudioManager::freeSource(int sourceId) {
    if (sourceId >= 0 && sourceId < (int)sources.size()) {
        alSourceStop(sources[sourceId].sourceId);
        freeSources.push_back(sourceId);
    }
}

int AudioManager::playSound(const std::string& name, const glm::vec3& position,
                            bool loop, float gain, float pitch) {
    auto it = buffers.find(name);
    if (it == buffers.end()) {
        HARUKA_AUDIO_ERROR(ErrorCode::ASSET_NOT_FOUND, "Sound not found: '" + name + "'");
        return -1;
    }
    
    int sourceId = allocateSource();
    if (sourceId < 0) return -1;
    
    AudioSource& source = sources[sourceId];
    source.position = position;
    source.gain = gain * masterVolume;
    source.pitch = pitch;
    source.looping = loop;
    source.positional = true;
    
    alSourcei(source.sourceId, AL_BUFFER, it->second->getBufferId());
    alSourcef(source.sourceId, AL_PITCH, pitch);
    alSourcef(source.sourceId, AL_GAIN, gain * masterVolume);
    alSource3f(source.sourceId, AL_POSITION, position.x, position.y, position.z);
    alSource3f(source.sourceId, AL_VELOCITY, 0, 0, 0);
    alSourcei(source.sourceId, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
    
    alSourcePlay(source.sourceId);
    
    checkALError("playSound");
    return sourceId;
}

int AudioManager::playSound2D(const std::string& name, bool loop, float gain) {
    auto it = buffers.find(name);
    if (it == buffers.end()) {
        HARUKA_AUDIO_ERROR(ErrorCode::ASSET_NOT_FOUND, "Sound not found: '" + name + "'");
        return -1;
    }
    
    int sourceId = allocateSource();
    if (sourceId < 0) return -1;
    
    AudioSource& source = sources[sourceId];
    source.gain = gain * masterVolume;
    source.looping = loop;
    source.positional = false;
    
    alSourcei(source.sourceId, AL_BUFFER, it->second->getBufferId());
    alSourcef(source.sourceId, AL_GAIN, gain * masterVolume);
    alSourcei(source.sourceId, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(source.sourceId, AL_POSITION, 0, 0, 0);
    alSourcei(source.sourceId, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
    
    alSourcePlay(source.sourceId);
    
    checkALError("playSound2D");
    return sourceId;
}

void AudioManager::stopSource(int sourceId) {
    if (sourceId >= 0 && sourceId < (int)sources.size()) {
        alSourceStop(sources[sourceId].sourceId);
        freeSource(sourceId);
    }
}

void AudioManager::pauseSource(int sourceId) {
    if (sourceId >= 0 && sourceId < (int)sources.size()) {
        alSourcePause(sources[sourceId].sourceId);
    }
}

void AudioManager::resumeSource(int sourceId) {
    if (sourceId >= 0 && sourceId < (int)sources.size()) {
        alSourcePlay(sources[sourceId].sourceId);
    }
}

void AudioManager::setSourcePosition(int sourceId, const glm::vec3& pos) {
    if (sourceId >= 0 && sourceId < (int)sources.size()) {
        sources[sourceId].position = pos;
        alSource3f(sources[sourceId].sourceId, AL_POSITION, pos.x, pos.y, pos.z);
    }
}

void AudioManager::setSourceVelocity(int sourceId, const glm::vec3& vel) {
    if (sourceId >= 0 && sourceId < (int)sources.size()) {
        sources[sourceId].velocity = vel;
        alSource3f(sources[sourceId].sourceId, AL_VELOCITY, vel.x, vel.y, vel.z);
    }
}

void AudioManager::setSourceGain(int sourceId, float gain) {
    if (sourceId >= 0 && sourceId < (int)sources.size()) {
        sources[sourceId].gain = gain;
        alSourcef(sources[sourceId].sourceId, AL_GAIN, gain * masterVolume);
    }
}

void AudioManager::setSourcePitch(int sourceId, float pitch) {
    if (sourceId >= 0 && sourceId < (int)sources.size()) {
        sources[sourceId].pitch = pitch;
        alSourcef(sources[sourceId].sourceId, AL_PITCH, pitch);
    }
}

void AudioManager::setMasterVolume(float volume) {
    masterVolume = glm::clamp(volume, 0.0f, 1.0f);
    
    for (auto& source : sources) {
        if (source.sourceId) {
            alSourcef(source.sourceId, AL_GAIN, source.gain * masterVolume);
        }
    }
}

void AudioManager::update(float deltaTime) {
    // Check finished sources
    for (size_t i = 0; i < sources.size(); i++) {
        if (sources[i].sourceId && !sources[i].looping) {
            ALint state;
            alGetSourcei(sources[i].sourceId, AL_SOURCE_STATE, &state);
            if (state == AL_STOPPED) {
                freeSource(i);
            }
        }
    }
}

void AudioManager::checkALError(const char* operation) {
    ALenum error = alGetError();
    if (error != AL_NO_ERROR) {
        HARUKA_AUDIO_ERROR(ErrorCode::AUDIO_LOAD_FAILED,
            std::string("OpenAL error in ") + operation + ": " + std::to_string(error));
    }
}

}