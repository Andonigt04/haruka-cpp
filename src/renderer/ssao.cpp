#include "ssao.h"
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include <random>

namespace Haruka { namespace Renderer {

SSAO::SSAO(unsigned int width, unsigned int height)
    : width(width), height(height) {
    setupSamples();
    setupFramebuffer();
}

void SSAO::setupSamples() {
    std::uniform_real_distribution<float> randomFloats(0.0, 1.0);
    std::mt19937 gen;

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

    for (unsigned int i = 0; i < 16; i++) {
        glm::vec3 noise(
            randomFloats(gen) * 2.0 - 1.0,
            randomFloats(gen) * 2.0 - 1.0,
            0.0f
        );
        ssaoNoise.push_back(noise);
    }

    if (RHI::Device* dev = RHI::device()) {
        RHI::TextureDesc nd;
        nd.width = 4; nd.height = 4; nd.format = RHI::Format::RGB32F;
        nd.filter = RHI::Filter::Nearest; nd.wrap = RHI::Wrap::Repeat;
        nd.initialData = &ssaoNoise[0];
        m_noise = dev->createTexture(nd);
    }
}

void SSAO::setupFramebuffer() {
    if (RHI::Device* dev = RHI::device()) {
        RHI::RenderTargetDesc d;
        d.width = width; d.height = height;
        d.colorFormats = { RHI::Format::R8 };
        d.colorFilter = RHI::Filter::Nearest;
        d.hasDepth = false;
        m_pass    = dev->createRenderTarget(d);
        m_ssaoTex = dev->getColorTexture(m_pass, 0);
    }
}

SSAO::~SSAO() {
    if (RHI::valid(m_pass)) {
        if (RHI::Device* dev = RHI::device()) { dev->destroy(m_pass); dev->destroy(m_noise); }
    }
}

}} // namespace Haruka::Renderer
