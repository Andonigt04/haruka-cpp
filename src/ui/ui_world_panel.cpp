#define GLM_ENABLE_EXPERIMENTAL
#include "ui/ui_world_panel.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include "renderer/shader.h"

// ImGui integration still uses GL for FBO management
#include <glad/glad.h>
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <cmath>
#include <vector>
#include "core/logger.h"

namespace Haruka::UI {

struct PanelVert { glm::vec3 pos; glm::vec2 uv; };

// Must match layout(std140, binding = 0) in ui_world_panel.vert/.frag
struct PanelUBOData {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    float     focused;
    float     _pad[3];
};

UIWorldPanel::~UIWorldPanel() {
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    if (RHI::valid(m_vboH)) { dev->destroy(m_vboH); dev->destroy(m_eboH); dev->destroy(m_uboH); }
    if (RHI::valid(m_pipeline)) dev->destroy(m_pipeline);
}

UIWorldPanel::UIWorldPanel(Desc desc)
    : m_desc(std::move(desc))
    , m_windowId("##wp_" + m_desc.id)
{
    m_fbo = std::make_unique<RenderTarget>(m_desc.fboWidth, m_desc.fboHeight);
    m_fboDirtyThisFrame = true;

    RHI::Device* dev = RHI::device();
    if (!dev) { HARUKA_LOGE("UIWorldPanel", "no RHI device"); return; }

    m_uboH = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(PanelUBOData), nullptr, RHI::BufferMemory::Dynamic);

    std::string vsPath = Shader::baseDir() + "shaders/ui_world_panel.vert";
    std::string fsPath = Shader::baseDir() + "shaders/ui_world_panel.frag";
    RHI::PipelineDesc pd;
    pd.vertexPath = vsPath.c_str();
    pd.fragmentPath = fsPath.c_str();
    pd.vertexLayout.strides = { (uint32_t)(sizeof(PanelVert)) };
    pd.vertexLayout.attributes = {
        { 0, offsetof(PanelVert, pos), RHI::Format::RGB32F },
        { 1, offsetof(PanelVert, uv),  RHI::Format::RG32F  },
    };
    pd.cull = RHI::CullMode::None;  // two-sided
    m_pipeline = dev->createPipeline(pd);

    rebuildMesh();
}

// ── ImGui render pass ────────────────────────────────────────────────────────

void UIWorldPanel::beginImGui() {
    m_fboDirtyThisFrame = true;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_savedFBO);
    glGetIntegerv(GL_VIEWPORT, m_savedViewport);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo->getFBO());
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClearDepth(1.0);
    glDepthFunc(GL_LESS);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)m_desc.fboWidth, (float)m_desc.fboHeight));
    ImGui::Begin(m_windowId.c_str(), nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBackground);
}

void UIWorldPanel::endImGui() {
    ImGui::End();
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)m_savedFBO);
    glViewport(m_savedViewport[0], m_savedViewport[1],
               m_savedViewport[2], m_savedViewport[3]);
}

// ── World-space draw ──────────────────────────────────────────────────────────

void UIWorldPanel::draw(const glm::mat4& view, const glm::mat4& proj,
                        const glm::vec3& camPos) const
{
    if (!m_visible || !RHI::valid(m_pipeline)) return;
    RHI::Device* dev = RHI::device();
    if (!dev) return;
    RHI::Context* ctx = dev->beginFrame();

    if (m_meshDirty) const_cast<UIWorldPanel*>(this)->rebuildMesh();

    glm::mat4 model = modelMatrix(camPos);

    PanelUBOData uboData;
    uboData.model   = model;
    uboData.view    = view;
    uboData.proj    = proj;
    uboData.focused = m_focused ? 1.0f : 0.0f;
    dev->updateBuffer(m_uboH, 0, sizeof(PanelUBOData), &uboData);

    if (m_fboDirtyThisFrame) {
        ctx->memoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
        m_fboDirtyThisFrame = false;
    }

    ctx->bindPipeline(m_pipeline);
    ctx->bindUniformBuffer(0, m_uboH);
    ctx->bindTexture(1, m_fbo->getColorTextureHandle());
    ctx->bindVertexBuffer(m_vboH);
    ctx->bindIndexBuffer(m_eboH);
    ctx->drawIndexed((uint32_t)m_indexCount);
}

// ── Raycast ──────────────────────────────────────────────────────────────────

bool UIWorldPanel::raycast(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                            glm::vec2& outUV) const
{
    glm::vec3 normal = glm::normalize(m_desc.anchor.normal);
    float denom = glm::dot(normal, rayDir);
    if (std::abs(denom) < 1e-6f) return false;

    float t = glm::dot(m_desc.anchor.worldPos - rayOrigin, normal) / denom;
    if (t < 0.f) return false;

    glm::vec3 hit = rayOrigin + rayDir * t;
    glm::vec3 local = hit - m_desc.anchor.worldPos;

    glm::vec3 worldUp = std::abs(normal.y) < 0.99f ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
    glm::vec3 right = glm::normalize(glm::cross(worldUp, normal));
    glm::vec3 up    = glm::cross(normal, right);

    float u = glm::dot(local, right) / m_desc.widthMeters  + m_desc.anchor.pivotNorm.x;
    float v = glm::dot(local, up)    / m_desc.heightMeters + m_desc.anchor.pivotNorm.y;
    (void)u; (void)v;

    if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) return false;

    outUV = glm::vec2(u, v);
    return true;
}

