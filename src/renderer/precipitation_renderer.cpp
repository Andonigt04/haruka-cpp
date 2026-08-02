#include "renderer/precipitation_renderer.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "renderer/shader.h"
#include "tools/profiler.h"
#include "core/logger.h"
#include <cstdio>

namespace Haruka {

namespace {

// UBO del pase (binding 5). Ver precip.vert para el significado de cada campo.
struct PrecipUBO {
    glm::vec4 up;       // xyz cénit · w lado de la celda (m)
    glm::vec4 vel;      // xyz caída+viento (m/s) · w intensidad
    glm::vec4 camCell;  // xyz cámara MOD celda · w tiempo
    glm::vec4 look;     // xyz vista · w 1=nieve
    glm::vec4 tint;     // rgb color · a alcance de fundido (m)
    glm::vec4 misc;     // x alto del viewport (px) · y 1 si hay máscara cenital · zw libre
    glm::mat4 skySpace; // matriz de la máscara cenital
};
static_assert(sizeof(PrecipUBO) == 160, "PrecipUBO std140 size mismatch");

// Lado de la celda que envuelve al observador. Es a la vez el ALCANCE visible de la precipitación:
// más allá, la lluvia se lee como niebla, no como gotas, así que ampliarla es gastar sin ver nada.
constexpr float  kBoxM       = 34.0f;
// Nº de gotas en la celda. La densidad aparente es N/box³; con 34 m y 4500 sale un chubasco denso.
constexpr int    kMaxDrops   = 4500;
// Velocidad terminal. La gota de lluvia real cae a ~9 m/s; el copo, a menos de 1.
constexpr float  kRainFallMS = 9.0f;
constexpr float  kSnowFallMS = 0.9f;

// `fmod` que devuelve SIEMPRE positivo. Con posiciones negativas (el otro lado del planeta) el fmod
// de C da negativo y la rejilla saltaría medio ciclo al cruzar el cero.
inline double wrap(double v, double m) { const double r = std::fmod(v, m); return (r < 0.0) ? r + m : r; }

} // namespace

void PrecipitationRenderer::render(const Params& p) {
    const float amount = p.amount * glm::clamp(p.skyVisibility, 0.0f, 1.0f);
    if (amount < 0.01f || m_failed) return;

    RHI::Device* dev = RHI::device();
    if (!dev) return;

    if (!RHI::valid(m_pso)) {
        const std::string vs = Shader::baseDir() + "shaders/precip.vert";
        const std::string fs = Shader::baseDir() + "shaders/precip.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath   = vs.c_str();
        pd.fragmentPath = fs.c_str();
        pd.topology     = RHI::PrimitiveTopology::Triangles;
        // Depth ON en LECTURA, OFF en ESCRITURA: la gota se somete al mundo (si hay un muro delante,
        // no se ve) pero no tapa a las gotas de detrás ni al agua transparente que venga después.
        // `Greater` es el test del motor (REVERSED-Z) — poner Less aquí no dibujaría nada.
        pd.depth.test = true; pd.depth.write = false; pd.depth.compare = RHI::CompareOp::Greater;
        pd.blend.enable = true; pd.blend.mode = RHI::BlendMode::Alpha;
        m_pso = dev->createPipeline(pd);
        if (!RHI::valid(m_pso)) {   // no linkó: avisar UNA vez y no reintentarlo cada frame
            m_failed = true;
            HARUKA_LOGE("Precipitation", "el pipeline NO linkó (%s) → sin lluvia ni nieve", vs.c_str());
            return;
        }
        m_ubo = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PrecipUBO), nullptr,
                                  RHI::BufferMemory::Dynamic);
    }
    if (!RHI::valid(m_ubo)) return;

    const glm::vec3 up = glm::normalize(p.up);

    // Velocidad = caída por gravedad + arrastre del viento. La misma `p.wind` que mueve las nubes y
    // ondea el follaje → las tres cosas van hacia el mismo lado (antes cada una tenía su dirección).
    const float fall = p.snow ? kSnowFallMS : kRainFallMS;
    // La nieve se deja llevar mucho más que la lluvia (masa/superficie): con 5 m/s de viento la
    // lluvia se inclina un poco y la nieve casi vuela en horizontal.
    const glm::vec3 vel = -up * fall + p.wind * (p.snow ? 0.9f : 0.35f);

    // ANCLAJE AL MUNDO: la rejilla se desplaza con la cámara módulo la celda. Se hace en DOUBLE y
    // aquí, no en el shader: la posición absoluta es del orden de 6.4e6 m y en float32 la parte
    // fraccionaria que necesita el `mod` ya no existe (la gota se quedaría clavada al jugador).
    // Y se proyecta al marco LOCAL (e1, up, e2) —el mismo que construye el shader, misma fórmula—
    // antes de envolver: envolver por ejes de mundo y proyectar después da un anclaje distinto
    // según la orientación, y la rejilla se arrastraría al moverse por el planeta.
    const glm::vec3 e1 = glm::normalize(glm::cross(up, (std::abs(up.y) < 0.9f) ? glm::vec3(0, 1, 0)
                                                                               : glm::vec3(1, 0, 0)));
    const glm::vec3 e2 = glm::cross(up, e1);
    const glm::dvec3 e1d(e1), upd(up), e2d(e2);
    const glm::vec3 camCell((float)wrap(glm::dot(p.cameraPos, e1d), kBoxM),
                            (float)wrap(glm::dot(p.cameraPos, upd), kBoxM),
                            (float)wrap(glm::dot(p.cameraPos, e2d), kBoxM));

    // El tiempo también se envuelve: a las horas de partida, `t·vel` en float perdería la resolución
    // de una gota y la lluvia se congelaría a saltos.
    const float tw = (float)wrap(p.timeSeconds, (double)kBoxM / (double)glm::max(0.05f, glm::length(vel)));

    PrecipUBO u{};
    u.up      = glm::vec4(up, kBoxM);
    u.vel     = glm::vec4(vel, amount);
    u.camCell = glm::vec4(camCell, tw);
    u.look    = glm::vec4(glm::normalize(p.lookDir), p.snow ? 1.0f : 0.0f);
    // La gota no tiene color propio: refleja la luz de la escena. Con luna es azulada y al atardecer
    // dorada — si fuera blanca fija, de noche la lluvia brillaría más que el cielo.
    const glm::vec3 base = p.snow ? glm::vec3(0.94f, 0.95f, 0.98f) : glm::vec3(0.72f, 0.80f, 0.92f);
    u.tint    = glm::vec4(glm::mix(base, base * glm::max(p.sunColor, glm::vec3(0.18f)), 0.55f), kBoxM * 0.5f);
    const bool haveMask = RHI::valid(p.skyMask);
    u.misc    = glm::vec4((float)glm::max(p.viewportHeightPx, 1), haveMask ? 1.0f : 0.0f, 0.0f, 0.0f);
    u.skySpace = p.skySpace;
    dev->updateBuffer(m_ubo, 0, sizeof(u), &u);

    // Menos gotas cuando la precipitación es débil: una llovizna no debe tener la misma densidad que
    // un chubasco con la alfa bajada (se vería como niebla de puntos).
    const int drops = (int)(kMaxDrops * glm::clamp(0.25f + 0.75f * amount, 0.0f, 1.0f));

    RHI::Context* ctx = dev->beginFrame();
    ctx->bindPipeline(m_pso);
    ctx->bindUniformBuffer(5, m_ubo);
    if (haveMask) ctx->bindTexture(0, p.skyMask);
    ctx->draw(drops * 6);
}

} // namespace Haruka
