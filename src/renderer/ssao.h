#pragma once
#include <glm/glm.hpp>
#include <vector>
#include "rhi/rhi_types.h"

namespace Haruka { namespace Renderer {

class SSAO {
public:
    SSAO(unsigned int width, unsigned int height);
    ~SSAO();

    Haruka::RHI::RenderPassHandle getPass() const { return m_pass; }
    Haruka::RHI::TextureHandle getSSAOTexture() const { return m_ssaoTex; }
    Haruka::RHI::TextureHandle getNoiseTexture() const { return m_noise; }
    const std::vector<glm::vec3>& getKernel() const { return ssaoKernel; }

private:
    void setupFramebuffer();
    void setupSamples();

    Haruka::RHI::RenderPassHandle m_pass;
    Haruka::RHI::TextureHandle m_ssaoTex;
    Haruka::RHI::TextureHandle m_noise;

    std::vector<glm::vec3> ssaoKernel;
    std::vector<glm::vec4> ssaoNoise;

    unsigned int width, height;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::SSAO;
namespace Haruka { using Renderer::SSAO; }
