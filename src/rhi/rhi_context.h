/**
 * @file rhi_context.h
 * @brief Interfaz de comandos de dibujo de UN frame.
 *
 * El backend GL ejecuta cada comando al momento; el backend Vulkan los graba en un
 * command buffer. El código llamador no nota la diferencia.
 *
 * @par API Status — FROZEN
 * Breaking changes will not be made without a major version bump.
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

            // --- Fence / sync (GPU→CPU readback). ---
            /** @brief Inserta una fence en la cola de la GPU. Útil tras un dispatch para
             *  saber cuándo los SSBO de readback están completos. GL: glFenceSync.
             *  Vulkan: vkQueueSubmit + vkGetFenceStatus. */
            virtual FenceHandle signalFence() = 0;
            /** @brief Espera (o comprueba) una fence. timeoutNs = 0 → no bloquea (solo
             *  consulta). >0 → bloquea hasta ese límite. Devuelve true si señaló, false si
             *  timeout. Para esperar indefinido pasa UINT64_MAX. */
            virtual bool waitFence(FenceHandle, uint64_t timeoutNs = 0) = 0;
            virtual void deleteFence(FenceHandle) = 0;

            // --- Extra utilities needed by the renderer ---
            /** @brief Begin rendering to one face of a cubemap texture at a given mip level.
             *  The texture must have been created with cube=true.
             *  After endRenderPass(), the face is ready for sampling.
             *  Used by IBL precomputation. */
            virtual void beginRenderPassCubemapFace(TextureHandle cubemap, int face,
                                                     int mipLevel, const ClearValues& clear) = 0;

            /** @brief Blit/copy depth from src to dst render pass. For water depth copy. */
            virtual void blitDepth(RenderPassHandle src, RenderPassHandle dst,
                                   int w, int h) = 0;
            /** @brief Blit/copy color from src to dst render pass. For fluid refraction copy. */
            virtual void blitColor(RenderPassHandle src, RenderPassHandle dst,
                                   int w, int h) = 0;
            /** @brief Memory barrier after compute writes (GL: glMemoryBarrier). */
            virtual void memoryBarrier(uint32_t barriers = 0xFF) = 0;
    };
}
