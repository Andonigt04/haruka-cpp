/**
 * @file vk_device.h
 * @brief Implementación Vulkan de RHI::Device. Posee las tablas de recursos VK y el contexto VK.
 */
#pragma once

#include "rhi/rhi_device.h"
#include "rhi/vulkan/vk_common.h"
#include "rhi/vulkan/vk_swapchain.h"

#include <vector>
#include <memory>
#include <functional>
#include <map>

namespace Haruka::RHI::vulkan
{
    class VKContext;

    // --- Recursos VK internos. Un handle.id == índice+1 en la tabla correspondiente. ---
    struct VKBuffer
    {
        VkBuffer       buffer  = VK_NULL_HANDLE;
        VkDeviceMemory memory  = VK_NULL_HANDLE;
        void*          mapped  = nullptr;   // mapeo persistente (Dynamic/Stream/Readback host-visible)
        size_t         size    = 0;
        VkBufferUsageFlags usage = 0;
        bool           hostVisible = false;
    };
    struct VKTexture
    {
        VkImage        image  = VK_NULL_HANDLE;
        VkImageView    view   = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkSampler      sampler = VK_NULL_HANDLE;   // sampler propio (equivalente a los params GL de la textura)
        VkFormat       format = VK_FORMAT_UNDEFINED;
        uint32_t       width = 0, height = 0;
        uint32_t       layers = 1;
        uint32_t       mipLevels = 1;
        bool           cube = false;
        bool           array = false;
        // Layout de imagen MÁS RECIENTE conocido. La fase 5 (VKContext) lo mantiene para insertar
        // solo las transiciones de layout que falten (UNDEFINED → SHADER_READ_OPTIMAL al muestrear).
        VkImageLayout  layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    struct VKSampler { VkSampler id = VK_NULL_HANDLE; };

    struct VKPipeline
    {
        VkPipeline       handle = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;   // layout COMPARTIDO (no owned por el pipeline)
        bool             compute = false;
        // Fase 5: pipelines SOLO se crean contra el backbuffer (fase 4). Para dibujar a targets
        // offscreen se construye una VARIANTE por (pipeline, render pass) de forma perezosa; guardamos
        // la descripcion original y el SPV compilado para poder recompilar contra otro render pass.
        PipelineDesc                          desc;
        std::vector<std::vector<uint32_t>>    spv;
        // Topología/estado copiados para que el VKContext grabe draws sin releer el pool (que puede
        // realocar). Igual que GLContext copia lo que cada draw necesita.
        PrimitiveTopology topology = PrimitiveTopology::Triangles;
        uint32_t          patchVertices = 0;      // >0 solo con topology == Patches
        uint32_t          bindingCount = 0;       // vertex bindings (máx 8)
        // Fase 4: el descriptor set layout es COMPARTIDO por todos los pipelines (set 0 con slots
        // UBO/SSBO/textura). Se crea una sola vez en el device; ver PLAN_VULKAN.md §4.3.
    };

    struct VKRenderTarget
    {
        VkRenderPass   renderPass  = VK_NULL_HANDLE;   // compartido/cacheado por el device (no owned aquí)
        VkFramebuffer  framebuffer = VK_NULL_HANDLE;
        std::vector<TextureHandle> colors;    // 0..N attachments de color (owned)
        TextureHandle  depthTex;               // profundidad como textura/cubemap (o vacío)
        VkImage        depthImage  = VK_NULL_HANDLE;   // profundidad como renderbuffer (o 0)
        VkDeviceMemory depthMemory = VK_NULL_HANDLE;
        VkImageView    depthView   = VK_NULL_HANDLE;
        uint32_t       width = 0, height = 0;
    };

    struct VKFence { VkFence sync = VK_NULL_HANDLE; };

    class VKDevice : public Device
    {
        public:
            /** `preferredGpus`: nombres por orden de preferencia (ver `Device::create`). */
            explicit VKDevice(SDL_Window* window, std::vector<std::string> preferredGpus = {});

            /** @brief Lista las GPUs con una instancia TEMPORAL. No toca el device en uso.
             *  @return false si Vulkan no está disponible (sin loader, sin driver) → el llamante
             *          informa de la de OpenGL en vez de dejar la lista vacía y sin explicación. */
            static bool enumerate(SDL_Window* window, std::vector<Device::AdapterInfo>& out);
            ~VKDevice() override;

