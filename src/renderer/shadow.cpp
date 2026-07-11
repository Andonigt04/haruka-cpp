#include "shadow.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"

#include <iostream>

namespace Haruka { namespace Renderer {

Shadow::Shadow(unsigned int width, unsigned int height) : shadowWidth(width), shadowHeight(height)
{
    setupFramebuffer();
}

void Shadow::setupFramebuffer()
{
    // Ruta RHI: depth-only, profundidad como TEXTURA muestreable con borde blanco (fuera = sin sombra).
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc d;
        d.width = shadowWidth; d.height = shadowHeight;
        d.hasDepth = true; d.depthFormat = RHI::Format::D32F;
        d.depthAsTexture = true; d.depthBorderClamp = true; d.depthFilter = RHI::Filter::Nearest;
        m_pass      = dev->createRenderTarget(d);
        depthMapFBO = dev->nativeFramebuffer(m_pass);
        depthMap    = dev->nativeTexture(dev->getDepthTexture(m_pass));
        return;
    }

    // Create Buffer
    glGenFramebuffers(1, &depthMapFBO);

    // Create textures
    glGenTextures(1, &depthMap);
    glBindTexture(GL_TEXTURE_2D, depthMap);

    // Texture Config
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, shadowWidth, shadowHeight, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

    // Attach to Framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, depthMapFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthMap, 0);

    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    // Verify
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { HARUKA_MOTOR_ERROR(ErrorCode::RENDER_TARGET_FAILED, "Shadow framebuffer no está completo!"); }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Shadow::bindForWriting()
{
    glBindFramebuffer(GL_FRAMEBUFFER, depthMapFBO);
    glViewport(0, 0, shadowWidth, shadowHeight);
}

void Shadow::bindForReading(unsigned int textureUnit)
{
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, depthMap);
}

void Shadow::unbind()
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

Shadow::~Shadow()
{
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);
        return;
    }
    glDeleteFramebuffers(1, &depthMapFBO);
    glDeleteTextures(1, &depthMap);
}


}} // namespace Haruka::Renderer
