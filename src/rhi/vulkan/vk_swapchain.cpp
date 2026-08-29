/**
 * @file vk_swapchain.cpp
 * @brief Surface + swapchain + image views + framebuffers del backbuffer.
 *        Setup de SDL3 + VK 1.3 probado en el spike `tests/vk_resize` (eliminado).
 */
#if defined(__has_include)
#  if __has_include(<vulkan/vulkan.h>)
#    define HARUKA_RHI_HAS_VULKAN 1
#  endif
#endif

#if defined(HARUKA_RHI_HAS_VULKAN)

#include "rhi/vulkan/vk_swapchain.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <vector>
#include <cstdlib>   // getenv: HARUKA_NO_VSYNC (ver la nota del present mode)

#include "core/logger.h"

namespace Haruka::RHI::vulkan
{
    static bool vkSuccess(VkResult r, const char* what)
    {
        if (r == VK_SUCCESS) return true;
        HARUKA_LOGE("RHI/VK", "%s -> VkResult %d", what, (int)r);
        return false;
    }

    VKSwapchain::VKSwapchain(VkInstance instance, SDL_Window* window)
        : m_instance(instance), m_window(window)
    {
    }

    VKSwapchain::~VKSwapchain()
    {
        destroyFramebuffers();
        destroySwapchain();
        if (m_surface) { vkDestroySurfaceKHR(m_instance, m_surface, nullptr); m_surface = VK_NULL_HANDLE; }
    }

    bool VKSwapchain::createSurface()
    {
        if (!m_window)
        {
            HARUKA_LOGE("RHI/VK", "VKSwapchain::createSurface sin ventana SDL");
            return false;
        }

        // Surface vía SDL (elige Wayland/X11 correctamente). Igual que el spike.
        if (SDL_Vulkan_CreateSurface(m_window, m_instance, nullptr, &m_surface) == false)
        {
            HARUKA_LOGE("RHI/VK", "SDL_Vulkan_CreateSurface fallo: %s", SDL_GetError());
            return false;
        }
        return true;
    }

    void VKSwapchain::configure(VkPhysicalDevice physical, VkDevice device, uint32_t graphicsFamily)
    {
        m_physical = physical;
        m_device   = device;
        m_graphicsFamily = graphicsFamily;
    }

    // [WAYLAND-1]: elige el extent correcto. En Wayland currentExtent es 0xFFFFFFFF ("tú decides")
    // → usamos el tamaño en píxeles de SDL y lo clampeamos al rango válido. En X11 currentExtent
    // ya trae el tamaño real y lo respetamos.
    VkExtent2D VKSwapchain::chooseExtent(const VkSurfaceCapabilitiesKHR& caps)
    {
        int spw = 0, sph = 0;
        SDL_GetWindowSizeInPixels(m_window, &spw, &sph);
        // ⚠️ SONDA: el swapchain salia a 1280x720 con la ventana a 1920x1080 (SDL lo confirmaba en
        // pixeles), asi que TODO el juego se renderizaba a 720p y se estiraba — y con el la UI, que
        // es como se noto ("ImGui no es nitido"). Esto dice quien manda en ese tamano.
        HARUKA_LOGI("RHI/VK", "swapchain extent: surface dice %ux%u%s · SDL en pixeles %dx%d · min %ux%u max %ux%u",
                    caps.currentExtent.width, caps.currentExtent.height,
                    (caps.currentExtent.width == 0xFFFFFFFFu) ? " (indefinido: mando yo)" : "",
                    spw, sph,
                    caps.minImageExtent.width, caps.minImageExtent.height,
                    caps.maxImageExtent.width, caps.maxImageExtent.height);
        if (caps.currentExtent.width != 0xFFFFFFFFu) return caps.currentExtent;
        const int pw = spw, ph = sph;
        VkExtent2D e{ (uint32_t)pw, (uint32_t)ph };
        e.width  = std::clamp(e.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
        e.height = std::clamp(e.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        return e;
    }

    bool VKSwapchain::create()
    {
        if (!m_surface || !m_device || !m_physical)
        {
            HARUKA_LOGE("RHI/VK", "VKSwapchain::create sin surface/device (orden: createSurface, configure, create)");
            return false;
        }
        return createSwapchain();
    }

    bool VKSwapchain::createSwapchain()
    {
        VkSurfaceCapabilitiesKHR caps;
        if (!vkSuccess(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical, m_surface, &caps), "surface caps"))
            return false;

        // Formato: preferimos BGRA8 UNORM; si no, el primero.
        uint32_t nf = 0;
        if (!vkSuccess(vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &nf, nullptr), "surface formats"))
            return false;
        std::vector<VkSurfaceFormatKHR> fmts(nf);
        if (!vkSuccess(vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &nf, fmts.data()), "surface formats"))
            return false;
        VkSurfaceFormatKHR chosen = fmts[0];
        for (auto& f : fmts)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
        m_format = chosen.format;

        uint32_t imgCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;
        m_minImageCount = caps.minImageCount;

        m_extent = chooseExtent(caps);

