#include "gbuffer.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include <iostream>

namespace Haruka { namespace Renderer {

GBuffer::GBuffer(unsigned int width, unsigned int height)
    : width(width), height(height) {
    setupFramebuffer();
}

void GBuffer::setupFramebuffer() {
    // Ruta RHI: MRT de 4 color (pos/normal RGBA16F, albedoSpec/emissive RGBA8) + depth. Filtro nearest.
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc d;
        d.width = width; d.height = height;
        d.colorFormats = { RHI::Format::RGBA16F, RHI::Format::RGBA16F, RHI::Format::RGBA8, RHI::Format::RGBA8 };
        d.colorFilter = RHI::Filter::Nearest;
        d.hasDepth = true; d.depthFormat = RHI::Format::D24;
        m_pass      = dev->createRenderTarget(d);
        gBufferFBO  = dev->nativeFramebuffer(m_pass);
        gPosition   = dev->nativeTexture(dev->getColorTexture(m_pass, 0));
        gNormal     = dev->nativeTexture(dev->getColorTexture(m_pass, 1));
        gAlbedoSpec = dev->nativeTexture(dev->getColorTexture(m_pass, 2));
        gEmissive   = dev->nativeTexture(dev->getColorTexture(m_pass, 3));
        return;
    }

    glGenFramebuffers(1, &gBufferFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, gBufferFBO);

    // Position buffer (world space)
    glGenTextures(1, &gPosition);
    glBindTexture(GL_TEXTURE_2D, gPosition);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gPosition, 0);

    // Normal buffer (world space)
    glGenTextures(1, &gNormal);
    glBindTexture(GL_TEXTURE_2D, gNormal);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gNormal, 0);

    // Albedo + Specular (RGBA8)
    glGenTextures(1, &gAlbedoSpec);
    glBindTexture(GL_TEXTURE_2D, gAlbedoSpec);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, gAlbedoSpec, 0);

    // Emissive buffer
    glGenTextures(1, &gEmissive);
    glBindTexture(GL_TEXTURE_2D, gEmissive);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, gEmissive, 0);

    unsigned int attachments[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
    glDrawBuffers(4, attachments);

    glGenRenderbuffers(1, &rboDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, rboDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rboDepth);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        HARUKA_MOTOR_ERROR(ErrorCode::RENDER_TARGET_FAILED, "GBuffer incomplete!");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GBuffer::bindForWriting() {
    glBindFramebuffer(GL_FRAMEBUFFER, gBufferFBO);
    glViewport(0, 0, width, height);
}

void GBuffer::unbind() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GBuffer::bindForReading(int index, unsigned int textureUnit) {
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    if (index == 0) glBindTexture(GL_TEXTURE_2D, gPosition);
    if (index == 1) glBindTexture(GL_TEXTURE_2D, gNormal);
    if (index == 2) glBindTexture(GL_TEXTURE_2D, gAlbedoSpec);
    if (index == 3) glBindTexture(GL_TEXTURE_2D, gEmissive);
}

GBuffer::~GBuffer() {
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) dev->destroy(m_pass);
        return;
    }
    glDeleteFramebuffers(1, &gBufferFBO);
    glDeleteTextures(1, &gPosition);
    glDeleteTextures(1, &gNormal);
    glDeleteTextures(1, &gAlbedoSpec);
    glDeleteTextures(1, &gEmissive);
    glDeleteRenderbuffers(1, &rboDepth);
}

}} // namespace Haruka::Renderer
