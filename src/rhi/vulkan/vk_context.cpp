/**
 * @file vk_context.cpp
 * @brief Implementación Vulkan de RHI::Context — graba los comandos en el command buffer del frame.
 *
 * Flujo del frame (orquestado por VKDevice):
 *   beginFrame() → (1ª llamada del frame) adquiere imagen + abre el command buffer. Idempotente.
 *   [comandos: beginRenderPass/…/endRenderPass, binds, draws, dispatches, blits, …]
 *   endFrame()   → cierra el buffer, submit (espera imageAvailable, señaliza renderFinished),
 *                  present (espera renderFinished) y espera la fence (sync completo).
 *
 * Los detalles "dónde dibujar" (RenderPassHandle{} id 0 = pantalla) y las transiciones de layout
 * de attachments viven aquí; el device solo presta el command buffer y los recursos.
 */
#include "rhi/vulkan/vk_context.h"
#include "rhi/vulkan/vk_device.h"
#include <algorithm>
#include <cstring>

namespace Haruka::RHI::vulkan
{
    static bool vkOk(VkResult r)
    {
        if (r == VK_SUCCESS) return true;
        HARUKA_LOGE("RHI/VK", "vkCmd* -> VkResult %d", (int)r);
        return false;
    }

    // ------------------------------------------------------------------ helpers de layout
    // Transición de una imagen a un layout dado (solo si el layout actual difiere). oldLayout puede
    // ser UNDEFINED (descarta el contenido: primer uso, o attachments que se reescriben).
    static void transitionImage(VkCommandBuffer cmd, VkImage img, VkImageLayout oldLayout,
                                VkImageLayout newLayout, VkImageAspectFlags aspect,
                                VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                                VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = oldLayout;
        b.newLayout = newLayout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // viewport dinámico (igual que GL): x/y/w/h directos, origen arriba-izquierda.
    static void setViewportCmd(VkCommandBuffer cmd, int x, int y, int w, int h)
    {
        // ⚠️ AQUÍ NO SE INVIERTE LA Y. Se probó (altura negativa, la compensación "estándar") y
        // ROMPE EL POST-PROCESO: un pase fullscreen dibuja un quad y muestrea una textura, así que
        // invertir su viewport ESPEJA la imagen una vez más. Con el bloom iterando un número
        // configurable de veces, la PARIDAD de espejados cambia y el frame sale derecho o del revés
        // ALTERNANDO — exactamente el síntoma que se reportó.
        //
        // La inversión va en la PROYECCIÓN (ver `camera.cpp`), que solo afecta a la geometría 3D y
        // deja intactos los quads a pantalla completa.
        VkViewport vp{};
        vp.x = (float)x;
        vp.y = (float)y;
        vp.width = (float)w;
        vp.height = (float)h;
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        VkRect2D sc{};
        sc.offset.x = std::max(0, x);
        sc.offset.y = std::max(0, y);
        sc.extent.width = (uint32_t)std::max(0, w);
        sc.extent.height = (uint32_t)std::max(0, h);
        vkCmdSetScissor(cmd, 0, 1, &sc);
    }

    // ------------------------------------------------------------------ dtor
    VKContext::~VKContext()
    {
        if (!m_dev.m_device) return;
        for (auto& [k, fb] : m_cubeFbs) if (fb) vkDestroyFramebuffer(m_dev.m_device, fb, nullptr);
        for (VkImageView v : m_cubeViews) if (v) vkDestroyImageView(m_dev.m_device, v, nullptr);
    }

    // ------------------------------------------------------------------ render passes
    void VKContext::beginRenderPass(RenderPassHandle target, const ClearValues& c)
    {
        if (!m_dev.m_frameCmd || !m_dev.m_frameActive) return;
        if (m_inPass) endRenderPass();   // igual que GL: un nuevo begin reinicia el pass

        const bool screen = (target.id == 0);
        VkRenderPass pass = VK_NULL_HANDLE;
        VkFramebuffer fb = VK_NULL_HANDLE;
        uint32_t colorCount = 0;
        bool hasDepth = false;

        // ── VARIANTE DE loadOp: LIMPIAR o CONSERVAR ─────────────────────────────────────────────
        //
        // ⚠️ AQUÍ ESTABA EL MUNDO NEGRO. En OpenGL, `beginRenderPass` con `clearColor=false` quiere
        // decir "solo ata el FBO, no borres". En Vulkan el loadOp va HORNEADO en la render pass, así
        // que con una sola variante (CLEAR) cada `begin` borraba la pantalla — y el motor abre el
        // pase varias veces por frame (cielo → escena → props → …) esperando la semántica de GL. El
        // cielo se dibujaba y el `begin` siguiente se lo comía. Por eso en RenderDoc cada draw salía
        // BIEN por separado y el resultado compuesto era negro.
        //
        // Color y profundidad son INDEPENDIENTES: la máscara de cielo limpia profundidad y conserva
        // color, así que hacen falta las cuatro combinaciones.
        const int loadVariant = (c.clearColor ? 1 : 0) | (c.clearDepth ? 2 : 0);

        if (screen)
        {
            if (!m_dev.m_swapchain || !m_dev.m_swapchain->valid()) return;
            pass = m_dev.m_backbufferPassVariant[loadVariant]
                 ? m_dev.m_backbufferPassVariant[loadVariant] : m_dev.m_backbufferPass;
            fb = m_dev.m_swapchain->framebuffer(m_dev.m_currentImage);
            colorCount = 1;
            hasDepth = true;
            m_extent = m_dev.m_swapchain->extent();
            m_dev.m_backbufferUsed = true;
        }
        else if (const VKRenderTarget* rt = m_dev.renderTarget(target))
        {
            pass = m_dev.renderPassFor(rt->desc, loadVariant);
            if (!pass) pass = rt->renderPass;
            fb = rt->framebuffer;
            colorCount = (uint32_t)rt->colors.size();
            // Profundidad: o renderbuffer (depthView) o textura muestreada (depthTex; p.ej. shadow
            // maps). El framebuffer SIEMPRE tiene el attachment de depth en el índice tras los de
            // color, así que hay que contarlo para dimensionar bien pClearValues (si no, clearValue
            // index 0..n → VUID 00902).
            hasDepth = (rt->depthView != VK_NULL_HANDLE) || RHI::valid(rt->depthTex);
            m_extent = { rt->width, rt->height };
        }
        if (!pass || !fb) return;

        // ⚠️ LOS ATTACHMENTS TIENEN QUE ESTAR EN EL LAYOUT QUE DECLARA LA RENDER PASS.
        //
        // La variante `LOAD` declara `initialLayout = COLOR_ATTACHMENT_OPTIMAL` — tiene que hacerlo,
        // porque `UNDEFINED` autorizaría a descartar el contenido y entonces conservar no serviría
        // de nada. Pero estas mismas texturas se MUESTREAN entre pases (el bloom hace ping-pong:
        // escribe tex[0], lo lee, vuelve a escribirlo), y al muestrearlas quedan en
        // `SHADER_READ_ONLY_OPTIMAL`. Abrir el pase declarando otro layout es comportamiento
        // indefinido, y el driver entrega basura — que se ve como un FRAME FANTASMA.
        //
        // Se transiciona aquí, FUERA del pase (dentro sería ilegal sin self-dependency), y solo
        // cuando de verdad se conserva algo: si se limpia, el layout de entrada da igual.
        if (!screen && loadVariant != 3)
        {
            if (const VKRenderTarget* rt = m_dev.renderTarget(target))
            {
                for (TextureHandle th : rt->colors)
                {
                    VKTexture* t = m_dev.texture(th);
                    if (!t || !t->image) continue;
                    if (t->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) continue;
                    transitionImage(m_dev.m_frameCmd, t->image, t->layout,
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    VK_IMAGE_ASPECT_COLOR_BIT,
                                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                    t->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                }
            }
        }

        // Todas las render passes del port son loadOp CLEAR → siempre hay que pasar clear values.
        std::vector<VkClearValue> clears(colorCount + (hasDepth ? 1u : 0u));
        for (uint32_t i = 0; i < colorCount; ++i)
        {
            clears[i].color = c.clearColor
                ? VkClearColorValue{ { c.color[0], c.color[1], c.color[2], c.color[3] } }
                : VkClearColorValue{ { 0.0f, 0.0f, 0.0f, 0.0f } };
        }
        if (hasDepth)
            clears.back().depthStencil = c.clearDepth ? VkClearDepthStencilValue{ c.depth, 0 }
                                                       : VkClearDepthStencilValue{ 0.0f, 0 };  // reversed-Z: 0 = lejano

        VkRenderPassBeginInfo bi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        bi.renderPass = pass;
        bi.framebuffer = fb;
        bi.renderArea = { {0, 0}, { m_extent.width, m_extent.height } };
        bi.clearValueCount = (uint32_t)clears.size();
        bi.pClearValues = clears.data();
        vkCmdBeginRenderPass(m_dev.m_frameCmd, &bi, VK_SUBPASS_CONTENTS_INLINE);

        // Viewport default: el target entero. Un setViewport pendiente (fuera de pass) lo reemplaza.
        setViewportCmd(m_dev.m_frameCmd, 0, 0, (int)m_extent.width, (int)m_extent.height);
        if (m_vpPending)
        {
            setViewportCmd(m_dev.m_frameCmd, m_vpX, m_vpY, m_vpW, m_vpH);
            m_vpPending = false;
        }

        m_inPass = true;
        m_target = target;
        m_hasDepth = hasDepth;
    }

    void VKContext::endRenderPass()
    {
        if (!m_inPass || !m_dev.m_frameCmd) return;
        vkCmdEndRenderPass(m_dev.m_frameCmd);

        // Transiciones de layout diferidas (bindTextura de UNDEFINED dentro del pase → no puede
        // emitir barrier dentro). Ahora fuera del pase sí es legal: UNDEFINED → SHADER_READ.
        for (TextureHandle th : m_pendingTransition)
        {
            // Se resuelve AHORA, no cuando se encoló: si la textura se destruyó entretanto, esto
            // devuelve null en vez de un puntero muerto (ver la nota de `m_pendingTransition`).
            VKTexture* t = m_dev.texture(th);
            if (!t || !t->image) continue;
            if (t->layout == VK_IMAGE_LAYOUT_UNDEFINED)
            {
                transitionImage(m_dev.m_frameCmd, t->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                                0, VK_ACCESS_SHADER_READ_BIT,
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }
        m_pendingTransition.clear();

        // Attachments offscreen: los deja el render pass en COLOR_ATTACHMENT_OPTIMAL /
        // DEPTH_STENCIL_ATTACHMENT_OPTIMAL. Para MUESTREARLOS después (bloom lee scene color,
        // sombras leen depth) hace falta pasarlos a SHADER_READ_ONLY_OPTIMAL. El siguiente
        // beginRenderPass de este target descarta el contenido (loadOp CLEAR) → no hay que volver.
        if (m_target.id != 0)
        {
            if (const VKRenderTarget* rt = m_dev.renderTarget(m_target))
            {
                for (TextureHandle th : rt->colors)
                {
                    if (VKTexture* t = m_dev.texture(th))
                    {
                        transitionImage(m_dev.m_frameCmd, t->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                        t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    }
                }
                if (rt->depthView)
                {
                    VkImage img = rt->depthImage ? rt->depthImage
                                 : (RHI::valid(rt->depthTex) ? m_dev.texture(rt->depthTex)->image : VK_NULL_HANDLE);
                    if (img)
                    {
                        transitionImage(m_dev.m_frameCmd, img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT,
                                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                        if (RHI::valid(rt->depthTex)) m_dev.texture(rt->depthTex)->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    }
                }
            }
        }

        m_inPass = false;
        m_target = {};
    }

    void VKContext::setViewport(int x, int y, int w, int h)
    {
        m_vpX = x; m_vpY = y; m_vpW = w; m_vpH = h;
        if (m_inPass) setViewportCmd(m_dev.m_frameCmd, x, y, w, h);
        else          m_vpPending = true;   // se aplica al siguiente beginRenderPass
    }

    void VKContext::clear(float r, float g, float b, float a)
    {
        if (!m_inPass || !m_dev.m_frameCmd) return;
        VkClearAttachment atts[2]{};
        uint32_t n = 0;
        atts[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        atts[n].colorAttachment = 0;
        atts[n].clearValue.color = { { r, g, b, a } };
        ++n;
        if (m_hasDepth)
        {
            atts[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            atts[n].clearValue.depthStencil = VkClearDepthStencilValue{ 0.0f, 0 };   // reversed-Z: lejano
            ++n;
        }
        VkClearRect rect;
        rect.rect = { {0, 0}, { m_extent.width, m_extent.height } };
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        vkCmdClearAttachments(m_dev.m_frameCmd, n, atts, 1, &rect);
    }

    // ------------------------------------------------------------------ bindings
    void VKContext::bindPipeline(PipelineHandle h)
    {
        const VKPipeline* p = m_dev.pipeline(h);
        if (!p) return;
        m_pipeline = h;
        m_hasPipeline = true;
        m_compute = p->compute;
    }

    void VKContext::bindVertexBuffer(BufferHandle h, uint32_t binding)
    {
        if (binding >= 8) return;
        const VKBuffer* b = m_dev.buffer(h);
        if (!b) return;
        m_vbs[binding] = b->buffer;
        m_vbUsed[binding] = true;
    }

    void VKContext::bindIndexBuffer(BufferHandle h)
    {
        const VKBuffer* b = m_dev.buffer(h);
        if (!b) return;
        m_ib = b->buffer;
    }

    // El descriptor set del draw actual: los bind* (desde el draw anterior) escriben aquí. Cada batch
    // de binds que acaba en un draw consume un set del ring; así no se reescribe un set ya bindeado
    // dentro del command buffer del frame (habría riesgo de update-after-bind).
    VkDescriptorSet VKContext::nextDescSet()
    {
        if (m_dev.m_descRing.empty()) return VK_NULL_HANDLE;
        VkDescriptorSet s = m_dev.m_descRing[m_dev.m_setCursor];
        // Wrap dentro del frame: legal solo para sets del frame ANTERIOR (ya yield), no del mismo
        // frame. kDescRing se dimensiona para el pico realista de draws por frame; si se supera,
        // reutilizar acarrea update-after-bind (la validación lo avisaría) pero nunca corrupción.
        if (m_dev.m_setCursor == VKDevice::kDescRing - 1)
        {
            static bool warned = false;
            if (!warned)
            {
                HARUKA_LOGW("RHI/VK", "anillo de descriptor sets agotado en este frame: > %u draws. "
                            "Reutilizando sets del frame anterior (posible update-after-bind).",
                            (unsigned)VKDevice::kDescRing);
                warned = true;
            }
        }
        m_dev.m_setCursor = (m_dev.m_setCursor + 1) % VKDevice::kDescRing;
        return s;
    }

    // Vuelca TODO lo atado en un set recien estrenado. Sin esto, un set nuevo solo contiene el
    // slot que se acaba de atar y el resto queda indefinido — ver la nota en vk_context.h.
    void VKContext::flushShadowInto(VkDescriptorSet set)
    {
        if (!set) return;
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(m_shadowUbo.size() + m_shadowSsbo.size() + m_shadowTex.size());

        auto addBuf = [&](const std::map<uint32_t, VkDescriptorBufferInfo>& src, VkDescriptorType ty) {
            for (const auto& kv : src) {
                VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                w.dstSet = set;
                w.dstBinding = kv.first;
                w.dstArrayElement = 0;
                w.descriptorCount = 1;
                w.descriptorType = ty;
                w.pBufferInfo = &kv.second;   // los infos viven en el map: siguen validos aqui
                writes.push_back(w);
            }
        };
        addBuf(m_shadowUbo,  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        addBuf(m_shadowSsbo, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        for (const auto& kv : m_shadowTex) {
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = set;
            w.dstBinding = kv.first;
            w.dstArrayElement = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &kv.second;
            writes.push_back(w);
        }
        if (!writes.empty())
            vkUpdateDescriptorSets(m_dev.m_device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    }

    // Ver la nota de `forgetBuffer` en la cabecera: un recurso destruido tiene que salir del estado
    // pegajoso, o el siguiente descriptor set lo reescribe muerto.
    void VKContext::forgetBuffer(VkBuffer b)
    {
        if (!b) return;
        for (auto it = m_shadowUbo.begin(); it != m_shadowUbo.end(); )
            it = (it->second.buffer == b) ? m_shadowUbo.erase(it) : std::next(it);
        for (auto it = m_shadowSsbo.begin(); it != m_shadowSsbo.end(); )
            it = (it->second.buffer == b) ? m_shadowSsbo.erase(it) : std::next(it);

        // ⚠️ LOS VERTEX/INDEX BUFFERS TAMBIÉN SON ESTADO PEGAJOSO, y se quedaban fuera.
        //
        // Esta función se escribió para los descriptores y solo limpiaba esos. Pero `m_vbs`/`m_ib`
        // sobreviven igual de un draw al siguiente: quien dibuja sin reatar TODOS los bindings
        // hereda los del draw anterior. Destruir una malla dejaba ahí un VkBuffer muerto y el
        // siguiente `vkCmdBindVertexBuffers` lo ataba:
        //     [VVL] vkCmdBindVertexBuffers(): pBuffers[0] Invalid VkBuffer Object 0x926...
        //     [VVL] Couldn't find VkBuffer Object ... may indicate a bug in the application
        // y detrás, un SIGSEGV. En OpenGL el mismo patrón es inofensivo (un nombre borrado se lee
        // como 0 y el draw no pinta nada), que es por qué solo se cae de este lado.
        //
        // Lo destapó `testCullWindingWithProjection`, que destruye su malla en cada llamada. En el
        // juego lo destapa cualquier cosa que libere geometría con el streaming andando.
        for (int i = 0; i < 8; ++i)
            if (m_vbs[i] == b) { m_vbs[i] = VK_NULL_HANDLE; m_vbUsed[i] = false; }
        if (m_ib == b) m_ib = VK_NULL_HANDLE;
    }

    void VKContext::forgetImageView(VkImageView v)
    {
        if (!v) return;
        for (auto it = m_shadowTex.begin(); it != m_shadowTex.end(); )
            it = (it->second.imageView == v) ? m_shadowTex.erase(it) : std::next(it);
    }

    void VKContext::bindUniformBuffer(uint32_t slot, BufferHandle h)
    {
        const VKBuffer* b = m_dev.buffer(h);
        if (!b || !b->buffer) return;
        if (!m_descDirty) { m_curSet = nextDescSet(); m_descDirty = true; flushShadowInto(m_curSet); }
        if (!m_curSet) return;
        VkDescriptorBufferInfo info{ b->buffer, 0, (VkDeviceSize)b->size };
        m_shadowUbo[uboSlotToBinding(slot)] = info;   // estado pegajoso: se replica en cada set nuevo
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = m_curSet;
        w.dstBinding = uboSlotToBinding(slot);
        w.dstArrayElement = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &info;
        vkUpdateDescriptorSets(m_dev.m_device, 1, &w, 0, nullptr);
    }

    void VKContext::bindStorageBuffer(uint32_t slot, BufferHandle h)
    {
        const VKBuffer* b = m_dev.buffer(h);
        if (!b || !b->buffer) return;
        if (!m_descDirty) { m_curSet = nextDescSet(); m_descDirty = true; flushShadowInto(m_curSet); }
        if (!m_curSet) return;
        VkDescriptorBufferInfo info{ b->buffer, 0, (VkDeviceSize)b->size };
        m_shadowSsbo[ssboSlotToBinding(slot)] = info;
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = m_curSet;
        w.dstBinding = ssboSlotToBinding(slot);
        w.dstArrayElement = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &info;
        vkUpdateDescriptorSets(m_dev.m_device, 1, &w, 0, nullptr);
    }

    void VKContext::bindTexture(uint32_t slot, TextureHandle th, SamplerHandle sh)
    {
        VKTexture* t = m_dev.texture(th);
        if (!t || !t->view || !t->image) return;
        if (!m_descDirty) { m_curSet = nextDescSet(); m_descDirty = true; flushShadowInto(m_curSet); }
        if (!m_curSet) return;

        // Textura nunca usada todavía (ni upload ni render): la pasamos a SHADER_READ. Un
        // beginRenderPass la volverá a UNDEFINED (loadOp CLEAR la descarta) → no hay conflicto.
        if (t->layout == VK_IMAGE_LAYOUT_UNDEFINED)
        {
            // OJO: vkCmdPipelineBarrier DENTRO de una render pass sin self-dependency es
            // comportamiento indefinido — en NVIDIA puede colgar la GPU (TDR) al samplear estas
            // texturas durante el pase de escena. Si estamos dentro de un pase activo, diferimos la
            // transición a endRenderPass (fuera del pase); la layout SIGUE UNDEFINED mientras tanto,
            // así que un próximo bind fuera de pase la retomará correctamente.
            if (m_inPass)
                m_pendingTransition.push_back(th);
            else
            {
                transitionImage(m_dev.m_frameCmd, t->image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                                0, VK_ACCESS_SHADER_READ_BIT,
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }

        VkSampler sampler = VK_NULL_HANDLE;
        if (const VKSampler* s = m_dev.sampler(sh)) sampler = s->id;
        if (!sampler) sampler = t->sampler;

        VkDescriptorImageInfo info{ sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        m_shadowTex[textureSlotToBinding(slot)] = info;
        VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = m_curSet;
        w.dstBinding = textureSlotToBinding(slot);
        w.dstArrayElement = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(m_dev.m_device, 1, &w, 0, nullptr);
    }

    // ------------------------------------------------------------------ pipeline + draws
    // Bindeada el pipeline (variante del render pass activo si hace falta) + el descriptor set del
    // draw actual. Devuelve false si no hay nada que dibujar.
    bool VKContext::ensurePipeline()
    {
        if (!m_dev.m_frameCmd || !m_hasPipeline) return false;
        const VKPipeline* p = m_dev.pipeline(m_pipeline);
        if (!p || !p->handle) return false;

        VkRenderPass pass = VK_NULL_HANDLE;
        uint32_t colorCount = 1;
        if (!m_compute)
        {
            if (m_target.id == 0) pass = m_dev.m_backbufferPass;
            else if (const VKRenderTarget* rt = m_dev.renderTarget(m_target))
            {
                pass = rt->renderPass;
                colorCount = (uint32_t)rt->colors.size();
            }
        }
        VkPipeline vk = m_dev.pipelineVariant(m_pipeline, pass, colorCount);
        if (!vk) return false;
        vkCmdBindPipeline(m_dev.m_frameCmd, m_compute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                                      : VK_PIPELINE_BIND_POINT_GRAPHICS, vk);

        // Descriptor set: si no hay ninguno en curso, se pide uno; se bindea y se marca como usado
        // (la próxima escritura de binds pedirá otro set del ring).
        if (!m_curSet) { m_curSet = nextDescSet(); flushShadowInto(m_curSet); }
        if (m_curSet)
            vkCmdBindDescriptorSets(m_dev.m_frameCmd, m_compute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                                                : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_dev.m_pipelineLayout, 0, 1, &m_curSet, 0, nullptr);
        m_descDirty = false;
        return true;
    }

    static void bindUsedVertexBuffers(VkCommandBuffer cmd, const VkBuffer* vbs, const bool* used)
    {
        VkBuffer buffers[8] = {};
        VkDeviceSize offsets[8] = {};
        uint32_t n = 0;
        for (uint32_t i = 0; i < 8; ++i)
            if (used[i]) { buffers[n] = vbs[i]; offsets[n] = 0; ++n; }
        if (n) vkCmdBindVertexBuffers(cmd, 0, n, buffers, offsets);
    }

    void VKContext::draw(uint32_t vertexCount, uint32_t first, uint32_t instanceCount)
    {
        if (!m_inPass || !instanceCount) return;
        if (!ensurePipeline()) return;
        bindUsedVertexBuffers(m_dev.m_frameCmd, m_vbs, m_vbUsed);
        vkCmdDraw(m_dev.m_frameCmd, vertexCount, instanceCount, first, 0);
    }

    void VKContext::drawIndexed(uint32_t indexCount, uint32_t first, uint32_t instanceCount)
    {
        if (!m_inPass || !instanceCount) return;
        if (!ensurePipeline()) return;
        bindUsedVertexBuffers(m_dev.m_frameCmd, m_vbs, m_vbUsed);
        if (m_ib) vkCmdBindIndexBuffer(m_dev.m_frameCmd, m_ib, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(m_dev.m_frameCmd, indexCount, instanceCount, first, 0, 0);
    }

    void VKContext::drawIndexedIndirect(BufferHandle cmds, uint32_t drawCount, uint32_t stride, size_t offset)
    {
        if (!m_inPass || !drawCount) return;
        if (!ensurePipeline()) return;
        bindUsedVertexBuffers(m_dev.m_frameCmd, m_vbs, m_vbUsed);
        const VKBuffer* b = m_dev.buffer(cmds);
        if (!b || !b->buffer) return;
        if (m_ib) vkCmdBindIndexBuffer(m_dev.m_frameCmd, m_ib, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(m_dev.m_frameCmd, b->buffer, (VkDeviceSize)offset, drawCount, stride);
    }

    void VKContext::dispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        if (!m_hasPipeline || !m_compute) return;

        // ⚠️ COMPUTE DENTRO DE UN RENDER PASS ES ILEGAL EN VULKAN, y cerraba el programa.
        //
        // La especificación prohíbe `vkCmdDispatch` dentro de una instancia de render pass. En
        // OpenGL lo equivalente (`glDispatchCompute` con un FBO atado) es legal y corriente, así que
        // el motor lo hace con naturalidad: el culling de parches del terreno se despacha entre el
        // `beginRenderPass` de la escena y sus draws (ver `TerrestrialPlanet::render`).
        //
        // El síntoma NO se parecía a la causa: el proceso moría tras un `vkCmdBeginRenderPass`, y en
        // una captura de RenderDoc los draws se veían BIEN uno a uno —porque el replay los ejecuta
        // aislados— mientras que en ejecución real el dispositivo se perdía a mitad de frame y todo
        // lo posterior se descartaba: pantalla negra.
        //
        // Aquí se rechaza y se avisa en vez de grabarlo, que es lo único correcto a este nivel: el
        // arreglo de verdad es sacar el compute FUERA del pase en quien lo llama, porque
        // reabrir el pase aquí volvería a limpiar los attachments (todas las render passes del port
        // son `loadOp CLEAR`) y borraría lo ya dibujado.
        if (m_inPass)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                HARUKA_LOGE("RHI/VK", "vkCmdDispatch DENTRO de un render pass: ilegal en Vulkan. "
                            "El dispatch se OMITE (antes cerraba el programa). Hay que mover el "
                            "compute fuera del pase; en GL esto es legal y por eso el motor lo hace.");
            }
            return;
        }

        if (!ensurePipeline()) return;
        vkCmdDispatch(m_dev.m_frameCmd, x, y, z);
    }

    // ------------------------------------------------------------------ fences (sync GPU→CPU)
    FenceHandle VKContext::signalFence()
    {
        VkFence f = VK_NULL_HANDLE;
        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (!vkOk(vkCreateFence(m_dev.m_device, &fi, nullptr, &f))) return {};

        // Submit VACÍO con la fence: se señaliza cuando la cola alcanza este punto del stream. Un
        // command buffer auxiliar del pool del frame (se libera al resetear el pool en el dtor).
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = m_dev.m_framePool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!vkOk(vkAllocateCommandBuffers(m_dev.m_device, &ai, &cb)))
        { vkDestroyFence(m_dev.m_device, f, nullptr); return {}; }
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(cb, &bi);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        if (!vkOk(vkQueueSubmit(m_dev.m_graphicsQueue, 1, &si, f)))
        { vkDestroyFence(m_dev.m_device, f, nullptr); return {}; }

        return FenceHandle{ VKDevice::alloc(m_dev.m_fences, m_dev.m_freeFences, f) };
    }

    bool VKContext::waitFence(FenceHandle h, uint64_t timeoutNs)
    {
        const VkFence* f = m_dev.fence(h);
        if (!f || !*f) return true;
        if (timeoutNs == 0)
            return vkGetFenceStatus(m_dev.m_device, *f) == VK_SUCCESS;
        return vkWaitForFences(m_dev.m_device, 1, f, VK_TRUE, timeoutNs) == VK_SUCCESS;
    }

    void VKContext::deleteFence(FenceHandle h)
    {
        const VkFence* f = m_dev.fence(h);
        if (f && *f) vkDestroyFence(m_dev.m_device, *f, nullptr);
        VKDevice::release(m_dev.m_fences, m_dev.m_freeFences, h.id);
    }

    // ------------------------------------------------------------------ cubemap (IBL)
    void VKContext::beginRenderPassCubemapFace(TextureHandle th, int face, int mipLevel, const ClearValues& c)
    {
        const VKTexture* t = m_dev.texture(th);
        if (!t || !t->image) return;
        if (m_inPass) endRenderPass();

        VkRenderPass pass = m_dev.cubeFacePass(t->format);
        if (!pass) return;

        const uint32_t w = std::max(1u, t->width >> mipLevel);
        const uint32_t h = std::max(1u, t->height >> mipLevel);

        const std::string key = std::to_string(th.id) + ":" + std::to_string(face) + ":" + std::to_string(mipLevel);
        VkFramebuffer fb = VK_NULL_HANDLE;
        auto it = m_cubeFbs.find(key);
        if (it != m_cubeFbs.end())
        {
            fb = it->second;
        }
        else
        {
            VkImageViewCreateInfo iv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            iv.image = t->image;
            iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
            iv.format = t->format;
            iv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)mipLevel, 1, (uint32_t)face, 1 };
            VkImageView view = VK_NULL_HANDLE;
            if (!vkOk(vkCreateImageView(m_dev.m_device, &iv, nullptr, &view))) return;

            VkFramebufferCreateInfo fbci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fbci.renderPass = pass;
            fbci.attachmentCount = 1;
            fbci.pAttachments = &view;
            fbci.width = w;
            fbci.height = h;
            fbci.layers = 1;
            if (!vkOk(vkCreateFramebuffer(m_dev.m_device, &fbci, nullptr, &fb)))
            { vkDestroyImageView(m_dev.m_device, view, nullptr); return; }

            m_cubeViews.push_back(view);   // vive hasta el dtor del context
            m_cubeFbs[key] = fb;
        }

        VkClearValue clear;
        clear.color = c.clearColor
            ? VkClearColorValue{ { c.color[0], c.color[1], c.color[2], c.color[3] } }
            : VkClearColorValue{ { 0.0f, 0.0f, 0.0f, 0.0f } };
        VkRenderPassBeginInfo bi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        bi.renderPass = pass;
        bi.framebuffer = fb;
        bi.renderArea = { {0, 0}, { w, h } };
        bi.clearValueCount = 1;
        bi.pClearValues = &clear;
        vkCmdBeginRenderPass(m_dev.m_frameCmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        setViewportCmd(m_dev.m_frameCmd, 0, 0, (int)w, (int)h);

        m_inPass = true;
        m_target = {};
        m_hasDepth = false;
        m_extent = { w, h };
    }

    // ------------------------------------------------------------------ blits (copia de imagen)
    VkImage VKContext::colorImageOf(RenderPassHandle h)
    {
        if (h.id == 0)
            return m_dev.m_swapchain && m_dev.m_swapchain->valid() ? m_dev.m_swapchain->image(0) : VK_NULL_HANDLE;
        const VKRenderTarget* rt = m_dev.renderTarget(h);
        if (rt && !rt->colors.empty()) return m_dev.texture(rt->colors[0])->image;
        return VK_NULL_HANDLE;
    }

    VkImage VKContext::depthImageOf(RenderPassHandle h)
    {
        if (h.id == 0) return m_dev.m_backbufferDepthImage;
        if (const VKRenderTarget* rt = m_dev.renderTarget(h))
        {
            if (rt->depthImage) return rt->depthImage;
            if (RHI::valid(rt->depthTex)) return m_dev.texture(rt->depthTex)->image;
        }
        return VK_NULL_HANDLE;
    }

    void VKContext::blitDepth(RenderPassHandle src, RenderPassHandle dst, int w, int h)
    {
        if (!m_dev.m_frameCmd) return;
        VkImage s = depthImageOf(src);
        VkImage d = depthImageOf(dst);
        if (!s || !d || s == d) return;
        if (w <= 0) w = (int)m_extent.width;
        if (h <= 0) h = (int)m_extent.height;

        // Los layouts actuales se asumen DEPTH_STENCIL_ATTACHMENT_OPTIMAL (acabados de renderizar).
        transitionImage(m_dev.m_frameCmd, s, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        transitionImage(m_dev.m_frameCmd, d, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy r{};
        r.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        r.srcOffset = { 0, 0, 0 };
        r.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        r.dstOffset = { 0, 0, 0 };
        r.extent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyImage(m_dev.m_frameCmd, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }

    void VKContext::blitColor(RenderPassHandle src, RenderPassHandle dst, int w, int h)
    {
        if (!m_dev.m_frameCmd) return;
        VkImage s = colorImageOf(src);
        VkImage d = colorImageOf(dst);
        if (!s || !d || s == d) return;
        if (w <= 0) w = (int)m_extent.width;
        if (h <= 0) h = (int)m_extent.height;

        transitionImage(m_dev.m_frameCmd, s, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        transitionImage(m_dev.m_frameCmd, d, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy r{};
        r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.srcOffset = { 0, 0, 0 };
        r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.dstOffset = { 0, 0, 0 };
        r.extent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyImage(m_dev.m_frameCmd, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }

    void VKContext::memoryBarrier(uint32_t /*barriers*/)
    {
        if (!m_dev.m_frameCmd) return;
        // Conservador (como el glMemoryBarrier(ALL) del GL): serializa reads/writes de compute y
        // de fragment. Correcto siempre, no óptimo.
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(m_dev.m_frameCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }
}
