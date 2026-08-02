#include "renderer/ground_stamp_renderer.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "renderer/shader.h"
#include "core/logger.h"
#include <glm/gtc/matrix_transform.hpp>
#include <cstdio>
#include <string>
#include <vector>

namespace Haruka {

namespace {
// El UBO es un array fijo: siempre se dibujan `kMaxStamps` quads y las muertas salen degeneradas en
// el vertex shader. Un draw de tamaño fijo evita re-crear buffers al cambiar el número de huellas.
struct StampUBO { glm::vec4 stamps[GroundLayer::kMaxStamps]; };
} // namespace

bool GroundStampRenderer::render(const GroundLayer& layer, const glm::dvec3& center,
                                 const glm::vec3& up, double now) {
    if (m_failed) return false;
    RHI::Device* dev = RHI::device();
    if (!dev) return false;

    if (!RHI::valid(m_pso)) {
        const std::string vs = Shader::baseDir() + "shaders/ground_stamp.vert";
        const std::string fs = Shader::baseDir() + "shaders/ground_stamp.frag";
        RHI::PipelineDesc pd;
        pd.vertexPath   = vs.c_str();
        pd.fragmentPath = fs.c_str();
        pd.topology     = RHI::PrimitiveTopology::Triangles;
        pd.depth.test = false; pd.depth.write = false;   // ventana 2D: no hay nada que ordenar
        // ADITIVO: pisar dos veces el mismo sitio hunde más. Con alfa normal, la huella nueva
        // SUSTITUIRÍA a la vieja y un camino muy pisado se vería igual que una sola pisada.
        pd.blend.enable = true; pd.blend.mode = RHI::BlendMode::Additive;
        m_pso = dev->createPipeline(pd);
        if (!RHI::valid(m_pso)) {
            m_failed = true;
            HARUKA_LOGE("GroundStamp", "el pipeline NO linkó → sin huellas");
            return false;
        }
        m_ubo = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(StampUBO), nullptr,
                                  RHI::BufferMemory::Dynamic);
        RHI::RenderTargetDesc rt;
        rt.width = kRes; rt.height = kRes;
        rt.colorFormats = { RHI::Format::RGBA8 };
        rt.hasDepth = false;                              // no hay geometría que ordenar
        m_pass = dev->createRenderTarget(rt);
        m_tex  = dev->getColorTexture(m_pass);
    }
    if (!RHI::valid(m_pass)) return false;

    // Marco tangente: el MISMO que usa la precipitación, para que "arriba" signifique lo mismo en
    // todas partes del sistema de clima.
    const glm::vec3 u  = glm::normalize(up);
    const glm::vec3 e1 = glm::normalize(glm::cross(u, (std::abs(u.y) < 0.9f) ? glm::vec3(0, 1, 0)
                                                                             : glm::vec3(1, 0, 0)));
    const glm::vec3 e2 = glm::cross(u, e1);
    const glm::dvec3 e1d(e1), e2d(e2);
    const double R = GroundLayer::kWindowM;

    // Proyección de cada pisada a la ventana [-1,1], EN DOUBLE (ver la nota de la cabecera).
    StampUBO ub{};
    size_t n = 0, live = 0;
    for (const auto& s : layer.stamps()) {
        if (n >= GroundLayer::kMaxStamps) break;
        const float d = GroundLayer::fade(s, now);
        if (d <= 0.0f) continue;
        const glm::dvec3 rel = s.pos - center;
        const double x = glm::dot(rel, e1d) / R;
        const double y = glm::dot(rel, e2d) / R;
        if (x < -1.2 || x > 1.2 || y < -1.2 || y > 1.2) continue;   // fuera de la ventana
        ub.stamps[n++] = glm::vec4((float)x, (float)y, (float)(s.radius / R), d);
        ++live;
    }
    if (live == 0) return false;                    // nada que pintar: el terreno queda virgen
    dev->updateBuffer(m_ubo, 0, sizeof(ub), &ub);

    // La matriz que el TERRENO usará: posición relativa a la cámara → ventana. Es la ortográfica
    // cenital equivalente, construida con el mismo marco → sin desalineación de medio téxel.
    const glm::mat4 view = glm::lookAt(u * 100.0f, glm::vec3(0.0f), e2);
    m_stampSpace = glm::ortho(-(float)R, (float)R, -(float)R, (float)R, 1.0f, 200.0f) * view;

    RHI::Context* ctx = dev->beginFrame();
    RHI::ClearValues cv; cv.clearColor = true; cv.clearDepth = false;
    cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f;   // 0 = capa intacta
    ctx->beginRenderPass(m_pass, cv);
    ctx->setViewport(0, 0, (int)kRes, (int)kRes);
    ctx->bindPipeline(m_pso);
    ctx->bindUniformBuffer(5, m_ubo);
    ctx->draw((int)(GroundLayer::kMaxStamps * 6));
    ctx->endRenderPass();
    return true;
}

} // namespace Haruka
