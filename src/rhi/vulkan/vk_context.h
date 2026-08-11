/**
 * @file vk_context.h
 * @brief Implementación Vulkan de RHI::Context — graba cada comando en el command buffer UNICO del
 *        frame que posee VKDevice (fase 5).
 *
 * Diferencia clave con GL:
 *  - En GL cada comando se ejecuta y un nuevo beginFrame() devuelve un context "fresco". En Vulkan
 *    TODO el frame se graba en UN command buffer: beginFrame() es idempotente (no re-adquiere la
 *    imagen ni reinicia el buffer) y solo endFrame() del frame real hace submit + present.
 *  - Los draws se ejecutan DESPUÉS (vacío): por eso los UBOs no se reescriben por draw — se usan
 *    descriptores en el set compartido (ring de sets, ver VKDevice::m_descRing).
 *  - El viewport de Vulkan tiene el origen arriba a la izquierda y el de GL abajo; se invierte con
 *    altura de viewport negativa (mismo truco que el spike).
 *
 * Constate: el descriptor set COMPARTIDO (set 0) se escribe por cada bind* (host-side, todos los
 * draws del frame) y se bindea de nuevo justo antes de cada draw → correcto incluso sin
 * descriptorBindingUpdateAfterBind (la unin está "en uso" por el command buffer si la bindearas
 * antes).
 */
#pragma once

#include "rhi/rhi_context.h"
#include "rhi/vulkan/vk_common.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Haruka::RHI::vulkan
{
    class VKDevice;
    struct VKTexture;

    class VKContext : public Context
    {
        public:
            explicit VKContext(VKDevice& dev) : m_dev(dev) {}
            ~VKContext() override;

            // context (command recording) interfaces
            void beginRenderPass(RenderPassHandle target, const ClearValues& clear) override;
            void endRenderPass() override;
            void setViewport(int x, int y, int w, int h) override;
            void clear(float r, float g, float b, float a) override;

            void bindPipeline(PipelineHandle) override;
            void bindVertexBuffer(BufferHandle, uint32_t binding = 0) override;
            void bindIndexBuffer(BufferHandle) override;
            void bindUniformBuffer(uint32_t slot, BufferHandle) override;
            void bindStorageBuffer(uint32_t slot, BufferHandle) override;
            void bindTexture(uint32_t slot, TextureHandle, SamplerHandle = {}) override;

            void draw(uint32_t vertexCount, uint32_t first = 0, uint32_t instanceCount = 1) override;
            void drawIndexed(uint32_t indexCount, uint32_t first = 0, uint32_t instanceCount = 1) override;
            void drawIndexedIndirect(BufferHandle cmds, uint32_t drawCount,
                                     uint32_t stride, size_t offset = 0) override;
            void dispatch(uint32_t x, uint32_t y, uint32_t z) override;

            FenceHandle signalFence() override;
            bool waitFence(FenceHandle, uint64_t timeoutNs = 0) override;
            void deleteFence(FenceHandle) override;

            void beginRenderPassCubemapFace(TextureHandle cubemap, int face,
                                            int mipLevel, const ClearValues& clear) override;
            void blitDepth(RenderPassHandle src, RenderPassHandle dst, int w, int h) override;
            void blitColor(RenderPassHandle src, RenderPassHandle dst, int w, int h) override;
            void memoryBarrier(uint32_t barriers = 0xFF) override;

            // ImGui (fase 7): se guarda el ImDrawData del frame; VKDevice::endFrame() lo dibuja en
            // su propio render pass sobre el backbuffer antes de presentar.
            void setUiDrawData(void* drawData) override { m_uiDrawData = drawData; }

        private:
            friend class VKDevice;

            VKDevice& m_dev;

            // Estado del pass activo: para bloquear draws fuera de un pass y para las transiciones
            // de final de pass. m_rtColorTex guarda las texturas de color del RT activo (para
            // pasarlas a SHADER_READ al acabar); m_backbuffer indica pantalla.
            RenderPassHandle m_target{};       // active render-pass target (o vacío)
            VkExtent2D       m_extent{};       // tamaño actual (viewport/scissor default)
            int              m_vpX = 0, m_vpY = 0, m_vpW = 0, m_vpH = 0;   // viewport vigente
            bool             m_vpPending = false;   // setViewport fuera de pass → se aplica al entrar
            bool             m_inPass = false;
            bool             m_hasDepth = false;   // el pass activo tiene attachment de profundidad

            // Pipeline pendiente del próximo draw + estado copiado (topología, teselación).
            PipelineHandle   m_pipeline{};
            bool             m_hasPipeline = false;
            bool             m_compute = false;

            // Vertex/index buffers pendientes de bindear (definir al draw). 8 bindings máx.
            VkBuffer m_vbs[8] = {};
            bool     m_vbUsed[8] = {};
            VkBuffer m_ib = VK_NULL_HANDLE;

            // Cada draw consume UN descriptor set del ring del device; todos los bind* desde el draw anterior
            // escriben en ese set. m_descDirty distingue "escribir de nuevo al mismo set (aún no
            // bindeado)" de "empezar una batch nueva (el set anterior ya se usó → pedir otro)".
            VkDescriptorSet m_curSet = VK_NULL_HANDLE;
            bool            m_descDirty = false;

                        // Cache del render a la cara/mip de un cubemap (IBL). Vivo mientras exista el context.
            // Cada framebuffer usa una image view de una cara a un mip (owned aquí: se destruyen
            // libres en el dtor). El render pass viene del device (cachado por formato).
            std::map<std::string, VkFramebuffer> m_cubeFbs;
            std::vector<VkImageView>             m_cubeViews;

            // ImDrawData del frame (ImGui) o nullptr si no se pidió dibujar UI este frame.
            void*                                m_uiDrawData = nullptr;

            bool ensurePipeline();
            VkDescriptorSet nextDescSet();                 // siguiente set libre del ring del device
            VkImage         colorImageOf(RenderPassHandle); // imagen de color del pass (o 0)
            VkImage         depthImageOf(RenderPassHandle); // imagen de profundidad del pass (o 0)

            // Transiciones UNDEFINED → SHADER_READ diferidas desde bindTexture dentro de un pase
            // activo (dónde una barrier es ilegal) y aplicadas en endRenderPass, ya fuera del pase.
            std::vector<VKTexture*> m_pendingTransition;
    };
}