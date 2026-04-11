#include "point_shadow.h"
#include "core/error_reporter.h"
#include <iostream>

PointShadow::PointShadow(unsigned int resolution) : resolution(resolution)
{
    setupFramebuffer();
}

void PointShadow::setupFramebuffer()
{
    // Cubemap de profundidad
    glGenTextures(1, &depthCubemap);
    glBindTexture(GL_TEXTURE_CUBE_MAP, depthCubemap);

    for (unsigned int i = 0; i < 6; ++i) {
        glTexImage2D(
            GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_DEPTH_COMPONENT,
            resolution, resolution, 0,
            GL_DEPTH_COMPONENT, GL_FLOAT, nullptr
        );
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    // Framebuffer
    glGenFramebuffers(1, &FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, FBO);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, depthCubemap, 0);

    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        HARUKA_MOTOR_ERROR(ErrorCode::RENDER_TARGET_FAILED, "Point Shadow framebuffer incomplete!");
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void PointShadow::bindForWriting()
{
    glBindFramebuffer(GL_FRAMEBUFFER, FBO);
    glViewport(0, 0, resolution, resolution);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void PointShadow::unbind()
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void PointShadow::bindForReading(unsigned int textureUnit)
{
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, depthCubemap);
}

PointShadow::~PointShadow()
{
    glDeleteFramebuffers(1, &FBO);
    glDeleteTextures(1, &depthCubemap);
}