#if defined(__has_include)
#  if __has_include(<vulkan/vulkan.h>)
#    define HARUKA_RHI_HAS_VULKAN 1
#  endif
#endif

#if defined(HARUKA_RHI_HAS_VULKAN)

#include "rhi/vulkan/vk_device.h"
#include "rhi/vulkan/vk_pipeline.h"
#include "rhi/vulkan/vk_context.h"

#include <cstdio>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>   // SDL_Vulkan_GetInstanceExtensions / SDL_Vulkan_CreateSurface
#include "core/logger.h"

// ImGui Vulkan (fase 7): se usa desde aquí porque el dibujo de la UI hay que insertarlo en el
// command buffer UNICO del frame (m_frameCmd) dentro de un render pass sobre el backbuffer. El
// backend es parte del engine (IMGUI_SOURCES) y vuelve con el mismo .so.
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

namespace Haruka::RHI::vulkan
{
    // ------------------------------------------------------------------ utilidades internas
    static bool vkSuccess(VkResult r, const char* what)
    {
        if (r == VK_SUCCESS) return true;
        HARUKA_LOGE("RHI/VK", "%s -> VkResult %d", what, (int)r);
        return false;
    }

    static bool hasLayer(const char* name)
    {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        for (auto& p : props) if (std::strcmp(p.layerName, name) == 0) return true;
        return false;
    }

    static uint32_t bytesPerPixel(Format f)
    {
        switch (f)
        {
            case Format::RGBA8:      return 4;
            case Format::SRGB8_ALPHA8: return 4;
            case Format::RGB8:       return 3;
            case Format::RGBA16F:    return 8;
            case Format::RGB16F:     return 6;
            case Format::RG16F:      return 4;
            case Format::RGBA32F:    return 16;
            case Format::RGB32F:     return 12;
            case Format::RG32F:      return 8;
            case Format::R11G11B10F: return 4;
            case Format::R8:         return 1;
            case Format::R32F:       return 4;
            default:                 return 0;
        }
    }

    // ------------------------------------------------------------------ arranque
    VKDevice::VKDevice(SDL_Window* window, std::vector<std::string> preferredGpus)
        : m_window(window), m_preferredGpus(std::move(preferredGpus))
    {
        createInstance();
        if (!m_instance) { HARUKA_LOGE("RHI/VK", "sin instancia Vulkan"); return; }
        createDebugMessenger();

        m_swapchain = std::make_unique<VKSwapchain>(m_instance, m_window);
        if (!m_swapchain->createSurface()) return;

        pickPhysicalDevice();
        if (!m_physical) { HARUKA_LOGE("RHI/VK", "sin physical device con graphics+present"); return; }
        createLogicalDevice();
        if (!m_device) return;

        m_swapchain->configure(m_physical, m_device, m_graphicsFamily);
        if (!m_swapchain->create()) return;

        createBackbufferResources();

        // Fase 5: pool/cmd buffer del frame + semáforos + fence + pool de descriptores del ring.
        // El ring de descriptores se crea perezoso en el primer beginFrame (necesita m_setLayout,
        // que se crea al crear el primer pipeline).
        if (ensureFrameSync())
            m_context = std::make_unique<VKContext>(*this);
    }

    void VKDevice::createInstance()
    {
        // Extensiones de instancia que pide SDL para el surface (Wayland/X11) + debug utils.
        uint32_t nSdlExt = 0;
        const char* const* sdlExt = SDL_Vulkan_GetInstanceExtensions(&nSdlExt);
        std::vector<const char*> exts(sdlExt, sdlExt + nSdlExt);
        std::vector<const char*> layers;

#ifndef NDEBUG
        // Capa de validación (opcional pero clave: queremos VER errores en dev).
        static const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
        if (hasLayer(kValidationLayer))
        {
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            layers.push_back(kValidationLayer);
        }
#endif

        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "haruka-engine";
        app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app.pEngineName = "Haruka Engine";
        app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = (uint32_t)exts.size();
        ci.ppEnabledExtensionNames = exts.data();
        ci.enabledLayerCount = (uint32_t)layers.size();
        ci.ppEnabledLayerNames = layers.data();

        if (vkCreateInstance(&ci, nullptr, &m_instance) != VK_SUCCESS)
            HARUKA_LOGE("RHI/VK", "vkCreateInstance falló");
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCb(
        VkDebugUtilsMessageSeverityFlagBitsEXT sev,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
    {
        if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            HARUKA_LOGE("RHI/VK", "[VVL] %s", data->pMessage);
        return VK_FALSE;
    }

    void VKDevice::createDebugMessenger()
    {
#ifndef NDEBUG
        if (m_debugMessenger) return;
        VkDebugUtilsMessengerCreateInfoEXT d{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        d.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        d.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                      | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        d.pfnUserCallback = debugCb;
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT");
        if (fn) fn(m_instance, &d, nullptr, &m_debugMessenger);
#endif
    }

    // Enumeración con instancia TEMPORAL. No se crea surface: sin ella no se puede saber si la GPU
    // presenta en ESTA ventana, así que `usable` queda en true y la comprobación real la hace
    // `pickPhysicalDevice` al arrancar. Una GPU que no pudiera presentar simplemente no se elegiría,
    // y eso es preferible a montar una surface (y su ventana) solo para poblar un desplegable.
    bool VKDevice::enumerate(SDL_Window* window, std::vector<Device::AdapterInfo>& out)
    {
        (void)window;
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;

        VkInstance inst = VK_NULL_HANDLE;
        if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS) return false;

        uint32_t n = 0;
        vkEnumeratePhysicalDevices(inst, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        if (n) vkEnumeratePhysicalDevices(inst, &n, devs.data());
        for (uint32_t i = 0; i < n; ++i)
        {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devs[i], &p);
            Device::AdapterInfo a;
            a.name     = p.deviceName[0] ? p.deviceName : "(sin nombre)";
            a.discrete = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
            a.usable   = true;
            out.push_back(std::move(a));
        }
        vkDestroyInstance(inst, nullptr);
        return true;
    }

    void VKDevice::pickPhysicalDevice()
    {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(m_instance, &n, nullptr);
        if (n == 0) return;
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(m_instance, &n, devs.data());

        // Selección, en orden:
        //  1) El AJUSTE del juego (`m_preferredGpus`, lista de nombres por preferencia). Es lo que
        //     elige el usuario en el panel de gráficos y se guarda por NOMBRE, no por índice.
        //  2) `HARUKA_VK_GPU=nombre` → sigue funcionando y GANA al ajuste: es la salida de emergencia
        //     para arrancar en otra GPU cuando la guardada no existe o su driver está roto, sin tener
        //     que editar la configuración a ciegas desde una máquina que no arranca.
        //  3) La DISCRETA (dedicada) sobre la integrada.
        //  4) La primera con graphics + present.
        // El present se evalúa contra la surface ya creada de la ventana.
        const char* env = getenv("HARUKA_VK_GPU");
        std::vector<std::string> wants;
        if (env && env[0]) wants.emplace_back(env);
        else               wants = m_preferredGpus;
        const auto toLower = [](unsigned char c) -> char { return (char)std::tolower(c); };

        std::string chosen_name;
        uint32_t    chosen_idx  = UINT32_MAX;
        int         chosen_score = INT32_MIN;

        for (uint32_t di = 0; di < n; ++di)
        {
            VkPhysicalDevice d = devs[di];
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(d, &p);
            const std::string name = p.deviceName ? p.deviceName : "(sin nombre)";

            // Familia graphics + present (contra la surface de la ventana).
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> q(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, q.data());
            uint32_t fam = UINT32_MAX;
            for (uint32_t i = 0; i < qn; ++i)
            {
                VkBool32 present = VK_FALSE;
                if (!m_swapchain || !m_swapchain->surface()) break;
                vkGetPhysicalDeviceSurfaceSupportKHR(d, i, m_swapchain->surface(), &present);
                if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { fam = i; break; }
            }
            if (fam == UINT32_MAX) continue;

            HARUKA_LOGI("RHI/VK", "  GPU [%u] %s (%u graphics+present)", di, name.c_str(), fam);

            // Preferencia automática: discreta >> integrada > el resto. Es también el DESEMPATE
            // cuando ninguna casa con lo pedido, así que un nombre guardado que ya no existe (otra
            // máquina, tarjeta cambiada) degrada a la elección sensata en vez de a la primera que
            // salga.
            int score = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 2
                      : (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) ? 1 : 0;
            if (!wants.empty())
            {
                std::string lower_name(name);
                std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), toLower);
                // La posición en la lista MANDA sobre el tipo de GPU: si el usuario pidió la
                // integrada, se le da la integrada. Cada puesto vale más que cualquier desempate
                // automático, y el primero de la lista más que el segundo (multi-GPU futuro).
                for (size_t wi = 0; wi < wants.size(); ++wi)
                {
                    std::string lw(wants[wi]);
                    std::transform(lw.begin(), lw.end(), lw.begin(), toLower);
                    if (!lw.empty() && lower_name.find(lw) != std::string::npos)
                    {
                        score = 1000 - (int)wi * 10;
                        break;
                    }
                }
            }

            if (score > chosen_score)
            {
                chosen_score = score;
                chosen_idx   = di;
                chosen_name  = name;
            }
        }

        if (chosen_idx != UINT32_MAX)
        {
            m_physical = devs[chosen_idx];
            // Re-derivar la familia graphics+present de la elegida.
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> q(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qn, q.data());
            for (uint32_t i = 0; i < qn; ++i)
            {
                VkBool32 present = VK_FALSE;
                if (!m_swapchain || !m_swapchain->surface()) break;
                vkGetPhysicalDeviceSurfaceSupportKHR(m_physical, i, m_swapchain->surface(), &present);
                if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
                {
                    m_graphicsFamily = i;
                    break;
                }
            }
            HARUKA_LOGI("RHI/VK", "GPU = %s (idx %u, %s)", chosen_name.c_str(), chosen_idx,
                        (env && env[0]) ? "via HARUKA_VK_GPU"
                                        : (m_preferredGpus.empty() ? "automatica" : "ajuste del juego"));
        }
    }

