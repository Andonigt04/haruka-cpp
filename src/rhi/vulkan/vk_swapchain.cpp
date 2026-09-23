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
        HARUKA_LOGD("RHI/VK", "swapchain extent: surface dice %ux%u%s · SDL en pixeles %dx%d · min %ux%u max %ux%u",
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
        // ▸ MODO DE PRESENT del ajuste `VSync` del juego. Antes era SIEMPRE FIFO y el ajuste solo
        // ataba a GL vía `SDL_GL_SetSwapInterval` — Vulkan lo ignoraba y con una pantalla a baja
        // frecuencia de refresco el bucle quedaba engarzado en 2-3 vblanks (60-100 ms) con el
        // "VSync" del panel en OFF (medido en la 3050 Laptop: F5 leía 10 fps / 100 ms clavados).
        // `HARUKA_NO_VSYNC=1` lo fuerza SUELTO siempre: es el mando de medir, y con FIFO el tiempo
        // por frame queda clavado en 1/refresco y deja de medir la GPU — mide la presentacion
        // (el coste del terreno en Vulkan salía 16,61 -> 16,68 ms, el número no significaba nada).
        //
        // AHORA, `vsync=true` NO es FIFO: el acoplamiento FIFO al refresh es el bug de los 100 ms
        // (con GPU a ~33 ms y el panel a 60 Hz, la cola de 2 imágenes engarzaba cada frame a 2-3
        // vblanks y el F5 leía 50-100 ms clavados pese a estar "sincronizado"). Para "sin tearing
        // pero sin stall" el modo correcto es MAILBOX: presenta el frame completado más reciente,
        // como los vsync adaptativos — el bucle corre a su ritmo natural y el confort del portátil
        // lo cubre el cap `maxFps` del juego, no el vblank. `vsync=false` → IMMEDIATE (lo suelto).
        const bool forceSuelto = [] {
            const char* nv = std::getenv("HARUKA_NO_VSYNC");
            return nv && nv[0] == '1';
        }();
        // MAILBOX e IMMEDIATE no son obligatorios en la spec: se comprueban antes de pedirlos.
        uint32_t pmCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &pmCount, nullptr);
        std::vector<VkPresentModeKHR> modes(pmCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &pmCount, modes.data());
        auto has = [&](VkPresentModeKHR pm) { return std::find(modes.begin(), modes.end(), pm) != modes.end(); };
        // Preferencia: vsync=true → MAILBOX (vsync adaptativo; "tear cuando llegas tarde", nunca
        // engarza a 2-3 vblanks como sana FIFO con GPU > 1/refresco). Si el driver no lo expone
        // (algunos Wayland/XWayland solo traen FIFO/FIFO_RELAXED), FIFO_RELAXED hace casi lo mismo:
        // espera al vblank si le das tiempo, pero si llegas tarde presenta en seguida en vez de
        // engancharse. Solo si tampoco está, FIFO a secas (el engarzado que se medía como 24 fps).
        // vsync=false → IMMEDIATE (lo suelto), con MAILBOX/FIFO_RELAXED como degradados razonables.
        const VkPresentModeKHR wanted = forceSuelto ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                     : (m_vsyncFifo ? VK_PRESENT_MODE_MAILBOX_KHR
                                                    : VK_PRESENT_MODE_IMMEDIATE_KHR);
        const VkPresentModeKHR fallbacksVsync[]  = { VK_PRESENT_MODE_MAILBOX_KHR,    VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR };
        const VkPresentModeKHR fallbacksSuelto[] = { VK_PRESENT_MODE_IMMEDIATE_KHR,  VK_PRESENT_MODE_MAILBOX_KHR,     VK_PRESENT_MODE_FIFO_RELAXED_KHR };
        const VkPresentModeKHR* fb = m_vsyncFifo && !forceSuelto ? fallbacksVsync : fallbacksSuelto;
        VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
        for (int i = 0; i < 3; ++i) { if (has(fb[i])) { pm = fb[i]; break; } }
        if (pm != wanted)
            HARUKA_LOGW("RHI/VK", "swapchain: present mode %d no soportado (modos: %u) -> %d",
                        (int)wanted, pmCount, (int)pm);
        sci.presentMode = pm;
        HARUKA_LOGI("RHI/VK", "swapchain: vsync=%d querido=%d usado=%d · imagenes %u (min %u)",
                    m_vsyncFifo ? 1 : 0, (int)wanted, (int)pm, imgCount, caps.minImageCount);
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