            friend class VKContext;

            BufferHandle     createBuffer(BufferUsage, size_t bytes, const void* data, BufferMemory) override;
            void             updateBuffer(BufferHandle, size_t offset, size_t bytes, const void* data) override;
            void             uploadBuffer(BufferHandle, size_t bytes, const void* data) override;
            void             copyBuffer(BufferHandle src, BufferHandle dst, size_t srcOffset, size_t dstOffset, size_t bytes) override;
            TextureHandle    createTexture(const TextureDesc&) override;
            SamplerHandle    createSampler(const SamplerDesc&) override;
            PipelineHandle   createPipeline(const PipelineDesc&) override;
            RenderPassHandle createRenderTarget(const RenderTargetDesc&) override;
            TextureHandle    getColorTexture(RenderPassHandle, uint32_t index) override;
            TextureHandle    getDepthTexture(RenderPassHandle) override;
            uint32_t         nativeTexture(TextureHandle) override;
            uint32_t         nativeFramebuffer(RenderPassHandle) override;
            uint32_t         nativeProgram(PipelineHandle) override;
            uint32_t         nativeBuffer(BufferHandle) override;
            const void*      mappedData(BufferHandle) override;
            void             updateCubemapFace(TextureHandle tex, int face, int width, int height,
                                                Format format, const void* data) override;

            void destroy(BufferHandle) override;
            void destroy(TextureHandle) override;
            void destroy(SamplerHandle) override;
            void destroy(PipelineHandle) override;
            void destroy(RenderPassHandle) override;

            Context* beginFrame() override;
            void     endFrame() override;
            // ImGui (fase 7): inicia ImGui_ImplVulkan + render pass/framebuffers propios para dibujar
            // la UI sobre el backbuffer. Solo Vulkan; los demás backends no-op.
            bool     initUi() override;
            Backend  backend() const override { return Backend::Vulkan; }
            void     readPixels(int x, int y, int w, int h, Format format, void* data) override;
            // Retorna el tamaño en píxeles del swapchain (extent físico de presentación). El host lo usa
            // para ajustar DisplayFramebufferScale de ImGui (el tamaño lógico de SDL puede diferir).
            void     framebufferSize(uint32_t& w, uint32_t& h) const override;

            /** @brief true si se creó un device y queue VK aprovechables. La fábrica Device::create
             *  lo consulta para decidir si quedarse con Vulkan o caer al fallback de OpenGL. */
            bool ready() const { return m_device != VK_NULL_HANDLE && m_swapchain && m_swapchain->valid(); }

            const VKBuffer*       buffer(BufferHandle h) const          { return get(m_buffers, h.id); }
            const VKTexture*      texture(TextureHandle h) const        { return get(m_textures, h.id); }
            const VKSampler*      sampler(SamplerHandle h) const        { return get(m_samplers, h.id); }
            const VKPipeline*     pipeline(PipelineHandle h) const       { return get(m_pipelines, h.id); }
            const VKRenderTarget* renderTarget(RenderPassHandle h) const { return get(m_targets, h.id); }
            const VkFence*        fence(FenceHandle h) const             { return get(m_fences, h.id); }

            // Overload NO-const: el VKContext mantiene el layout conocido de cada imagen (transiciones),
            // así que necesita mutar el VKTexture. Solo lo usa el backend VK.
            VKTexture* texture(TextureHandle h)
            { return (h.id == 0 || h.id > m_textures.size()) ? nullptr : &m_textures[h.id - 1]; }

        private:
            template <class T>
            static const T* get(const std::vector<T>& pool, uint32_t id)
            {
                if (id == 0 || id > pool.size()) return nullptr;
                return &pool[id - 1];
            }

