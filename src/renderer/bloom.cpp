#include "bloom.h"
#include "tools/error_reporter.h"
#include <iostream>

Bloom::Bloom(unsigned int width, unsigned int height) : width(width), height(height)
{
    setupFramebuffer();
}

void Bloom::setupFramebuffer()
{
    glGenFramebuffers(1, &bloomFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO);
    
    // Bright texture
    glGenTextures(1, &brightTexture);
    glBindTexture(GL_TEXTURE_2D, brightTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, brightTexture, 0);
    
    // Blurred texture
    glGenTextures(1, &blurredTexture);
    glBindTexture(GL_TEXTURE_2D, blurredTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, blurredTexture, 0);
    
    unsigned int attachments[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, attachments);
    
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        HARUKA_MOTOR_ERROR(ErrorCode::RENDER_TARGET_FAILED, "Bloom framebuffer incomplete");
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Bloom::bindForWriting()
{
    glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO);
    glViewport(0, 0, width, height);
}

void Bloom::unbind()
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Bloom::bindForReading(unsigned int textureUnit, int index)
{
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, index == 0 ? brightTexture : blurredTexture);
}

Bloom::~Bloom()
{
    glDeleteFramebuffers(1, &bloomFBO);
    glDeleteTextures(1, &brightTexture);
    glDeleteTextures(1, &blurredTexture);
}