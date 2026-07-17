/**
 * @file gl_context.h
 * @brief Implementación OpenGL de RHI::Context. Ejecuta cada comando de forma inmediata.
 */
#pragma once

#include "rhi/rhi_context.h"

namespace Haruka::RHI::opengl
{
    class GLDevice;          // forward-decl: el context resuelve handles contra el device

    class GLContext : public Context
    {
        public:
            explicit GLContext(GLDevice* device) : m_device(device) {}

            void beginRenderPass(RenderPassHandle target, const ClearValues& clear) override;
            void endRenderPass() override;
            void setViewport(int x, int y, int w, int h) override;
            void clear(float r, float g, float b, float a) override;

            void bindPipeline(PipelineHandle) override;
            void bindVertexBuffer(BufferHandle, uint32_t binding) override;
            void bindIndexBuffer(BufferHandle) override;
            void bindUniformBuffer(uint32_t slot, BufferHandle) override;
            void bindStorageBuffer(uint32_t slot, BufferHandle) override;
            void bindTexture(uint32_t slot, TextureHandle, SamplerHandle) override;

            void draw(uint32_t vertexCount, uint32_t first, uint32_t instanceCount) override;
            void drawIndexed(uint32_t indexCount, uint32_t first, uint32_t instanceCount) override;
            void drawIndexedIndirect(BufferHandle cmds, uint32_t drawCount,
                                     uint32_t stride, size_t offset) override;
            void dispatch(uint32_t x, uint32_t y, uint32_t z) override;

        private:
            GLDevice*         m_device = nullptr;
            // Estado del pipeline activo COPIADO por valor, NO un puntero al pool del device:
            // `m_pipelines` es un std::vector y crear un pipeline nuevo (push_back) puede
            // REALOCAR → un puntero guardado aquí quedaría COLGANDO, y el siguiente draw leería
            // topología/VAO basura (→ GL_INVALID_OPERATION opaco). Los pipelines se crean de
            // forma perezosa durante el render, así que la realocación ocurre de verdad.
            unsigned int m_vao      = 0;
            unsigned int m_topology = 0;
            int          m_strides[8] = {0};   // stride por binding (copiado del pipeline)
            unsigned int m_bindingCount = 0;
            bool         m_hasPipeline = false;
    };
}
