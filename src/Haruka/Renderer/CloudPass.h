#pragma once
/**
 * @file Renderer/CloudPass.h
 * @brief EL PASE VOLUMETRICO DE NUBES como modulo: el horneado del cielo del planeta (cobertura,
 *        base y techo por direccion, en un hilo), el fundido entre horneados, la deriva integrada
 *        del campo, la marcha a 1/N de resolucion contra una copia del depth de la escena y la
 *        composicion a resolucion completa. Vivia en `Application` (ApplicationRender.cpp, ~640
 *        lineas y 24 miembros `m_cloud*`); Andoni (21-09): "los modulos estan en application_*".
 *
 * Un gancho por frame: `draw()` despues de toda la geometria (la nube es un medio que se recorre,
 * no una superficie) y antes del post. `enabled()` es lo que consulta `sky.frag` para saber si
 * debe pintar el cumulo plano de respaldo — los dos sitios tienen que estar de acuerdo.
 */
#include <future>
#include <vector>
#include <glm/glm.hpp>

#include "Haruka/RHI/RHITypes.h"
#include "Haruka/RHI/RHIResources.h"
#include "Haruka/World/CloudMotion.h"   // CloudDrift (deriva integrada) + BakeBlend (fundido de horneados) + CloudSpin
#include "Haruka/World/Props/ShellScatter.h"

namespace Haruka { namespace Core { class Camera; } }
namespace Haruka::World::Planet { class PlanetarySystem; }
namespace Haruka::World { class WorldSystem; }

namespace Haruka::Renderer {

class CloudPass {
public:
    /// Lo que el pase necesita del frame y no es suyo.
    struct Frame {
        Core::Camera*          camera  = nullptr;
        World::Planet::PlanetarySystem*       planets = nullptr;
        const World::WorldSystem*     world   = nullptr;
        RHI::RenderPassHandle  sceneTargetPass{};   ///< invalido = la escena vive en el backbuffer
        int   width = 1, height = 1;                ///< del target de escena (reducido si hay post)
        float rainOverride = -1.0f;                 ///< consola `rain`: fuerza precipitacion; <0 = auto
        glm::vec3 windVec{0.0f};                    ///< el UNICO viento del mundo (deriva del campo)
        glm::vec4 aerial{0.0f};                     ///< perspectiva aerea (lib/aerial.glsl)
        double dynamicWaterM3 = 0.0;                ///< solo para el log del ciclo del agua
    };

    ~CloudPass();
    void shutdown();   ///< suelta la GPU (el device sigue vivo)

    /// Pase apagado en ajustes. `enabled()` incluye ademas `HARUKA_CLOUD_VOL=0`.
    void setEnabled(bool on) { m_enabled = on; }
    bool enabled() const { return m_enabled && !envOff(); }
    /// `HARUKA_CLOUD_STEPS=N` (8..128; 48 por defecto) y `HARUKA_CLOUD_VOL=0`.
    static int  steps();
    static bool envOff();

    /// Despues de toda la geometria y antes del post: hornea si toca, marcha y compone.
    void draw(const Frame& f);

    // ── LOS CÚMULOS DE MALLA (low-poly anime) ─────────────────────────────────────────────────
    // Andoni (17-10): "nubes mas tipo low-poly anime", con la panza plana. El cúmulo deja de ser un
    // medio que se marcha: son racimos de domos (tapa plana en el nivel de condensación) y esferas,
    // sembrados en la GPU (`World/Props/ShellScatter`) desde el MISMO horneado del clima (cobertura,
    // base, techo, lluvia) y dibujados con el pase de props. La retícula gira con el viento
    // (`World::CloudSpin`). Mientras están activos, la marcha volumétrica solo pinta el vapor en
    // altura (máscara 6): el cúmulo lo pone la malla. `HARUKA_CLUMPS=0` vuelve al cúmulo volumétrico.
    struct ClumpFrame {
        Core::Camera*                   camera  = nullptr;
        World::Planet::PlanetarySystem* planets = nullptr;
        glm::vec3 windVec{0.0f};   ///< el viento del mundo (m/s, superficie)
        int viewW = 0, viewH = 0;  ///< el framebuffer que se dibuja
    };
    /// Gira la retícula con el viento y siembra (compute: FUERA de cualquier render pass).
    void prepareClumps(RHI::Context* ctx, const ClumpFrame& f);
    /// Dibuja lo sembrado DENTRO del pase de escena.
    void drawClumps(RHI::Context* ctx, RHI::BufferHandle perFrameUBO, RHI::PipelineHandle restorePSO, float time);
    bool clumpsActive() const { return m_clumpsActive; }
    static bool clumpsEnvOff();
    /// Cota de ojo por encima de la cual el cúmulo vuelve a ser volumétrico. ⚠️ PROVISIONAL: desde
    /// órbita un racimo de 1,5 km mide 2 px y harían falta racimos jerárquicos (el siguiente paso).
    static constexpr double kClumpMaxEyeAltM = 30000.0;
    const World::Props::ShellScatter& clumps() const { return m_clumps; }

private:
    /// `mid` y `high`: las capas de vapor en ALTURA del clima (−10 °C y −40 °C, `World/CloudColumn.h`)
    /// en el mismo formato que `rgba` (cobertura, base, techo, oscurecer), con su banda de cotas.
    struct SkyBake {
        std::vector<float> rgba, hi, mid, high;
        float coverMax = 0, baseMin = 700, topMax = 1800;
        float midLo = 4000, midHi = 6000, highLo = 8000, highHi = 11000;
        double ms = 0; int w = 0, h = 0;
    };

