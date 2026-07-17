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
            /** @brief Ata un vertex buffer a un BINDING del layout (default 0). Una malla con
             *  streams separados llama una vez por stream. Vulkan: vkCmdBindVertexBuffers. */
            virtual void bindVertexBuffer(BufferHandle, uint32_t binding = 0) = 0;
            virtual void bindIndexBuffer(BufferHandle) = 0;
            virtual void bindUniformBuffer(uint32_t slot, BufferHandle) = 0;
            /** @brief Storage buffer (SSBO) para el shader. Datos por-draw indexados por gl_DrawID
             *  en el camino indirecto (terreno/agua). Vulkan: descriptor de storage buffer. */
            virtual void bindStorageBuffer(uint32_t slot, BufferHandle) = 0;
            virtual void bindTexture(uint32_t slot, TextureHandle, SamplerHandle = {}) = 0;

            // --- Draws. ---
            // instanceCount: nº de instancias (1 = draw normal). En Vulkan va NATIVO en
            // vkCmdDraw/vkCmdDrawIndexed, así que es un parámetro, no un método aparte.
            virtual void draw(uint32_t vertexCount, uint32_t first = 0,
                              uint32_t instanceCount = 1) = 0;                          // glDrawArrays[Instanced]
            virtual void drawIndexed(uint32_t indexCount, uint32_t first = 0,
                                     uint32_t instanceCount = 1) = 0;                   // glDrawElements[Instanced]
            /** @brief Multi-draw INDIRECTO: la GPU lee `drawCount` comandos del buffer `cmds`
             *  (cada uno de `stride` bytes). Colapsa miles de draws en una llamada — es como
             *  dibujan el terreno y el agua. GL: glMultiDrawElementsIndirect.
             *  Vulkan: vkCmdDrawIndexedIndirect (drawCount + stride son nativos).
             *  El vertex/index buffer y el pipeline deben estar ya bindeados. */
            virtual void drawIndexedIndirect(BufferHandle cmds, uint32_t drawCount,
                                             uint32_t stride, size_t offset = 0) = 0;
            virtual void dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;          // glDispatchCompute
    };
}
