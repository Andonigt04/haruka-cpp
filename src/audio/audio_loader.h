#pragma once
/**
 * @file audio/audio_loader.h
 * @brief Crea/gestiona buffers de audio (OpenAL). Hoy: tono PROCEDURAL (sin assets), cacheado
 *        por parámetros (se reutiliza, no se libera por uso). Futuro: carga de ficheros
 *        (WAV/OGG) desde assets/audio/<idioma>. Devuelve handles de buffer AL.
 */
#include <cstdint>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>

namespace Haruka {

class AudioLoader {
public:
    // Tono procedural → buffer AL (cacheado). loop=true → onda continua (sin decaimiento).
    uint32_t tone(float freq, float dur, const glm::vec3& timbre, bool loop);

    // Carga un fichero de audio → buffer AL (cacheado por ruta). Soporta WAV PCM (8/16-bit,
    // mono/estéreo). Devuelve 0 si falla. (OGG: pendiente, vía libvorbis.) MONO = espacializable.
    uint32_t file(const std::string& path);

    void clear();   // libera todos los buffers (al cerrar)

private:
    std::unordered_map<uint64_t, uint32_t>    m_cache;   // tonos: clave(params) → buffer
    std::unordered_map<std::string, uint32_t> m_files;   // ficheros: ruta → buffer
};

} // namespace Haruka
