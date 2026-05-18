#pragma once
#include "ui/ui_types.h"
#include "renderer/render_target.h"
#include "renderer/shader.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <functional>
#include <string>
#include <memory>

namespace Haruka::UI {

/**
 * A panel rendered into an off-screen FBO and projected onto a flat or
 * cylindrically-curved quad in world space.
 *
 * Workflow per frame:
 *   1. beginImGui()  — bind FBO, push ImGui window
 *   2. <caller draws ImGui widgets>
 *   3. endImGui()    — pop ImGui window, restore default FBO
 *   4. draw(shader, view, proj, camPos) — render textured quad(s) in world space
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

    // Draws the panel's texture onto a world-space quad using the provided shader.
    // Shader must accept uniforms: uModel (mat4), uTexture (sampler2D).
    void draw(Shader& shader, const glm::mat4& view, const glm::mat4& proj,
              const glm::vec3& camPos) const;

    // -- Interaction ----------------------------------------------------------

    // Returns true if the given ray hits the panel's collider.
    // If hit, outUV receives the normalized [0,1] UV of the hit point.
    bool raycast(const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                 glm::vec2& outUV) const;

    // -- State ----------------------------------------------------------------

    const std::string& id() const { return m_desc.id; }
    bool isVisible()        const { return m_visible; }
    void setVisible(bool v)       { m_visible = v; }
    bool isFocused()        const { return m_focused; }

    // Called by UIInteractionSystem when focus/interact events fire.
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
    // Build the quad/curved-strip vertex data into m_vao/m_vbo.
    void rebuildMesh();

    // Returns the model matrix, applying billboard rotation if needed.
    glm::mat4 modelMatrix(const glm::vec3& camPos) const;

    Desc        m_desc;
    std::string m_windowId; // "##wp_" + m_desc.id, pre-computed to avoid PLT preemption crash
    bool m_visible   = true;
    bool m_focused   = false;
    bool m_meshDirty = true;

    std::unique_ptr<RenderTarget> m_fbo;

    // GPU mesh (flat quad or curved strip)
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_ebo = 0;
    int          m_indexCount = 0;

    // UBO for PanelTransform block (binding 0)
    unsigned int m_ubo = 0;

    // Previous FBO binding saved across begin/end
    int m_savedFBO = 0;
};

} // namespace Haruka::UI
