#define GLM_ENABLE_EXPERIMENTAL
#include "ui/ui_world_panel.h"

#include <glad/glad.h>
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <cmath>
#include <vector>

namespace Haruka::UI {

// ── Construction ─────────────────────────────────────────────────────────────

// Must match layout(std140, binding = 0) in ui_world_panel.vert/.frag
struct PanelUBOData {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    float     focused;
    float     _pad[3];
};

UIWorldPanel::~UIWorldPanel() {
    if (m_vao) { glDeleteVertexArrays(1, &m_vao); m_vao = 0; }
    if (m_vbo) { glDeleteBuffers(1, &m_vbo);      m_vbo = 0; }
    if (m_ebo) { glDeleteBuffers(1, &m_ebo);      m_ebo = 0; }
    if (m_ubo) { glDeleteBuffers(1, &m_ubo);      m_ubo = 0; }
}

UIWorldPanel::UIWorldPanel(Desc desc)
    : m_desc(std::move(desc))
    , m_windowId("##wp_" + m_desc.id)
{
    m_fbo = std::make_unique<RenderTarget>(m_desc.fboWidth, m_desc.fboHeight);

    glGenBuffers(1, &m_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, m_ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(PanelUBOData), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    rebuildMesh();
}

// ── ImGui render pass ────────────────────────────────────────────────────────

void UIWorldPanel::beginImGui() {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_savedFBO);
    m_fbo->bindForWriting();
    glClearColor(0.f, 0.f, 0.f, 0.f);
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
}

// ── World-space draw ──────────────────────────────────────────────────────────

void UIWorldPanel::draw(Shader& shader, const glm::mat4& view, const glm::mat4& proj,
                        const glm::vec3& camPos) const
{
    if (!m_visible) return;
    if (m_meshDirty) const_cast<UIWorldPanel*>(this)->rebuildMesh();

    glm::mat4 model = modelMatrix(camPos);

    // Upload PanelTransform UBO (binding 0)
    PanelUBOData uboData;
    uboData.model   = model;
    uboData.view    = view;
    uboData.proj    = proj;
    uboData.focused = m_focused ? 1.0f : 0.0f;

    glBindBuffer(GL_UNIFORM_BUFFER, m_ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(PanelUBOData), &uboData);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_ubo);

    // Bind FBO color texture to unit 1 (matches layout(binding=1) sampler)
    m_fbo->bindForReading(1);

    shader.use();
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

// ── Raycast ──────────────────────────────────────────────────────────────────

bool UIWorldPanel::raycast(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                            glm::vec2& outUV) const
{
    // Only flat-panel raycast for now (arcAngle == 0 or small).
    // Build local-space transform from anchor.
    glm::vec3 normal = glm::normalize(m_desc.anchor.normal);
    float denom = glm::dot(normal, rayDir);
    if (std::abs(denom) < 1e-6f) return false;

    float t = glm::dot(m_desc.anchor.worldPos - rayOrigin, normal) / denom;
    if (t < 0.f) return false;

    glm::vec3 hit = rayOrigin + rayDir * t;
    glm::vec3 local = hit - m_desc.anchor.worldPos;

    // Build right/up axes from normal
    glm::vec3 worldUp = std::abs(normal.y) < 0.99f ? glm::vec3(0,1,0) : glm::vec3(1,0,0);
    glm::vec3 right = glm::normalize(glm::cross(worldUp, normal));
    glm::vec3 up    = glm::cross(normal, right);

    float halfW = m_desc.widthMeters  * 0.5f;
    float halfH = m_desc.heightMeters * 0.5f;

    float u = glm::dot(local, right) / m_desc.widthMeters  + m_desc.anchor.pivotNorm.x;
    float v = glm::dot(local, up)    / m_desc.heightMeters + m_desc.anchor.pivotNorm.y;
    (void)halfW; (void)halfH;

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
    // Vertex layout: vec3 pos, vec2 uv
    struct PanelVert { glm::vec3 pos; glm::vec2 uv; };

    std::vector<PanelVert> verts;
    std::vector<unsigned int> indices;

    float w = m_desc.widthMeters;
    float h = m_desc.heightMeters;
    // Pivot offset — shifts so pivotNorm=(0.5,0.5) means centered.
    float ox = -(m_desc.anchor.pivotNorm.x - 0.5f) * w;
    float oy = -(m_desc.anchor.pivotNorm.y - 0.5f) * h;

    if (m_desc.curve.arcAngle < 0.01f) {
        // Flat quad: 4 vertices, 2 triangles
        verts = {
            { glm::vec3(-w*0.5f + ox,  h*0.5f + oy, 0.f), glm::vec2(0.f, 1.f) },
            { glm::vec3( w*0.5f + ox,  h*0.5f + oy, 0.f), glm::vec2(1.f, 1.f) },
            { glm::vec3( w*0.5f + ox, -h*0.5f + oy, 0.f), glm::vec2(1.f, 0.f) },
            { glm::vec3(-w*0.5f + ox, -h*0.5f + oy, 0.f), glm::vec2(0.f, 0.f) },
        };
        indices = { 0,1,2, 0,2,3 };
    } else {
        // Curved strip: subdivide horizontally along the arc
        const int segs = 32;
        float arc = m_desc.curve.arcAngle;
        float R   = m_desc.curve.radius;

        for (int i = 0; i <= segs; ++i) {
            float t     = (float)i / (float)segs;       // 0..1
            float theta = (t - 0.5f) * arc;             // -arc/2 .. +arc/2

            float px = R * std::sin(theta);
            float pz = R * (1.f - std::cos(theta));     // curves toward viewer

            // Top vertex
            verts.push_back({ glm::vec3(px + ox,  h*0.5f + oy, pz), glm::vec2(t, 1.f) });
            // Bottom vertex
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

    // Upload
    if (m_vao == 0) {
        glGenVertexArrays(1, &m_vao);
        glGenBuffers(1, &m_vbo);
        glGenBuffers(1, &m_ebo);
    }

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(PanelVert)),
                 verts.data(), GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(indices.size() * sizeof(unsigned int)),
                 indices.data(), GL_DYNAMIC_DRAW);

    // pos = attrib 0
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(PanelVert),
                          (void*)offsetof(PanelVert, pos));
    // uv = attrib 1
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(PanelVert),
                          (void*)offsetof(PanelVert, uv));

    glBindVertexArray(0);

    m_indexCount = (int)indices.size();
    m_meshDirty  = false;
}

// ── Model matrix ─────────────────────────────────────────────────────────────

glm::mat4 UIWorldPanel::modelMatrix(const glm::vec3& camPos) const {
    glm::vec3 fwd = glm::normalize(m_desc.anchor.normal);

    switch (m_desc.billboard) {
        case UIBillboardMode::FaceCamera: {
            // Recompute forward to always face camera
            glm::vec3 toCamera = glm::normalize(camPos - m_desc.anchor.worldPos);
            fwd = toCamera;
            break;
        }
        case UIBillboardMode::AxisLocked: {
            // Rotate around Y only
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

    // Build rotation from local +Z to fwd
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
