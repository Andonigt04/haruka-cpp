#pragma once

#include "rhi/rhi_types.h"
#include <glm/glm.hpp>
#include <string>

namespace Haruka { namespace RHI { class Context; class Device; } }

namespace Haruka { namespace Renderer {

class IBL {
public:
    IBL();
    ~IBL();

    void generateDefaultSky(glm::vec3 skyColor = glm::vec3(0.15f, 0.20f, 0.40f));
    void loadHDRI(const std::string& imagePath);
    void generateIrradianceMap();
    void generatePrefilterMap();
    void generateBRDFLUT();

    Haruka::RHI::TextureHandle getIrradianceMap() const { return hIrradiance; }
    Haruka::RHI::TextureHandle getPrefilterMap() const { return hPrefilter; }
    Haruka::RHI::TextureHandle getBRDFLUT() const { return hBrdf; }
    Haruka::RHI::TextureHandle getEnvCubemap() const { return hEnv; }

private:
    void setupCubemap();
    void renderCube(Haruka::RHI::Context& ctx);
    void renderQuad(Haruka::RHI::Context& ctx);

    Haruka::RHI::TextureHandle hEnv, hIrradiance, hPrefilter, hBrdf;
    Haruka::RHI::BufferHandle  m_cubeBuf, m_quadBuf;
    Haruka::RHI::BufferHandle  m_matrixUBO, m_paramUBO;
    Haruka::RHI::PipelineHandle m_equirectPipe, m_irradiancePipe, m_prefilterPipe, m_brdfPipe;
    Haruka::RHI::RenderPassHandle m_brdfPass;

    int m_cubeCount = 36;
    int m_quadCount = 6;
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::IBL;
namespace Haruka { using Renderer::IBL; }