            // Asigna un slot: reusa uno liberado (free-list) o crece el pool. handle.id = slot+1.
            // Evita el crecimiento sin límite de los pools bajo churn de chunks (terreno).
            template <class T>
            static uint32_t alloc(std::vector<T>& pool, std::vector<uint32_t>& freeList, const T& v)
            {
                if (!freeList.empty()) { uint32_t s = freeList.back(); freeList.pop_back(); pool[s] = v; return s + 1; }
                pool.push_back(v); return (uint32_t)pool.size();
            }
            template <class T>
            static void release(std::vector<T>& pool, std::vector<uint32_t>& freeList, uint32_t id)
            {
                if (id == 0 || id > pool.size()) return;
                pool[id - 1] = T{};              // limpia el slot
                freeList.push_back(id - 1);      // disponible para reutilizar
            }

            void createInstance();
            void createDebugMessenger();
            void pickPhysicalDevice();
            void createLogicalDevice();
            void createBackbufferResources();
            bool createBackbufferDepth(const VkExtent2D& e);
            void destroyBackbufferDepth();
            void onSwapchainResize();

            uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props);
            bool     createStagingBuffer(VkBuffer& buffer, VkDeviceMemory& memory, size_t bytes, const void* data);
            bool     submitOneShot(const std::function<void(VkCommandBuffer)>& record);
            void     uploadViaStaging(const VKBuffer& dst, size_t dstOffset, size_t bytes, const void* data);

            // Render pass CARHEADO por firma de attachments (reutiliza passes entre targets con
            // el mismo formato → evita un render pass por target, que bajo churn de chunks son muchos).
            std::string renderPassKey(const RenderTargetDesc&) const;
            VkRenderPass renderPassFor(const RenderTargetDesc&);

            // ImGui (fase 7): render pass de UN color para la UI sobre el backbuffer (loadOp LOAD,
            // preserva la escena ya dibujada; finalLayout PRESENT). Los framebuffers se montan por
            // image view de la swapchain (recreados al redimensionar).
            bool             createImguiResources();
            void             destroyImguiResources();
            void             drawImgui(void* drawData, VkCommandBuffer cmd);
            void             transitionBackbufferToPresent(VkCommandBuffer cmd);
            VkRenderPass               m_imguiPass = VK_NULL_HANDLE;
            std::vector<VkFramebuffer> m_imguiFbs;
            bool                       m_imguiActive = false;

            // Fase 5: pipeline para UN render pass concreto. Devuelve la variante creada contra el
            // backbuffer (fase 4) o construye y cachea una contra `pass` (offscreen). Compute: igual
            // siempre (no depende del render pass). `colorCount` = nº de color attachments del pass
            // (el Vulkan exige que el pipeline declare UN estado de blend por attachment).
            VkPipeline pipelineVariant(PipelineHandle, VkRenderPass, uint32_t colorCount);
            // Render pass de un solo color para renderizar a una cara/mip de un cubemap (init LOAD
            // CLEAR, final SHADER_READ). Cachado por formato en la phase 5.
            VkRenderPass cubeFacePass(VkFormat colorFormat);

            // Frame-sync (fase 5). Modelo SENCILLO y correcto: aun UN command buffer y UN fence por
            // frame, y se espera la fence al final de endFrame → el N-buffering real se deja para una
            // fase posterior (aquí prima que funcione sobre que vaya rápido).
            //   beginFrame(): idempotente por frame real (el renderer llama beginFrame MUCHAS veces
            //                 por frame pero endFrame UNA). Adquiere la imagen una sola vez.
            //   endFrame():   submit + present + espera la fence (sync completo) → el image disponible
            //                 para el próximo frame.
            bool     ensureFrameSync();
            void     destroyFrameSync();
            // Tamaño del "ring" de descriptor sets del frame: uno por draw. Cada draw usa un set
            // distinto dentro del frame (se reescribe el mismo set dentro del frame sería legal solo
            // con update-after-bind; la validación lo avisaría). Basta con que ring >= draws/frame.
            static constexpr uint32_t kDescRing = 2048;
            std::vector<VkDescriptorSet> m_descRing;      // sets del ring (por draw)
            uint32_t                     m_setCursor = 0; // próximo set a usar en este frame

