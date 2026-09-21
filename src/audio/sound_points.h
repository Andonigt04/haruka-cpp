#pragma once
/**
 * @file audio/sound_points.h
 * @brief PUNTOS LOGICOS DE SONIDO: cada hecho del mundo (una rama que se rompe, una pisada) o
 *        cada objeto que suena mientras algo le pasa (un arbol al viento, la espuma de la orilla)
 *        es un punto con un sonido y una potencia. Y los que hacen EL MISMO sonido se AGRUPAN.
 *
 * Andoni (21-09): "puntos logicos de sonido, como si un jugador pone un pie sobre una rama y
 * suena, crea un punto de sonido donde se ha roto la rama", y "si se capta cuantos objetos hacen
 * el mismo sonido, se agrupan si son muchos y se mezclan".
 *
 * Como se agrupa: el espacio alrededor del oido se parte en SECTORES (12 de azimut × 3 anillos
 * de distancia). Todos los puntos con el mismo sonido que caen en el mismo sector son UNA voz:
 * suena en el centroide ponderado por energia y con la energia SUMADA (cien arboles a 40 m no
 * son cien fuentes, son un bosque al este). Cada grupo conserva su voz de frame a frame — no se
 * reinicia el bucle— y si desaparece se apaga en 100 ms. Presupuesto: `maxVoices` grupos, los mas
 * fuertes ganan.
 *
 * La atenuacion por distancia la hace ESTO (energia = potencia · (ref/d)²), no OpenAL: las voces
 * se crean con rolloff 0 y la ganancia es la del grupo. Asi el mismo numero sirve para el playback
 * y para preguntar "¿cuanto se oye aqui?".
 *
 * Esto es puro (sin OpenAL): el banco lo mide. Quien lo enchufa es `AudioManager`.
 */
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>

#include "audio/sound_synth.h"

namespace Haruka::Audio {

class SoundPoints {
public:
    using Id = uint32_t;

    struct Config {
        float refDistM     = 6.0f;      ///< a esta distancia una potencia 1 suena a ganancia 1
        float hearMinGain  = 0.004f;    ///< por debajo, el grupo no merece voz
        int   sectors      = 12;        ///< de azimut
        float ringEdgesM[3] = { 15.0f, 45.0f, 160.0f };   ///< anillos de distancia; mas alla, nada
        int   maxVoices    = 24;
        float smoothS      = 0.12f;     ///< constante de tiempo de ganancia y posicion
        float fadeOutS     = 0.10f;     ///< un grupo que desaparece se apaga en esto
        float gainCap      = 1.5f;      ///< tope de ganancia de un grupo (energia sumada)
    };

    SoundPoints() = default;
    explicit SoundPoints(const Config& c) : m_cfg(c) {}
    const Config& config() const { return m_cfg; }

    // ── Puntos persistentes (objetos que suenan mientras algo les pasa) ────────────────────────
    /// `power` 1 = sonoridad nominal a `refDistM`; 0 = mudo (el punto sigue existiendo). `pitch`
    /// multiplica el tono (el viento silba mas agudo cuanto mas sopla).
    Id   add(const glm::dvec3& pos, Sound sound, float power, float pitch = 1.0f);
    void set(Id id, const glm::dvec3& pos, float power, float pitch = 1.0f);
    void setPower(Id id, float power);
    void remove(Id id);
    bool has(Id id) const { return m_points.count(id) != 0; }
    size_t pointCount() const { return m_points.size(); }

    // ── Hechos (un disparo) ────────────────────────────────────────────────────────────────────
    /// Se agrupan como los persistentes, pero solo dentro del frame: diez ramas que crujen a la vez
    /// en el mismo sector son un crujido mas fuerte, no diez.
    void emit(const glm::dvec3& pos, Sound sound, float power, float pitch = 1.0f);

    // ── Un grupo ya mezclado ───────────────────────────────────────────────────────────────────
    struct Voice {
        Sound     sound  = Sound::WindEar;
        glm::vec3 relPos{0.0f};     ///< respecto al oido (marco del mundo); (0,0,0) = sin direccion
        float     gain   = 0.0f;    ///< ya suavizada (persistentes) o final (disparos)
        float     pitch  = 1.0f;
        uint32_t  key    = 0;       ///< sonido+sector; identidad del grupo entre frames
        bool      active = false;   ///< false = ranura libre (o apagandose si gain > 0)
        int       count  = 0;       ///< cuantos puntos mezcla
    };

    /// Recalcula grupos y ranuras. `up` = vertical local (los sectores giran alrededor de ella).
    void update(const glm::dvec3& listener, const glm::vec3& up, float dt);

    /// Ranuras de voz de los persistentes: indices ESTABLES (la voz de OpenAL vive en la misma
    /// ranura hasta que `active` cae y `gain` llega a 0).
    const std::vector<Voice>& voices() const { return m_slots; }
    /// Disparos ya mezclados de este frame (se vacian en el siguiente `update`).
    const std::vector<Voice>& oneShots() const { return m_shots; }

    /// Para el depurador: cuantos grupos habia antes del presupuesto.
    int groupsBeforeBudget() const { return m_groupsBefore; }

    // ── Puro, publico para el banco ────────────────────────────────────────────────────────────
    /// Sector de un punto relativo al oido: (anillo, azimut) o −1 si esta fuera de alcance.
    int sectorOf(const glm::vec3& rel, const glm::vec3& up, const glm::vec3& e1, const glm::vec3& e2) const;
    /// Atenuacion de energia por distancia: (ref / max(ref, d))².
    float energyAt(float distM) const;

private:
    struct Point { glm::dvec3 pos; Sound sound; float power; float pitch; };
    struct Group {
        uint32_t key; Sound sound; glm::vec3 posE{0.0f}; float energy = 0.0f; float pitchE = 0.0f; int count = 0;
    };
    void cluster(const std::vector<Point>& pts, const glm::dvec3& listener, const glm::vec3& up,
                 std::vector<Group>& out) const;
    void tangentFrame(const glm::vec3& up, glm::vec3& e1, glm::vec3& e2) const;

    Config m_cfg;
    std::unordered_map<Id, Point> m_points;
    std::vector<Point>  m_pending;      // disparos de este frame
    std::vector<Voice>  m_slots;
    std::vector<Voice>  m_shots;
    Id  m_next = 1;
    int m_groupsBefore = 0;
};

} // namespace Haruka::Audio