// ── Focus / interact ──────────────────────────────────────────────────────────

void UIWorldPanel::setFocused(bool focused) {
    if (m_focused == focused) return;
    m_focused = focused;
    if (focused && m_desc.interact.onFocus)
        m_desc.interact.onFocus();
    else if (!focused && m_desc.interact.onUnfocus)
        m_desc.interact.onUnfocus();
}

void UIWorldPanel::triggerInteract(glm::vec2 hitUV) {
    if (m_desc.interact.onInteract)
        m_desc.interact.onInteract(hitUV);
}

// ── Mesh building ─────────────────────────────────────────────────────────────

void UIWorldPanel::rebuildMesh() {
    std::vector<PanelVert> verts;
    std::vector<unsigned int> indices;

    float w = m_desc.widthMeters;
    float h = m_desc.heightMeters;
    float ox = -(m_desc.anchor.pivotNorm.x - 0.5f) * w;
    float oy = -(m_desc.anchor.pivotNorm.y - 0.5f) * h;

    if (m_desc.curve.arcAngle < 0.01f) {
        verts = {
            { glm::vec3(-w*0.5f + ox,  h*0.5f + oy, 0.f), glm::vec2(0.f, 1.f) },
            { glm::vec3( w*0.5f + ox,  h*0.5f + oy, 0.f), glm::vec2(1.f, 1.f) },
            { glm::vec3( w*0.5f + ox, -h*0.5f + oy, 0.f), glm::vec2(1.f, 0.f) },
            { glm::vec3(-w*0.5f + ox, -h*0.5f + oy, 0.f), glm::vec2(0.f, 0.f) },
        };
        indices = { 0,1,2, 0,2,3 };
    } else {
        const int segs = 32;
        float arc = m_desc.curve.arcAngle;
        float R   = m_desc.curve.radius;

        for (int i = 0; i <= segs; ++i) {
            float t     = (float)i / (float)segs;
            float theta = (t - 0.5f) * arc;

            float px = R * std::sin(theta);
            float pz = R * (1.f - std::cos(theta));

            verts.push_back({ glm::vec3(px + ox,  h*0.5f + oy, pz), glm::vec2(t, 1.f) });
            verts.push_back({ glm::vec3(px + ox, -h*0.5f + oy, pz), glm::vec2(t, 0.f) });

            if (i > 0) {
                unsigned int base = (unsigned int)(i - 1) * 2;
                indices.insert(indices.end(), {
                    base, base+2, base+1,
                    base+1, base+2, base+3
                });
            }
        }
    }

    RHI::Device* dev = RHI::device();
    if (!dev) return;

    if (RHI::valid(m_vboH))
        dev->uploadBuffer(m_vboH, verts.size() * sizeof(PanelVert), verts.data());
    else
        m_vboH = dev->createBuffer(RHI::BufferUsage::Vertex, verts.size() * sizeof(PanelVert), verts.data(), RHI::BufferMemory::Stream);

    if (RHI::valid(m_eboH))
        dev->uploadBuffer(m_eboH, indices.size() * sizeof(unsigned int), indices.data());
    else
        m_eboH = dev->createBuffer(RHI::BufferUsage::Index, indices.size() * sizeof(unsigned int), indices.data(), RHI::BufferMemory::Stream);

    m_indexCount = (int)indices.size();
    m_meshDirty  = false;
}

// ── Model matrix ─────────────────────────────────────────────────────────────

glm::mat4 UIWorldPanel::modelMatrix(const glm::vec3& camPos) const {
    glm::vec3 fwd = glm::normalize(m_desc.anchor.normal);

    switch (m_desc.billboard) {
        case UIBillboardMode::FaceCamera: {
            glm::vec3 toCamera = glm::normalize(camPos - m_desc.anchor.worldPos);
            fwd = toCamera;
            break;
        }
        case UIBillboardMode::AxisLocked: {
            glm::vec3 toCamera = camPos - m_desc.anchor.worldPos;
            toCamera.y = 0.f;
            if (glm::length(toCamera) > 1e-6f)
                fwd = glm::normalize(toCamera);
            break;
        }
        case UIBillboardMode::None:
        default:
            break;
    }

    glm::vec3 localZ(0.f, 0.f, 1.f);
    glm::mat4 rot(1.f);
    float d = glm::dot(localZ, fwd);
    if (d < -0.9999f) {
        rot = glm::rotate(glm::mat4(1.f), glm::pi<float>(), glm::vec3(0.f, 1.f, 0.f));
    } else if (d < 0.9999f) {
        glm::vec3 axis = glm::normalize(glm::cross(localZ, fwd));
        float angle    = std::acos(glm::clamp(d, -1.f, 1.f));
        rot = glm::rotate(glm::mat4(1.f), angle, axis);
    }

    glm::mat4 model = glm::translate(glm::mat4(1.f), m_desc.anchor.worldPos) * rot;
    return model;
}

} // namespace Haruka::UI