            SDL_Window*                m_window = nullptr;
            // GPUs preferidas por NOMBRE y en orden (ver `Device::create`). Vacío = automático.
            // Hoy solo decide cuál se usa; la lista existe para que un reparto multi-GPU futuro no
            // obligue a cambiar la forma del ajuste ya guardado por los usuarios.
            std::vector<std::string>   m_preferredGpus;
            VkInstance                 m_instance = VK_NULL_HANDLE;
            VkDebugUtilsMessengerEXT   m_debugMessenger = VK_NULL_HANDLE;
            VkPhysicalDevice           m_physical = VK_NULL_HANDLE;
            VkDevice                   m_device = VK_NULL_HANDLE;
            VkQueue                    m_graphicsQueue = VK_NULL_HANDLE;
            uint32_t                   m_graphicsFamily = 0;

            std::unique_ptr<VKSwapchain> m_swapchain;
            VkRenderPass               m_backbufferPass = VK_NULL_HANDLE;

            // Descriptor set layout y pipeline layout COMPARTIDOS (fase 4): UN descriptor set por
            // pipeline (bindings UBO 0..31 / SSBO 32..63 / textura 64..95 de PLAN_VULKAN.md §4.3).
            // Se crean perezosamente en el primer createPipeline y viven hasta el dtor del device.
            VkDescriptorSetLayout      m_setLayout = VK_NULL_HANDLE;
            VkPipelineLayout           m_pipelineLayout = VK_NULL_HANDLE;
            VkImage                    m_backbufferDepthImage = VK_NULL_HANDLE;
            VkDeviceMemory             m_backbufferDepthMemory = VK_NULL_HANDLE;
            VkImageView                m_backbufferDepthView = VK_NULL_HANDLE;

            // Pool de transferencias de UN SOLO uso (staging → device-local). Serializa con la
            // queue (espera su fence) → correcto para uploads puntuales; el N-buffering se decide
            // en la fase 5.
            VkCommandPool              m_transferPool = VK_NULL_HANDLE;
            VkCommandBuffer            m_transferCmd = VK_NULL_HANDLE;
            VkFence                    m_transferFence = VK_NULL_HANDLE;

            // Fase 5: pool/cmd BUFFER de UN frame + semáfor/fen de presentación (ver ensureFrameSync).
            // m_backbufferUsed: el renderer puede no llegar a dibujar a la pantalla (id==0) en un
            // frame; si no se usó, endFrame salta el present para no mostrar basura de un acquire
            // que nunca se rasterizó.
            VkCommandPool              m_framePool = VK_NULL_HANDLE;
            VkCommandBuffer            m_frameCmd = VK_NULL_HANDLE;
            VkSemaphore                m_imgAvailable = VK_NULL_HANDLE;
            VkSemaphore                m_renderFinished = VK_NULL_HANDLE;
            VkFence                    m_frameFence = VK_NULL_HANDLE;
            bool                       m_frameActive = false;
            bool                       m_backbufferUsed = false;
            uint32_t                   m_currentImage = 0;
            /// Última imagen del swapchain PRESENTADA, y por tanto la única cuyo contenido está
            /// completo y es legible. `readPixels` la necesita: se le llama a mitad de frame, con el
            /// command buffer de ESTE frame todavía abierto y sin enviar, así que `m_currentImage`
            /// aún no tiene dibujado nada. UINT32_MAX = todavía no se ha presentado ninguna.
            uint32_t                   m_lastPresentedImage = UINT32_MAX;
            std::unique_ptr<VKContext> m_context;   // único contexto de comandos (bye per-frame it)

            // Extra (fase 5): pool de descriptores para el ring del frame, variantes de pipeline y
            // render passes de cubemap (todo perezoso, se libera en el dtor).
            VkDescriptorPool           m_descPool = VK_NULL_HANDLE;
            std::map<std::string, VkPipeline> m_pipelineVariants;
            std::map<std::string, VkRenderPass> m_cubePasses;

            std::map<std::string, VkRenderPass> m_renderPasses;

            std::vector<VKBuffer>       m_buffers;
            std::vector<VKTexture>      m_textures;
            std::vector<VKSampler>      m_samplers;
            std::vector<VKPipeline>     m_pipelines;
            std::vector<VKRenderTarget> m_targets;
            std::vector<VkFence>        m_fences;
            std::vector<uint32_t>       m_freeBuffers, m_freeTextures;
            std::vector<uint32_t>       m_freeSamplers, m_freePipelines, m_freeTargets, m_freeFences;
    };
}
