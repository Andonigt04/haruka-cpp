#pragma once
#include "ui/ui_types.h"
#include "renderer/render_target.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <functional>
#include <string>
#include <memory>
#include "rhi/rhi_types.h"

namespace Haruka::UI {

/**
 * A panel rendered into an off-screen FBO and projected onto a flat or
 * cylindrically-curved quad in world space.
 *
 * Workflow per frame:
 *   1. beginImGui()  — bind FBO, push ImGui window
 *   2. <caller draws ImGui widgets>
 *   3. endImGui()    — pop ImGui window, restore default FBO
 *   4. draw(view, proj, camPos) — render textured quad(s) in world space
 */
class UIWorldPanel {
public:
    struct Desc {
        std::string     id;
        UIAnchor        anchor;
        UICurve         curve;
        UIBillboardMode billboard = UIBillboardMode::None;
        UIInteractConfig interact;
        float           widthMeters  = 1.0f;
        float           heightMeters = 0.75f;
        int             fboWidth     = 512;
        int             fboHeight    = 384;
    };

    explicit UIWorldPanel(Desc desc);
    ~UIWorldPanel();

    UIWorldPanel(const UIWorldPanel&)            = delete;
    UIWorldPanel& operator=(const UIWorldPanel&) = delete;

    // -- Rendering ------------------------------------------------------------

    // Call before drawing ImGui widgets into this panel's FBO.
    void beginImGui();
    // Call after drawing ImGui widgets. Restores default FBO.
    void endImGui();

    // Draws the panel's texture onto a world-space quad using RHI pipeline.
    void draw(const glm::mat4& view, const glm::mat4& proj,
              const glm::vec3& camPos) const;

    // -- Interaction ----------------------------------------------------------

    bool raycast(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                 glm::vec2& outUV) const;

    // -- State ----------------------------------------------------------------

    const std::string& id() const { return m_desc.id; }
    bool isVisible()        const { return m_visible; }
    void setVisible(bool v)       { m_visible = v; }
    bool isFocused()        const { return m_focused; }

    void setFocused(bool focused);
    void triggerInteract(glm::vec2 hitUV);

    UIInteractConfig&       interactConfig()       { return m_desc.interact; }
    const UIInteractConfig& interactConfig() const { return m_desc.interact; }

    const UIAnchor& anchor() const { return m_desc.anchor; }
    void setAnchor(UIAnchor a) { m_desc.anchor = a; m_meshDirty = true; }

    const UICurve& curve() const { return m_desc.curve; }
    void setCurve(UICurve c) { m_desc.curve = c; m_meshDirty = true; }

    UIBillboardMode billboard() const { return m_desc.billboard; }

private:
    void rebuildMesh();
    glm::mat4 modelMatrix(const glm::vec3& camPos) const;

    Desc        m_desc;
    std::string m_windowId;
    bool m_visible   = true;
    bool m_focused   = false;
    bool m_meshDirty = true;

    std::unique_ptr<RenderTarget> m_fbo;

    // RHI pipeline and buffers
    Haruka::RHI::PipelineHandle m_pipeline;
    Haruka::RHI::BufferHandle m_vboH, m_eboH, m_uboH;
    int          m_indexCount = 0;

    mutable bool m_fboDirtyThisFrame = false;

    int m_savedFBO         = 0;
    int m_savedViewport[4] = {};
};

} // namespace Haruka::UI
