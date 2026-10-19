#include "Haruka/Renderer/GroundField.h"

#include "Haruka/Core/Logger.h"
#include "Haruka/Renderer/Shader.h"
#include "Haruka/RHI/RHIContext.h"
#include "Haruka/RHI/RHIDevice.h"
#include "Haruka/RHI/RHIGPUScope.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace Haruka::Renderer {

namespace {
/// Gemelo std140 del UBO `GroundField` de ground_field.frag.
struct FieldUBO {
    glm::vec4 shift;
    glm::vec4 decay;
    glm::vec4 stamp[GroundField::kMaxStamps];
    glm::vec4 stampDir[GroundField::kMaxStamps];
    glm::vec4 shape;
};
static_assert(sizeof(FieldUBO) == (3 + 2 * GroundField::kMaxStamps) * 16, "FieldUBO descuadrado (std140)");
} // namespace

bool GroundField::init(RHI::Device* dev, const Config& cfg) {
    shutdown();
    if (!dev) return false;
    m_dev = dev; m_cfg = cfg;
    m_cfg.res = std::clamp(m_cfg.res, 16, 4096);
    const std::string vs = Shader::baseDir() + "shaders/fullscreen_tri.vert";
    const std::string fs = Shader::baseDir() + "shaders/ground_field.frag";
    RHI::PipelineDesc pd;
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.topology = RHI::PrimitiveTopology::Triangles;   // fullscreen_tri.vert: sin VBO, draw(3)
    pd.depth.test = false; pd.depth.write = false;
    pd.blend.enable = false;
    m_pipe = dev->createPipeline(pd);
    m_ubo  = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(FieldUBO), nullptr, RHI::BufferMemory::Dynamic);
    for (int i = 0; i < 2; ++i) {
        RHI::RenderTargetDesc rd;
        rd.width = rd.height = (uint32_t)m_cfg.res;
        rd.colorFormats = { RHI::Format::RGBA16F };
        rd.colorFilter  = RHI::Filter::Linear;
        rd.hasDepth     = false;
        m_rt[i]  = dev->createRenderTarget(rd);
        m_tex[i] = RHI::valid(m_rt[i]) ? dev->getColorTexture(m_rt[i], 0) : RHI::TextureHandle{};
    }
    m_ready = RHI::valid(m_pipe) && RHI::valid(m_ubo) && RHI::valid(m_tex[0]) && RHI::valid(m_tex[1]);
    if (!m_ready) {
        HARUKA_LOGW("GroundField", "no se pudo crear (pipeline %s, destinos %s)",
                    RHI::valid(m_pipe) ? "ok" : "FALLO", RHI::valid(m_tex[0]) && RHI::valid(m_tex[1]) ? "ok" : "FALLO");
        shutdown();
        return false;
    }
    m_anchored = false;
    return true;
}

void GroundField::shutdown() {
    if (m_dev) {
        if (RHI::valid(m_pipe)) m_dev->destroy(m_pipe);
        if (RHI::valid(m_ubo))  m_dev->destroy(m_ubo);
        for (auto& rt : m_rt) if (RHI::valid(rt)) m_dev->destroy(rt);
    }
    m_pipe = {}; m_ubo = {};
    for (int i = 0; i < 2; ++i) { m_rt[i] = {}; m_tex[i] = {}; }
    m_ready = false; m_anchored = false; m_dev = nullptr;
    m_stamps.clear();
}

void GroundField::stamp(const glm::dvec3& worldPos, float radiusM, const glm::vec3& velocity,
                        float strength, float mark) {
    if (m_stamps.size() >= (size_t)kMaxStamps || radiusM <= 0.0f) return;
    m_stamps.push_back({ worldPos, radiusM, velocity, std::clamp(strength, 0.0f, 1.0f), std::clamp(mark, 0.0f, 1.0f) });
}

GroundField::View GroundField::view() const {
    View v;
    if (!m_ready || !m_anchored) return v;
    v.anchor = m_anchor; v.east = m_east; v.north = m_north;
    v.texelM = m_cfg.texelM; v.res = m_cfg.res;
    return v;
}

void GroundField::update(RHI::Context* ctx, const glm::dvec3& camPos, const glm::dvec3& upIn, float dt) {
    if (!m_ready || !ctx) { m_stamps.clear(); return; }
    HARUKA_GPU_SCOPE("ground.field");
    const double texel = (double)m_cfg.texelM;
    const double sideM = (double)m_cfg.res * texel;
    const glm::dvec3 up = glm::normalize(upIn);
    glm::ivec2 shift(0, 0);
    if (!m_anchored || glm::dot(up, m_up) < std::cos(0.02)) {
        m_up    = up;
        m_east  = glm::normalize(glm::cross(up, std::abs(up.y) < 0.99 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)));
        m_north = glm::cross(up, m_east);
        m_anchor = camPos;
        shift = glm::ivec2(1 << 20, 1 << 20);   // fuera del campo viejo: todo a cero
        m_anchored = true;
    } else {
        const glm::dvec3 d = camPos - m_anchor;
        const double ex = glm::dot(d, m_east), ny = glm::dot(d, m_north);
        if (std::abs(ex) > sideM * 0.25 || std::abs(ny) > sideM * 0.25) {
            shift.x = (int)std::llround(ex / texel);
            shift.y = (int)std::llround(ny / texel);
            m_anchor += m_east * ((double)shift.x * texel) + m_north * ((double)shift.y * texel);
        }
    }

    FieldUBO u{};
    u.shift = glm::vec4((float)shift.x, (float)shift.y, m_cfg.texelM, (float)m_cfg.res);
    u.decay = glm::vec4(m_cfg.pushRecoverS, std::max(dt, 0.0f), m_cfg.markRecoverS, 0.0f);
    u.shape = glm::vec4(m_cfg.soft, m_cfg.spread, m_cfg.rimGain, m_cfg.accumulate ? 1.0f : 0.0f);
    int n = 0;
    for (const Stamp& s : m_stamps) {
        const glm::dvec3 d = s.pos - m_anchor;
        const glm::vec2 xy((float)glm::dot(d, m_east), (float)glm::dot(d, m_north));
        if (std::abs(xy.x) > sideM * 0.5 + s.radius || std::abs(xy.y) > sideM * 0.5 + s.radius) continue;
        glm::vec2 dir((float)glm::dot(glm::dvec3(s.vel), m_east), (float)glm::dot(glm::dvec3(s.vel), m_north));
        const float dl = glm::length(dir);
        dir = (dl > 1e-3f) ? dir / dl : glm::vec2(0.0f);
        u.stamp[n]    = glm::vec4(xy, s.radius, s.strength);
        u.stampDir[n] = glm::vec4(dir, s.mark, 1.0f);
        if (++n >= kMaxStamps) break;
    }
    m_stamps.clear();
    m_dev->updateBuffer(m_ubo, 0, sizeof(u), &u);

    const int prev = m_cur, cur = 1 - m_cur;
    RHI::ClearValues cv; cv.clearColor = true; cv.clearDepth = false;
    cv.color[0] = cv.color[1] = cv.color[2] = cv.color[3] = 0.0f;
    ctx->beginRenderPass(m_rt[cur], cv);
    ctx->setViewport(0, 0, m_cfg.res, m_cfg.res);
    ctx->bindPipeline(m_pipe);
    ctx->bindUniformBuffer(0, m_ubo);
    ctx->bindTexture(5, m_tex[prev]);
    ctx->draw(3, 0, 1);
    ctx->endRenderPass();
    m_cur = cur;
}

} // namespace Haruka::Renderer
