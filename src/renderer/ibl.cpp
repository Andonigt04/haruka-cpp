#include "ibl.h"

#include <iostream>
#include <vector>
#include <cstring>
#include <stb_image.h>
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_context.h"
#include <glm/gtc/matrix_transform.hpp>

namespace Haruka { namespace Renderer {

static const float cubeVerts[] = {
    -1.0f,-1.0f,-1.0f,  1.0f,-1.0f,-1.0f,  1.0f, 1.0f,-1.0f,  1.0f, 1.0f,-1.0f, -1.0f, 1.0f,-1.0f, -1.0f,-1.0f,-1.0f,
    -1.0f,-1.0f, 1.0f,  1.0f,-1.0f, 1.0f,  1.0f, 1.0f, 1.0f,  1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f,-1.0f, 1.0f,
    -1.0f, 1.0f, 1.0f, -1.0f, 1.0f,-1.0f, -1.0f,-1.0f,-1.0f, -1.0f,-1.0f,-1.0f, -1.0f,-1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
     1.0f, 1.0f, 1.0f,  1.0f, 1.0f,-1.0f,  1.0f,-1.0f,-1.0f,  1.0f,-1.0f,-1.0f,  1.0f,-1.0f, 1.0f,  1.0f, 1.0f, 1.0f,
    -1.0f,-1.0f,-1.0f,  1.0f,-1.0f,-1.0f,  1.0f,-1.0f, 1.0f,  1.0f,-1.0f, 1.0f, -1.0f,-1.0f, 1.0f, -1.0f,-1.0f,-1.0f,
    -1.0f, 1.0f,-1.0f,  1.0f, 1.0f,-1.0f,  1.0f, 1.0f, 1.0f,  1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f,-1.0f
};

static const float quadVerts[] = {
    -1.0f,  1.0f,  0.0f, 1.0f,
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
};

struct IBLMatrices { glm::mat4 projection; glm::mat4 view; };
struct IBLParams { float roughness; float _pad[3]; };

IBL::IBL() {
    RHI::Device* dev = RHI::device();
    if (!dev) return;

    m_matrixUBO = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(IBLMatrices), nullptr, RHI::BufferMemory::Dynamic);
    m_paramUBO  = dev->createBuffer(RHI::BufferUsage::Uniform, sizeof(IBLParams), nullptr, RHI::BufferMemory::Dynamic);
    m_cubeBuf   = dev->createBuffer(RHI::BufferUsage::Vertex, sizeof(cubeVerts), cubeVerts);
    m_quadBuf   = dev->createBuffer(RHI::BufferUsage::Vertex, sizeof(quadVerts), quadVerts);

    auto makePipe = [&](const char* vs, const char* fs, int stride,
                        std::initializer_list<RHI::VertexAttribute> attrs,
                        RHI::PrimitiveTopology topo = RHI::PrimitiveTopology::Triangles) {
        RHI::PipelineDesc pd;
        pd.vertexPath = vs; pd.fragmentPath = fs;
        pd.vertexLayout.strides = { (uint32_t)stride };
        for (auto& a : attrs) pd.vertexLayout.attributes.push_back(a);
        pd.topology = topo;
        pd.depth.test = true; pd.depth.write = true; pd.depth.compare = RHI::CompareOp::Less;
        return dev->createPipeline(pd);
    };

    m_equirectPipe = makePipe("shaders/equirect_to_cubemap.vert", "shaders/equirect_to_cubemap.frag",
                               12, {{0, 0, RHI::Format::RGB32F, 0}});
    m_irradiancePipe = makePipe("shaders/irradiance_convolution.vert", "shaders/irradiance_convolution.frag",
                                12, {{0, 0, RHI::Format::RGB32F, 0}});
    m_prefilterPipe = makePipe("shaders/prefilter_env.vert", "shaders/prefilter_env.frag",
                               12, {{0, 0, RHI::Format::RGB32F, 0}});
    m_brdfPipe = makePipe("shaders/brdf_lut.vert", "shaders/brdf_lut.frag",
                          16, {{0, 0, RHI::Format::RG32F, 0}, {1, 8, RHI::Format::RG32F, 0}});

    // BRDF LUT render target (2D texture, not cubemap)
    RHI::RenderTargetDesc rtd;
    rtd.width = 512; rtd.height = 512;
    rtd.colorFormats = { RHI::Format::RG16F };
    rtd.hasDepth = false;
    m_brdfPass = dev->createRenderTarget(rtd);
    hBrdf = dev->getColorTexture(m_brdfPass, 0);

