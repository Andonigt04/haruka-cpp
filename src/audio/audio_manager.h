#pragma once
/**
 * @file audio/audio_manager.h
 * @brief API de audio de alto nivel (módulo AUDIO). Orquesta AudioSystem (backend OpenAL) y
 *        AudioLoader (buffers). Mantiene el listener (cámara), las FUENTES del mundo (emisores
 *        persistentes) y la PROPAGACIÓN (cuánta señal llega de A a B; oclusión por terreno hoy,
 *        portales/puertas en el futuro). propagation() sirve para el playback Y como consulta
 *        lógica (IA: ¿oye al jugador? → sigilo).
 */
#include <cstdint>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>

#include "audio/audio_system.h"
#include "audio/audio_loader.h"

namespace Haruka {

class AudioManager {
public:
    static AudioManager& get();

    bool init(const std::string& outputDevice = "");
    void shutdown();
    bool available() const { return m_sys.ok(); }

    void setListener(const glm::dvec3& pos, const glm::dvec3& fwd, const glm::dvec3& up);

    // One-shot posicional (impactos, etc.). Atenuado por propagación.
    void playTone(const glm::dvec3& pos, float freq, float dur, float volume,
                  const glm::vec3& timbre = {1, 1, 1});
    // One-shot desde un FICHERO (WAV). Para audio espacial usa WAV mono.
    void playSound(const glm::dvec3& pos, const std::string& path, float volume);

    // Fuente persistente que emite desde un sitio (hoguera, máquina…). Handle (0 = fallo).
    using SourceId = uint32_t;
    SourceId addSource(const glm::dvec3& pos, float freq, float volume,
                       const glm::vec3& timbre = {1, 1, 1});
    SourceId addSourceFile(const glm::dvec3& pos, const std::string& path, float volume);
    void     setSourcePosition(SourceId id, const glm::dvec3& pos);
    void     removeSource(SourceId id);

    // Cuánta señal llega de 'from' a 'to' (0 tapado .. 1 libre). Playback + consulta IA (sigilo).
    float propagation(const glm::dvec3& from, const glm::dvec3& to) const;

    void update();   // recoloca/atenúa las fuentes persistentes cada frame

private:
    AudioSystem m_sys;
    AudioLoader m_loader;
    glm::dvec3  m_listenerPos{0.0};

    struct World { glm::dvec3 pos; uint32_t voice; float gain; };
    std::unordered_map<SourceId, World> m_world;
    SourceId m_nextId = 1;
};

} // namespace Haruka
