/**
 * @file gl_context.h
 * @brief Implementación OpenGL de RHI::Context. Ejecuta cada comando de forma inmediata.
 */
#pragma once

#include "rhi/rhi_context.h"

namespace Haruka::RHI::opengl
{
    class GLDevice;          // forward-decl: el context resuelve handles contra el device
    struct GLPipeline;       // definido en gl_device.h

    class GLContext : public Context
    {
        public:
            explicit GLContext(GLDevice* device) : m_device(device) {}

            void beginRenderPass(RenderPassHandle target, const ClearValues& clear) override;
            void endRenderPass() override;
            void setViewport(int x, int y, int w, int h) override;
            void clear(float r, float g, float b, float a) override;

            void bindPipeline(PipelineHandle) override;
            void bindVertexBuffer(BufferHandle) override;
            void bindIndexBuffer(BufferHandle) override;
            void bindUniformBuffer(uint32_t slot, BufferHandle) override;
            void bindTexture(uint32_t slot, TextureHandle, SamplerHandle) override;

            void draw(uint32_t vertexCount, uint32_t first) override;
            void drawIndexed(uint32_t indexCount, uint32_t first) override;
            void dispatch(uint32_t x, uint32_t y, uint32_t z) override;

        private:
            GLDevice*         m_device = nullptr;
            const GLPipeline* m_pipeline = nullptr;  // pipeline activo (topología, VAO, stride)
    };
}
