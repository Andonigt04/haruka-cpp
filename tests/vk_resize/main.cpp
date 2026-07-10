// =================================================================================================
// vk_resize_test — SPIKE de recreación de swapchain al REDIMENSIONAR (Wayland + SDL3 + ImGui).
//
// Por qué existe: Vulkan sobre Wayland tiene gotchas de resize que NO aparecen en X11. Este test
// aislado (no toca el motor) demuestra el manejo correcto, para decidir si el backend Vulkan del
// RHI es viable antes de invertir en él. Dibuja un triángulo + una ventana ImGui; redimensiona la
// ventana (arrastra el borde, maximiza, tile en el compositor) y NO debe haber ni parpadeos negros,
// ni stretching, ni errores de validación, ni cuelgues.
//
// GOTCHAS DE WAYLAND cubiertas (buscar [WAYLAND] abajo):
//   1. VkSurfaceCapabilitiesKHR::currentExtent == 0xFFFFFFFF en Wayland (en X11 da el tamaño real).
//      → hay que usar el tamaño en PÍXELES que reporta SDL y clampearlo a min/maxImageExtent.
//   2. En Wayland vkAcquire/Present puede NO devolver OUT_OF_DATE al redimensionar (el resize lo
//      dirige el compositor vía xdg configure). → hay que recrear también al recibir el evento SDL
//      de cambio de tamaño, no solo confiando en OUT_OF_DATE/SUBOPTIMAL.
//   3. Minimizar/tamaño 0 → no se puede crear swapchain; hay que pausar y esperar.
// =================================================================================================
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"

#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <fstream>
#include <stdexcept>

#define VK_CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    std::fprintf(stderr, "[FATAL] %s -> VkResult %d (%s:%d)\n", #x, (int)_r, __FILE__, __LINE__); \
    std::abort(); } } while(0)

static const int MAX_FRAMES_IN_FLIGHT = 2;
static bool g_enableValidation = true; // se desactiva solo si la capa no está disponible

// ------------------------------------------------------------------------- utilidades
static std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) { std::fprintf(stderr, "[FATAL] no puedo abrir %s\n", path.c_str()); std::abort(); }
    size_t sz = (size_t)f.tellg();
    std::vector<char> buf(sz);
    f.seekg(0); f.read(buf.data(), (std::streamsize)sz);
    return buf;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCb(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "[VVL] %s\n", data->pMessage);
    return VK_FALSE;
}

static void checkVkImGui(VkResult r) { if (r != VK_SUCCESS) std::fprintf(stderr, "[imgui-vk] VkResult %d\n", (int)r); }

// ------------------------------------------------------------------------- la app
struct App {
    SDL_Window* window = nullptr;
    int wantW = 1280, wantH = 720;

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapExtent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    std::vector<VkFramebuffer> framebuffers;
    uint32_t minImageCount = 2;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmdBufs;                 // por frame en vuelo

    std::vector<VkSemaphore> imageAvailable;              // por frame en vuelo
    std::vector<VkSemaphore> renderFinished;              // por IMAGEN de swapchain (evita reuse del semáforo)
    std::vector<VkFence> inFlight;                        // por frame en vuelo
    uint32_t currentFrame = 0;

    bool framebufferResized = false;                     // [WAYLAND-2] lo pone el evento SDL de resize
    int resizeEventCount = 0, recreateCount = 0;         // telemetría visible en ImGui

    // ---- ciclo de vida
    void run() { initWindow(); initVulkan(); initImGui(); loop(); cleanup(); }

