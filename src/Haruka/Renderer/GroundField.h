#pragma once
/**
 * @file GroundField.h
 * @brief EL CAMPO CENITAL DE SUELO alrededor de la cámara (`assets/shaders/ground_field.frag`): lo
 *        que pasa por el suelo deja marca y la marca se borra con el tiempo. No sabe QUIÉN lo lee.
 *
 * Canales (RGBA16F): rg = EMPUJE (dirección · fuerza) · b = MARCA/hundido · a = REBORDE. Cada lector
 * decide qué significan: la hierba se tumba con `rg`, una huella en nieve lee `b` y `a`, una estela
 * en agua el empuje… Aquí solo se ancla, se desplaza, se decae y se estampa.
 *
 * ── EL ANCLA ─────────────────────────────────────────────────────────────────────────────────────
 * Un marco Este/Norte tangente a la esfera, fijado al anclar. El ancla se mueve a saltos de téxel
 * ENTERO cuando la cámara se aleja más de un cuarto del lado (el contenido no se emborrona al
 * recentrar) y el marco solo se rehace si la vertical ha girado ~1° (≈110 km en la Tierra): al
 * rehacerlo el campo se vacía.
 *
 * Con memoria (`Config::pushRecoverS` / `markRecoverS` > 0) un sello es un EVENTO: se estampa una vez
 * y el campo lo recuerda. Hasta `kMaxStamps` por frame; los demás de ese frame no dejan marca.
 */
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "Haruka/RHI/RHIResources.h"

namespace Haruka { namespace RHI { class Context; class Device; } }

namespace Haruka::Renderer {

class GroundField {
public:
    static constexpr int kMaxStamps = 64;   ///< gemelo de `uStamp[64]` en ground_field.frag

    struct Config {
        int   res          = 512;     ///< lado (téxeles)
        float texelM       = 0.25f;   ///< metros por téxel (512 × 0,25 = 128 m de lado)
        float pushRecoverS = 5.0f;    ///< s en recuperar el EMPUJE (0 = sin memoria)
        float markRecoverS = 0.0f;    ///< s en borrar MARCA y REBORDE (0 = sin memoria)
        float soft         = 0.35f;   ///< borde blando del sello (fracción del radio)
        float spread       = 0.6f;    ///< cuánto abre el empuje hacia fuera en el borde
        float rimGain      = 0.0f;    ///< ganancia del reborde (0 = sin reborde)
        bool  accumulate   = false;   ///< marca/reborde se suman (camino pisado) o se quedan con el máximo
    };
    /// El marco del campo ESTE frame: con esto un lector pasa de mundo a UV.
    struct View {
        glm::dvec3 anchor{0.0};   ///< centro del campo (mundo)
        glm::dvec3 east{1, 0, 0}, north{0, 0, 1};
        float      texelM = 0.25f;
        int        res    = 0;    ///< 0 = no hay campo
    };

    GroundField() = default;
    GroundField(const GroundField&) = delete;
    GroundField& operator=(const GroundField&) = delete;
    ~GroundField() { shutdown(); }

    bool init(RHI::Device* dev, const Config& cfg);
    void shutdown();
    bool ready() const { return m_ready; }

    /** @brief Un sello este frame: disco de `radiusM` en `worldPos`, empujando hacia `velocity`
     *  (0 = solo hacia fuera). `mark` = cuánto deja en la MARCA (0..1). */
    void stamp(const glm::dvec3& worldPos, float radiusM, const glm::vec3& velocity,
               float strength = 1.0f, float mark = 0.0f);
    size_t pendingStamps() const { return m_stamps.size(); }

    /** @brief Recentra, decae y estampa: UN pase de pantalla completa (fuera de cualquier otro
     *  render pass). `up` = vertical local de la cámara. */
    void update(RHI::Context* ctx, const glm::dvec3& camPos, const glm::dvec3& up, float dt);

    RHI::TextureHandle texture() const { return m_ready ? m_tex[m_cur] : RHI::TextureHandle{}; }
    View view() const;

private:
    struct Stamp { glm::dvec3 pos; float radius; glm::vec3 vel; float strength; float mark; };
    RHI::Device*         m_dev = nullptr;
    Config               m_cfg;
    bool                 m_ready = false;
    RHI::PipelineHandle  m_pipe{};
    RHI::BufferHandle    m_ubo{};
    RHI::RenderPassHandle m_rt[2]{};
    RHI::TextureHandle   m_tex[2]{};
    int                  m_cur = 0;
    bool                 m_anchored = false;
    glm::dvec3           m_anchor{0.0}, m_up{0, 1, 0}, m_east{1, 0, 0}, m_north{0, 0, 1};
    std::vector<Stamp>   m_stamps;
};

} // namespace Haruka::Renderer