    void VKDevice::createLogicalDevice()
    {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qci.queueFamilyIndex = m_graphicsFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;

        const char* devExt[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        // Fase 4: el descriptor set COMPARTIDO usa PARTIALLY_BOUND en todos los bindings (un slot
        // sin recurso no invalida el set). Es un feature de Vulkan 1.2, hay que pedirlo al crear el
        // device; si el HW no lo soporta, vkCreateDescriptorSetLayout falla y se avisa (glslang y
        // la validación lo verían). El GL fallback sigue disponible.
        VkPhysicalDeviceVulkan12Features v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        v12.descriptorBindingPartiallyBound = VK_TRUE;
        v12.runtimeDescriptorArray = VK_TRUE;   // usado por los arrays de texturas del set compartido

        // Características base del device que el motor pide EXPLÍCITAMENTE: los pipelines de teselado
        // del planeta usan PATCH_LIST + tess control/eval, los samplers usan aniso, y los shaders de
        // terreno usan doubles (Float64). Sin ellas declaradas, la validación lo marca y en NVIDIA crear
        // un pipeline de teselación sin el feature activado puede colgar la GPU (→ TDR → device lost).
        // Se piden tal cual las soportadas para no fallar en HW modesto.
        VkPhysicalDeviceFeatures feats{};
        vkGetPhysicalDeviceFeatures(m_physical, &feats);
        feats.tessellationShader           = feats.tessellationShader && VK_TRUE;
        feats.samplerAnisotropy            = feats.samplerAnisotropy && VK_TRUE;
        feats.shaderFloat64                = feats.shaderFloat64 && VK_TRUE;

        // Cadenar base features + Vulkan 1.2 features bajo VkDeviceCreateInfo.
        VkPhysicalDeviceFeatures2 devFeats{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        devFeats.features = feats;
        devFeats.pNext = &v12;

        VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = devExt;
        dci.pNext = &devFeats;

        if (!vkSuccess(vkCreateDevice(m_physical, &dci, nullptr, &m_device), "vkCreateDevice")) return;
        vkGetDeviceQueue(m_device, m_graphicsFamily, 0, &m_graphicsQueue);

        // Pool de transferencias de UN SOLO uso (staging → device-local).
        VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = m_graphicsFamily;
        if (!vkSuccess(vkCreateCommandPool(m_device, &pci, nullptr, &m_transferPool), "create transfer pool")) return;

        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = m_transferPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!vkSuccess(vkAllocateCommandBuffers(m_device, &ai, &m_transferCmd), "alloc transfer cmd")) return;

        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (!vkSuccess(vkCreateFence(m_device, &fi, nullptr, &m_transferFence), "create transfer fence")) return;
    }

    uint32_t VKDevice::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags props)
    {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(m_physical, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((typeFilter & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
                return i;
        return UINT32_MAX;
    }

    // Crea un buffer host-visible de solo-transferencia (staging) con los datos.
    bool VKDevice::createStagingBuffer(VkBuffer& buffer, VkDeviceMemory& memory, size_t bytes, const void* data)
    {
        if (bytes == 0) return false;
        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = (VkDeviceSize)bytes;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!vkSuccess(vkCreateBuffer(m_device, &bi, nullptr, &buffer), "create staging buffer")) return false;

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, buffer, &mr);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX) { vkDestroyBuffer(m_device, buffer, nullptr); return false; }
        if (!vkSuccess(vkAllocateMemory(m_device, &ai, nullptr, &memory), "alloc staging memory"))
        {
            vkDestroyBuffer(m_device, buffer, nullptr);
            return false;
        }
        vkBindBufferMemory(m_device, buffer, memory, 0);

        if (data)
        {
            void* p = nullptr;
            vkMapMemory(m_device, memory, 0, bytes, 0, &p);
            if (p) { std::memcpy(p, data, bytes); vkUnmapMemory(m_device, memory); }
        }
        return true;
    }

    // Graba `record` en un command buffer de un solo uso, lo envía y ESPERA su fence.
    // Serializa con la queue: correcto y simple para uploads; el N-buffering es de la fase 5.
    bool VKDevice::submitOneShot(const std::function<void(VkCommandBuffer)>& record)
    {
        // Devolvia `false` para CUATRO fallos distintos sin decir cual: con eso, cualquier cosa que
        // dependa de esta funcion (subidas por staging, readback de capturas) falla en silencio y
        // el sintoma aparece muy lejos de la causa. Cada paso dice ahora su VkResult.
        if (!m_device) return false;
        vkResetCommandBuffer(m_transferCmd, 0);
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkResult r = vkBeginCommandBuffer(m_transferCmd, &bi);
        if (r != VK_SUCCESS) { HARUKA_LOGE("RHI/VK", "submitOneShot: vkBeginCommandBuffer = %d", (int)r); return false; }
        record(m_transferCmd);
        r = vkEndCommandBuffer(m_transferCmd);
        if (r != VK_SUCCESS) { HARUKA_LOGE("RHI/VK", "submitOneShot: vkEndCommandBuffer = %d", (int)r); return false; }

        vkResetFences(m_device, 1, &m_transferFence);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &m_transferCmd;
        r = vkQueueSubmit(m_graphicsQueue, 1, &si, m_transferFence);
        if (r != VK_SUCCESS) { HARUKA_LOGE("RHI/VK", "submitOneShot: vkQueueSubmit = %d", (int)r); return false; }
        r = vkWaitForFences(m_device, 1, &m_transferFence, VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS) HARUKA_LOGE("RHI/VK", "submitOneShot: vkWaitForFences = %d", (int)r);
        return r == VK_SUCCESS;
    }

