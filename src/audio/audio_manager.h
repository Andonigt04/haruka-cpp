/**
 * @file audio_manager.h
 * @brief OpenAL audio subsystem singleton — device/context, buffer cache, and 3D source pool.
 */
#pragma once

#include <AL/al.h>
#include <AL/alc.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace Haruka {

/** @brief OpenAL source state tracked by the manager. */
struct AudioSource {
    ALuint sourceId = 0;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 velocity = glm::vec3(0.0f);
    float gain = 1.0f;
    float pitch = 1.0f;
    bool looping = false;
    bool positional = true;
};

/**
 * @brief Loaded audio buffer resource wrapper.
 *
 * Owns one OpenAL buffer and loads it from WAV/OGG disk assets.
 */
class AudioBuffer {
public:
    /** @brief Loads an audio file into an OpenAL buffer. */
    AudioBuffer(const std::string& filepath);
    ~AudioBuffer();
    
    /** @brief OpenAL buffer handle. */
    ALuint getBufferId() const { return bufferId; }
    /** @brief Returns true when the buffer has been loaded successfully. */
    bool isLoaded() const { return bufferId != 0; }

private:
    ALuint bufferId = 0;
    /** @brief Loads a WAV file from disk into the buffer. */
    bool loadWAV(const std::string& filepath);
    /** @brief Loads an OGG file from disk into the buffer. */
    bool loadOGG(const std::string& filepath);
};

/**
 * @brief Audio subsystem singleton (device/context, buffers, and source pool).
 */
class AudioManager {
public:
    static AudioManager& getInstance() {
        static AudioManager instance;
        return instance;
    }
    
    /** @brief Initializes the OpenAL device/context and source pool. */
    bool init();
    /** @brief Releases OpenAL resources and clears managed audio state. */
    void shutdown();
    
    /** @name Listener state */
    ///@{
    void setListenerPosition(const glm::vec3& pos);
    void setListenerVelocity(const glm::vec3& vel);
    void setListenerOrientation(const glm::vec3& forward, const glm::vec3& up);
    ///@}
    
    /** @brief Loads a named sound buffer into the manager cache. */
    bool loadSound(const std::string& name, const std::string& filepath);
    
    /** @name Playback helpers */
    ///@{
    int playSound(const std::string& name, const glm::vec3& position = glm::vec3(0.0f), 
                  bool loop = false, float gain = 1.0f, float pitch = 1.0f);
    int playSound2D(const std::string& name, bool loop = false, float gain = 1.0f);
    ///@}
    
    /** @name Source control */
    ///@{
    void stopSource(int sourceId);
    void pauseSource(int sourceId);
    void resumeSource(int sourceId);
    
    void setSourcePosition(int sourceId, const glm::vec3& pos);
    void setSourceVelocity(int sourceId, const glm::vec3& vel);
    void setSourceGain(int sourceId, float gain);
    void setSourcePitch(int sourceId, float pitch);
    ///@}
    
    /** @name Master output */
    ///@{
    void setMasterVolume(float volume);
    float getMasterVolume() const { return masterVolume; }
    ///@}
    
    /** @brief Updates listener/source state for 3D audio calculations. */
    void update(float deltaTime);

private:
    AudioManager();
    ~AudioManager();
    
    ALCdevice* device = nullptr;
    ALCcontext* context = nullptr;
    
    std::map<std::string, std::shared_ptr<AudioBuffer>> buffers;
    std::vector<AudioSource> sources;
    std::vector<int> freeSources;
    
    float masterVolume = 1.0f;
    
    /** @brief Allocates an available source slot. */
    int allocateSource();
    /** @brief Returns a source slot to the free pool. */
    void freeSource(int sourceId);
    
    /** @brief Reports OpenAL errors for a specific operation. */
    void checkALError(const char* operation);
};

}