    // ---------------------------------------------------------------- ventana
    void initWindow() {
        if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "[FATAL] SDL_Init: %s\n", SDL_GetError()); std::abort(); }
        SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        window = SDL_CreateWindow("vk_resize_test — arrastra el borde para redimensionar (ESC salir)",
                                  wantW, wantH, flags);
        if (!window) { std::fprintf(stderr, "[FATAL] SDL_CreateWindow: %s\n", SDL_GetError()); std::abort(); }
        std::fprintf(stderr, "[info] video driver = %s\n", SDL_GetCurrentVideoDriver());
    }

    // ---------------------------------------------------------------- instancia + surface + device
    void initVulkan() {
        // Extensiones de instancia que pide SDL para el surface (Wayland/X11) + debug utils.
        Uint32 nSdlExt = 0;
        const char* const* sdlExt = SDL_Vulkan_GetInstanceExtensions(&nSdlExt);
        std::vector<const char*> exts(sdlExt, sdlExt + nSdlExt);

        // Capa de validación (opcional pero clave para un test de resize: queremos VER errores).
        std::vector<const char*> layers;
        if (g_enableValidation && hasLayer("VK_LAYER_KHRONOS_validation")) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else { g_enableValidation = false; std::fprintf(stderr, "[info] sin capa de validación\n"); }

        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "vk_resize_test"; app.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = (uint32_t)exts.size();   ci.ppEnabledExtensionNames = exts.data();
        ci.enabledLayerCount     = (uint32_t)layers.size(); ci.ppEnabledLayerNames     = layers.data();
        VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));

        if (g_enableValidation) {
            VkDebugUtilsMessengerCreateInfoEXT d{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            d.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            d.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                          | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            d.pfnUserCallback = debugCb;
            auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
            if (fn) fn(instance, &d, nullptr, &debugMessenger);
        }

        // Surface vía SDL (elige Wayland/X11 correctamente).
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
            std::fprintf(stderr, "[FATAL] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError()); std::abort();
        }

        pickPhysicalDevice();
        createLogicalDevice();

        VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily;
        VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &cmdPool));

        createSwapchain();
        createRenderPass();
        createPipeline();
        createFramebuffers();
        createCommandBuffers();
        createSyncObjects();
    }

    bool hasLayer(const char* name) {
        uint32_t n = 0; vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n); vkEnumerateInstanceLayerProperties(&n, props.data());
        for (auto& p : props) if (std::strcmp(p.layerName, name) == 0) return true;
        return false;
    }

    void pickPhysicalDevice() {
        uint32_t n = 0; vkEnumeratePhysicalDevices(instance, &n, nullptr);
        if (n == 0) { std::fprintf(stderr, "[FATAL] sin GPU Vulkan\n"); std::abort(); }
        std::vector<VkPhysicalDevice> devs(n); vkEnumeratePhysicalDevices(instance, &n, devs.data());
        for (auto d : devs) {
            uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> q(qn); vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, q.data());
            for (uint32_t i = 0; i < qn; ++i) {
                VkBool32 present = VK_FALSE; vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &present);
                if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    phys = d; queueFamily = i; break;
                }
            }
            if (phys != VK_NULL_HANDLE) break;
        }
        if (phys == VK_NULL_HANDLE) { std::fprintf(stderr, "[FATAL] sin cola gráfica+present\n"); std::abort(); }
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(phys, &p);
        std::fprintf(stderr, "[info] GPU = %s\n", p.deviceName);
    }

    void createLogicalDevice() {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qci.queueFamilyIndex = queueFamily; qci.queueCount = 1; qci.pQueuePriorities = &prio;
        const char* devExt[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = devExt;
        VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &device));
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
    }

    // ---------------------------------------------------------------- swapchain
    // [WAYLAND-1]: elige el extent correcto. En Wayland currentExtent es 0xFFFFFFFF ("tú decides")
    // → usamos el tamaño en píxeles de SDL y lo clampeamos al rango válido. En X11 currentExtent ya
    // trae el tamaño real y lo respetamos.
    VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& caps) {
        if (caps.currentExtent.width != 0xFFFFFFFFu) return caps.currentExtent;
        int pw = 0, ph = 0; SDL_GetWindowSizeInPixels(window, &pw, &ph);
        VkExtent2D e{ (uint32_t)pw, (uint32_t)ph };
        e.width  = std::clamp(e.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
        e.height = std::clamp(e.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        return e;
    }

    void createSwapchain() {
        VkSurfaceCapabilitiesKHR caps; VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));

        // Formato: preferimos BGRA8 sRGB/UNORM; si no, el primero.
        uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(nf); vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, fmts.data());
        VkSurfaceFormatKHR chosen = fmts[0];
        for (auto& f : fmts)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
        swapFormat = chosen.format;

        swapExtent = chooseExtent(caps);

        uint32_t imgCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;
        minImageCount = caps.minImageCount;

        VkSwapchainCreateInfoKHR sci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        sci.surface = surface;
        sci.minImageCount = imgCount;
        sci.imageFormat = chosen.format; sci.imageColorSpace = chosen.colorSpace;
        sci.imageExtent = swapExtent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;    // siempre soportado (vsync)
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain));

        uint32_t n = 0; vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
        swapImages.resize(n); vkGetSwapchainImagesKHR(device, swapchain, &n, swapImages.data());

        swapViews.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            VkImageViewCreateInfo iv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            iv.image = swapImages[i]; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = swapFormat;
            iv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VK_CHECK(vkCreateImageView(device, &iv, nullptr, &swapViews[i]));
        }
        std::fprintf(stderr, "[swapchain] %ux%u  %u imgs\n", swapExtent.width, swapExtent.height, n);
    }

    void createRenderPass() {
        VkAttachmentDescription color{};
        color.format = swapFormat; color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rp.attachmentCount = 1; rp.pAttachments = &color;
        rp.subpassCount = 1; rp.pSubpasses = &sub;
        rp.dependencyCount = 1; rp.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(device, &rp, nullptr, &renderPass));
    }

    VkShaderModule loadShader(const std::string& path) {
        auto code = readFile(path);
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = code.size(); ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m; VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
        return m;
    }

    void createPipeline() {
        const char* base = SDL_GetBasePath();          // dir del ejecutable (los .spv están al lado)
        std::string dir = base ? base : "./";
        VkShaderModule vs = loadShader(dir + "tri.vert.spv");
        VkShaderModule fs = loadShader(dir + "tri.frag.spv");

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        // Viewport/scissor DINÁMICOS → el resize no obliga a recrear el pipeline (solo el swapchain).
        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkPipelineLayoutCreateInfo pl{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VK_CHECK(vkCreatePipelineLayout(device, &pl, nullptr, &pipeLayout));

        VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
        gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
        gp.pDynamicState = &ds; gp.layout = pipeLayout; gp.renderPass = renderPass; gp.subpass = 0;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline));

        vkDestroyShaderModule(device, vs, nullptr);
        vkDestroyShaderModule(device, fs, nullptr);
    }

    void createFramebuffers() {
        framebuffers.resize(swapViews.size());
        for (size_t i = 0; i < swapViews.size(); ++i) {
            VkFramebufferCreateInfo fb{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fb.renderPass = renderPass; fb.attachmentCount = 1; fb.pAttachments = &swapViews[i];
            fb.width = swapExtent.width; fb.height = swapExtent.height; fb.layers = 1;
            VK_CHECK(vkCreateFramebuffer(device, &fb, nullptr, &framebuffers[i]));
        }
    }

    void createCommandBuffers() {
        cmdBufs.resize(MAX_FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = cmdPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
        VK_CHECK(vkAllocateCommandBuffers(device, &ai, cmdBufs.data()));
    }

    void createSyncObjects() {
        imageAvailable.resize(MAX_FRAMES_IN_FLIGHT);
        inFlight.resize(MAX_FRAMES_IN_FLIGHT);
        renderFinished.resize(swapImages.size());        // uno por imagen → sin reuse del semáforo de present
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            VK_CHECK(vkCreateSemaphore(device, &si, nullptr, &imageAvailable[i]));
            VK_CHECK(vkCreateFence(device, &fi, nullptr, &inFlight[i]));
        }
        for (size_t i = 0; i < renderFinished.size(); ++i)
            VK_CHECK(vkCreateSemaphore(device, &si, nullptr, &renderFinished[i]));
    }

    // ---------------------------------------------------------------- recreación
    void cleanupSwapchain() {
        for (auto fb : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
        for (auto v : swapViews)     vkDestroyImageView(device, v, nullptr);
        for (auto s : renderFinished) vkDestroySemaphore(device, s, nullptr);
        framebuffers.clear(); swapViews.clear(); renderFinished.clear();
        if (swapchain) { vkDestroySwapchainKHR(device, swapchain, nullptr); swapchain = VK_NULL_HANDLE; }
    }

    void recreateSwapchain() {
        // [WAYLAND-3] minimizado / tamaño 0 → no se puede crear swapchain; pausa hasta recuperar tamaño.
        int pw = 0, ph = 0; SDL_GetWindowSizeInPixels(window, &pw, &ph);
        while (pw == 0 || ph == 0) {
            SDL_Event e; SDL_WaitEvent(&e); ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) return;
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
        }
        vkDeviceWaitIdle(device);
        cleanupSwapchain();
        createSwapchain();
        createFramebuffers();
        // renderFinished depende del nº de imágenes → recrearlos (los recrea createSyncObjects, pero
        // aquí solo hace falta el vector de present, no las fences/imageAvailable por-frame).
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        renderFinished.resize(swapImages.size());
        for (size_t i = 0; i < renderFinished.size(); ++i)
            VK_CHECK(vkCreateSemaphore(device, &si, nullptr, &renderFinished[i]));
        ImGui_ImplVulkan_SetMinImageCount(minImageCount);   // por si cambió
        recreateCount++;
        framebufferResized = false;
    }

    // ---------------------------------------------------------------- ImGui (bring-your-own-renderpass)
    void initImGui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui_ImplSDL3_InitForVulkan(window);
        ImGui_ImplVulkan_InitInfo ii{};
        ii.ApiVersion = VK_API_VERSION_1_3;
        ii.Instance = instance; ii.PhysicalDevice = phys; ii.Device = device;
        ii.QueueFamily = queueFamily; ii.Queue = queue;
        ii.DescriptorPoolSize = 16;                          // el backend crea su propio pool
        ii.MinImageCount = minImageCount; ii.ImageCount = (uint32_t)swapImages.size();
        ii.PipelineInfoMain.RenderPass = renderPass;         // API 1.92: render pass va aquí
        ii.PipelineInfoMain.Subpass = 0;
        ii.CheckVkResultFn = checkVkImGui;
        ImGui_ImplVulkan_Init(&ii);
    }

    // ---------------------------------------------------------------- bucle + dibujo
    void loop() {
        bool running = true;
        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_QUIT) running = false;
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) running = false;
                // [WAYLAND-2] recrear también por evento de resize (no solo por OUT_OF_DATE).
                if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || e.type == SDL_EVENT_WINDOW_RESIZED) {
                    framebufferResized = true; resizeEventCount++;
                }
            }
            if (!running) break;

            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            drawUI();
            ImGui::Render();

            drawFrame();
        }
        vkDeviceWaitIdle(device);
    }

    void drawUI() {
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::Begin("resize test");
        ImGui::Text("swapchain: %u x %u  (%zu imgs)", swapExtent.width, swapExtent.height, swapImages.size());
        int pw = 0, ph = 0, ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph); SDL_GetWindowSize(window, &ww, &wh);
        ImGui::Text("SDL px: %d x %d   logical: %d x %d", pw, ph, ww, wh);
        ImGui::Text("eventos de resize: %d", resizeEventCount);
        ImGui::Text("recreaciones swapchain: %d", recreateCount);
        ImGui::Text("%.1f FPS (%.2f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
        ImGui::Separator();
        ImGui::TextWrapped("Arrastra el borde, maximiza, o haz tiling en el compositor. El triangulo "
                           "debe seguir el tamano SIN estirarse ni parpadear en negro, y sin errores [VVL].");
        ImGui::End();
    }

    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) {
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

        VkClearValue clear{}; clear.color = { { 0.06f, 0.07f, 0.09f, 1.0f } };
        VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rp.renderPass = renderPass; rp.framebuffer = framebuffers[imageIndex];
        rp.renderArea.extent = swapExtent;
        rp.clearValueCount = 1; rp.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vpt{ 0, 0, (float)swapExtent.width, (float)swapExtent.height, 0.0f, 1.0f };
        VkRect2D sc{ {0,0}, swapExtent };
        vkCmdSetViewport(cmd, 0, 1, &vpt);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);   // ImGui dentro de NUESTRO render pass

        vkCmdEndRenderPass(cmd);
        VK_CHECK(vkEndCommandBuffer(cmd));
    }

    void drawFrame() {
        vkWaitForFences(device, 1, &inFlight[currentFrame], VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        VkResult acq = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                             imageAvailable[currentFrame], VK_NULL_HANDLE, &imageIndex);
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(); return; }
        if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) { std::fprintf(stderr, "[acquire] %d\n", acq); return; }

        vkResetFences(device, 1, &inFlight[currentFrame]);   // solo tras confirmar que vamos a enviar

        vkResetCommandBuffer(cmdBufs[currentFrame], 0);
        recordCommandBuffer(cmdBufs[currentFrame], imageIndex);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &imageAvailable[currentFrame]; si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmdBufs[currentFrame];
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &renderFinished[imageIndex];
        VK_CHECK(vkQueueSubmit(queue, 1, &si, inFlight[currentFrame]));

        VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &renderFinished[imageIndex];
        pi.swapchainCount = 1; pi.pSwapchains = &swapchain; pi.pImageIndices = &imageIndex;
        VkResult pres = vkQueuePresentKHR(queue, &pi);

        // [WAYLAND-2] recrear si el present dice OUT_OF_DATE/SUBOPTIMAL o si el evento SDL lo marcó.
        if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR || framebufferResized)
            recreateSwapchain();
        else if (pres != VK_SUCCESS)
            std::fprintf(stderr, "[present] %d\n", pres);

        currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    // ---------------------------------------------------------------- teardown
    void cleanup() {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();

        cleanupSwapchain();
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, pipeLayout, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            vkDestroySemaphore(device, imageAvailable[i], nullptr);
            vkDestroyFence(device, inFlight[i], nullptr);
        }
        vkDestroyCommandPool(device, cmdPool, nullptr);
        vkDestroyDevice(device, nullptr);
        if (debugMessenger) {
            auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
            if (fn) fn(instance, debugMessenger, nullptr);
        }
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

int main(int, char**) {
    if (const char* v = SDL_getenv("VK_RESIZE_NO_VALIDATION")) { if (v[0] == '1') g_enableValidation = false; }
    App app;
    app.run();
    std::fprintf(stderr, "[ok] salida limpia\n");
    return 0;
}
