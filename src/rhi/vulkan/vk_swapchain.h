/**
 * @file vk_swapchain.h
 * @brief Surface + swapchain + image views + framebuffers del backbuffer (fase 2 del port).
 *
 * Propiedad: el device VK crea UN VKSwapchain al arrancar y lo recrea al redimensionar.
 * La surface y el swapchain viven y mueren aquí (no los toca el device salvo via recreate()).
 * El render pass del backbuffer lo crea el device (fase 4) y se presta aquí SOLO para montar
 * los framebuffers (no owned: lo sigue poseyendo el device).
 *
 * Orden de arranque (lo orquesta VKDevice):
 *   1. createSurface()  — necesita solo instance + window.
 *   2. El device elige physical/device (la surface ya existe para consultar present support).
 *   3. configure(physical, device, family).
 *   4. create() → swapchain + image views.
 */
#pragma once

#include <vector>

#include "rhi/vulkan/vk_common.h"

struct SDL_Window;   // solo puntero: SDL solo se usa en el .cpp

namespace Haruka::RHI::vulkan
{
    class VKSwapchain
    {
        public:
            VKSwapchain(VkInstance instance, SDL_Window* window);
            ~VKSwapchain();

            VKSwapchain(const VKSwapchain&) = delete;
            VKSwapchain& operator=(const VKSwapchain&) = delete;

            /** Crea la surface vía SDL (Wayland/X11). false si falla (el device cae a GL). */
            bool createSurface();

            /** Fija physical/device/family tras el arranque del device (ver cabecera). */
            void configure(VkPhysicalDevice physical, VkDevice device, uint32_t graphicsFamily);

            /** Crea swapchain + image views (requiere surface + configure hechos). false si falla. */
            bool create();

            /** Recrea swapchain + framebuffers al redimensionar.
             *  [WAYLAND-3] con ventana minimizada/tamaño 0 NO se puede crear: se omite y se
             *  reintenta en el siguiente frame (el device re-triggera recreate mientras no valga). */
            bool recreate();

            /** Presta el render pass del backbuffer (lo crea el device). Necesario antes de
             *  createFramebuffers(). No se destruye aquí. */
            void attachRenderPass(VkRenderPass renderPass);
            /** Presta la image view de profundidad del backbuffer (imagen owned por el device,
             *  recreada al redimensionar). Los framebuffers del backbuffer usan color+depth. */
            void attachBackbufferDepth(VkImageView depthView);
            /** Monta un framebuffer por image view del backbuffer. */
            bool createFramebuffers();

            uint32_t      imageCount() const     { return (uint32_t)m_views.size(); }
            VkImage       image(uint32_t i) const { return m_images[i]; }
            VkImageView   view(uint32_t i) const { return m_views[i]; }
            VkFramebuffer framebuffer(uint32_t i) const { return m_framebuffers[i]; }
            VkImageView   imageView(uint32_t i)    const { return m_views[i]; }
            VkSwapchainKHR swapchain() const      { return m_swapchain; }
            VkFormat      format() const          { return m_format; }
            VkExtent2D    extent() const          { return m_extent; }
            /// La ventana a la que sigue. La consulta `beginFrame` para detectar un resize
            /// que Wayland NO señala con OUT_OF_DATE (ver la nota en `VKDevice::beginFrame`).
            SDL_Window*   window() const          { return m_window; }
            VkSurfaceKHR  surface() const         { return m_surface; }
            uint32_t      minImageCount() const   { return m_minImageCount; }
            bool          valid() const           { return m_swapchain != VK_NULL_HANDLE; }

        private:
            // [WAYLAND-1]: en Wayland currentExtent es 0xFFFFFFFF ("tú decides") → tamaño en píxeles
            // de SDL clampeado al rango válido. En X11 currentExtent trae el tamaño real.
            VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& caps);
            bool       createSwapchain();
            void       destroyFramebuffers();
            void       destroySwapchain();

            VkInstance       m_instance = VK_NULL_HANDLE;
            VkPhysicalDevice m_physical = VK_NULL_HANDLE;
            VkDevice         m_device   = VK_NULL_HANDLE;
            uint32_t         m_graphicsFamily = 0;   // una sola familia graphics+present (como el spike)
            SDL_Window*      m_window   = nullptr;

            VkSurfaceKHR   m_surface       = VK_NULL_HANDLE;
            VkSwapchainKHR m_swapchain     = VK_NULL_HANDLE;
            VkFormat       m_format        = VK_FORMAT_B8G8R8A8_UNORM;
            VkExtent2D     m_extent{};
            uint32_t       m_minImageCount = 0;

            std::vector<VkImage>      m_images;
            std::vector<VkImageView>  m_views;
            std::vector<VkFramebuffer> m_framebuffers;
            VkRenderPass  m_fbRenderPass   = VK_NULL_HANDLE;   // prestado del device (no owned)
            VkImageView   m_backbufferDepth = VK_NULL_HANDLE;  // prestado del device (no owned)
    };
}
