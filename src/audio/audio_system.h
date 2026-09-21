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
    void voiceSetPitch(uint32_t voice, float pitch);
    /// rolloff 0 = la distancia NO atenua (quien atenua es el llamador: ver SoundPoints).
    void voiceSetRolloff(uint32_t voice, float rolloff);

    /// Disparo con la ganancia YA atenuada (rolloff 0) y tono propio: los grupos de SoundPoints.
    void playOneShotFlat(uint32_t buffer, const glm::vec3& relPos, float gain, float pitch);

    /// Volumen general (AL_GAIN del listener). Antes `masterVolume` no se aplicaba en ningun sitio.
    void setMasterGain(float g);

private:
    bool  m_ok = false;
    void* m_device  = nullptr;   // ALCdevice*
    void* m_context = nullptr;   // ALCcontext*
    std::vector<uint32_t> m_pool;   // voces reutilizables para one-shots
};

} // namespace Haruka
