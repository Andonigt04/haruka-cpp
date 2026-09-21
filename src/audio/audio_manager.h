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
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>

#include "audio/audio_system.h"
#include "audio/audio_loader.h"
#include "audio/sound_points.h"

namespace Haruka {

class AudioManager {
public:
    static AudioManager& get();

    bool init(const std::string& outputDevice = "");
    void shutdown();
    bool available() const { return m_sys.ok(); }

    // Dispositivos de SALIDA según OpenAL (la salida va por OpenAL, no por SDL). El nombre
    // sirve tal cual para init()/alcOpenDevice. Vacío = solo el predeterminado.
    static std::vector<std::string> playbackDevices();

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

    // ── PUNTOS LOGICOS DE SONIDO (sound_points.h) ────────────────────────────────────────────
    // El juego crea puntos (una pisada aqui, un arbol al viento alli, la espuma de la orilla) y
    // los que hacen el mismo sonido se agrupan por sector en una voz. `update()` los mezcla y
    // los enchufa a OpenAL. Los bucles (viento/hojas/agua) van por `ambientVolume`; los disparos
    // por `sfxVolume`. Ver `setVolumes`.
    Audio::SoundPoints&       points()       { return m_points; }
    const Audio::SoundPoints& points() const { return m_points; }
    /// Volumenes: general (listener), ambiente (bucles de puntos), efectos (disparos de puntos).
    void setVolumes(float master, float ambient, float sfx);
    /// Oido bajo el agua: los demas puntos se amortiguan (×0,15) mientras dure.
    void setSubmerged(bool s) { m_submerged = s; }
    bool submerged() const { return m_submerged; }
    /// La vertical local del oyente (los sectores giran alrededor de ella). `setListener` la fija.
    const glm::dvec3& listenerUp() const { return m_listenerUp; }

private:
    AudioSystem m_sys;
    AudioLoader m_loader;
    glm::dvec3  m_listenerPos{0.0};

    // gainBase = volumen pedido; gainCached = volumen ya con propagación (se recalcula a ratos,
    // no cada frame — el pathfind es caro). listenerAtCalc/recalcIn controlan cuándo recalcular.
    struct World {
        glm::dvec3 pos; uint32_t voice; float gainBase;
        float gainCached = 1.0f; int recalcIn = 0; glm::dvec3 listenerAtCalc{0.0};
    };
    std::unordered_map<SourceId, World> m_world;
    SourceId m_nextId = 1;

    // Puntos logicos: una voz AL por ranura de SoundPoints (misma indexacion).
    Audio::SoundPoints m_points;
    struct SlotVoice { uint32_t voice = 0; uint32_t key = 0; bool playing = false; };
    std::vector<SlotVoice> m_slotVoices;
    glm::dvec3 m_listenerUp{0.0, 1.0, 0.0};
    float m_volAmbient = 0.8f, m_volSfx = 1.0f;
    bool  m_submerged = false;
    double m_lastUpdateS = -1.0;
    void updatePoints();
};

} // namespace Haruka
