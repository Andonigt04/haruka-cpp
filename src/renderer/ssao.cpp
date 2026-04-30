#include "ssao.h"
#include "core/error_reporter.h"
#include <random>
#include <iostream>

SSAO::SSAO(unsigned int width, unsigned int height)
    : width(width), height(height) {
    setupSamples();
    setupFramebuffer();
}

void SSAO::setupSamples() {
    std::uniform_real_distribution<float> randomFloats(0.0, 1.0);
    std::mt19937 gen;
    
    // Kernel de 64 muestras en hemisferio
    for (unsigned int i = 0; i < 64; ++i) {
        glm::vec3 sample(
            randomFloats(gen) * 2.0 - 1.0,
            randomFloats(gen) * 2.0 - 1.0,
            randomFloats(gen)
        );
        sample = glm::normalize(sample);
        sample *= randomFloats(gen);
        
        float scale = float(i) / 64.0;
        scale = 0.1f + (scale * scale) * (1.0f - 0.1f);
        sample *= scale;
        ssaoKernel.push_back(sample);
    }
    
    // Noise texture (4x4)
    for (unsigned int i = 0; i < 16; i++) {
        glm::vec3 noise(
            randomFloats(gen) * 2.0 - 1.0,
            randomFloats(gen) * 2.0 - 1.0,
            0.0f
        );
        ssaoNoise.push_back(noise);
    }
    
    glGenTextures(1, &noiseTexture);
    glBindTexture(GL_TEXTURE_2D, noiseTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4, 4, 0, GL_RGB, GL_FLOAT, &ssaoNoise[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
}

void SSAO::setupFramebuffer() {
    glGenFramebuffers(1, &ssaoFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO);
    
    glGenTextures(1, &ssaoColorBuffer);
    glBindTexture(GL_TEXTURE_2D, ssaoColorBuffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ssaoColorBuffer, 0);
    
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        HARUKA_MOTOR_ERROR(ErrorCode::RENDER_TARGET_FAILED, "SSAO framebuffer incomplete!");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SSAO::bindForWriting() {
    glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO);
    glViewport(0, 0, width, height);
}

void SSAO::unbind() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SSAO::bindForReading(unsigned int textureUnit) {
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_2D, ssaoColorBuffer);
}

SSAO::~SSAO() {
    glDeleteFramebuffers(1, &ssaoFBO);
    glDeleteTextures(1, &ssaoColorBuffer);
    glDeleteTextures(1, &noiseTexture);
}