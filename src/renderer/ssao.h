#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>

/**
 * @brief Screen-space ambient occlusion resources and sample generation.
 */
class SSAO {
public:
    /** @brief Creates SSAO buffers at the given resolution. */
    SSAO(unsigned int width, unsigned int height);
    ~SSAO();

    /** @brief Binds SSAO framebuffer for the occlusion pass. */
    void bindForWriting();
    /** @brief Restores default framebuffer binding. */
    void unbind();
    /** @brief Binds SSAO texture for composition. */
    void bindForReading(unsigned int textureUnit);

    /** @brief Returns SSAO output texture id. */
    unsigned int getSSAOTexture() const { return ssaoColorBuffer; }

private:
    /** @brief Allocates framebuffer and textures. */
    void setupFramebuffer();
    /** @brief Generates kernel and noise sample data. */
    void setupSamples();

    unsigned int ssaoFBO = 0;
    unsigned int ssaoColorBuffer = 0;
    unsigned int noiseTexture = 0;
    
    std::vector<glm::vec3> ssaoKernel;
    std::vector<glm::vec3> ssaoNoise;
    
    unsigned int width, height;
};