    setupCubemap();
    generateDefaultSky();
}

IBL::~IBL() {
    if (RHI::Device* dev = RHI::device(); dev) {
        if (RHI::valid(hEnv)) dev->destroy(hEnv);
        if (RHI::valid(hIrradiance)) dev->destroy(hIrradiance);
        if (RHI::valid(hPrefilter)) dev->destroy(hPrefilter);
        if (RHI::valid(hBrdf)) dev->destroy(hBrdf);
        if (RHI::valid(m_cubeBuf)) dev->destroy(m_cubeBuf);
        if (RHI::valid(m_quadBuf)) dev->destroy(m_quadBuf);
        if (RHI::valid(m_matrixUBO)) dev->destroy(m_matrixUBO);
        if (RHI::valid(m_paramUBO)) dev->destroy(m_paramUBO);
        if (RHI::valid(m_equirectPipe)) dev->destroy(m_equirectPipe);
        if (RHI::valid(m_irradiancePipe)) dev->destroy(m_irradiancePipe);
        if (RHI::valid(m_prefilterPipe)) dev->destroy(m_prefilterPipe);
        if (RHI::valid(m_brdfPipe)) dev->destroy(m_brdfPipe);
        if (RHI::valid(m_brdfPass)) dev->destroy(m_brdfPass);
    }
}

void IBL::setupCubemap() {
    if (RHI::Device* dev = RHI::device()) {
        RHI::TextureDesc d;
        d.width = d.height = 512;
        d.format = RHI::Format::RGBA16F; // RGBA (NO RGB16F): Vulkan/NVIDIA no soporta muestrear 3 canales
        d.cube = true;
        d.filter = RHI::Filter::Linear;
        d.wrap = RHI::Wrap::ClampToEdge;
        hEnv = dev->createTexture(d);
    }
}

void IBL::generateDefaultSky(glm::vec3 skyColor) {
    RHI::Device* dev = RHI::device();
    if (!dev || !RHI::valid(hEnv)) return;

    const int sz = 512;
    std::vector<float> data(sz * sz * 4);
    for (unsigned int face = 0; face < 6; ++face) {
        for (int y = 0; y < sz; ++y) {
            float t = (float)y / (sz - 1);
            glm::vec3 color;
            if (face == 2) {
                color = skyColor * 0.8f;
            } else if (face == 3) {
                color = glm::vec3(0.05f, 0.04f, 0.03f);
            } else {
                glm::vec3 horizon = skyColor * 1.6f;
                color = glm::mix(horizon, skyColor, t);
            }
            for (int x = 0; x < sz; ++x) {
                int idx = (y * sz + x) * 4;
                data[idx + 0] = color.r;
                data[idx + 1] = color.g;
                data[idx + 2] = color.b;
                data[idx + 3] = 1.0f;
            }
        }
        dev->updateCubemapFace(hEnv, face, sz, sz, RHI::Format::RGBA16F, data.data());
    }

    generateIrradianceMap();
    generatePrefilterMap();
    generateBRDFLUT();
}

void IBL::loadHDRI(const std::string& imagePath) {
    RHI::Device* dev = RHI::device();
    if (!dev || !RHI::valid(hEnv)) return;

    stbi_set_flip_vertically_on_load(true);
    int width, height, nrComponents;
    unsigned char* data = stbi_load(imagePath.c_str(), &width, &height, &nrComponents, 0);
    if (!data) {
        HARUKA_RENDERER_ERROR(ErrorCode::TEXTURE_LOAD_FAILED, "Failed to load LDR image: " + imagePath);
        return;
    }

    RHI::TextureDesc td;
    td.width = width; td.height = height;
    td.format = (nrComponents == 3) ? RHI::Format::RGB8 : RHI::Format::RGBA8;
    td.filter = RHI::Filter::Linear;
    td.wrap = RHI::Wrap::ClampToEdge;
    td.initialData = data;
    RHI::TextureHandle ldrTex = dev->createTexture(td);
    stbi_image_free(data);

    if (!RHI::valid(ldrTex)) return;

    glm::mat4 captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
    glm::mat4 captureViews[] = {
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 1,  0,  0), glm::vec3(0, -1,  0)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3(-1,  0,  0), glm::vec3(0, -1,  0)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 0,  1,  0), glm::vec3(0,  0,  1)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 0, -1,  0), glm::vec3(0,  0, -1)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 0,  0,  1), glm::vec3(0, -1,  0)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 0,  0, -1), glm::vec3(0, -1,  0))
    };

    RHI::Context* ctx = dev->beginFrame();
    if (!ctx) return;

    ctx->bindPipeline(m_equirectPipe);
    for (unsigned int i = 0; i < 6; ++i) {
        IBLMatrices mats;
        mats.projection = captureProjection;
        mats.view = captureViews[i];
        dev->updateBuffer(m_matrixUBO, 0, sizeof(IBLMatrices), &mats);

        ctx->bindUniformBuffer(0, m_matrixUBO);
        ctx->bindTexture(0, ldrTex);

        RHI::ClearValues clear;
        clear.depth = 1.0f;
        ctx->beginRenderPassCubemapFace(hEnv, i, 0, clear);
        renderCube(*ctx);
        ctx->endRenderPass();
    }

    generateIrradianceMap();
    generatePrefilterMap();
    generateBRDFLUT();

    dev->destroy(ldrTex);
}

