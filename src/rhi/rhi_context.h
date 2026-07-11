/**
 * @file rhi_context.h
 * @brief Interfaz de comandos de dibujo de UN frame.
 *
 * El backend GL ejecuta cada comando al momento; el backend Vulkan los graba en un
 * command buffer. El código llamador no nota la diferencia.
 */
#pragma once

#include "rhi_types.h"
#include "rhi_resources.h"

namespace Haruka::RHI
{
    class Context
    {
        public:
            virtual ~Context() = default;

            // --- Render passes: a dónde se dibuja. RenderPassHandle{} (id 0) = pantalla. ---
            virtual void beginRenderPass(RenderPassHandle target, const ClearValues& clear) = 0;
            virtual void endRenderPass() = 0;

            virtual void setViewport(int x, int y, int w, int h) = 0;
            virtual void clear(float r, float g, float b, float a) = 0; // limpia el pass activo

            // --- Bindings: qué se usa para el próximo draw. ---
            virtual void bindPipeline(PipelineHandle) = 0;                 // absorbe glUseProgram + estado
            virtual void bindVertexBuffer(BufferHandle) = 0;
            virtual void bindIndexBuffer(BufferHandle) = 0;
            virtual void bindUniformBuffer(uint32_t slot, BufferHandle) = 0;
            virtual void bindTexture(uint32_t slot, TextureHandle, SamplerHandle = {}) = 0;

            // --- Draws. ---
            virtual void draw(uint32_t vertexCount, uint32_t first = 0) = 0;        // glDrawArrays
            virtual void drawIndexed(uint32_t indexCount, uint32_t first = 0) = 0;  // glDrawElements
            virtual void dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;          // glDispatchCompute
    };
}
