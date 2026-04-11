#include "hdr.h"
#include "core/error_reporter.h"

#include <iostream>

HDR::HDR(unsigned int width, unsigned int height) : width(width), height(height)
{
    setupFramebuffer();
}

void HDR::setupFramebuffer()
{
    glGenFramebuffers(1, &hdrFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, hdrFBO);

    // Color texture (HDR)
    glGenTextures(1, &colorTexture);
    glBindTexture(GL_TEXTURE_2D, colorTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture, 0);

    // Bright texture (para Bloom)
    glGenTextures(1, &brightTexture);  // PRIMERO crear la textura
    glBindTexture(GL_TEXTURE_2D, brightTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, brightTexture, 0);

    // Depth
    glGenRenderbuffers(1, &rboDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, rboDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rboDepth);

    // Attachments
    unsigned int attachments[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, attachments);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) 
        HARUKA_MOTOR_ERROR(ErrorCode::RENDER_TARGET_FAILED, "HDR framebuffer incomplete");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void HDR::bindForWriting()
{
    glBindFramebuffer(GL_FRAMEBUFFER, hdrFBO);
    glViewport(0, 0, width, height);
}

void HDR::unbind()
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void HDR::bindForReading(unsigned int textureUnit, int index)
{
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, index == 0 ? colorTexture : brightTexture);
}

HDR::~HDR()
{
    glDeleteFramebuffers(1, &hdrFBO);
    glDeleteTextures(1, &colorTexture);
    glDeleteTextures(1, &brightTexture);
    glDeleteRenderbuffers(1, &rboDepth);
}