void IBL::renderCube(RHI::Context& ctx) {
    ctx.bindVertexBuffer(m_cubeBuf);
    ctx.draw(m_cubeCount);
}

void IBL::renderQuad(RHI::Context& ctx) {
    ctx.bindVertexBuffer(m_quadBuf);
    ctx.draw(m_quadCount);
}

void IBL::generateIrradianceMap() {
    RHI::Device* dev = RHI::device();
    if (!dev || !RHI::valid(hEnv)) return;

    {
        RHI::TextureDesc d;
        d.width = d.height = 32;
        d.format = RHI::Format::RGBA16F; // RGBA (NO RGB16F): Vulkan/NVIDIA no soporta muestrear 3 canales
        d.cube = true;
        d.filter = RHI::Filter::Linear;
        d.wrap = RHI::Wrap::ClampToEdge;
        hIrradiance = dev->createTexture(d);
    }

    if (!RHI::valid(hIrradiance)) return;

    glm::mat4 captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
    glm::mat4 captureViews[] = {
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 1,  0,  0), glm::vec3(0, -1,  0)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3(-1,  0,  0), glm::vec3(0, -1,  0)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 0,  1,  0), glm::vec3(0,  0,  1)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 0, -1,  0), glm::vec3(0,  0, -1)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 0,  0,  1), glm::vec3(0, -1,  0)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 0,  0, -1), glm::vec3(0, -1,  0))
    };

    RHI::Context* ctx = dev->beginFrame();
    if (!ctx) return;

    ctx->bindPipeline(m_irradiancePipe);
    for (unsigned int i = 0; i < 6; ++i) {
        IBLMatrices mats;
        mats.projection = captureProjection;
        mats.view = captureViews[i];
        dev->updateBuffer(m_matrixUBO, 0, sizeof(IBLMatrices), &mats);

        ctx->bindUniformBuffer(0, m_matrixUBO);
        ctx->bindTexture(0, hEnv);

        RHI::ClearValues clear;
        clear.depth = 1.0f;
        ctx->beginRenderPassCubemapFace(hIrradiance, i, 0, clear);
        renderCube(*ctx);
        ctx->endRenderPass();
    }
}

void IBL::generatePrefilterMap() {
    RHI::Device* dev = RHI::device();
    if (!dev || !RHI::valid(hEnv)) return;

    {
        RHI::TextureDesc d;
        d.width = d.height = 128;
        d.format = RHI::Format::RGBA16F; // RGBA (NO RGB16F): Vulkan/NVIDIA no soporta muestrear 3 canales
        d.cube = true;
        d.mipmaps = true;
        d.filter = RHI::Filter::Linear;
        d.wrap = RHI::Wrap::ClampToEdge;
        hPrefilter = dev->createTexture(d);
    }

    if (!RHI::valid(hPrefilter)) return;

    glm::mat4 captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
    glm::mat4 captureViews[] = {
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 1,  0,  0), glm::vec3(0, -1,  0)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3(-1,  0,  0), glm::vec3(0, -1,  0)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 0,  1,  0), glm::vec3(0,  0,  1)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 0, -1,  0), glm::vec3(0,  0, -1)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 0,  0,  1), glm::vec3(0, -1,  0)),
        glm::lookAt(glm::vec3(0.0f), glm::vec3( 0,  0, -1), glm::vec3(0, -1,  0))
    };

    RHI::Context* ctx = dev->beginFrame();
    if (!ctx) return;

    unsigned int maxMipLevels = 5;
    for (unsigned int mip = 0; mip < maxMipLevels; ++mip) {
        float roughness = (float)mip / (float)(maxMipLevels - 1);
        IBLParams params;
        params.roughness = roughness;
        dev->updateBuffer(m_paramUBO, 0, sizeof(IBLParams), &params);

        ctx->bindPipeline(m_prefilterPipe);
        for (unsigned int i = 0; i < 6; ++i) {
            IBLMatrices mats;
            mats.projection = captureProjection;
            mats.view = captureViews[i];
            dev->updateBuffer(m_matrixUBO, 0, sizeof(IBLMatrices), &mats);

            ctx->bindUniformBuffer(0, m_matrixUBO);
            ctx->bindUniformBuffer(1, m_paramUBO);
            ctx->bindTexture(0, hEnv);

            RHI::ClearValues clear;
            clear.depth = 1.0f;
            ctx->beginRenderPassCubemapFace(hPrefilter, i, mip, clear);
            renderCube(*ctx);
            ctx->endRenderPass();
        }
    }
}

void IBL::generateBRDFLUT() {
    RHI::Device* dev = RHI::device();
    if (!dev || !RHI::valid(m_brdfPass)) return;

    RHI::Context* ctx = dev->beginFrame();
    if (!ctx) return;

    RHI::ClearValues clear;
    ctx->beginRenderPass(m_brdfPass, clear);
    ctx->bindPipeline(m_brdfPipe);
    renderQuad(*ctx);
    ctx->endRenderPass();
}

}} // namespace Haruka::Renderer