        VkSwapchainCreateInfoKHR sci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        sci.surface = m_surface;
        sci.minImageCount = imgCount;
        sci.imageFormat = chosen.format; sci.imageColorSpace = chosen.colorSpace;
        sci.imageExtent = m_extent;
        sci.imageArrayLayers = 1;
        // TRANSFER_SRC: para readPixels/blitColor del backbuffer (fase 5). No molesta al present.
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;   // una sola familia graphics+present
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        // ⚠️ FIFO (vsync) por defecto, y SIN vsync solo si se pide. Con FIFO, cualquier medida de
        // tiempo por frame queda clavada en 16,6 ms —que es 1/60— y deja de medir la GPU: mide la
        // presentacion. Paso midiendo el coste del sombreado del terreno, donde el numero de Vulkan
        // salio 16,61 -> 16,68 ms y no significaba nada.
        //
        // `HARUKA_NO_VSYNC=1` pide MAILBOX (triple buffer sin espera) y, si no lo hay, IMMEDIATE.
        // Ninguno de los dos es obligatorio en la spec, asi que se comprueba antes de pedirlo.
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;         // siempre soportado (vsync, como GL)
        if (const char* nv = std::getenv("HARUKA_NO_VSYNC")) {
            if (nv[0] == '1') {
                uint32_t pmCount = 0;
                vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &pmCount, nullptr);
                std::vector<VkPresentModeKHR> modes(pmCount);
                vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &pmCount, modes.data());
                for (VkPresentModeKHR want : { VK_PRESENT_MODE_MAILBOX_KHR,
                                               VK_PRESENT_MODE_IMMEDIATE_KHR }) {
                    if (std::find(modes.begin(), modes.end(), want) != modes.end()) {
                        sci.presentMode = want;
                        break;
                    }
                }
            }
        }
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = VK_NULL_HANDLE;
        if (!vkSuccess(vkCreateSwapchainKHR(m_device, &sci, nullptr, &m_swapchain), "create swapchain"))
            return false;

        uint32_t n = 0;
        if (!vkSuccess(vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, nullptr), "swapchain images"))
            return false;
        m_images.resize(n);
        if (!vkSuccess(vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, m_images.data()), "swapchain images"))
            return false;

        m_views.resize(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            VkImageViewCreateInfo iv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            iv.image = m_images[i];
            iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
            iv.format = m_format;
            iv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (!vkSuccess(vkCreateImageView(m_device, &iv, nullptr, &m_views[i]), "create image view"))
                return false;
        }

        return true;
    }

    void VKSwapchain::attachRenderPass(VkRenderPass renderPass)
    {
        m_fbRenderPass = renderPass;
    }

    void VKSwapchain::attachBackbufferDepth(VkImageView depthView)
    {
        m_backbufferDepth = depthView;
    }

    bool VKSwapchain::createFramebuffers()
    {
        if (!m_fbRenderPass)
        {
            HARUKA_LOGE("RHI/VK", "createFramebuffers: sin render pass (attachRenderPass primero)");
            return false;
        }
        // Dos attachments: color (swapchain) + depth (imagen del device, prestada). Solo color si
        // el render pass del backbuffer no tiene profundidad.
        std::vector<VkImageView> attachments = { m_views.empty() ? VK_NULL_HANDLE : m_views[0] };
        if (m_backbufferDepth) attachments.push_back(m_backbufferDepth);
        m_framebuffers.resize(m_views.size());
        for (size_t i = 0; i < m_views.size(); ++i)
        {
            attachments[0] = m_views[i];
            VkFramebufferCreateInfo fb{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fb.renderPass = m_fbRenderPass;
            fb.attachmentCount = (uint32_t)attachments.size();
            fb.pAttachments = attachments.data();
            fb.width = m_extent.width;
            fb.height = m_extent.height;
            fb.layers = 1;
            if (!vkSuccess(vkCreateFramebuffer(m_device, &fb, nullptr, &m_framebuffers[i]), "create framebuffer"))
                return false;
        }
        return true;
    }

    bool VKSwapchain::recreate()
    {
        // [WAYLAND-3] minimizado / tamaño 0 → no se puede crear swapchain. Se omite y se reintenta
        // en el siguiente frame (el device re-triggera recreate mientras valid() sea false).
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
        if (pw == 0 || ph == 0)
        {
            HARUKA_LOGW("RHI/VK", "swapchain recreate omitido: ventana minimizada (0x0)");
            return true;
        }

        vkDeviceWaitIdle(m_device);
        destroyFramebuffers();
        destroySwapchain();
        if (!createSwapchain()) return false;
        // Nota: los framebuffers NO se (re)crean aquí. En el resize el device tiene que recrear la
        // imagen de profundidad (que va dentro de cada framebuffer) ANTES de pedirlos, así que el
        // device los construye vía createFramebuffers() y nosotros solo recreamos swapchain+views.
        return true;
    }

    void VKSwapchain::destroyFramebuffers()
    {
        for (auto fb : m_framebuffers) if (fb) vkDestroyFramebuffer(m_device, fb, nullptr);
        m_framebuffers.clear();
    }

    void VKSwapchain::destroySwapchain()
    {
        for (auto v : m_views) if (v) vkDestroyImageView(m_device, v, nullptr);
        m_views.clear();
        m_images.clear();
        if (m_swapchain) { vkDestroySwapchainKHR(m_device, m_swapchain, nullptr); m_swapchain = VK_NULL_HANDLE; }
    }
}

#endif // HARUKA_RHI_HAS_VULKAN