    RHI::PipelineHandle   m_cloudPSO{}, m_cloudUpPSO{};
    RHI::BufferHandle     m_cloudUBO{};
    /// Copia del depth de la escena: no se puede samplear la profundidad del MISMO target al que se
    /// dibuja (realimentacion). Su formato sigue al de la FUENTE del blit (backbuffer o target de post).
    RHI::RenderPassHandle m_cloudDepthRT{};
    int m_cloudDepthW = 0, m_cloudDepthH = 0;
    RHI::Format m_cloudDepthFmt = RHI::Format::D32F;
    /// Target REDUCIDO donde se marcha la nube (1/N de lado): el coste manda por pixel, no por paso.
    RHI::RenderPassHandle m_cloudRT{};
    int m_cloudRTW = 0, m_cloudRTH = 0;
    /// El cielo del planeta por direccion (equirect 256x128, RGBA32F): cobertura/base/techo/lluvia,
    /// y las capas altas aparte. Nuevo y ANTERIOR: el shader los funde con `blend.x`.
    RHI::TextureHandle m_cloudCoverTex{}, m_cloudHiTex{}, m_cloudCoverTexPrev{}, m_cloudHiTexPrev{};
    RHI::TextureHandle m_cloudCoverDummy{};   // 1x1: nunca un sampler sin atar en Vulkan
    float m_cloudCoverMax = 0.0f;
    float m_cloudBaseMin = 700.0f, m_cloudTopMax = 1800.0f;   // banda GLOBAL que marcha el pase
    /// Campos ESTATICOS del planeta (humedad, temperatura, mar, cota), horneados una vez cuando el
    /// clima ya existe. Vacios = aun no listos (un horneado degenerado no se cachea).
    std::vector<float> m_cloudHumField, m_cloudTempField, m_cloudWaterField, m_cloudGroundField;
    /// El horneado del cielo EN VUELO (134 ms medidos a 256x128: en el hilo de render eran 8 frames).
    std::future<SkyBake> m_skyBakeJob;
    std::vector<float>   m_lastSkyRGBA;   ///< ultimo cielo horneado, para el ciclo del agua
    std::vector<float>   m_lastMid, m_lastHigh;   ///< sus capas en altura (diagnóstico: qué pone el clima)
    int                  m_lastSkyW = 0, m_lastSkyH = 0;
    double               m_lastWeatherT = -1.0;
    World::CloudDrift   m_cloudDrift;    ///< deriva del campo, INTEGRADA (core/cloud_motion.h)
    World::CloudSpin    m_cloudSpin;     ///< giro de la retícula de cúmulos de malla con el viento
    World::Props::ShellScatter m_clumps;
    /// LAS CAPAS EN ALTURA, sembradas igual que el cúmulo. Ninguna se enciende ni se apaga aquí: hay
    /// nube donde el CLIMA pone vapor, a la cota de su temperatura. Para cambiar dónde o cuánta, se
    /// cambia el clima (`WeatherSystem`, `CloudColumn.h`), no el render.
    World::Props::ShellScatter m_clumpsMid, m_clumpsHigh;
    RHI::TextureHandle m_cloudMidTex{}, m_cloudMidTexPrev{}, m_cloudHighTex{}, m_cloudHighTexPrev{};
    float  m_midLoM = 4000.0f, m_midHiM = 6000.0f, m_highLoM = 8000.0f, m_highHiM = 11000.0f;
    bool   m_clumpsTried = false, m_clumpsActive = false;
    float  m_cloudConvTopMax = 1800.0f;  ///< techo CONVECTIVO máximo del último horneado (sin el vapor en altura)
    World::BakeBlend    m_cloudBlend;    ///< fundido entre horneados
    bool m_enabled = true;
};

} // namespace Haruka::Renderer
