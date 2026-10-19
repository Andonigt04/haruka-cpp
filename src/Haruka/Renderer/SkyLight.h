#pragma once
/**
 * @file Renderer/SkyLight.h
 * @brief LA LUZ DEL FRAME POR FISICA: Sol y Luna con su irradiancia real, el aire que la tiñe y la
 *        exposicion de la camara. Un UBO (binding 31) que leen TODOS los pases: cielo, terreno, agua,
 *        props, objetos y nubes — una sola respuesta a "cuanta luz hay aqui y de que color".
 *
 * Andoni (17-10): "que la luz la haga real con radiacion de luz etc no post añadido". La fisica vive en
 * `World/Planet/Atmosphere.h` y `SkyRadiometry.h` (puros y con tests); esto la evalua para el ojo de
 * este frame y la sube. Todo lo que va al UBO esta YA EXPUESTO (multiplicado por la apertura de la
 * camara), asi que los shaders no saben nada de exposicion: reciben luz y la sombrean.
 *
 * Gemelo GLSL del bloque: `assets/shaders/lib/sky_light.glsl`.
 */
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

#include "Haruka/RHI/RHIResources.h"
#include "Haruka/World/Planet/Atmosphere.h"

namespace Haruka { namespace RHI { class Context; } }

namespace Haruka::Renderer {

/// std140. ⚠️ Gemelo de `SkyLight` en lib/sky_light.glsl: un campo aqui, el mismo alli y en su sitio.
struct alignas(16) SkyLightUBO {
    glm::vec4 sunDir;       ///< xyz hacia el Sol (mundo) · w = radio angular del disco (rad)
    glm::vec4 sunE;         ///< rgb irradiancia solar en lo alto de la atmosfera, expuesta · w = exposicion
    glm::vec4 moonDir;      ///< xyz hacia la Luna · w = radio angular
    glm::vec4 moonE;        ///< rgb irradiancia de la Luna en lo alto de la atmosfera, expuesta · w = estrella de magnitud 0, expuesta
    glm::vec4 planet;       ///< xyz centro del planeta MENOS el ojo (m) · w = radio (m)
    glm::vec4 sunGround;    ///< rgb el Sol que llega al ojo (tras el aire), expuesto
    glm::vec4 moonGround;   ///< rgb idem la Luna
    glm::vec4 ambientUp;    ///< rgb irradiancia del cielo sobre una cara que mira arriba, expuesta
    glm::vec4 ambientDown;  ///< rgb idem una cara que mira abajo (el suelo devuelve luz)
    glm::vec4 up;           ///< xyz vertical del ojo · w = altitud del ojo (m)
    glm::vec4 sunAtMoon;    ///< rgb irradiancia del Sol EN la Luna, vista a traves del aire hasta el ojo, expuesta
    glm::vec4 skyZenith;    ///< rgb radiancia del cielo al CENIT (π·L), expuesta: lo que refleja una cara al cielo
    glm::vec4 skyHorizon;   ///< rgb idem en el HORIZONTE (media en acimut, a 3°)
};
static_assert(sizeof(SkyLightUBO) == 208, "SkyLightUBO std140 size mismatch");

/**
 * @brief Gemelo CPU de `harukaDisplay` (lib/sky_light.glsl): radiancia lineal expuesta -> pantalla.
 * Rodilla a 0,8 sobre el canal mayor conservando el tono, a blanco solo con sobreexposicion extrema
 * (8-200 veces), y gamma 2,2. Para las sondas que comparan el cielo de la GPU con la integral de CPU.
 */
inline glm::dvec3 skyDisplay(glm::dvec3 lin) {
    lin = glm::max(lin, glm::dvec3(0.0));
    const double m = std::max(lin.x, std::max(lin.y, lin.z));
    const double knee = 0.8;
    if (m > knee) {
        const double mc = knee + (1.0 - knee) * (1.0 - std::exp(-(m - knee) / (1.0 - knee)));
        const double t = std::clamp((m - 8.0) / (200.0 - 8.0), 0.0, 1.0);
        const double w = t * t * (3.0 - 2.0 * t);
        lin = glm::mix(lin * (mc / m), glm::dvec3(mc), w);
    }
    return glm::pow(lin, glm::dvec3(1.0 / 2.2));
}

class SkyLight {
public:
    struct Input {
        glm::dvec3 eye{0.0};             ///< posicion del ojo (mundo, m)
        glm::dvec3 planetCenter{0.0};
        double     planetRadius = 0.0;   ///< 0 = sin planeta: sin aire (espacio)
        bool       hasSun = false;
        glm::dvec3 sunPos{0.0};
        double     sunRadius = 6.957e8;
        bool       hasMoon = false;
        glm::dvec3 moonPos{0.0};
        double     moonRadius = 0.0;
        double     dtS = 0.016;          ///< para la adaptacion de la exposicion
    };

    /// Evalua la luz para el ojo de este frame (CPU) y deja el bloque listo en `data()`.
    void update(const Input& in);
    /// Sube el bloque y lo ata al binding 31 del contexto (estado pegajoso para el resto del frame).
    void bind(RHI::Context* ctx);
    void shutdown();

    const SkyLightUBO& data() const { return m_ubo; }
    double exposure() const { return m_exposure; }
    /// Exposicion fija (pruebas y capturas comparables): <= 0 = automatica.
    void setFixedExposure(double k) { m_fixedExposure = k; }

    static constexpr uint32_t kBinding = 31;
    static constexpr uint32_t kMsLutBinding = 20;   ///< la tabla de dispersion multiple (lib/atmosphere.glsl)
    /// La tabla con la que se integra (la misma que se sube): para las sondas y los tests.
    const World::Planet::AtmoMsLut& msLut() const { return m_ms; }

private:
    SkyLightUBO       m_ubo{};
    RHI::BufferHandle m_buffer{};
    World::Planet::AtmoMsLut m_ms;          ///< por radio de planeta: se rehace si cambia
    RHI::TextureHandle m_msTex{};
    bool               m_msDirty = false;
    double            m_exposure = 0.0;
    double            m_fixedExposure = -1.0;
    // Irradiancia del cielo SIN exponer: integrarla cuesta unos cientos de rayos, asi que se rehace
    // solo cuando se mueven el Sol, la Luna o la altitud del ojo.
    glm::dvec3        m_skyUp{0.0}, m_skyZen{0.0}, m_skyHor{0.0};
    double            m_cacheSunEl = -9.0, m_cacheMoonEl = -9.0, m_cacheMoonE = -1.0, m_cacheAlt = -1e9;
};

} // namespace Haruka::Renderer
