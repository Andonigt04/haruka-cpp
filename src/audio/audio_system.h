#pragma once
/**
 * @file audio/audio_system.h
 * @brief Backend OpenAL PURO (módulo AUDIO). Contexto/dispositivo, listener y "voces" (fuentes
 *        AL): un pool reutilizable para one-shots y voces persistentes con handle. Sin lógica
 *        de juego ni espacial — eso vive en AudioManager. Los buffers los da AudioLoader.
 */
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace Haruka {

class AudioSystem {
public:
    bool init(const std::string& outputDevice = "");   // "" = predeterminado
    void shutdown();
    bool ok() const { return m_ok; }

    // El listener va SIEMPRE en el origen (esquema camera-relativo); solo orientación.
    void setListenerOrientation(const glm::vec3& fwd, const glm::vec3& up);

    // One-shot: coge una voz libre del pool y reproduce un buffer en relPos (relativo al oído).
    void playOneShot(uint32_t buffer, const glm::vec3& relPos, float gain, float refDist = 6.0f);

    // Voces persistentes (fuentes del mundo): handle propio que tú gestionas.
    uint32_t voiceCreate();
    void voiceDestroy(uint32_t voice);
    void voicePlay(uint32_t voice, uint32_t buffer, const glm::vec3& relPos,
                   float gain, bool loop, float refDist = 6.0f);
    void voiceSetPos(uint32_t voice, const glm::vec3& relPos);
    void voiceSetGain(uint32_t voice, float gain);

private:
    bool  m_ok = false;
    void* m_device  = nullptr;   // ALCdevice*
    void* m_context = nullptr;   // ALCcontext*
    std::vector<uint32_t> m_pool;   // voces reutilizables para one-shots
};

} // namespace Haruka