    void VKDevice::uploadViaStaging(const VKBuffer& dst, size_t dstOffset, size_t bytes, const void* data)
    {
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (!createStagingBuffer(staging, mem, bytes, data)) return;

        submitOneShot([&](VkCommandBuffer c) {
            VkBufferCopy r{ 0, (VkDeviceSize)dstOffset, bytes };
            vkCmdCopyBuffer(c, staging, dst.buffer, 1, &r);
        });

        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, mem, nullptr);
    }

    // ------------------------------------------------------------------ render pass del backbuffer
    // Profundidad del backbuffer (imagen owned por el device, recreada al redimensionar). Split en
    // helper para poder recrearla sola en el resize sin tocar el render pass (que no depende del
    // extent). En tiling OPTIMAL + no hosing: los datos se descartan → crearla = dejar en UNDEFINED.
    bool VKDevice::createBackbufferDepth(const VkExtent2D& e)
    {
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_D32_SFLOAT;
        ii.extent = { e.width, e.height, 1 };
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        // DEPTH_STENCIL + SAMPLED: algún pase la muestrea como SHADER_READ (SSAO/tele/etc.), y sin el
        // bit VVL aborta la transición (VUID-01211). En D32 no encarece casi nada.
        ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!vkSuccess(vkCreateImage(m_device, &ii, nullptr, &m_backbufferDepthImage), "create bb depth image")) return false;

        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(m_device, m_backbufferDepthImage, &mr);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX ||
            !vkSuccess(vkAllocateMemory(m_device, &ai, nullptr, &m_backbufferDepthMemory), "alloc bb depth mem"))
            return false;
        vkBindImageMemory(m_device, m_backbufferDepthImage, m_backbufferDepthMemory, 0);

        VkImageViewCreateInfo iv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        iv.image = m_backbufferDepthImage;
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = VK_FORMAT_D32_SFLOAT;
        iv.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (!vkSuccess(vkCreateImageView(m_device, &iv, nullptr, &m_backbufferDepthView), "create bb depth view")) return false;
        return true;
    }

    void VKDevice::createBackbufferResources()
    {
        const VkFormat swapFormat = m_swapchain->format();
        const VkExtent2D e = m_swapchain->extent();
        if (!createBackbufferDepth(e)) return;

        // Render pass: color (swapchain) + depth. REVERSED-Z: loadOp CLEAR, el clear depth lo da el
        // contexto (0.0 = lejano). finalLayout del color = PRESENT (la imagen va al present).
        VkAttachmentDescription atts[2]{};
        atts[0].format = swapFormat;
        atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // finalLayout COLOR_ATTACHMENT_OPTIMAL (NO PRESENT): la UI de ImGui (fase 7) es el último
        // pass sobre el backbuffer y es quien hace la transición final a PRESENT_SRC_KHR. Si no hay
        // UI, el pass de ImGui igual corre (vacío) solo para dejar la imagen lista para presentar.
        atts[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        atts[1].format = VK_FORMAT_D32_SFLOAT;
        atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &colorRef;
        sub.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rp.attachmentCount = 2;
        rp.pAttachments = atts;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 1;
        rp.pDependencies = &dep;
        if (!vkSuccess(vkCreateRenderPass(m_device, &rp, nullptr, &m_backbufferPass), "create bb render pass")) return;

        // ── VARIANTES POR loadOp: LA CAUSA DEL MUNDO NEGRO ──────────────────────────────────────
        //
        // En Vulkan el "¿limpio o conservo?" va HORNEADO en la render pass; en OpenGL es un
        // parámetro de la llamada. El motor abre el pase VARIAS VECES por frame con
        // `clearColor=false` esperando la semántica de GL ("solo ata el FBO, no borres") — el cielo
        // se dibuja, se cierra el pase, se reabre para la escena... y con una única variante CLEAR
        // ese segundo `begin` BORRABA lo ya dibujado. Los draws salían correctos uno a uno en
        // RenderDoc y la pantalla acababa negra, que es exactamente el síntoma que se reportó.
        //
        // Se crean las cuatro combinaciones (color y profundidad son independientes: el pase de
        // máscara de cielo limpia profundidad y CONSERVA color). El índice 3 —limpiar ambos— es el
        // canónico: es el que se usa para compatibilidad de pipelines, porque en Vulkan una render
        // pass es compatible con otra si coinciden formatos y número de attachments, no los loadOp.
        for (int v = 0; v < 4; ++v)
        {
            const bool cCol = (v & 1) != 0;
            const bool cDep = (v & 2) != 0;
            VkAttachmentDescription va[2] = { atts[0], atts[1] };
            va[0].loadOp = cCol ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            va[1].loadOp = cDep ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            // Con LOAD hay que declarar el layout REAL de entrada: UNDEFINED autoriza a descartar el
            // contenido, que es justo lo que se quiere evitar.
            if (!cCol) va[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            if (!cDep) va[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

            VkRenderPassCreateInfo vrp = rp;
            vrp.pAttachments = va;
            if (!vkSuccess(vkCreateRenderPass(m_device, &vrp, nullptr, &m_backbufferPassVariant[v]),
                           "create bb render pass (variante loadOp)"))
                m_backbufferPassVariant[v] = m_backbufferPass;   // degradar antes que quedarse sin pase
        }

        m_swapchain->attachRenderPass(m_backbufferPass);
        m_swapchain->attachBackbufferDepth(m_backbufferDepthView);
        m_swapchain->createFramebuffers();
    }

    void VKDevice::destroyBackbufferDepth()
    {
        if (m_backbufferDepthView) { vkDestroyImageView(m_device, m_backbufferDepthView, nullptr); m_backbufferDepthView = VK_NULL_HANDLE; }
        if (m_backbufferDepthImage) { vkDestroyImage(m_device, m_backbufferDepthImage, nullptr); m_backbufferDepthImage = VK_NULL_HANDLE; }
        if (m_backbufferDepthMemory) { vkFreeMemory(m_device, m_backbufferDepthMemory, nullptr); m_backbufferDepthMemory = VK_NULL_HANDLE; }
    }

    // [RESIZE] Recrea swapchain + profundidad del backbuffer + framebuffers al detectar
    // OUT_OF_DATE/SUBOPTIMAL en beginFrame. El render pass del backbuffer no depende del extent,
    // así que se conserva tal cual. Orden obligatorio: primero la swapchain (para conocer el extent
    // nuevo), luego la profundidad al nuevo tamaño, y SOLO entonces los framebuffers (que referencian
    // tanto la vista de color nueva como la de profundidad nueva y deben tener todas el mismo tamaño).
    // Si la ventana está minimizada (0x0) recreate() omite y valid() queda false → se reintenta en
    // cada beginFrame siguiente, sin dibujar hasta que la ventana vuelva a tener tamaño.
    void VKDevice::onSwapchainResize()
    {
        if (!m_swapchain || m_frameActive) return;
        if (!m_swapchain->recreate()) return;
        destroyBackbufferDepth();
        if (!createBackbufferDepth(m_swapchain->extent())) return;
        m_swapchain->attachRenderPass(m_backbufferPass);
        m_swapchain->attachBackbufferDepth(m_backbufferDepthView);
        m_swapchain->createFramebuffers();
        m_currentImage = 0;
        if (m_imguiActive)
        {
            destroyImguiResources();
            createImguiResources();
        }
    }

    // ------------------------------------------------------------------ ImGui (fase 7)
    // Render pass de un solo color para la UI sobre el backbuffer: LOAD (preserva la escena ya
    // dibujada), STORE, finalLayout PRESENT. No usa profundidad. El pipeline de ImGui se crea
    // contra este pass por el backend (PipelineInfoMain.RenderPass).
    bool VKDevice::createImguiResources()
    {
        if (!m_swapchain || !m_swapchain->valid()) return false;
        const VkFormat fmt = m_swapchain->format();
        const VkExtent2D e = m_swapchain->extent();

        VkAttachmentDescription att{};
        att.format = fmt;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;       // conserva la escena del pass anterior
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;  // sale del pass de la escena
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;             // al present

        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rp.attachmentCount = 1;
        rp.pAttachments = &att;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 1;
        rp.pDependencies = &dep;
        if (!vkSuccess(vkCreateRenderPass(m_device, &rp, nullptr, &m_imguiPass), "create imgui pass")) return false;

        VkImageView attViews[1]{ VK_NULL_HANDLE };
        m_imguiFbs.resize(m_swapchain->imageCount());
        for (uint32_t i = 0; i < m_imguiFbs.size(); ++i)
        {
            attViews[0] = m_swapchain->imageView(i);
            VkFramebufferCreateInfo fb{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fb.renderPass = m_imguiPass;
            fb.attachmentCount = 1;
            fb.pAttachments = attViews;
            fb.width = e.width;
            fb.height = e.height;
            fb.layers = 1;
            if (!vkSuccess(vkCreateFramebuffer(m_device, &fb, nullptr, &m_imguiFbs[i]), "create imgui fb")) return false;
        }
        return true;
    }

    void VKDevice::destroyImguiResources()
    {
        for (auto fb : m_imguiFbs) if (fb) vkDestroyFramebuffer(m_device, fb, nullptr);
        m_imguiFbs.clear();
        if (m_imguiPass) { vkDestroyRenderPass(m_device, m_imguiPass, nullptr); m_imguiPass = VK_NULL_HANDLE; }
    }

    bool VKDevice::initUi()
    {
        if (m_imguiActive || !m_swapchain || !m_swapchain->valid() || !m_device) return false;
        if (!createImguiResources()) return false;

        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_3;                        // la instancia se crea con 1.3
        info.Instance = m_instance;
        info.PhysicalDevice = m_physical;
        info.Device = m_device;
        info.QueueFamily = m_graphicsFamily;
        info.Queue = m_graphicsQueue;
        info.MinImageCount = m_swapchain->minImageCount();
        info.ImageCount = m_swapchain->imageCount();
        info.PipelineInfoMain.RenderPass = m_imguiPass;
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        // Backend interno: crea su propio descriptor pool e inicializa el data del frame.
        info.DescriptorPoolSize = 1000;

        if (!ImGui_ImplVulkan_Init(&info))
        {
            HARUKA_LOGE("RHI/VK", "ImGui_ImplVulkan_Init fallo");
            destroyImguiResources();
            return false;
        }
        m_imguiActive = true;
        HARUKA_LOGI("RHI/VK", "ImGui Vulkan activo (fase 7)");
        return true;
    }

    // Graba el pass de UI en el command buffer del frame: comienza el render pass de ImGui sobre el
    // framebuffer de la imagen actual (que ya contiene la escena), dibuja el draw data y cierra el
    // pass → deja la imagen en PRESENT_SRC_KHR lista para presentar. Se llama SIEMPRE que se use el
    // backbuffer (aunque el draw data esté vacío) para garantizar la transición de layout final.
    void VKDevice::drawImgui(void* drawData, VkCommandBuffer cmd)
    {
        if (!m_imguiActive || !cmd || !m_swapchain || !m_swapchain->valid()) return;
        uint32_t idx = m_currentImage;
        if (idx >= m_imguiFbs.size() || !m_imguiFbs[idx]) return;
        const VkExtent2D e = m_swapchain->extent();

        VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rp.renderPass = m_imguiPass;
        rp.framebuffer = m_imguiFbs[idx];
        rp.renderArea = { {0, 0}, { e.width, e.height } };
        rp.clearValueCount = 0;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

        // Sin invertir: la Y se compensa en la PROYECCIÓN, no en el viewport (ver `VKContext::setViewport`).
        VkViewport vp{ 0.0f, 0.0f, (float)e.width, (float)e.height, 0.0f, 1.0f };
        VkRect2D sc{ {0, 0}, { e.width, e.height } };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        if (drawData) ImGui_ImplVulkan_RenderDrawData((ImDrawData*)drawData, cmd);
        vkCmdEndRenderPass(cmd);
    }

    // Sin UI (ImGui desactivado): transición de layout del backbuffer a PRESENT_SRC_KHR para poder
    // presentar. drawImgui hace el equivalente vía su render pass; esto cubre el caso sin UI.
    void VKDevice::transitionBackbufferToPresent(VkCommandBuffer cmd)
    {
        if (!cmd || !m_swapchain || !m_swapchain->valid()) return;
        uint32_t idx = m_currentImage;
        if (idx >= m_swapchain->imageCount() || !m_swapchain->image(idx)) return;

        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_swapchain->image(idx);
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ------------------------------------------------------------------ buffers
    BufferHandle VKDevice::createBuffer(BufferUsage usage, size_t bytes, const void* data, BufferMemory mem)
    {
        VKBuffer b;
        b.size = bytes;
        b.usage = toUsage(usage) | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (usage == BufferUsage::Indirect) b.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        // NVIDIA rechaza vkAllocateMemory con allocationSize==0 (devuelve OUT_OF_HOST_MEMORY).
        // Un buffer de 0 bytes (se crea vacío y se agranda luego con uploadBuffer, como en GL) debe
        // reservar al menos 1 byte: mantenemos b.size=0 lógicamente y solo el VkBuffer es >=1.
        const VkDeviceSize allocBytes = (bytes == 0) ? 1 : (VkDeviceSize)bytes;
        bi.size = allocBytes;
        bi.usage = b.usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!vkSuccess(vkCreateBuffer(m_device, &bi, nullptr, &b.buffer), "create buffer")) return {};

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, b.buffer, &mr);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = mr.size;

        VkMemoryPropertyFlags props;
        if (mem == BufferMemory::Readback)
            props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        else if (mem == BufferMemory::Static)
            props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        else   // Dynamic / Stream: la CPU escribe cada frame (updateBuffer = memcpy, como glBufferSubData)
            props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, props);
        if (ai.memoryTypeIndex == UINT32_MAX ||
            !vkSuccess(vkAllocateMemory(m_device, &ai, nullptr, &b.memory), "alloc buffer memory"))
        {
            HARUKA_LOGE("RHI/VK", "createBuffer FALLÓ: bytes=%zu usage=%d mem=%d memTypeIndex=%u mr.size=%llu",
                        bytes, (int)usage, (int)mem, ai.memoryTypeIndex,
                        (unsigned long long)mr.size);
            vkDestroyBuffer(m_device, b.buffer, nullptr);
            return {};
        }
        vkBindBufferMemory(m_device, b.buffer, b.memory, 0);

        b.hostVisible = (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        if (b.hostVisible && bytes > 0)
        {
            vkMapMemory(m_device, b.memory, 0, bytes, 0, &b.mapped);
            if (data && bytes) std::memcpy(b.mapped, data, bytes);
        }
        else if (data && bytes)
        {
            uploadViaStaging(b, 0, bytes, data);
        }

        return BufferHandle{ alloc(m_buffers, m_freeBuffers, b) };
    }

    void VKDevice::updateBuffer(BufferHandle h, size_t offset, size_t bytes, const void* data)
    {
        const VKBuffer* b = buffer(h);
        if (!b || !data || bytes == 0) return;
        if (b->mapped) std::memcpy((char*)b->mapped + offset, data, bytes);
        else           uploadViaStaging(*b, offset, bytes, data);
    }

    void VKDevice::uploadBuffer(BufferHandle h, size_t bytes, const void* data)
    {
        // Stream: MUTABLE y reasignable (glBufferData). Vulkan es fijo en creación → recreamos el
        // VkBuffer con el nuevo tamaño. El handle NO cambia: la tabla apunta al objeto nuevo.
        if (h.id == 0 || h.id > m_buffers.size()) return;
        VKBuffer& b = m_buffers[h.id - 1];
        VkBuffer newBuf = VK_NULL_HANDLE;
        VkDeviceMemory newMem = VK_NULL_HANDLE;

        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = (VkDeviceSize)bytes;
        bi.usage = b.usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!vkSuccess(vkCreateBuffer(m_device, &bi, nullptr, &newBuf), "realloc buffer")) return;

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, newBuf, &mr);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || !vkSuccess(vkAllocateMemory(m_device, &ai, nullptr, &newMem), "realloc buffer mem"))
        {
            vkDestroyBuffer(m_device, newBuf, nullptr);
            return;
        }
        vkBindBufferMemory(m_device, newBuf, newMem, 0);

        void* p = nullptr;
        vkMapMemory(m_device, newMem, 0, bytes, 0, &p);
        if (p && data && bytes) std::memcpy(p, data, bytes);

        vkDestroyBuffer(m_device, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(m_device, b.memory, nullptr);
        b.buffer = newBuf;
        b.memory = newMem;
        b.mapped = p;
        b.size = bytes;
        b.hostVisible = true;
    }

    void VKDevice::copyBuffer(BufferHandle src, BufferHandle dst, size_t srcOff, size_t dstOff, size_t bytes)
    {
        const VKBuffer* s = buffer(src);
        const VKBuffer* d = buffer(dst);
        if (!s || !d || bytes == 0) return;
        submitOneShot([&](VkCommandBuffer c) {
            VkBufferCopy r{ (VkDeviceSize)srcOff, (VkDeviceSize)dstOff, bytes };
            vkCmdCopyBuffer(c, s->buffer, d->buffer, 1, &r);
        });
    }

    const void* VKDevice::mappedData(BufferHandle h)
    {
        const VKBuffer* b = buffer(h);
        return b ? b->mapped : nullptr;
    }

    // ------------------------------------------------------------------ texturas
    TextureHandle VKDevice::createTexture(const TextureDesc& d)
    {
        VKTexture t;
        t.format = texFmt(d.format);
        t.width = d.width;
        t.height = d.height;
        t.cube = d.cube;
        t.array = (d.layers > 1) && !d.cube;
        t.layers = d.cube ? 6 : std::max(1u, d.layers);
        t.mipLevels = (d.mipmaps && d.width && d.height) ? (uint32_t)(1 + std::floor(std::log2((float)std::max(d.width, d.height)))) : 1;
        // ⚠️ Y SOLO SI EL FORMATO ADMITE BLIT LINEAL. Si no, se anuncia UN nivel: mas vale perder el
        // filtrado por distancia que reservar niveles que nadie va a rellenar — que es justo el bug
        // que esto viene a cerrar (ver `generateMips`).
        if (t.mipLevels > 1)
        {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(m_physical, t.format, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
                !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
                !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
            {
                HARUKA_LOGW("RHI/VK", "el formato %d no admite blit lineal: se crea SIN mips "
                            "(perdemos filtrado a distancia, pero no se muestrea basura)", (int)t.format);
                t.mipLevels = 1;
            }
        }

        const bool isDepth = (d.format == Format::D24 || d.format == Format::D24S8 || d.format == Format::D32F);

        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = t.format;
        ii.extent = { d.width, d.height, 1 };
        ii.mipLevels = t.mipLevels;
        ii.arrayLayers = t.layers;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        // ⚠️ TRANSFER_SRC ademas de DST: los mips se generan con un blit de cada nivel al siguiente,
        // asi que la propia imagen es ORIGEN. Sin este bit la cadena de abajo es ilegal.
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                 | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (d.renderTarget) ii.usage |= isDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (d.cube) ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (!vkSuccess(vkCreateImage(m_device, &ii, nullptr, &t.image), "create image")) return {};

        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(m_device, t.image, &mr);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || !vkSuccess(vkAllocateMemory(m_device, &ai, nullptr, &t.memory), "alloc image mem"))
        {
            vkDestroyImage(m_device, t.image, nullptr);
            return {};
        }
        vkBindImageMemory(m_device, t.image, t.memory, 0);

        // View. Cubemap = vista cubo; array = vista 2D array; resto 2D.
        VkImageViewCreateInfo iv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        iv.image = t.image;
        iv.viewType = d.cube ? VK_IMAGE_VIEW_TYPE_CUBE
                             : (t.array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        iv.format = t.format;
        iv.subresourceRange = { (VkImageAspectFlags)(isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, t.mipLevels, 0, t.layers };
        if (!vkSuccess(vkCreateImageView(m_device, &iv, nullptr, &t.view), "create image view"))
        {
            vkDestroyImage(m_device, t.image, nullptr);
            vkFreeMemory(m_device, t.memory, nullptr);
            return {};
        }

        // Sampler propio (equivalente a los parámetros de la textura en GL; bindTexture con sampler
        // vacío usará ESTE).
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = toFilter(d.filter);
        si.minFilter = toFilter(d.filter);
        si.mipmapMode = (d.mipmaps && d.filter == Filter::Linear) ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                  : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = toWrap(d.wrap);
        si.addressModeV = toWrap(d.wrap);
        si.addressModeW = toWrap(d.wrap);
        si.mipLodBias = d.lodBias;
        // ⚠️ PRUEBA: `HARUKA_VK_MIP0=1` capa el muestreo al nivel 0. Los mips 1..N se RESERVAN aqui
        // pero NADIE los escribe (no hay `vkCmdBlitImage` ni ningun `baseMipLevel > 0` en todo el
        // backend), mientras OpenGL llama a `glGenerateTextureMipmap`. Si con esto los props lejanos
        // dejan de salir blancos, el fallo es ese: se esta muestreando memoria indefinida.
        { static const bool s_mip0 = [] { const char* e = std::getenv("HARUKA_VK_MIP0");
                                          return e && e[0] == '1'; }();
          si.maxLod = s_mip0 ? 0.0f : (float)t.mipLevels; }
        if (d.maxAnisotropy > 1.0f)
        {
            VkPhysicalDeviceProperties pp;
            vkGetPhysicalDeviceProperties(m_physical, &pp);
            si.anisotropyEnable = VK_TRUE;
            si.maxAnisotropy = std::min(d.maxAnisotropy, pp.limits.maxSamplerAnisotropy);
        }
        if (!vkSuccess(vkCreateSampler(m_device, &si, nullptr, &t.sampler), "create sampler"))
        {
            vkDestroyImageView(m_device, t.view, nullptr);
            vkDestroyImage(m_device, t.image, nullptr);
            vkFreeMemory(m_device, t.memory, nullptr);
            return {};
        }

        if (d.initialData)
        {
            // Alinea filas a 1 byte y sube capas CONTIGUAS (igual que GL_UNPACK_ALIGNMENT=1).
            const size_t bpp = bytesPerPixel(d.format);
            const size_t rowBytes = (size_t)d.width * bpp;
            const size_t total = rowBytes * d.height * t.layers;
            if (bpp && total)
            {
                VkBuffer staging = VK_NULL_HANDLE;
                VkDeviceMemory mem = VK_NULL_HANDLE;
                if (createStagingBuffer(staging, mem, total, d.initialData))
                {
                    submitOneShot([&](VkCommandBuffer c) {
                        VkImageMemoryBarrier b0{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                        b0.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        b0.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        b0.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        b0.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        b0.image = t.image;
                        b0.subresourceRange = { (VkImageAspectFlags)(isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                                                0, t.mipLevels, 0, t.layers };
                        b0.srcAccessMask = 0;
                        b0.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             0, 0, nullptr, 0, nullptr, 1, &b0);

                        VkBufferImageCopy r{};
                        r.bufferOffset = 0;
                        r.bufferRowLength = 0;   // datos apretados (rowBytes = width*bpp)
                        r.bufferImageHeight = 0;
                        r.imageSubresource = { (VkImageAspectFlags)(isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                                               0, 0, t.layers };
                        r.imageExtent = { d.width, d.height, 1 };
                        vkCmdCopyBufferToImage(c, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);

                        // ── LOS MIPS SE GENERAN AQUI, Y ANTES NO SE GENERABAN EN ABSOLUTO ──────
                        //
                        // ⚠️ ESTE ERA EL BUG QUE SE VEIA COMO "los arboles lejanos salen BLANCOS, y
                        // solo en Vulkan". Se reservaban `mipLevels` niveles, el muestreador los
                        // permitia (`maxLod = mipLevels`) y **nadie escribia ninguno**: memoria
                        // INDEFINIDA. Como el nivel lo elige la DISTANCIA, de cerca se veia bien
                        // (nivel 0, el unico subido) y de lejos salia basura. OpenGL no lo sufria
                        // porque llama a `glGenerateTextureMipmap`.
                        //
                        // Confirmado capando el muestreo al nivel 0 (`HARUKA_VK_MIP0=1`), color medio
                        // de los arboles lejanos:
                        //     Vulkan normal          123,1 121,8 129,2   <- verde el canal MAS BAJO
                        //     Vulkan capado a mip 0  128,4 134,1 122,1
                        //     OpenGL (referencia)    125,8 130,5 118,2
                        // Con el tope, Vulkan reproduce OpenGL.
                        //
                        // Afecta a todo lo que pide `mipmaps`: props, las cuatro capas de terreno, el
                        // IBL y las texturas procedurales.
                        //
                        // La cadena es la estandar: cada nivel se blitea del anterior a la mitad de
                        // tamano, con el origen en TRANSFER_SRC y el destino en TRANSFER_DST, y cada
                        // nivel ya consumido pasa a SHADER_READ_ONLY. `layerCount` cubre los arrays
                        // de una vez (el terreno son 4 capas).
                        if (t.mipLevels > 1 && !isDepth)
                        {
                            int32_t mw = (int32_t)d.width, mh = (int32_t)d.height;
                            for (uint32_t level = 1; level < t.mipLevels; ++level)
                            {
                                VkImageMemoryBarrier bs{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                                bs.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                bs.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                                bs.image = t.image;
                                bs.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1, 0, t.layers };
                                bs.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                                bs.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                                bs.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                                bs.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                                vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                     0, 0, nullptr, 0, nullptr, 1, &bs);

                                const int32_t nw = (mw > 1) ? mw / 2 : 1;
                                const int32_t nh = (mh > 1) ? mh / 2 : 1;
                                VkImageBlit bl{};
                                bl.srcOffsets[0] = { 0, 0, 0 };
                                bl.srcOffsets[1] = { mw, mh, 1 };
                                bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, t.layers };
                                bl.dstOffsets[0] = { 0, 0, 0 };
                                bl.dstOffsets[1] = { nw, nh, 1 };
                                bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, t.layers };
                                vkCmdBlitImage(c,
                                               t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                               t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                               1, &bl, VK_FILTER_LINEAR);

                                // el nivel que acaba de servir de origen ya no se toca mas
                                VkImageMemoryBarrier br = bs;
                                br.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                                br.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                                br.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                                br.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                                vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                     0, 0, nullptr, 0, nullptr, 1, &br);
                                mw = nw; mh = nh;
                            }
                            // el ULTIMO nivel nunca fue origen: sigue en TRANSFER_DST
                            VkImageMemoryBarrier bl1{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                            bl1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            bl1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            bl1.image = t.image;
                            bl1.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, t.mipLevels - 1, 1, 0, t.layers };
                            bl1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                            bl1.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                            bl1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                            bl1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                 0, 0, nullptr, 0, nullptr, 1, &bl1);
                        }
                        else
                        {
                        VkImageMemoryBarrier b1 = b0;
                        b1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        b1.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        b1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                             0, 0, nullptr, 0, nullptr, 1, &b1);
                        }
                    });
                    vkDestroyBuffer(m_device, staging, nullptr);
                    vkFreeMemory(m_device, mem, nullptr);
                }
            }
        }

        return TextureHandle{ alloc(m_textures, m_freeTextures, t) };
    }

    void VKDevice::updateCubemapFace(TextureHandle th, int face, int width, int height,
                                     Format format, const void* data)
    {
        const VKTexture* t = texture(th);
        if (!t || !data) return;
        const size_t bpp = bytesPerPixel(format);
        if (!bpp) return;
        const size_t total = (size_t)width * height * bpp;

        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (!createStagingBuffer(staging, mem, total, data)) return;

        submitOneShot([&](VkCommandBuffer c) {
            VkImageMemoryBarrier b0{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b0.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b0.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b0.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b0.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b0.image = t->image;
            b0.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, (uint32_t)face, 1 };
            b0.srcAccessMask = 0;
            b0.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b0);

            VkBufferImageCopy r{};
            r.bufferOffset = 0;
            r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)face, 1 };
            r.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
            vkCmdCopyBufferToImage(c, staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);

            VkImageMemoryBarrier b1 = b0;
            b1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b1.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b1.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b1.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b1);
        });

        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, mem, nullptr);
    }

    SamplerHandle VKDevice::createSampler(const SamplerDesc& d)
    {
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = toFilter(d.filter);
        si.minFilter = toFilter(d.filter);
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = toWrap(d.wrap);
        si.addressModeV = toWrap(d.wrap);
        si.addressModeW = toWrap(d.wrap);
        if (d.maxAnisotropy > 1.0f)
        {
            VkPhysicalDeviceProperties pp;
            vkGetPhysicalDeviceProperties(m_physical, &pp);
            si.anisotropyEnable = VK_TRUE;
            si.maxAnisotropy = std::min(d.maxAnisotropy, pp.limits.maxSamplerAnisotropy);
        }
        VKSampler s;
        if (!vkSuccess(vkCreateSampler(m_device, &si, nullptr, &s.id), "create sampler")) return {};
        return SamplerHandle{ alloc(m_samplers, m_freeSamplers, s) };
    }

    // ------------------------------------------------------------------ render targets
    std::string VKDevice::renderPassKey(const RenderTargetDesc& d) const
    {
        std::string k;
        for (Format f : d.colorFormats) k += std::to_string((int)f) + ",";
        k += "|" + std::to_string((int)d.hasDepth) + "," + std::to_string((int)d.depthFormat)
           + "," + std::to_string((int)d.depthCube);
        return k;
    }

    // `loadVariant`: (clearColor ? 1 : 0) | (clearDepth ? 2 : 0). Ver la nota larga en
    // `createBackbufferResources`: en Vulkan el "¿limpio o conservo?" va horneado en la render pass,
    // y con una sola variante (CLEAR) cada `beginRenderPass` borraba lo ya dibujado.
    VkRenderPass VKDevice::renderPassFor(const RenderTargetDesc& d, int loadVariant)
    {
        const std::string key = renderPassKey(d) + "|L" + std::to_string(loadVariant);
        auto it = m_renderPasses.find(key);
        if (it != m_renderPasses.end()) return it->second;

        std::vector<VkAttachmentDescription> atts;
        std::vector<VkAttachmentReference> colorRefs;
        VkAttachmentReference depthRef{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

        for (size_t i = 0; i < d.colorFormats.size(); ++i)
        {
            VkAttachmentDescription a{};
            const bool clearCol = (loadVariant & 1) != 0;
            a.format = texFmt(d.colorFormats[i]);
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = clearCol ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            // Con LOAD hay que declarar el layout REAL: UNDEFINED autoriza a descartar el contenido.
            a.initialLayout = clearCol ? VK_IMAGE_LAYOUT_UNDEFINED
                                       : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            atts.push_back(a);
            colorRefs.push_back({ (uint32_t)i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
        }

        uint32_t depthIdx = VK_ATTACHMENT_UNUSED;
        if (d.hasDepth)
        {
            depthIdx = (uint32_t)atts.size();
            VkAttachmentDescription a{};
            const bool clearDep = (loadVariant & 2) != 0;
            a.format = texFmt(d.depthFormat);
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = clearDep ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = clearDep ? VK_IMAGE_LAYOUT_UNDEFINED
                                       : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            atts.push_back(a);
            depthRef = { depthIdx, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        }

        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = (uint32_t)colorRefs.size();
        sub.pColorAttachments = colorRefs.empty() ? nullptr : colorRefs.data();
        sub.pDepthStencilAttachment = d.hasDepth ? &depthRef : nullptr;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rp.attachmentCount = (uint32_t)atts.size();
        rp.pAttachments = atts.empty() ? nullptr : atts.data();
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 1;
        rp.pDependencies = &dep;

        VkRenderPass pass = VK_NULL_HANDLE;
        if (!vkSuccess(vkCreateRenderPass(m_device, &rp, nullptr, &pass), "create render pass")) return VK_NULL_HANDLE;
        m_renderPasses[key] = pass;
        return pass;
    }

    RenderPassHandle VKDevice::createRenderTarget(const RenderTargetDesc& d)
    {
        VKRenderTarget rt;
        rt.width = d.width;
        rt.height = d.height;
        rt.desc = d;                       // para pedir variantes de loadOp después
        rt.renderPass = renderPassFor(d);  // canónica (limpiar ambos): compatibilidad de pipelines
        if (!rt.renderPass) return {};

        // Color attachments (MRT). Cada uno es una textura muestreable.
        std::vector<VkImageView> views;
        for (size_t i = 0; i < d.colorFormats.size(); ++i)
        {
            TextureDesc cd;
            cd.width = d.width; cd.height = d.height; cd.format = d.colorFormats[i];
            cd.renderTarget = true; cd.filter = d.colorFilter; cd.wrap = Wrap::ClampToEdge;
            TextureHandle th = createTexture(cd);
            if (!valid(th)) { for (TextureHandle c : rt.colors) destroy(c); return {}; }
            rt.colors.push_back(th);
            if (const VKTexture* t = texture(th)) views.push_back(t->view);
        }

        if (d.hasDepth)
        {
            const bool samplable = d.depthAsTexture || d.depthCube;
            VKTexture dt;
            dt.format = texFmt(d.depthFormat);
            dt.width = d.width;
            dt.height = d.height;
            dt.cube = d.depthCube;
            dt.layers = d.depthCube ? 6 : 1;
            dt.mipLevels = 1;

            VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = dt.format;
            ii.extent = { d.width, d.height, 1 };
            ii.mipLevels = 1;
            ii.arrayLayers = dt.layers;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            if (d.depthCube) ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!vkSuccess(vkCreateImage(m_device, &ii, nullptr, &dt.image), "create depth image"))
            { for (TextureHandle c : rt.colors) destroy(c); return {}; }

            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(m_device, dt.image, &mr);
            VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (ai.memoryTypeIndex == UINT32_MAX || !vkSuccess(vkAllocateMemory(m_device, &ai, nullptr, &dt.memory), "alloc depth mem"))
            { vkDestroyImage(m_device, dt.image, nullptr); for (TextureHandle c : rt.colors) destroy(c); return {}; }
            vkBindImageMemory(m_device, dt.image, dt.memory, 0);

            VkImageViewCreateInfo iv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            iv.image = dt.image;
            iv.viewType = d.depthCube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
            iv.format = dt.format;
            iv.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, dt.layers };
            if (!vkSuccess(vkCreateImageView(m_device, &iv, nullptr, &dt.view), "create depth view"))
            { vkDestroyImage(m_device, dt.image, nullptr); vkFreeMemory(m_device, dt.memory, nullptr); for (TextureHandle c : rt.colors) destroy(c); return {}; }

            // Sampler de sombra / PCF cuando hace falta (CLAMP_TO_BORDER blanco = fuera del mapa = sin sombra).
            if (samplable)
            {
                VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                si.magFilter = toFilter(d.depthFilter);
                si.minFilter = toFilter(d.depthFilter);
                si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                si.addressModeU = d.depthBorderClamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER
                                                     : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                si.addressModeV = si.addressModeU;
                si.addressModeW = si.addressModeU;
                si.mipLodBias = 0.0f;
                si.maxLod = 1.0f;
                if (d.depthCompare)
                {
                    si.compareEnable = VK_TRUE;
                    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;   // shadow map (PCF)
                }
                if (d.depthBorderClamp)
                {
                    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
                }
                if (!vkSuccess(vkCreateSampler(m_device, &si, nullptr, &dt.sampler), "create depth sampler"))
                { vkDestroyImageView(m_device, dt.view, nullptr); vkDestroyImage(m_device, dt.image, nullptr); vkFreeMemory(m_device, dt.memory, nullptr); for (TextureHandle c : rt.colors) destroy(c); return {}; }
            }

            // La profundidad como textura muestreable se registra en la tabla (owned); como
            // "renderbuffer" el target la posee directa (la destruye destroy(RenderPassHandle)).
            if (samplable)
            {
                rt.depthTex = TextureHandle{ alloc(m_textures, m_freeTextures, dt) };
                views.push_back(dt.view);
            }
            else
            {
                rt.depthImage = dt.image;
                rt.depthMemory = dt.memory;
                rt.depthView = dt.view;
                views.push_back(dt.view);
            }
        }

        // Un cubemap depth (shadow map puntual) son 6 layers → el framebuffer debe cubrirlos.
        VkFramebufferCreateInfo fb{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fb.renderPass = rt.renderPass;
        fb.attachmentCount = (uint32_t)views.size();
        fb.pAttachments = views.data();
        fb.width = d.width;
        fb.height = d.height;
        fb.layers = d.depthCube ? 6 : 1;
        if (!vkSuccess(vkCreateFramebuffer(m_device, &fb, nullptr, &rt.framebuffer), "create framebuffer"))
        {
            for (TextureHandle c : rt.colors) destroy(c);
            if (RHI::valid(rt.depthTex)) destroy(rt.depthTex);
            if (rt.depthView) vkDestroyImageView(m_device, rt.depthView, nullptr);
            if (rt.depthImage) vkDestroyImage(m_device, rt.depthImage, nullptr);
            if (rt.depthMemory) vkFreeMemory(m_device, rt.depthMemory, nullptr);
            return {};
        }

        return RenderPassHandle{ alloc(m_targets, m_freeTargets, rt) };
    }

    TextureHandle VKDevice::getColorTexture(RenderPassHandle h, uint32_t index)
    {
        const VKRenderTarget* rt = renderTarget(h);
        return (rt && index < rt->colors.size()) ? rt->colors[index] : TextureHandle{};
    }

    TextureHandle VKDevice::getDepthTexture(RenderPassHandle h)
    {
        const VKRenderTarget* rt = renderTarget(h);
        return rt ? rt->depthTex : TextureHandle{};
    }

    // En Vulkan el backbuffer lo crea ESTE motor, no SDL: la profundidad del swapchain se declara
    // `VK_FORMAT_D32_SFLOAT` (ver la creación de la imagen de profundidad y del render pass), así que
    // no hay nada que preguntarle al driver. Si algún día se elige otro formato allí, este valor tiene
    // que seguirlo o los blits de profundidad contra pantalla volverán a ser ilegales.
    Format VKDevice::backbufferDepthFormat()
    {
        return Format::D32F;
    }

    // ------------------------------------------------------------------ escapes nativos
    // En Vulkan NO existe el GLuint: son escotillas de la migración que solo tenían sentido en GL
    // (ImGui GL, glBind* directo). Devuelven 0. Los llamadores que dependan de estos ids hay que
    // portarlos a slots RHI puros (ver PLAN_VULKAN.md §6.4).
    uint32_t VKDevice::nativeTexture(TextureHandle)      { return 0; }
    uint32_t VKDevice::nativeFramebuffer(RenderPassHandle) { return 0; }
    uint32_t VKDevice::nativeProgram(PipelineHandle)     { return 0; }
    uint32_t VKDevice::nativeBuffer(BufferHandle)        { return 0; }

    uint64_t VKDevice::imguiTextureId(TextureHandle h)
    {
        // ImGui_ImplVulkan quiere un VkDescriptorSet, no un id de imagen. Se crea una vez por
        // textura y se guarda: AddTexture consume un slot del pool interno del backend (1000).
        if (!m_imguiActive) return 0;                    // aun no hay pool: el llamante reintentara
        const VKTexture* t = texture(h);
        if (!t || !t->view || !t->sampler) return 0;

        auto it = m_imguiTextures.find(h.id);
        if (it != m_imguiTextures.end()) return (uint64_t)it->second;

        VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(
            t->sampler, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!ds) { HARUKA_LOGW("RHI/VK", "ImGui_ImplVulkan_AddTexture fallo"); return 0; }
        m_imguiTextures.emplace(h.id, ds);
        return (uint64_t)ds;
    }

    // ------------------------------------------------------------------ destrucción
    void VKDevice::destroy(BufferHandle h)
    {
        if (const VKBuffer* b = buffer(h))
        {
            // Fuera del estado pegajoso ANTES de destruirlo (ver VKContext::forgetBuffer).
            if (m_context && b->buffer) m_context->forgetBuffer(b->buffer);
            if (b->buffer) vkDestroyBuffer(m_device, b->buffer, nullptr);
            if (b->memory) vkFreeMemory(m_device, b->memory, nullptr);
            release(m_buffers, m_freeBuffers, h.id);
        }
    }

    void VKDevice::destroy(TextureHandle h)
    {
        if (const VKTexture* t = texture(h))
        {
            // El descriptor set de ImGui apunta a este view/sampler: soltarlo ANTES de destruirlos.
            if (auto it = m_imguiTextures.find(h.id); it != m_imguiTextures.end())
            {
                if (m_imguiActive) ImGui_ImplVulkan_RemoveTexture(it->second);
                m_imguiTextures.erase(it);
            }
            if (m_context && t->view) m_context->forgetImageView(t->view);
            if (t->sampler) vkDestroySampler(m_device, t->sampler, nullptr);
            if (t->view) vkDestroyImageView(m_device, t->view, nullptr);
            if (t->image) vkDestroyImage(m_device, t->image, nullptr);
            if (t->memory) vkFreeMemory(m_device, t->memory, nullptr);
            release(m_textures, m_freeTextures, h.id);
        }
    }

    void VKDevice::destroy(SamplerHandle h)
    {
        if (const VKSampler* s = sampler(h))
        {
            if (s->id) vkDestroySampler(m_device, s->id, nullptr);
            release(m_samplers, m_freeSamplers, h.id);
        }
    }

    void VKDevice::destroy(PipelineHandle h)
    {
        if (const VKPipeline* p = pipeline(h))
        {
            if (p->handle) vkDestroyPipeline(m_device, p->handle, nullptr);
            // Variantes (fase 5): pipielines que viven en m_pipelineVariants bajo la key "id:pass".
            const std::string prefix = std::to_string(h.id) + ":";
            for (auto it = m_pipelineVariants.begin(); it != m_pipelineVariants.end(); )
            {
                if (it->first.compare(0, prefix.size(), prefix) == 0)
                {
                    if (it->second) vkDestroyPipeline(m_device, it->second, nullptr);
                    it = m_pipelineVariants.erase(it);
                }
                else ++it;
            }
            // El pipeline layout es COMPARTIDO por todos los pipelines: se libera en el dtor del
            // device (m_pipelineLayout), no aquí.
            release(m_pipelines, m_freePipelines, h.id);
        }
    }

    void VKDevice::destroy(RenderPassHandle h)
    {
        if (const VKRenderTarget* rt = renderTarget(h))
        {
            for (TextureHandle c : rt->colors) destroy(c);
            if (RHI::valid(rt->depthTex)) destroy(rt->depthTex);
            if (rt->depthView) vkDestroyImageView(m_device, rt->depthView, nullptr);
            if (rt->depthImage) vkDestroyImage(m_device, rt->depthImage, nullptr);
            if (rt->depthMemory) vkFreeMemory(m_device, rt->depthMemory, nullptr);
            if (rt->framebuffer) vkDestroyFramebuffer(m_device, rt->framebuffer, nullptr);
            // El render pass es CACHEADO por el device (no owned por el target): se libera en el dtor.
            release(m_targets, m_freeTargets, h.id);
        }
    }

    // ------------------------------------------------------------------ pipelines
    // Fase 4: la compilación GLSL→SPIR-V (glslang + shifts de binding), el descriptor set layout
    // COMPARTIDO y la construcción de los pipelines viven en vk_pipeline.{h,cpp}. Aquí SOLO se
    // cargan/compilan las etapas y se delega, siempre contra el pipeline layout compartido.
    // ⚠️ INSTRUMENTO, NO ADORNO: aqui es donde puede estar el segundo de arranque.
    //
    // Crear un pipeline hace dos cosas caras y muy distintas: traducir GLSL a SPIR-V (que ya se
    // cachea en disco, ver `compileGlslToSpv`) y que el DRIVER convierta ese SPIR-V en codigo
    // maquina. Lo segundo es por fabricante y no se cachea en la aplicacion: no hay `VkPipelineCache`
    // ni binarios de programa de GL en este motor. Andoni reporta ~1000 ms al alternar AMD/NVIDIA y
    // adivinar cual de los dos es sale caro, asi que se MIDE y se avisa de los que pasen del umbral.
    PipelineHandle VKDevice::createPipeline(const PipelineDesc& d)
    {
        const auto t0_pipe = std::chrono::steady_clock::now();
        struct PipeTimer {
            std::chrono::steady_clock::time_point t0; const PipelineDesc* d;
            ~PipeTimer() {
                const double ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t0).count();
                static double s_total = 0.0; static int s_count = 0;
                s_total += ms; ++s_count;
                if (ms > 20.0)
                    HARUKA_LOGI("RHI/VK", "pipeline LENTO: %.0f ms  (%s | %s | %s)  · acumulado %.0f ms en %d",
                                ms, d->vertexPath ? d->vertexPath : "-",
                                d->fragmentPath ? d->fragmentPath : "-",
                                d->computePath ? d->computePath : "-", s_total, s_count);
            }
        } pipeTimer{ t0_pipe, &d };

        // Crea UNA vez el set + layout compartidos (bindings UBO 0..31 / SSBO 32..63 / textura
        // 64..95 de PLAN_VULKAN.md §4.3). Vive hasta el dtor del device.
        if (!m_pipelineLayout)
        {
            if (!m_setLayout)
                m_setLayout = createSharedSetLayout(m_device);
            if (m_setLayout)
                m_pipelineLayout = createSharedPipelineLayout(m_device, m_setLayout);
            if (!m_pipelineLayout)
            {
                HARUKA_LOGE("RHI/VK", "createPipeline: layout compartido no disponible (¿descriptorIndexing no soportado?)");
                return {};
            }
        }

        VKPipeline p;
        p.layout = m_pipelineLayout;
        // Guardamos la descripción original + topología para poder RE-COMPILAR variantes contra
        // otros render passes (fase 5, targets offscreen) a partir del SPV ya compilado.
        p.desc = d;
        p.topology = d.topology;
        p.patchVertices = (d.topology == PrimitiveTopology::Patches) ? d.patchVertices : 0;
        p.bindingCount = (uint32_t)std::min<size_t>(d.vertexLayout.strides.size(), 8);

        std::vector<std::vector<uint32_t>> spv(kStageCount);
        const bool isCompute = d.computePath || d.computeSource || d.spirvCompute;
        if (isCompute)
        {
            spv[kStageComp] = loadStageSpv(d.computePath, d.computeSource,
                                           d.spirvCompute, d.spirvComputeSize,
                                           VK_SHADER_STAGE_COMPUTE_BIT);
            p.handle = buildComputePipeline(m_device, spv[kStageComp], m_pipelineLayout);
        }
        else
        {
            spv[kStageVert] = loadStageSpv(d.vertexPath, d.vertexSource,
                                           d.spirvVertex, d.spirvVertexSize,
                                           VK_SHADER_STAGE_VERTEX_BIT);
            spv[kStageFrag] = loadStageSpv(d.fragmentPath, d.fragmentSource,
                                           d.spirvFragment, d.spirvFragmentSize,
                                           VK_SHADER_STAGE_FRAGMENT_BIT);
            spv[kStageGeom] = loadStageSpv(d.geometryPath, nullptr,
                                           d.spirvGeometry, d.spirvGeometrySize,
                                           VK_SHADER_STAGE_GEOMETRY_BIT);
            spv[kStageTesc] = loadStageSpv(d.tessControlPath, d.tessControlSource, nullptr, 0,
                                           VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
            spv[kStageTese] = loadStageSpv(d.tessEvalPath, d.tessEvalSource, nullptr, 0,
                                           VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
            // Diagnóstico: qué etapa(s) faltan y de qué pipeline (rutas o inline).
            if (spv[kStageVert].empty())
                HARUKA_LOGE("RHI/VK", "pipeline sin vertex: %s", d.vertexPath ? d.vertexPath : "(inline)");
            if (spv[kStageFrag].empty())
                HARUKA_LOGE("RHI/VK", "pipeline sin fragment: %s", d.fragmentPath ? d.fragmentPath : "(inline)");
            if (d.geometryPath && spv[kStageGeom].empty())
                HARUKA_LOGE("RHI/VK", "pipeline sin geometry: %s", d.geometryPath);
            if ((d.tessControlPath || d.tessControlSource) && spv[kStageTesc].empty())
                HARUKA_LOGE("RHI/VK", "pipeline sin tesc: %s", d.tessControlPath ? d.tessControlPath : "(inline)");
            if ((d.tessEvalPath || d.tessEvalSource) && spv[kStageTese].empty())
                HARUKA_LOGE("RHI/VK", "pipeline sin tese: %s", d.tessEvalPath ? d.tessEvalPath : "(inline)");
            // El pipeline de esta fase se crea contra el render pass del backbuffer; el vínculo
            // pipeline↔target específico (variantes por render pass) se resuelve en la fase 5.
            p.handle = buildGraphicsPipeline(m_device, d, spv, m_pipelineLayout, m_backbufferPass);
        }

        if (!p.handle)
        {
            HARUKA_LOGW("RHI/VK", "createPipeline: fallo al crear pipeline (ver mensajes anteriores)");
            return {};
        }

        p.compute = isCompute;
        p.spv = std::move(spv);
        return PipelineHandle{ alloc(m_pipelines, m_freePipelines, p) };
    }

    // ------------------------------------------------------------------ frame (fase 5)
    bool VKDevice::ensureFrameSync()
    {
        if (m_frameCmd) return true;   // ya creado
        if (!m_device || !m_swapchain || !m_swapchain->valid()) return false;

        VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = m_graphicsFamily;
        if (!vkSuccess(vkCreateCommandPool(m_device, &pci, nullptr, &m_framePool), "create frame pool")) return false;

        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = m_framePool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!vkSuccess(vkAllocateCommandBuffers(m_device, &ai, &m_frameCmd), "alloc frame cmd")) return false;

        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!vkSuccess(vkCreateSemaphore(m_device, &si, nullptr, &m_imgAvailable), "create imageAvailable sem")) return false;
        if (!vkSuccess(vkCreateSemaphore(m_device, &si, nullptr, &m_renderFinished), "create renderFinished sem")) return false;

        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // señalizada desde el inicio: la primera vkWaitForFences del cierre no bloquea
        if (!vkSuccess(vkCreateFence(m_device, &fi, nullptr, &m_frameFence), "create frame fence")) return false;
        return true;
    }

    void VKDevice::destroyFrameSync()
    {
        if (!m_device) return;
        if (m_frameFence) { vkDestroyFence(m_device, m_frameFence, nullptr); m_frameFence = VK_NULL_HANDLE; }
        if (m_renderFinished) { vkDestroySemaphore(m_device, m_renderFinished, nullptr); m_renderFinished = VK_NULL_HANDLE; }
        if (m_imgAvailable) { vkDestroySemaphore(m_device, m_imgAvailable, nullptr); m_imgAvailable = VK_NULL_HANDLE; }
        if (m_frameCmd) { vkFreeCommandBuffers(m_device, m_framePool, 1, &m_frameCmd); m_frameCmd = VK_NULL_HANDLE; }
        if (m_framePool) { vkDestroyCommandPool(m_device, m_framePool, nullptr); m_framePool = VK_NULL_HANDLE; }
        if (m_descPool) { vkDestroyDescriptorPool(m_device, m_descPool, nullptr); m_descPool = VK_NULL_HANDLE; }
        m_descRing.clear();
    }

    // beginFrame() es IDEMPOTENTE dentro del frame real: el renderer lo llama muchísimas veces por
    // frame (cada renderer pide "su" context) pero endFrame() solo una. La primera llamada de cada
    // frame adquiere la imagen de present y abre el command buffer; el resto devuelven el mismo.
    Context* VKDevice::beginFrame()
    {
        if (!m_device || !m_swapchain || !m_swapchain->valid() || !m_context) return nullptr;
        if (m_frameActive) return m_context.get();

        // Ring de descriptores: perezoso (necesita el descriptor set layout compartido, que se crea
        // con el primer pipeline — aquí también por si aún no hay ninguno).
        if (m_descRing.empty())
        {
            if (!m_setLayout)
                m_setLayout = createSharedSetLayout(m_device);
            if (!m_pipelineLayout && m_setLayout)
                m_pipelineLayout = createSharedPipelineLayout(m_device, m_setLayout);
            if (!m_setLayout) return nullptr;
            // Cada set del ring RESERVA en el pool la cuenta COMPLETA del layout (32 bindings de
            // cada tipo: 0..31 UBO, 32..63 SSBO, 64..95 sampler) aunque no se escriban — así lo hace
            // vkAllocateDescriptorSets. Hay que dimensionar el pool por ring×32, no por uso real.
            VkDescriptorPoolSize sizes[3] = {
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  kDescRing * kUboBindingCount },
                { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  kDescRing * kSsboBindingCount },
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDescRing * kTextureBindingCount },
            };
            VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            dpi.maxSets = kDescRing;
            dpi.poolSizeCount = 3;
            dpi.pPoolSizes = sizes;
            if (!vkSuccess(vkCreateDescriptorPool(m_device, &dpi, nullptr, &m_descPool), "create desc pool")) return nullptr;

            std::vector<VkDescriptorSetLayout> layouts(kDescRing, m_setLayout);
            VkDescriptorSetAllocateInfo sai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            sai.descriptorPool = m_descPool;
            sai.descriptorSetCount = kDescRing;
            sai.pSetLayouts = layouts.data();
            m_descRing.resize(kDescRing);
            if (!vkSuccess(vkAllocateDescriptorSets(m_device, &sai, m_descRing.data()), "alloc desc ring")) return nullptr;
        }
        m_setCursor = 0;

        // ── EL SWAPCHAIN NO SE ENTERA SOLO DE QUE LA VENTANA HA CRECIDO ─────────────────────────
        //
        // ⚠️ Esperar a `OUT_OF_DATE` es asumir una semantica que Wayland NO da. Ahi el surface
        // responde `currentExtent = 0xFFFFFFFF` ("decide tu") y **no marca el swapchain como
        // obsoleto** al redimensionar: si nadie mira, se queda con el tamano que tuviera al crearse.
        //
        // Y se creaba pronto, con la ventana todavia en su tamano inicial. Medido:
        //
        //     swapchain extent: surface dice 0xFFFFFFFF (indefinido) · SDL en pixeles 1280x720
        //     ...mas tarde, ya en juego: SDL en pixeles 1920x1080, swapchain SEGUIA en 1280x720
        //
        // O sea que **todo el juego se renderizaba a 720p y se estiraba a 1080p**. Se noto por la UI
        // ("ImGui no es nitido"): su atlas se rasterizaba a 8,7 px y se agrandaba x1,5. Y ademas
        // falseaba cualquier comparativa de rendimiento contra OpenGL, que si sigue a la ventana:
        // Vulkan estaba dibujando el 44 % de los pixeles.
        //
        // Aqui se compara el tamano REAL en pixeles con el del swapchain y se recrea si no casan. Es
        // barato (dos enteros por frame) y no depende de que el driver avise.
        if (m_swapchain && m_swapchain->window()) {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(m_swapchain->window(), &pw, &ph);
            const VkExtent2D cur = m_swapchain->extent();
            if (pw > 0 && ph > 0 && ((uint32_t)pw != cur.width || (uint32_t)ph != cur.height)) {
                HARUKA_LOGI("RHI/VK", "la ventana es %dx%d y el swapchain %ux%u: recreando",
                            pw, ph, cur.width, cur.height);
                onSwapchainResize();
                return nullptr;
            }
        }

        // Sync completo: el frame anterior terminó (fence esperada en endFrame) → la imagen que
        // devuelva acquire no está en uso. Adquirir + abrir el command buffer del frame.
        VkResult r = vkAcquireNextImageKHR(m_device, m_swapchain->swapchain(), UINT64_MAX,
                                           m_imgAvailable, VK_NULL_HANDLE, &m_currentImage);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        {
            // El swapchain quedó desincronizado con la ventana (resize). Recreamos swapchain +
            // framebuffers + profundidad del backbuffer y omitimos este frame: beginFrame es
            // idempotente y el siguiente intento ya adquiere de la swapchain nueva.
            onSwapchainResize();
            return nullptr;
        }
        if (!vkSuccess(r, "vkAcquireNextImageKHR")) return nullptr;

        vkResetCommandBuffer(m_frameCmd, 0);
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(m_frameCmd, &bi) != VK_SUCCESS) return nullptr;

        m_frameActive = true;
        m_backbufferUsed = false;
        return m_context.get();
    }

    void VKDevice::endFrame()
    {
        if (!m_device || !m_frameActive) return;
        if (m_frameCmd)
        {
            if (m_context->m_inPass) m_context->endRenderPass();   // defensivo: passes abiertos
            // ImGui (fase 7): la UI se graba en este mismo command buffer, tras la escena, dentro de
            // su propio render pass (finalLayout PRESENT). Solo si la escena usó el backbuffer (si
            // no, no se presenta y no hay imagen con la escena para cargar).
            if (m_backbufferUsed)
            {
                if (m_imguiActive)
                    drawImgui(m_context->m_uiDrawData, m_frameCmd);
                else
                    transitionBackbufferToPresent(m_frameCmd);
            }
            vkEndCommandBuffer(m_frameCmd);

            // Submit: espera la señal de acquire (imagen disponible), señala renderFinished; la fence
            // del frame nos permite esperar a que termine antes del siguiente beginFrame.
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &m_imgAvailable;
            si.pWaitDstStageMask = &waitStage;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &m_frameCmd;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &m_renderFinished;
            // Sin fence del frame: usamos vkDeviceWaitIdle al final para sincronizar submit+present,
            // así que el fence no hace falta y NO se reusa (evita el "fence still signaled" → 0001).
            if (vkQueueSubmit(m_graphicsQueue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
            {
                // Submit falló: aun así PRESENTA la imagen adquirida para devolverla a la swapchain
                // (si no, se acumulan hasta "acquired 2 images, only 2 available" → acquire VK_NOT_READY
                // → beginFrame() null → SEGV).
                if (m_swapchain && m_swapchain->valid())
                {
                    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
                    pi.swapchainCount = 1;
                    VkSwapchainKHR sc = m_swapchain->swapchain();
                    pi.pSwapchains = &sc;
                    pi.pImageIndices = &m_currentImage;
                    vkQueuePresentKHR(m_graphicsQueue, &pi);
                    m_lastPresentedImage = m_currentImage;   // ya tiene contenido: readPixels la usa
                }
                vkDeviceWaitIdle(m_device);   // deja la cola/present consistente para el próximo frame
                m_frameActive = false;
                return;
            }

if (m_swapchain && m_swapchain->valid())
                {
                    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
                    pi.waitSemaphoreCount = 1;
                    pi.pWaitSemaphores = &m_renderFinished;
                    pi.swapchainCount = 1;
                    VkSwapchainKHR sc = m_swapchain->swapchain();
                    pi.pSwapchains = &sc;
                    pi.pImageIndices = &m_currentImage;
                    vkQueuePresentKHR(m_graphicsQueue, &pi);
                    m_lastPresentedImage = m_currentImage;   // ya tiene contenido: readPixels la usa
                }
            // Sync COMPLETO del frame: espera a que la GPU termine el submit Y el present (el fence
            // solo cubre el submit; el present reusa m_renderFinished, así que hay que garantizar que
            // SU wait haya consumido la señal antes de re-signalizarla en el próximo submit. Sin esto
            // la sem repite la señal sin espera → VVL 00067 → cuelga la GPU → NVIDIA TDR ~5s/frame.
            vkDeviceWaitIdle(m_device);
        }
        m_frameActive = false;
        m_backbufferUsed = false;
    }

    // Variante del pipeline para UN render pass concreto. Los pipelines de la fase 4 se crean
    // SOLO contra el backbuffer; para targets offscreen se recompila el MISMO SPV contra el render
    // pass del target (cacheado). Compute no depende del render pass.
    VkPipeline VKDevice::pipelineVariant(PipelineHandle h, VkRenderPass pass, uint32_t colorCount)
    {
        if (!m_device) return VK_NULL_HANDLE;
        const VKPipeline* p = pipeline(h);
        if (!p || !p->handle) return VK_NULL_HANDLE;
        if (p->compute) return p->handle;
        if (!pass || pass == m_backbufferPass) return p->handle;   // variante de la fase 4

        const std::string key = std::to_string(h.id) + ":" + std::to_string((uintptr_t)pass);
        auto it = m_pipelineVariants.find(key);
        if (it != m_pipelineVariants.end()) return it->second;

        VkPipeline v = buildGraphicsPipeline(m_device, p->desc, p->spv, m_pipelineLayout, pass, colorCount);
        if (!v) return VK_NULL_HANDLE;
        m_pipelineVariants[key] = v;
        return v;
    }

    // Render pass de UN color para renderizar a una cara/mip de un cubemap (IBL): CLEAR + deja la
    // imagen en SHADER_READ para muestrearla después. Cachado por formato.
    VkRenderPass VKDevice::cubeFacePass(VkFormat colorFormat)
    {
        const std::string key = std::to_string((int)colorFormat);
        auto it = m_cubePasses.find(key);
        if (it != m_cubePasses.end()) return it->second;

        VkAttachmentDescription a{};
        a.format = colorFormat;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference cref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;

        VkRenderPassCreateInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rp.attachmentCount = 1;
        rp.pAttachments = &a;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        VkRenderPass pass = VK_NULL_HANDLE;
        if (!vkSuccess(vkCreateRenderPass(m_device, &rp, nullptr, &pass), "create cube face pass")) return VK_NULL_HANDLE;
        m_cubePasses[key] = pass;
        return pass;
    }

    void VKDevice::framebufferSize(uint32_t& w, uint32_t& h) const
    {
        if (m_swapchain && m_swapchain->valid())
        {
            const VkExtent2D e = m_swapchain->extent();
            w = e.width;
            h = e.height;
        }
        else { w = 0; h = 0; }
    }

    void VKDevice::readPixels(int x, int y, int w, int h, Format format, void* data)    {
        // Lee el backbuffer a RAM (screenshots). Copia de la imagen de present del swapchain.
        if (!m_swapchain || !m_swapchain->valid() || w <= 0 || h <= 0) return;
        const size_t bpp = bytesPerPixel(format);
        if (!bpp) return;
        const size_t total = (size_t)w * h * bpp;

        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (!createStagingBuffer(staging, mem, total, nullptr)) {
            HARUKA_LOGE("RHI/VK", "readPixels: createStagingBuffer fallo (%zu B)", total);
            return;
        }
        // El staging de createStagingBuffer es TRANSFER_SRC; para LEER de la imagen hace falta un
        // buffer TRANSFER_DST. Crear uno propio no es eficiente pero readPixels es depuración.
        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, mem, nullptr);

        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = (VkDeviceSize)total;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!vkSuccess(vkCreateBuffer(m_device, &bi, nullptr, &staging), "readback buffer")) return;
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(m_device, staging, &mr);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = findMemoryType(mr.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || !vkSuccess(vkAllocateMemory(m_device, &ai, nullptr, &mem), "readback mem"))
        { vkDestroyBuffer(m_device, staging, nullptr); return; }
        vkBindBufferMemory(m_device, staging, mem, 0);

        // ⚠️ AQUÍ HABÍA TRES FALLOS QUE DABAN UNA CAPTURA EN NEGRO ABSOLUTO (0,0,0,0), y ninguno
        // tenía que ver con lo que se dibuja: el render podía estar perfecto y el PNG salía vacío.
        //
        //  1. `image(0)`: la imagen 0 estaba CABLEADA. Con un swapchain de 3 imágenes, la que se
        //     está usando casi nunca es la 0.
        //  2. `oldLayout = UNDEFINED`: transicionar DESDE `UNDEFINED` autoriza explícitamente al
        //     driver a DESCARTAR el contenido — es lo que significa ese layout como origen. La
        //     barrera tiraba el frame justo antes de copiarlo.
        //  3. Se leía la imagen del frame EN CURSO, cuyo command buffer sigue abierto y sin enviar
        //     cuando `readPixels` se llama (la captura ocurre a mitad de frame): no se había
        //     ejecutado ni un draw sobre ella.
        //
        // Se copia por tanto la última imagen PRESENTADA, que sí está completa —los dos caminos de
        // present hacen `vkDeviceWaitIdle` justo después— y está en `PRESENT_SRC_KHR`. La captura
        // queda un frame por detrás, que para una foto es exactamente lo que se quiere.
        if (m_lastPresentedImage == UINT32_MAX) {
            HARUKA_LOGW("RHI/VK", "readPixels: todavia no se ha presentado ningun frame; sin captura");
            vkDestroyBuffer(m_device, staging, nullptr);
            vkFreeMemory(m_device, mem, nullptr);
            return;
        }
        VkImage swapImg = m_swapchain->image(m_lastPresentedImage);
        const VkExtent2D e = m_swapchain->extent();
        uint32_t cwLog = 0, chLog = 0;
        if (!submitOneShot([&](VkCommandBuffer c) {
            VkImageMemoryBarrier b0{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b0.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;   // CONSERVA el contenido
            b0.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b0.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b0.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b0.image = swapImg;
            b0.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            b0.srcAccessMask = 0;
            b0.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b0);

            // ⚠️ ACOTADO AL TAMAÑO REAL DEL SWAPCHAIN. `e` se obtenia y NO se usaba: la copia iba
            // con el tamaño de VENTANA, y en cuanto los dos no coinciden (escalado del compositor,
            // HiDPI, un resize a medias) se pide una region FUERA de la imagen. Eso no da un error
            // de validacion amable: pierde el dispositivo (`VK_ERROR_DEVICE_LOST`), que fue
            // exactamente lo que se midio aqui.
            const uint32_t cw = (x < (int)e.width)  ? std::min((uint32_t)w, e.width  - (uint32_t)x) : 0u;
            const uint32_t ch = (y < (int)e.height) ? std::min((uint32_t)h, e.height - (uint32_t)y) : 0u;
            cwLog = cw; chLog = ch;
            VkBufferImageCopy r{};
            r.bufferOffset = 0;
            r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            r.imageOffset = { (int32_t)x, (int32_t)y, 0 };
            r.imageExtent = { cw, ch, 1 };
            vkCmdCopyImageToBuffer(c, swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &r);

            VkImageMemoryBarrier b1 = b0;
            b1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b1.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            b1.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            b1.dstAccessMask = 0;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b1);
        }))
        {
            HARUKA_LOGE("RHI/VK", "readPixels: submitOneShot fallo (la copia no se ejecuto)");
            vkDestroyBuffer(m_device, staging, nullptr);
            vkFreeMemory(m_device, mem, nullptr);
            return;
        }

        void* p = nullptr;
        vkMapMemory(m_device, mem, 0, total, 0, &p);
        if (p) {
            std::memcpy(data, p, total);

            // ⚠️ EL SWAPCHAIN ES BGRA, EL LLAMADOR PIDE RGBA. Sin esto, `readPixels` devuelve los
            // bytes crudos y el rojo sale azul: medido en el banco del RHI, que pedía rojo y leía
            // (0,0,255). Afecta a TODA captura de pantalla en Vulkan, así que se corrige aquí y no
            // en cada llamador. Formato de 4 bytes: se intercambian los canales 0 y 2.
            if ((m_swapchain->format() == VK_FORMAT_B8G8R8A8_UNORM ||
                 m_swapchain->format() == VK_FORMAT_B8G8R8A8_SRGB) && bpp == 4)
            {
                uint8_t* q = (uint8_t*)data;
                for (size_t i = 0; i + 3 < total; i += 4) std::swap(q[i], q[i + 2]);
            }
            // Sonda: distingue "la imagen esta vacia" de "la copia no ocurrio". Sin esto las dos
            // producen el MISMO PNG en negro y no hay forma de saber cual de las dos es.
            const uint8_t* q = (const uint8_t*)p;
            uint64_t sum = 0, nz = 0;
            for (size_t i = 0; i < total; i += 97) { sum += q[i]; if (q[i]) ++nz; }
            HARUKA_LOGI("RHI/VK", "readPixels: img=%u pedido %dx%d · swapchain %ux%u · copiado %ux%u"
                        " · %zu B · muestreo: %llu no-cero, suma=%llu",
                        m_lastPresentedImage, w, h, e.width, e.height, cwLog, chLog, total,
                        (unsigned long long)nz, (unsigned long long)sum);
        } else {
            HARUKA_LOGE("RHI/VK", "readPixels: vkMapMemory devolvio null");
        }
        vkUnmapMemory(m_device, mem);

        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, mem, nullptr);
    }

    // ------------------------------------------------------------------ teardown
    VKDevice::~VKDevice()
    {
        if (!m_device) return;

        // Espera a que la GPU termine: destruir recursos en uso es UB.
        vkDeviceWaitIdle(m_device);

        // El context libera sus cachés de cubemap (framebuffers/image views) antes que el device.
        m_context.reset();

        // ImGui (fase 7): shutdown del backend de UI y de su render pass/framebuffers propios.
        if (m_imguiActive)
        {
            // Shutdown destruye el pool interno: los sets cacheados mueren con el, solo hay que olvidarlos.
            m_imguiTextures.clear();
            ImGui_ImplVulkan_Shutdown();
            m_imguiActive = false;
        }
        destroyImguiResources();

        // Variantes de pipeline (fase 5) y render passes de caras de cubemap.
        for (auto& [k, v] : m_pipelineVariants) if (v) vkDestroyPipeline(m_device, v, nullptr);
        m_pipelineVariants.clear();
        for (auto& [k, p] : m_cubePasses) if (p) vkDestroyRenderPass(m_device, p, nullptr);
        m_cubePasses.clear();

        // Pool/cmd del frame + semáforos + fence + pool de descriptores.
        destroyFrameSync();

        // Render passes cacheados (owned por el device).
        for (auto& [k, pass] : m_renderPasses) if (pass) vkDestroyRenderPass(m_device, pass, nullptr);
        m_renderPasses.clear();
        if (m_backbufferPass) { vkDestroyRenderPass(m_device, m_backbufferPass, nullptr); m_backbufferPass = VK_NULL_HANDLE; }
        destroyBackbufferDepth();

        for (auto& b : m_buffers)  { if (b.buffer) vkDestroyBuffer(m_device, b.buffer, nullptr); if (b.memory) vkFreeMemory(m_device, b.memory, nullptr); }
        for (auto& t : m_textures) { if (t.sampler) vkDestroySampler(m_device, t.sampler, nullptr); if (t.view) vkDestroyImageView(m_device, t.view, nullptr); if (t.image) vkDestroyImage(m_device, t.image, nullptr); if (t.memory) vkFreeMemory(m_device, t.memory, nullptr); }
        for (auto& s : m_samplers) { if (s.id) vkDestroySampler(m_device, s.id, nullptr); }
        for (auto& p : m_pipelines) { if (p.handle) vkDestroyPipeline(m_device, p.handle, nullptr); }
        // Layouts compartidos (fase 4), tras destruir todos los pipelines que los referencian.
        if (m_pipelineLayout) { vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
        if (m_setLayout) { vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr); m_setLayout = VK_NULL_HANDLE; }
        for (auto& rt : m_targets) { if (rt.framebuffer) vkDestroyFramebuffer(m_device, rt.framebuffer, nullptr); }
        for (auto& f : m_fences) { if (f) vkDestroyFence(m_device, f, nullptr); }

        if (m_transferFence) vkDestroyFence(m_device, m_transferFence, nullptr);
        if (m_transferCmd) vkFreeCommandBuffers(m_device, m_transferPool, 1, &m_transferCmd);
        if (m_transferPool) vkDestroyCommandPool(m_device, m_transferPool, nullptr);

        m_swapchain.reset();   // destruye surface + swapchain + framebuffers del backbuffer

        vkDestroyDevice(m_device, nullptr);
        if (m_debugMessenger)
        {
            auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT");
            if (fn) fn(m_instance, m_debugMessenger, nullptr);
        }
        vkDestroyInstance(m_instance, nullptr);
        // NO destruye m_window; de eso se encarga la clase Window del motor.
    }
}

#endif // HARUKA_RHI_HAS_VULKAN
