#include "application.h"

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdexcept>
#include <vector>
#include <iostream>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
#include <cassert>

#include "error_reporter.h"
#include "core/game_interface.h"
#include "core/object_types.h"
#include "core/terrain_streaming_system.h"
#include "renderer/mesh.h"
#include "renderer/motor_instance.h"
#include "renderer/shader.h"
#include "renderer/model.h"
#include "renderer/shadow.h"
#include "renderer/hdr.h"
#include "renderer/bloom.h"
#include "renderer/gbuffer.h"
#include "renderer/ssao.h"
#include "renderer/ibl.h"
#include "renderer/point_shadow.h"
#include "renderer/render_target.h"
#include "renderer/simple_mesh.h"
#include "renderer/primitive_shapes.h"
#include "renderer/gpu_instancing.h"
#include "physics/raycast_simple.h"

Haruka::GameInterface* gameInterface = nullptr;

// Mouse input state
struct MouseState {
    float lastX = 640.0f;
    float lastY = 360.0f;
    bool firstMouse = true;
};

static MouseState g_mouseState;

namespace {
std::vector<const Haruka::SceneObject*> g_sceneRenderQueue;
Haruka::Scene* g_sceneForRender = nullptr;
std::unordered_map<std::string, std::shared_ptr<Model>> g_modelCache;

bool isRenderDisabledByEditor(const Haruka::SceneObject& obj) {
    if (!obj.properties.is_object()) return false;
    if (!obj.properties.contains("terrainEditor")) return false;
    const auto& te = obj.properties["terrainEditor"];
    return te.value("disableRender", false);
}

bool shouldUseProceduralTerrainLook(const Haruka::SceneObject& obj) {
    if (obj.name.find("_chunk_") != std::string::npos) return true;
    if (!obj.properties.is_object()) return false;
    if (!obj.properties.contains("terrainEditor")) return false;
    return obj.properties["terrainEditor"].is_object();
}

bool isTerrainChunkFacingCamera(const Haruka::SceneObject& obj, const glm::vec3& cameraPos) {
    if (!obj.properties.is_object()) return true;
    if (!obj.properties.contains("terrainEditor")) return true;
    const auto& te = obj.properties["terrainEditor"];
    if (!te.is_object() || !te.value("isChunk", false)) return true;

    if (!te.contains("chunkX") || !te.contains("chunkY") || !te.contains("chunkTilesX") || !te.contains("chunkTilesY"))
        return true;

    const int chunkX = te.value("chunkX", -1);
    const int chunkY = te.value("chunkY", -1);
    const int tilesX = te.value("chunkTilesX", 0);
    const int tilesY = te.value("chunkTilesY", 0);
    if (chunkX < 0 || chunkY < 0 || tilesX <= 0 || tilesY <= 0) return true;

    const double pi = 3.14159265358979323846;
    const float lat = ((static_cast<float>(chunkY) + 0.5f) / static_cast<float>(tilesY)) * static_cast<float>(pi) - static_cast<float>(pi * 0.5);
    const float lon = ((static_cast<float>(chunkX) + 0.5f) / static_cast<float>(tilesX)) * static_cast<float>(2.0 * pi) - static_cast<float>(pi);

    glm::vec3 chunkDir(
        std::cos(lat) * std::cos(lon),
        std::sin(lat),
        std::cos(lat) * std::sin(lon)
    );

    glm::vec3 camDir = cameraPos;
    float camLen = glm::length(camDir);
    if (camLen > 1e-6f) camDir /= camLen;
    else camDir = glm::vec3(0.0f, 0.0f, 1.0f);

    return glm::dot(chunkDir, camDir) > -0.15f;
}

void buildPrimitiveMeshFromProperties(Haruka::SceneObject& obj) {
    if (!obj.meshRenderer)
        obj.meshRenderer = std::make_shared<MeshRendererComponent>();
    if (!obj.meshRenderer || obj.meshRenderer->isResident()) return;
    if (!obj.properties.contains("meshRenderer")) return;

    const auto& mr = obj.properties["meshRenderer"];
    std::string meshType = mr.value("meshType", "");
    std::vector<glm::vec3> verts, norms;
    std::vector<unsigned int> indices;

    if (meshType == "cube") {
        PrimitiveShapes::createCube(mr.value("size", 1.0f), verts, norms, indices);
    } else if (meshType == "sphere") {
        int seg = mr.value("segments", 32);
        PrimitiveShapes::createSphere(mr.value("radius", 1.0f), seg, seg, verts, norms, indices);
    } else if (meshType == "capsule") {
        PrimitiveShapes::createCapsule(mr.value("radius", 0.5f), mr.value("height", 2.0f),
                                       mr.value("segments", 24), mr.value("stacks", 16),
                                       verts, norms, indices);
    } else if (meshType == "plane") {
        PrimitiveShapes::createPlane(mr.value("width", 2.0f), mr.value("height", 2.0f),
                                     mr.value("subdivisions", 10), verts, norms, indices);
    }

    if (!verts.empty())
        obj.meshRenderer->setMesh(verts, norms, indices);
}

void maybeReleasePrimitiveMesh(Haruka::SceneObject& obj) {
    if (obj.meshRenderer && obj.meshRenderer->isResident())
        obj.meshRenderer->releaseMesh();
}

std::shared_ptr<Model> getOrLoadModel(const std::string& path) {
    auto it = g_modelCache.find(path);
    if (it != g_modelCache.end()) return it->second;
    try {
        auto model = std::make_shared<Model>(path);
        g_modelCache[path] = model;
        return model;
    } catch (...) { return nullptr; }
}

void releaseModelFromCache(const std::string& path) {
    g_modelCache.erase(path);
}

bool isSphereInsideCameraFrustum(const glm::dvec3& center, double radius, const glm::mat4& viewProj) {
    glm::vec4 planes[6];
    planes[0] = glm::vec4(viewProj[0][3]+viewProj[0][0], viewProj[1][3]+viewProj[1][0], viewProj[2][3]+viewProj[2][0], viewProj[3][3]+viewProj[3][0]);
    planes[1] = glm::vec4(viewProj[0][3]-viewProj[0][0], viewProj[1][3]-viewProj[1][0], viewProj[2][3]-viewProj[2][0], viewProj[3][3]-viewProj[3][0]);
    planes[2] = glm::vec4(viewProj[0][3]+viewProj[0][1], viewProj[1][3]+viewProj[1][1], viewProj[2][3]+viewProj[2][1], viewProj[3][3]+viewProj[3][1]);
    planes[3] = glm::vec4(viewProj[0][3]-viewProj[0][1], viewProj[1][3]-viewProj[1][1], viewProj[2][3]-viewProj[2][1], viewProj[3][3]-viewProj[3][1]);
    planes[4] = glm::vec4(viewProj[0][3]+viewProj[0][2], viewProj[1][3]+viewProj[1][2], viewProj[2][3]+viewProj[2][2], viewProj[3][3]+viewProj[3][2]);
    planes[5] = glm::vec4(viewProj[0][3]-viewProj[0][2], viewProj[1][3]-viewProj[1][2], viewProj[2][3]-viewProj[2][2], viewProj[3][3]-viewProj[3][2]);

    glm::vec3 c = glm::vec3(center);
    float r = static_cast<float>(radius);
    for (int i = 0; i < 6; ++i) {
        glm::vec3 n(planes[i].x, planes[i].y, planes[i].z);
        float len = glm::length(n);
        if (len < 1e-6f) continue;
        n /= len;
        float d = planes[i].w / len;
        if (glm::dot(n, c) + d < -r) return false;
    }
    return true;
}

} // namespace

// ---- Helpers de selección de GPU (métodos privados de Application) ----

bool Application::checkDeviceExtensionSupport(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, exts.data());
    for (const auto& e : exts)
        if (strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) return true;
    return false;
}

bool Application::isDeviceSuitable(VkPhysicalDevice device, VkSurfaceKHR surface) {
    if (!checkDeviceExtensionSupport(device)) return false;
    VkBool32 presentSupport = false;
    vkGetPhysicalDeviceSurfaceSupportKHR(device, 0, surface, &presentSupport);
    return presentSupport == VK_TRUE;
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

Application::Application() : _window(nullptr) {
    // FIX: Los RenderTarget de Vulkan no necesitan dimensiones reales hasta que
    // el contexto Vulkan exista. Se construyen vacíos y se inicializan en
    // create_vulkan_context(). Por ahora solo reservamos los unique_ptr.
    vkShadowTarget     = std::make_unique<RenderTarget>(_width, _height);
    vkBloomTarget      = std::make_unique<RenderTarget>(_width, _height);
    vkPostprocessTarget= std::make_unique<RenderTarget>(_width, _height);
    // FIX: _shadowSystem se crea aquí para que el destructor pueda
    // llamar destroyResources() de forma segura aunque init() no se ejecute.
    _shadowSystem = std::make_unique<Shadow>(2048, 2048);
}

Application::~Application() {
    if (vkDevice != VK_NULL_HANDLE)
        vkDeviceWaitIdle(vkDevice);

    // Render targets Vulkan
    if (vkShadowTarget)      { vkShadowTarget->destroyVulkanResources();      vkShadowTarget.reset(); }
    if (vkBloomTarget)       { vkBloomTarget->destroyVulkanResources();       vkBloomTarget.reset(); }
    if (vkPostprocessTarget) { vkPostprocessTarget->destroyVulkanResources(); vkPostprocessTarget.reset(); }

    // FIX: Comprobar antes de llamar destroyResources() para evitar crash si
    // create_vulkan_context() falló antes de que Shadow tuviera un VkDevice.
    if (_shadowSystem) { _shadowSystem->destroyResources(); _shadowSystem.reset(); }

    // Quad buffer
    if (vkQuadBuffer       != VK_NULL_HANDLE) { vkDestroyBuffer(vkDevice, vkQuadBuffer, nullptr);       vkQuadBuffer       = VK_NULL_HANDLE; }
    if (vkQuadBufferMemory != VK_NULL_HANDLE) { vkFreeMemory(vkDevice, vkQuadBufferMemory, nullptr);     vkQuadBufferMemory = VK_NULL_HANDLE; }

    // Semáforos y fences
    if (vkShadowToGeometrySemaphore)      { vkDestroySemaphore(vkDevice, vkShadowToGeometrySemaphore,      nullptr); vkShadowToGeometrySemaphore      = VK_NULL_HANDLE; }
    if (vkGeometryToLightingSemaphore)    { vkDestroySemaphore(vkDevice, vkGeometryToLightingSemaphore,    nullptr); vkGeometryToLightingSemaphore    = VK_NULL_HANDLE; }
    if (vkLightingToPostprocessSemaphore) { vkDestroySemaphore(vkDevice, vkLightingToPostprocessSemaphore, nullptr); vkLightingToPostprocessSemaphore = VK_NULL_HANDLE; }
    if (vkImageAvailableSemaphore)        { vkDestroySemaphore(vkDevice, vkImageAvailableSemaphore,        nullptr); vkImageAvailableSemaphore        = VK_NULL_HANDLE; }
    if (vkRenderFinishedSemaphore)        { vkDestroySemaphore(vkDevice, vkRenderFinishedSemaphore,        nullptr); vkRenderFinishedSemaphore        = VK_NULL_HANDLE; }
    if (vkInFlightFence)                  { vkDestroyFence(vkDevice, vkInFlightFence, nullptr);             vkInFlightFence                  = VK_NULL_HANDLE; }

    for (auto fb : vkFramebuffers)
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(vkDevice, fb, nullptr);
    vkFramebuffers.clear();

    if (vkRenderPass) { vkDestroyRenderPass(vkDevice, vkRenderPass, nullptr); vkRenderPass = VK_NULL_HANDLE; }

    for (auto view : vkSwapchainImageViews)
        if (view != VK_NULL_HANDLE) vkDestroyImageView(vkDevice, view, nullptr);
    vkSwapchainImageViews.clear();

    if (vkSwapchain)    { vkDestroySwapchainKHR(vkDevice, vkSwapchain, nullptr);    vkSwapchain    = VK_NULL_HANDLE; }
    if (vkCommandPool)  { vkDestroyCommandPool(vkDevice, vkCommandPool, nullptr);    vkCommandPool  = VK_NULL_HANDLE; }

    if (vkDescriptorPool)              { vkDestroyDescriptorPool(vkDevice, vkDescriptorPool, nullptr);                           vkDescriptorPool              = VK_NULL_HANDLE; }
    if (vkMaterialDescriptorSetLayout) { vkDestroyDescriptorSetLayout(vkDevice, vkMaterialDescriptorSetLayout, nullptr);          vkMaterialDescriptorSetLayout = VK_NULL_HANDLE; }
    if (vkUniformDescriptorSetLayout)  { vkDestroyDescriptorSetLayout(vkDevice, vkUniformDescriptorSetLayout, nullptr);           vkUniformDescriptorSetLayout  = VK_NULL_HANDLE; }

    if (vkUniformBuffer)       { vkDestroyBuffer(vkDevice, vkUniformBuffer, nullptr);       vkUniformBuffer       = VK_NULL_HANDLE; }
    if (vkUniformBufferMemory) { vkFreeMemory(vkDevice, vkUniformBufferMemory, nullptr);     vkUniformBufferMemory = VK_NULL_HANDLE; }

    if (vkDevice)   { vkDestroyDevice(vkDevice, nullptr);                     vkDevice   = VK_NULL_HANDLE; }
    if (vkSurface)  { vkDestroySurfaceKHR(vkInstance, vkSurface, nullptr);    vkSurface  = VK_NULL_HANDLE; }
    if (vkInstance) { vkDestroyInstance(vkInstance, nullptr);                  vkInstance = VK_NULL_HANDLE; }

    assert(vkFramebuffers.empty()          && "vkFramebuffers no vacío");
    assert(vkSwapchainImageViews.empty()   && "vkSwapchainImageViews no vacío");
    assert(vkSwapchain         == VK_NULL_HANDLE && "vkSwapchain no destruido");
    assert(vkCommandPool       == VK_NULL_HANDLE && "vkCommandPool no destruido");
    assert(vkDescriptorPool    == VK_NULL_HANDLE && "vkDescriptorPool no destruido");
    assert(vkUniformBuffer     == VK_NULL_HANDLE && "vkUniformBuffer no destruido");
    assert(vkUniformBufferMemory == VK_NULL_HANDLE && "vkUniformBufferMemory no destruido");
    assert(vkDevice            == VK_NULL_HANDLE && "vkDevice no destruido");
    assert(vkInstance          == VK_NULL_HANDLE && "vkInstance no destruido");
}

void Application::cleanup() {
    MotorInstance::getInstance().clear();
    _mainShader.reset();    _lampShader.reset();    _shadowSystem.reset();
    _hdrSystem.reset();     _bloomSystem.reset();   _gBuffer.reset();
    _ssaoSystem.reset();    _iblSystem.reset();     _worldSystem.reset();
    _pointShadowSystem.reset(); _lightingTarget.reset(); _bloomExtractTarget.reset();
    _lightCuller.reset();   _instancing.reset();    _computePostProcess.reset();
    _cascadedShadow.reset(); _virtualTexturing.reset(); _planetarySystem.reset();
    _raycastSystem.reset(); _terrainStreamingSystem.reset();
    _camera.reset();        _currentScene.reset();
    for (auto& lod : sphereLOD) lod.reset();
}

// =============================================================================
// run() — orden correcto del ciclo de vida
// =============================================================================

void Application::run(const std::string& startScenePath) {
    create_window();            // 1. Ventana SDL
    create_vulkan_context();    // 2. Contexto Vulkan (necesita la ventana)

    Haruka::Scene scene;
    if (!startScenePath.empty())
        scene.load(startScenePath);
    else
        scene = Haruka::Scene("DefaultScene");

    init(scene);                // 3. Sistemas del motor (necesita el contexto)
    main_loop();                // 4. Bucle principal
}

// =============================================================================
// create_window()
// =============================================================================

void Application::create_window() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        HARUKA_MOTOR_ERROR(ErrorCode::MOTOR_INIT_FAILED, "Failed to initialize SDL");
        throw std::runtime_error("Fallo al inicializar SDL");
    }

    _window = SDL_CreateWindow("Haruka Engine", _width, _height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    
    if (!_window) {
        HARUKA_MOTOR_ERROR(ErrorCode::WINDOW_CREATION_FAILED, "Failed to create SDL window");
        throw std::runtime_error("Fallo al crear la ventana");
    }
}

// =============================================================================
// create_vulkan_context() — UN solo swapchain, queues recuperadas, sin draw calls
// =============================================================================

void Application::create_vulkan_context() {
    printf("[Haruka] >> create_vulkan_context\n");

    // ------------------------------------------------------------------
    // 1. VkInstance
    // ------------------------------------------------------------------
    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName   = "HarukaEngine";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName        = "HarukaEngine";
    appInfo.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion         = VK_API_VERSION_1_2;

    // 1. Obtener extensiones básicas de SDL
    Uint32 sdlExtCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
    if (!sdlExts) {
        std::cerr << "SDL_Vulkan_GetInstanceExtensions error: " << SDL_GetError() << std::endl;
        throw std::runtime_error("SDL_Vulkan_GetInstanceExtensions failed");
    }

    // 2. Creamos nuestro propio vector para añadir las que FALTAN
    std::vector<const char*> allExtensions;
    for (Uint32 i = 0; i < sdlExtCount; i++) {
        allExtensions.push_back(sdlExts[i]);
    }

    // 3. Añadimos las extensiones manuales que "curan" el error de superficie en Fedora
    allExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    allExtensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    allExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

    // 4. Configuramos el instInfo usando nuestro vector, NO el de SDL directamente
    VkInstanceCreateInfo instInfo{};
    instInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instInfo.pApplicationInfo        = &appInfo;
    instInfo.enabledExtensionCount   = static_cast<uint32_t>(allExtensions.size());
    instInfo.ppEnabledExtensionNames = allExtensions.data();
    instInfo.enabledLayerCount       = 0;

    // IMPORTANTE: Este flag es necesario por la extensión de Portability en drivers modernos
    instInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

    if (vkCreateInstance(&instInfo, nullptr, &vkInstance) != VK_SUCCESS)
        throw std::runtime_error("Fallo al crear VkInstance");

    // --- Diagnóstico justo antes del crash ---
    SDL_ClearError(); 
    if (!SDL_Vulkan_CreateSurface(_window, vkInstance, nullptr, &vkSurface)) {
        // Si vuelve a fallar, este print nos dirá la verdad absoluta:
        std::cerr << "CRASH LOG - SDL Error: " << SDL_GetError() << std::endl;
        throw std::runtime_error("Fallo al crear VkSurfaceKHR con SDL");
    }

    // ------------------------------------------------------------------
    // 3. Physical device
    // ------------------------------------------------------------------
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(vkInstance, &deviceCount, nullptr);
    if (deviceCount == 0) throw std::runtime_error("No hay GPUs con soporte Vulkan");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(vkInstance, &deviceCount, devices.data());

    vkPhysicalDevice = VK_NULL_HANDLE;
    for (const auto& dev : devices) {
        if (isDeviceSuitable(dev, vkSurface)) { vkPhysicalDevice = dev; break; }
    }
    if (vkPhysicalDevice == VK_NULL_HANDLE) throw std::runtime_error("GPU no apta");

    // ------------------------------------------------------------------
    // 4. Logical device
    // ------------------------------------------------------------------
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCI{};
    queueCI.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCI.queueFamilyIndex = 0;
    queueCI.queueCount       = 1;
    queueCI.pQueuePriorities = &queuePriority;

    const std::vector<const char*> devExts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo devCI{};
    devCI.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    devCI.queueCreateInfoCount    = 1;
    devCI.pQueueCreateInfos       = &queueCI;
    devCI.enabledExtensionCount   = (uint32_t)devExts.size();
    devCI.ppEnabledExtensionNames = devExts.data();

    if (vkCreateDevice(vkPhysicalDevice, &devCI, nullptr, &vkDevice) != VK_SUCCESS)
        throw std::runtime_error("Fallo al crear VkDevice");

    // FIX: Recuperar queues después de crear el device.
    // Sin esto vkGraphicsQueue y vkPresentQueue son VK_NULL_HANDLE y
    // todo vkQueueSubmit posterior falla silenciosamente.
    vkGetDeviceQueue(vkDevice, 0, 0, &vkGraphicsQueue);
    vkGetDeviceQueue(vkDevice, 0, 0, &vkPresentQueue);

    // ------------------------------------------------------------------
    // 5. Swapchain — UNA sola creación, usando capabilities reales
    // FIX: La versión anterior contenía una segunda declaración de
    // swapchainCreateInfo más abajo dentro de la misma función, lo que
    // causaba un error de compilación. Se elimina completamente esa
    // segunda declaración y se usa un único bloque con capabilities.
    // ------------------------------------------------------------------
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vkPhysicalDevice, vkSurface, &caps);

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    // Elegir extent seguro: usar el extent actual de la superficie, o
    // el tamaño de ventana si la superficie reporta un extent especial.
    VkExtent2D swapExtent = caps.currentExtent;
    if (swapExtent.width == UINT32_MAX) {
        swapExtent.width  = std::clamp((uint32_t)_width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
        swapExtent.height = std::clamp((uint32_t)_height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    _swapchainExtent = swapExtent;

    VkSwapchainCreateInfoKHR scCI{};
    scCI.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    scCI.surface          = vkSurface;
    scCI.minImageCount    = imageCount;
    scCI.imageFormat      = VK_FORMAT_B8G8R8A8_SRGB;
    scCI.imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scCI.imageExtent      = swapExtent;
    scCI.imageArrayLayers = 1;
    scCI.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    scCI.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    scCI.preTransform     = caps.currentTransform;
    scCI.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    scCI.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    scCI.clipped          = VK_TRUE;
    scCI.oldSwapchain     = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(vkDevice, &scCI, nullptr, &vkSwapchain) != VK_SUCCESS)
        throw std::runtime_error("Fallo al crear Swapchain");

    // Guardar dimensiones reales para usarlas en framebuffers
    _width  = (int)swapExtent.width;
    _height = (int)swapExtent.height;

    // ------------------------------------------------------------------
    // 6. Swapchain images & image views
    // ------------------------------------------------------------------
    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &imgCount, nullptr);
    vkSwapchainImages.resize(imgCount);
    vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &imgCount, vkSwapchainImages.data());

    vkSwapchainImageViews.resize(imgCount);
    for (size_t i = 0; i < imgCount; ++i) {
        VkImageViewCreateInfo viewCI{};
        viewCI.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewCI.image                           = vkSwapchainImages[i];
        viewCI.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        viewCI.format                          = VK_FORMAT_B8G8R8A8_SRGB;
        viewCI.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewCI.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewCI.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewCI.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewCI.subresourceRange.baseMipLevel   = 0;
        viewCI.subresourceRange.levelCount     = 1;
        viewCI.subresourceRange.baseArrayLayer = 0;
        viewCI.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(vkDevice, &viewCI, nullptr, &vkSwapchainImageViews[i]) != VK_SUCCESS)
            throw std::runtime_error("Fallo al crear VkImageView de swapchain");
    }

    // ------------------------------------------------------------------
    // 7. Sync primitives
    // ------------------------------------------------------------------
    VkSemaphoreCreateInfo semCI{};
    semCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(vkDevice, &semCI, nullptr, &vkImageAvailableSemaphore);
    vkCreateSemaphore(vkDevice, &semCI, nullptr, &vkRenderFinishedSemaphore);
    // Semáforos inter-pass (geometry→lighting→postprocess)
    vkCreateSemaphore(vkDevice, &semCI, nullptr, &vkGeometryToLightingSemaphore);
    vkCreateSemaphore(vkDevice, &semCI, nullptr, &vkLightingToPostprocessSemaphore);
    vkCreateSemaphore(vkDevice, &semCI, nullptr, &vkShadowToGeometrySemaphore);

    VkFenceCreateInfo fenceCI{};
    fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCI.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(vkDevice, &fenceCI, nullptr, &vkInFlightFence);

    // ------------------------------------------------------------------
    // 8. Render pass
    // ------------------------------------------------------------------
    VkAttachmentDescription colorAtt{};
    colorAtt.format         = VK_FORMAT_B8G8R8A8_SRGB;
    colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAtt.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpCI{};
    rpCI.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpCI.attachmentCount = 1;
    rpCI.pAttachments    = &colorAtt;
    rpCI.subpassCount    = 1;
    rpCI.pSubpasses      = &subpass;
    rpCI.dependencyCount = 1;
    rpCI.pDependencies   = &dep;

    if (vkCreateRenderPass(vkDevice, &rpCI, nullptr, &vkRenderPass) != VK_SUCCESS)
        throw std::runtime_error("Fallo al crear VkRenderPass");

    // ------------------------------------------------------------------
    // 9. Framebuffers
    // ------------------------------------------------------------------
    vkFramebuffers.resize(vkSwapchainImageViews.size());
    for (size_t i = 0; i < vkSwapchainImageViews.size(); ++i) {
        VkImageView attachments[] = { vkSwapchainImageViews[i] };
        VkFramebufferCreateInfo fbCI{};
        fbCI.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbCI.renderPass      = vkRenderPass;
        fbCI.attachmentCount = 1;
        fbCI.pAttachments    = attachments;
        fbCI.width           = (uint32_t)_width;
        fbCI.height          = (uint32_t)_height;
        fbCI.layers          = 1;
        if (vkCreateFramebuffer(vkDevice, &fbCI, nullptr, &vkFramebuffers[i]) != VK_SUCCESS)
            throw std::runtime_error("Fallo al crear VkFramebuffer");
    }

    // ------------------------------------------------------------------
    // 10. Command pool
    // ------------------------------------------------------------------
    VkCommandPoolCreateInfo poolCI{};
    poolCI.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCI.queueFamilyIndex = 0;
    poolCI.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(vkDevice, &poolCI, nullptr, &vkCommandPool) != VK_SUCCESS)
        throw std::runtime_error("Fallo al crear VkCommandPool");

    // ------------------------------------------------------------------
    // 11. Command buffers — uno por imagen de swapchain
    // ------------------------------------------------------------------
    vkCommandBuffers.resize(vkFramebuffers.size());
    VkCommandBufferAllocateInfo cbAI{};
    cbAI.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAI.commandPool        = vkCommandPool;
    cbAI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAI.commandBufferCount = (uint32_t)vkCommandBuffers.size();
    if (vkAllocateCommandBuffers(vkDevice, &cbAI, vkCommandBuffers.data()) != VK_SUCCESS)
        throw std::runtime_error("Fallo al crear VkCommandBuffers");

    // ------------------------------------------------------------------
    // 12. Descriptor pool + layouts
    // ------------------------------------------------------------------
    // Pool combinado: sampler para materiales + uniform buffer para matrices
    std::vector<VkDescriptorPoolSize> poolSizes = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1   }
    };
    VkDescriptorPoolCreateInfo dpCI{};
    dpCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCI.poolSizeCount = (uint32_t)poolSizes.size();
    dpCI.pPoolSizes    = poolSizes.data();
    dpCI.maxSets       = 129;
    vkCreateDescriptorPool(vkDevice, &dpCI, nullptr, &vkDescriptorPool);

    // Layout para texturas de material
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding         = 0;
    samplerBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo matLayout{};
    matLayout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    matLayout.bindingCount = 1;
    matLayout.pBindings    = &samplerBinding;
    vkCreateDescriptorSetLayout(vkDevice, &matLayout, nullptr, &vkMaterialDescriptorSetLayout);

    // ------------------------------------------------------------------
    // 13. Uniform buffer (view + proj matrices)
    // ------------------------------------------------------------------
    VkDeviceSize uboSize = sizeof(glm::mat4) * 2; // view + proj
    VkBufferCreateInfo uboCI{};
    uboCI.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    uboCI.size        = uboSize;
    uboCI.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    uboCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(vkDevice, &uboCI, nullptr, &vkUniformBuffer);

    VkMemoryRequirements uboMemReq{};
    vkGetBufferMemoryRequirements(vkDevice, vkUniformBuffer, &uboMemReq);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(vkPhysicalDevice, &memProps);

    uint32_t memTypeIdx = 0;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((uboMemReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memTypeIdx = i; break;
        }
    }
    VkMemoryAllocateInfo uboAI{};
    uboAI.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    uboAI.allocationSize  = uboMemReq.size;
    uboAI.memoryTypeIndex = memTypeIdx;
    vkAllocateMemory(vkDevice, &uboAI, nullptr, &vkUniformBufferMemory);
    vkBindBufferMemory(vkDevice, vkUniformBuffer, vkUniformBufferMemory, 0);

    // Layout + descriptor set para el UBO
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding         = 0;
    uboBinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo uboLayout{};
    uboLayout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    uboLayout.bindingCount = 1;
    uboLayout.pBindings    = &uboBinding;
    vkCreateDescriptorSetLayout(vkDevice, &uboLayout, nullptr, &vkUniformDescriptorSetLayout);

    VkDescriptorSetAllocateInfo uboAllocDS{};
    uboAllocDS.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    uboAllocDS.descriptorPool     = vkDescriptorPool;
    uboAllocDS.descriptorSetCount = 1;
    uboAllocDS.pSetLayouts        = &vkUniformDescriptorSetLayout;
    vkAllocateDescriptorSets(vkDevice, &uboAllocDS, &vkUniformDescriptorSet);

    VkDescriptorBufferInfo uboBufInfo{};
    uboBufInfo.buffer = vkUniformBuffer;
    uboBufInfo.offset = 0;
    uboBufInfo.range  = uboSize;
    VkWriteDescriptorSet uboWrite{};
    uboWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    uboWrite.dstSet          = vkUniformDescriptorSet;
    uboWrite.dstBinding      = 0;
    uboWrite.dstArrayElement = 0;
    uboWrite.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboWrite.descriptorCount = 1;
    uboWrite.pBufferInfo     = &uboBufInfo;
    vkUpdateDescriptorSets(vkDevice, 1, &uboWrite, 0, nullptr);

    // Nota: createOffscreenResources() se llama desde HarukaVulkanEngine::initImGui()
    // porque necesita que ImGui_ImplVulkan esté inicializado para crear el descriptor set.
}

// =============================================================================
// init() — sistemas del motor; se llama DESPUÉS de create_vulkan_context()
// =============================================================================

void Application::init(Haruka::Scene& scene) {
    _currentScene = std::make_unique<Haruka::Scene>(scene);

    Camera* sceneCamera = nullptr;
    if (gameInterface && gameInterface->getCamera)
        sceneCamera = gameInterface->getCamera();

    if (sceneCamera) {
        _camera = std::make_unique<Camera>(sceneCamera->position);
        _camera->orientation  = sceneCamera->orientation;
        _camera->zoom         = sceneCamera->zoom;
        _camera->speed        = sceneCamera->speed;
        _camera->sensitivity  = sceneCamera->sensitivity;
    } else {
        _camera = std::make_unique<Camera>(Haruka::WorldPos(0.0, 2.0, 8.0));
    }

    _mainShader = std::make_unique<Shader>("shaders/simple.vert", "shaders/pbr.frag");
    _lampShader = std::make_unique<Shader>("shaders/simple.vert", "shaders/light_cube.frag");

    // FIX: _shadowSystem ya existe (creado en el constructor) — solo lo
    // re-inicializamos con la resolución de producción de 1024x1024.
    // Si no lo re-creamos, usamos el 2048x2048 del constructor. Comentar
    // la siguiente línea si quieres conservar la resolución del constructor.
    _shadowSystem = std::make_unique<Shadow>(1024, 1024);

    _hdrSystem          = std::make_unique<HDR>(_width, _height);
    _bloomSystem        = std::make_unique<Bloom>(_width, _height);
    _gBuffer            = std::make_unique<GBuffer>(_width, _height);
    _ssaoSystem         = std::make_unique<SSAO>(_width, _height);
    _iblSystem          = std::make_unique<IBL>();
    _worldSystem        = std::make_unique<Haruka::WorldSystem>();
    _pointShadowSystem  = std::make_unique<PointShadow>(1024, 1024);
    _lightingTarget     = std::make_unique<RenderTarget>(_width, _height);
    _bloomExtractTarget = std::make_unique<RenderTarget>(_width, _height);

    if (!MotorInstance::getInstance().getRenderTarget())
        MotorInstance::getInstance().setRenderTarget(_lightingTarget.get());
    MotorInstance::getInstance().setApplication(this);

    _lightCuller        = std::make_unique<LightCuller>();
    _instancing         = std::make_unique<GPUInstancing>();

    AssetStreamer::getInstance().init(512, 2);
    DebugOverlay::getInstance().init();

    _computePostProcess = std::make_unique<ComputePostprocess>(_width, _height);
    _cascadedShadow     = std::make_unique<CascadedShadow>(_width, _height, 4);
    _virtualTexturing   = std::make_unique<VirtualTexturing>(_width, _height);
    _planetarySystem    = std::make_unique<Haruka::PlanetarySystem>();
    _raycastSystem      = std::make_unique<RaycastSimple>();
    _terrainStreamingSystem = std::make_unique<Haruka::TerrainStreamingSystem>();

    int lodCfg[4][2] = { {64,32},{32,16},{16,8},{8,4} };
    for (int i = 0; i < 4; i++) {
        std::vector<glm::vec3> v, n;
        std::vector<unsigned int> idx;
        PrimitiveShapes::createSphere(1.0f, lodCfg[i][0], lodCfg[i][1], v, n, idx);
        sphereLOD[i] = std::make_unique<SimpleMesh>(v, idx);
    }

    _worldSystem->initComputeShaders();

    _geomShader         = std::make_unique<Shader>("shaders/deferred_geom.vert",   "shaders/deferred_geom.frag");
    _ssaoShader         = std::make_unique<Shader>("shaders/screenquad.vert",       "shaders/ssao.frag");
    _lightShader        = std::make_unique<Shader>("shaders/screenquad.vert",       "shaders/deferred_light.frag");
    _compositeShader    = std::make_unique<Shader>("shaders/screenquad.vert",       "shaders/bloom_composite.frag");
    _flatShader         = std::make_unique<Shader>("shaders/simple.vert",           "shaders/light_cube.frag");
    _cascadeShadowShader= std::make_unique<Shader>("shaders/shadow.vert",           "shaders/shadow.frag");

    // FIX: setupQuad() necesita vkDevice, que ahora sí existe (fue creado
    // en create_vulkan_context, que se llama antes que init en run()).
    setupQuad();
}

// =============================================================================
// setupQuad() — vertex buffer del fullscreen quad para post-proceso
// =============================================================================

void Application::setupQuad() {
    // Triangle-strip que cubre toda la pantalla en NDC.
    // Se usa en los passes de lighting, postprocess y present.
    float quadVertices[] = {
        -1.0f,  1.0f,  0.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
    };
    VkDeviceSize bufferSize = sizeof(quadVertices);

    VkBufferCreateInfo bCI{};
    bCI.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bCI.size        = bufferSize;
    bCI.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vkDevice, &bCI, nullptr, &vkQuadBuffer) != VK_SUCCESS)
        throw std::runtime_error("Fallo al crear VkBuffer para quad");

    VkMemoryRequirements memReq{};
    vkGetBufferMemoryRequirements(vkDevice, vkQuadBuffer, &memReq);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(vkPhysicalDevice, &memProps);
    uint32_t memIdx = 0;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memIdx = i; break;
        }
    }
    VkMemoryAllocateInfo mAI{};
    mAI.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mAI.allocationSize  = memReq.size;
    mAI.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(vkDevice, &mAI, nullptr, &vkQuadBufferMemory) != VK_SUCCESS)
        throw std::runtime_error("Fallo al asignar memoria para quad");
    vkBindBufferMemory(vkDevice, vkQuadBuffer, vkQuadBufferMemory, 0);

    void* data = nullptr;
    vkMapMemory(vkDevice, vkQuadBufferMemory, 0, bufferSize, 0, &data);
    memcpy(data, quadVertices, (size_t)bufferSize);
    vkUnmapMemory(vkDevice, vkQuadBufferMemory);
}

// =============================================================================
// recordCommandBuffer() — graba un frame en un command buffer
// =============================================================================

void Application::recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
        throw std::runtime_error("Fallo al empezar a grabar command buffer");

    // Delegar al método que contiene todos los passes de render.
    renderFrameContentVulkan(commandBuffer, imageIndex);

    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
        throw std::runtime_error("Fallo al cerrar command buffer");
}

// =============================================================================
// main_loop()
// =============================================================================

void Application::main_loop() {
    bool running = true;
    SDL_Event event;
    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
        }
        renderFrame();
    }
    vkDeviceWaitIdle(vkDevice);
}

// =============================================================================
// renderFrame() — acquire → record → submit (1 vez) → present
//
// FIX CRÍTICO: La versión anterior hacía 3 vkQueueSubmit encadenados sobre el
// MISMO command buffer en el mismo frame. Vulkan no permite someter el mismo
// command buffer varias veces sin sincronización de fence entre submissions.
// En Wayland esto provocaba VK_ERROR_DEVICE_LOST silencioso.
//
// Solución: UNA sola grabación (recordCommandBuffer) y UN solo vkQueueSubmit
// por frame. Todos los passes (geometry, lighting, postprocess, present) van
// dentro de esa única grabación.
// =============================================================================

void Application::renderFrame() {
    auto frameStart = std::chrono::high_resolution_clock::now();

    vkWaitForFences(vkDevice, 1, &vkInFlightFence, VK_TRUE, UINT64_MAX);
    vkResetFences(vkDevice, 1, &vkInFlightFence);

    uint32_t imageIndex = 0;
    VkResult acquireResult = vkAcquireNextImageKHR(
        vkDevice, vkSwapchain, UINT64_MAX, vkImageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR) {
        recreateSwapchainAndResources(); return;
    } else if (acquireResult != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vkAcquireNextImageKHR: %d\n", acquireResult);
        recreateSwapchainAndResources(); return;
    }

    // Grabar todos los passes en un único command buffer
    vkResetCommandBuffer(vkCommandBuffers[imageIndex], 0);
    recordCommandBuffer(vkCommandBuffers[imageIndex], imageIndex);

    // FIX: Un solo Submit. El semáforo de espera es vkImageAvailableSemaphore
    // (la imagen del swapchain está lista) y el de señal es vkRenderFinishedSemaphore
    // (el frame está listo para presentar). Los semáforos inter-pass
    // (geometry↔lighting↔postprocess) quedan reservados para cuando se usen
    // command buffers separados por pass.
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount   = 1;
    submitInfo.pWaitSemaphores      = &vkImageAvailableSemaphore;
    submitInfo.pWaitDstStageMask    = &waitStage;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &vkCommandBuffers[imageIndex];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores    = &vkRenderFinishedSemaphore;

    if (vkQueueSubmit(vkGraphicsQueue, 1, &submitInfo, vkInFlightFence) != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vkQueueSubmit falló\n");
        recreateSwapchainAndResources(); return;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = &vkRenderFinishedSemaphore;
    presentInfo.swapchainCount     = 1;
    presentInfo.pSwapchains        = &vkSwapchain;
    presentInfo.pImageIndices      = &imageIndex;

    VkResult presentResult = vkQueuePresentKHR(vkPresentQueue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        recreateSwapchainAndResources();
    } else if (presentResult != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vkQueuePresentKHR: %d\n", presentResult);
        recreateSwapchainAndResources();
    }

    auto frameEnd = std::chrono::high_resolution_clock::now();
    g_lastFrameTimeMs = (float)std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
    g_frameCount++;
    double now = std::chrono::duration<double>(frameEnd.time_since_epoch()).count();
    if (now - g_lastFpsTime > 1.0) {
        g_lastFps    = (float)(g_frameCount / (now - g_lastFpsTime));
        g_lastFpsTime = now;
        g_frameCount  = 0;
    }
    FrameMetrics metrics;
    metrics.fps         = g_lastFps;
    metrics.frameTimeMs = g_lastFrameTimeMs;
    metrics.gpuTimeMs   = g_lastGpuTimeMs;
    DebugOverlay::getInstance().updateMetrics(metrics);
}

// =============================================================================
// recreateSwapchainAndResources() — resize / surface out-of-date
//
// FIX: Eliminado el bucle de re-grabación de command buffers al final.
// Los command buffers se graban en renderFrame() cada frame, con
// vkResetCommandBuffer antes de cada grabación, por lo que no hace falta
// pre-grabarlos aquí. Hacerlo causaba que buildRenderQueue() se llamara
// con la escena posiblemente no inicializada.
// =============================================================================

// ─── helpers offscreen ───────────────────────────────────────────────────────
static uint32_t findMemoryTypeApp(VkPhysicalDevice physDev, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physDev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((filter & (1 << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    throw std::runtime_error("No suitable memory type for offscreen image");
}

void Application::createOffscreenResources() {
    destroyOffscreenResources();

    VkFormat fmt = VK_FORMAT_B8G8R8A8_SRGB;

    // 1. Imagen
    VkImageCreateInfo imgCI{};
    imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = fmt;
    imgCI.extent        = { (uint32_t)_width, (uint32_t)_height, 1 };
    imgCI.mipLevels     = 1;
    imgCI.arrayLayers   = 1;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                        | VK_IMAGE_USAGE_SAMPLED_BIT
                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateImage(vkDevice, &imgCI, nullptr, &_offscreenImage);

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vkDevice, _offscreenImage, &mr);
    VkMemoryAllocateInfo allocI{};
    allocI.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocI.allocationSize  = mr.size;
    allocI.memoryTypeIndex = findMemoryTypeApp(vkPhysicalDevice, mr.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(vkDevice, &allocI, nullptr, &_offscreenMemory);
    vkBindImageMemory(vkDevice, _offscreenImage, _offscreenMemory, 0);

    // 2. Image view
    VkImageViewCreateInfo viewCI{};
    viewCI.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCI.image                           = _offscreenImage;
    viewCI.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format                          = fmt;
    viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.levelCount     = 1;
    viewCI.subresourceRange.layerCount     = 1;
    vkCreateImageView(vkDevice, &viewCI, nullptr, &_offscreenImageView);

    // 3. Sampler
    VkSamplerCreateInfo sampCI{};
    sampCI.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter    = VK_FILTER_LINEAR;
    sampCI.minFilter    = VK_FILTER_LINEAR;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vkDevice, &sampCI, nullptr, &_offscreenSampler);

    // 4. Framebuffer (usando el mismo render pass del swapchain)
    VkFramebufferCreateInfo fbCI{};
    fbCI.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbCI.renderPass      = vkRenderPass;
    fbCI.attachmentCount = 1;
    fbCI.pAttachments    = &_offscreenImageView;
    fbCI.width           = (uint32_t)_width;
    fbCI.height          = (uint32_t)_height;
    fbCI.layers          = 1;
    vkCreateFramebuffer(vkDevice, &fbCI, nullptr, &_offscreenFramebuffer);

    // 5. Descriptor set para ImGui::Image()
    //    ImGui_ImplVulkan_AddTexture crea un VkDescriptorSet listo para usar
    _offscreenDescSet = VK_NULL_HANDLE;
    
    fprintf(stderr, "[Haruka] Offscreen resources created %dx%d\n", _width, _height);
}

void Application::destroyOffscreenResources() {
    if (!vkDevice) return;
    vkDeviceWaitIdle(vkDevice);

    if (_offscreenFramebuffer) {
        vkDestroyFramebuffer(vkDevice, _offscreenFramebuffer, nullptr);
        _offscreenFramebuffer = VK_NULL_HANDLE;
    }
    if (_offscreenSampler) {
        vkDestroySampler(vkDevice, _offscreenSampler, nullptr);
        _offscreenSampler = VK_NULL_HANDLE;
    }
    if (_offscreenImageView) {
        vkDestroyImageView(vkDevice, _offscreenImageView, nullptr);
        _offscreenImageView = VK_NULL_HANDLE;
    }
    if (_offscreenImage) {
        vkDestroyImage(vkDevice, _offscreenImage, nullptr);
        _offscreenImage = VK_NULL_HANDLE;
    }
    if (_offscreenMemory) {
        vkFreeMemory(vkDevice, _offscreenMemory, nullptr);
        _offscreenMemory = VK_NULL_HANDLE;
    }
}

void Application::recreateSwapchainAndResources() {
    vkDeviceWaitIdle(vkDevice);

    // Destruir recursos dependientes del swapchain
    for (auto fb : vkFramebuffers)
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(vkDevice, fb, nullptr);
    vkFramebuffers.clear();

    for (auto view : vkSwapchainImageViews)
        if (view != VK_NULL_HANDLE) vkDestroyImageView(vkDevice, view, nullptr);
    vkSwapchainImageViews.clear();

    if (vkSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(vkDevice, vkSwapchain, nullptr);
        vkSwapchain = VK_NULL_HANDLE;
    }

    // Re-consultar capabilities (el tamaño de la ventana puede haber cambiado)
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vkPhysicalDevice, vkSurface, &caps);

    VkExtent2D swapExtent = caps.currentExtent;
    if (swapExtent.width == UINT32_MAX) {
        swapExtent.width  = std::clamp((uint32_t)_width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
        swapExtent.height = std::clamp((uint32_t)_height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    _width  = (int)swapExtent.width;
    _height = (int)swapExtent.height;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR scCI{};
    scCI.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    scCI.surface          = vkSurface;
    scCI.minImageCount    = imageCount;
    scCI.imageFormat      = VK_FORMAT_B8G8R8A8_SRGB;
    scCI.imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scCI.imageExtent      = swapExtent;
    scCI.imageArrayLayers = 1;
    scCI.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    scCI.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    scCI.preTransform     = caps.currentTransform;
    scCI.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    scCI.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    scCI.clipped          = VK_TRUE;
    scCI.oldSwapchain     = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(vkDevice, &scCI, nullptr, &vkSwapchain) != VK_SUCCESS)
        throw std::runtime_error("Fallo al recrear VkSwapchainKHR");

    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &imgCount, nullptr);
    vkSwapchainImages.resize(imgCount);
    vkGetSwapchainImagesKHR(vkDevice, vkSwapchain, &imgCount, vkSwapchainImages.data());

    vkSwapchainImageViews.resize(imgCount);
    for (size_t i = 0; i < imgCount; ++i) {
        VkImageViewCreateInfo viewCI{};
        viewCI.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewCI.image                           = vkSwapchainImages[i];
        viewCI.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        viewCI.format                          = VK_FORMAT_B8G8R8A8_SRGB;
        viewCI.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewCI.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewCI.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewCI.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewCI.subresourceRange.baseMipLevel   = 0;
        viewCI.subresourceRange.levelCount     = 1;
        viewCI.subresourceRange.baseArrayLayer = 0;
        viewCI.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(vkDevice, &viewCI, nullptr, &vkSwapchainImageViews[i]) != VK_SUCCESS)
            throw std::runtime_error("Fallo al recrear VkImageView");
    }

    vkFramebuffers.resize(vkSwapchainImageViews.size());
    for (size_t i = 0; i < vkSwapchainImageViews.size(); ++i) {
        VkImageView attachments[] = { vkSwapchainImageViews[i] };
        VkFramebufferCreateInfo fbCI{};
        fbCI.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbCI.renderPass      = vkRenderPass;
        fbCI.attachmentCount = 1;
        fbCI.pAttachments    = attachments;
        fbCI.width           = (uint32_t)_width;
        fbCI.height          = (uint32_t)_height;
        fbCI.layers          = 1;
        if (vkCreateFramebuffer(vkDevice, &fbCI, nullptr, &vkFramebuffers[i]) != VK_SUCCESS)
            throw std::runtime_error("Fallo al recrear VkFramebuffer");
    }

    // Si el número de imágenes cambió, reasignar command buffers
    if (vkCommandBuffers.size() != vkFramebuffers.size()) {
        vkFreeCommandBuffers(vkDevice, vkCommandPool, (uint32_t)vkCommandBuffers.size(), vkCommandBuffers.data());
        vkCommandBuffers.resize(vkFramebuffers.size());
        VkCommandBufferAllocateInfo cbAI{};
        cbAI.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbAI.commandPool        = vkCommandPool;
        cbAI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAI.commandBufferCount = (uint32_t)vkCommandBuffers.size();
        vkAllocateCommandBuffers(vkDevice, &cbAI, vkCommandBuffers.data());
    }

    // Recrear offscreen tras resize
    createOffscreenResources();
}

// =============================================================================
// buildRenderQueue() — construye g_sceneRenderQueue con frustum culling
// =============================================================================

void Application::buildRenderQueue() {
    Haruka::Scene* scene = MotorInstance::getInstance().getScene();
    if (!scene) scene = _currentScene.get();
    g_sceneForRender = scene;
    g_sceneRenderQueue.clear();
    if (!scene) return;

    Camera* activeCamera = MotorInstance::getInstance().getCamera();
    if (!activeCamera) activeCamera = _camera.get();

    const double aspect = (_height > 0) ? (double)_width / (double)_height : (16.0 / 9.0);

    glm::dvec3 camPos(0.0);
    glm::mat4 viewProj(1.0f);
    bool canCull = false;

    if (activeCamera) {
        camPos = activeCamera->position;
        glm::mat4 proj = glm::perspective(glm::radians(activeCamera->zoom),
                                          (float)aspect, 0.0001f, 300000000.0f);
        viewProj = proj * activeCamera->getViewMatrix();
        canCull  = true;
    }

    Haruka::TerrainStreamingStats terrainStats;
    if (_terrainStreamingSystem)
        _terrainStreamingSystem->update(scene, _worldSystem.get(), _planetarySystem.get(),
                                        _raycastSystem.get(), activeCamera, &terrainStats);

    s_lastVisibleChunks       = terrainStats.visibleChunks;
    s_lastResidentChunks      = terrainStats.residentChunks;
    s_lastPendingChunkLoads   = terrainStats.pendingChunkLoads;
    s_lastPendingChunkEvictions = terrainStats.pendingChunkEvictions;
    s_lastResidentMemoryMB    = terrainStats.residentMemoryMB;
    s_lastTrackedChunks       = terrainStats.trackedChunks;
    s_lastMaxMemoryMB         = terrainStats.maxMemoryMB;

    for (auto& obj : scene->getObjectsMutable()) {
        if (isRenderDisabledByEditor(obj)) continue;
        if (!isTerrainChunkFacingCamera(obj, activeCamera ? glm::vec3(activeCamera->position) : glm::vec3(0.f))) continue;

        Haruka::ObjectType objType = Haruka::stringToObjectType(obj.type);
        if (!Haruka::isRenderableObjectType(objType)) continue;

        int layer = std::clamp(obj.renderLayer, 1, 5);
        const glm::dvec3 worldPos   = obj.getWorldPosition(scene);
        const glm::dvec3 worldScale = obj.getWorldScale(scene);
        double maxScale = std::max(std::abs(worldScale.x), std::max(std::abs(worldScale.y), std::abs(worldScale.z)));
        const bool isHugeBody = (maxScale > 1000.0) || (obj.name == "Earth") || (obj.name == "Sun");

        if (!isHugeBody) {
            double unloadDist = (double)s_layerMaxDistance[layer];
            double loadDist   = unloadDist * 0.85;
            if (layer >= 4 && obj.meshRenderer) {
                glm::dvec3 diff = worldPos - camPos;
                double distSq   = glm::dot(diff, diff);
                if (distSq > unloadDist * unloadDist * 1.32)
                    maybeReleasePrimitiveMesh(obj);
                else if (!obj.meshRenderer->isResident() && distSq < loadDist * loadDist)
                    buildPrimitiveMeshFromProperties(obj);
            }
            if (layer >= 4 && !obj.modelPath.empty()) {
                glm::dvec3 diff = worldPos - camPos;
                if (glm::dot(diff, diff) > unloadDist * unloadDist * 1.32)
                    releaseModelFromCache(obj.modelPath);
            }
        }

        if (isHugeBody) { g_sceneRenderQueue.push_back(&obj); continue; }

        if (canCull) {
            double radius = std::max(0.5 * glm::length(worldScale), maxScale);
            if (radius < 0.001) radius = 0.5;

            if (layer != 1) {
                static constexpr double qMul[4] = {0.45, 0.70, 1.00, 1.35};
                glm::dvec3 diff = worldPos - camPos;
                double dist = std::sqrt(glm::dot(diff, diff));
                if (dist > radius + (double)s_layerMaxDistance[layer] * qMul[s_renderQualityPreset]) continue;
            }
            if (!isSphereInsideCameraFrustum(worldPos, radius, viewProj)) continue;
        }
        g_sceneRenderQueue.push_back(&obj);
    }
}

// =============================================================================
// renderFrameContentVulkan() — graba todos los render passes en cmd
// =============================================================================

void Application::renderFrameContentVulkan(VkCommandBuffer cmd, uint32_t imageIndex) {
    buildRenderQueue();

    Camera* cam = MotorInstance::getInstance().getCamera();
    if (!cam) cam = _camera.get();
    if (!cam) return;

    // Actualizar UBO (view + proj)
    struct ViewProjUBO { glm::mat4 view; glm::mat4 proj; } vpUbo;
    vpUbo.view = cam->getViewMatrix();
    vpUbo.proj = glm::perspective(glm::radians(cam->zoom),
                                  (float)_width / (float)_height, 0.0001f, 300000000.0f);
    void* data = nullptr;
    vkMapMemory(vkDevice, vkUniformBufferMemory, 0, sizeof(ViewProjUBO), 0, &data);
    memcpy(data, &vpUbo, sizeof(ViewProjUBO));
    vkUnmapMemory(vkDevice, vkUniformBufferMemory);

    // ------------------------------------------------------------------
    // GEOMETRY PASS — dibuja en el framebuffer OFFSCREEN
    // Al final del command buffer se hace blit al swapchain para presentar,
    // y el offscreen queda disponible como textura para ImGui::Image().
    // ------------------------------------------------------------------

    // Transición offscreen: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
    if (_offscreenImage != VK_NULL_HANDLE) {
        VkImageMemoryBarrier barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = _offscreenImage;
        barrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcAccessMask       = 0;
        barrier.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    VkClearValue clearValues[2]{};
    clearValues[0].color        = { {0.05f, 0.05f, 0.08f, 1.0f} };
    clearValues[1].depthStencil = { 1.0f, 0 };

    VkFramebuffer targetFB = (_offscreenFramebuffer != VK_NULL_HANDLE)
                           ? _offscreenFramebuffer
                           : vkFramebuffers[imageIndex];

    VkRenderPassBeginInfo geomPass{};
    geomPass.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    geomPass.renderPass        = vkRenderPass;
    geomPass.framebuffer       = targetFB;
    geomPass.renderArea.offset = {0, 0};
    geomPass.renderArea.extent = { (uint32_t)_width, (uint32_t)_height };
    geomPass.clearValueCount   = 2;
    geomPass.pClearValues      = clearValues;

    vkCmdBeginRenderPass(cmd, &geomPass, VK_SUBPASS_CONTENTS_INLINE);

    // Solo dibujar si el pipeline existe (se crea cuando los shaders SPIR-V estén compilados)
    if (vkGeometryPipeline != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkGeometryPipeline);
        for (const auto* obj : g_sceneRenderQueue) {
            if (!obj) continue;
            glm::mat4 model = obj->getWorldTransform(g_sceneForRender);
            if (vkGeometryPipelineLayout != VK_NULL_HANDLE)
                vkCmdPushConstants(cmd, vkGeometryPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &model);
            if (vkUniformDescriptorSet != VK_NULL_HANDLE && vkGeometryPipelineLayout != VK_NULL_HANDLE)
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkGeometryPipelineLayout, 0, 1, &vkUniformDescriptorSet, 0, nullptr);
            if (obj->meshRenderer && obj->meshRenderer->isResident())
                obj->meshRenderer->getMesh()->draw(cmd);
            else if (!obj->modelPath.empty()) {
                auto mdl = getOrLoadModel(obj->modelPath);
                if (mdl) mdl->draw(cmd);
            }
        }
    }

    if (_imguiCallback)
        _imguiCallback(cmd, imageIndex);

    vkCmdEndRenderPass(cmd);

    // ------------------------------------------------------------------
    // BLIT: offscreen → swapchain image (para presentar en pantalla)
    // Y transición del offscreen a SHADER_READ_ONLY para ImGui::Image()
    // ------------------------------------------------------------------
    if (_offscreenImage != VK_NULL_HANDLE) {
        // Transición offscreen: COLOR_ATTACHMENT → TRANSFER_SRC
        VkImageMemoryBarrier toSrc{};
        toSrc.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrc.oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image               = _offscreenImage;
        toSrc.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        toSrc.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toSrc);

        // Transición swapchain: UNDEFINED → TRANSFER_DST
        VkImageMemoryBarrier swapToDst{};
        swapToDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        swapToDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        swapToDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        swapToDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        swapToDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        swapToDst.image               = vkSwapchainImages[imageIndex];
        swapToDst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        swapToDst.srcAccessMask       = 0;
        swapToDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &swapToDst);

        // Blit
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[0]  = { 0, 0, 0 };
        blit.srcOffsets[1]  = { _width, _height, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[0]  = { 0, 0, 0 };
        blit.dstOffsets[1]  = { _width, _height, 1 };
        vkCmdBlitImage(cmd,
            _offscreenImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            vkSwapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        // Transición swapchain: TRANSFER_DST → PRESENT_SRC
        VkImageMemoryBarrier swapToPresent{};
        swapToPresent.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        swapToPresent.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        swapToPresent.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        swapToPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        swapToPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        swapToPresent.image               = vkSwapchainImages[imageIndex];
        swapToPresent.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        swapToPresent.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        swapToPresent.dstAccessMask       = 0;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &swapToPresent);

        // Transición offscreen: TRANSFER_SRC → SHADER_READ_ONLY (para ImGui::Image)
        VkImageMemoryBarrier offToRead{};
        offToRead.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        offToRead.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        offToRead.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        offToRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        offToRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        offToRead.image               = _offscreenImage;
        offToRead.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        offToRead.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        offToRead.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &offToRead);
    }
    if (vkLightingTarget && vkLightingTarget->vkFramebuffer != VK_NULL_HANDLE &&
        vkLightingPipeline != VK_NULL_HANDLE) {
        VkClearValue lc[2]{};
        lc[0].color = { {0.f,0.f,0.f,1.f} };
        lc[1].depthStencil = {1.f, 0};
        VkRenderPassBeginInfo lp{};
        lp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        lp.renderPass        = vkRenderPass;
        lp.framebuffer       = vkLightingTarget->vkFramebuffer;
        lp.renderArea.offset = {0, 0};
        lp.renderArea.extent = { (uint32_t)_width, (uint32_t)_height };
        lp.clearValueCount   = 2;
        lp.pClearValues      = lc;
        vkCmdBeginRenderPass(cmd, &lp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkLightingPipeline);
        if (vkUniformDescriptorSet != VK_NULL_HANDLE && vkLightingPipelineLayout != VK_NULL_HANDLE)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkLightingPipelineLayout, 0, 1, &vkUniformDescriptorSet, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // ------------------------------------------------------------------
    // POSTPROCESS PASS (solo si el framebuffer y pipeline existen)
    // ------------------------------------------------------------------
    if (vkPostprocessTarget && vkPostprocessTarget->vkFramebuffer != VK_NULL_HANDLE &&
        vkPostprocessPipeline != VK_NULL_HANDLE) {
        VkClearValue pc[1]{};
        pc[0].color = { {0.f,0.f,0.f,1.f} };
        VkRenderPassBeginInfo pp{};
        pp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pp.renderPass        = vkRenderPass;
        pp.framebuffer       = vkPostprocessTarget->vkFramebuffer;
        pp.renderArea.offset = {0, 0};
        pp.renderArea.extent = { (uint32_t)_width, (uint32_t)_height };
        pp.clearValueCount   = 1;
        pp.pClearValues      = pc;
        vkCmdBeginRenderPass(cmd, &pp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPostprocessPipeline);
        if (vkUniformDescriptorSet != VK_NULL_HANDLE && vkPostprocessPipelineLayout != VK_NULL_HANDLE)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPostprocessPipelineLayout, 0, 1, &vkUniformDescriptorSet, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }
}

// =============================================================================
// renderScene() — mantiene g_sceneRenderQueue para uso externo (editor, etc.)
// La lógica real vive en buildRenderQueue(); este método es un wrapper.
// =============================================================================

void Application::renderScene(Shader* /*shader*/) {
    buildRenderQueue();
}
// loadScene() — carga una escena en tiempo de ejecución
void Application::loadScene(const std::string& scenePath) {
    Haruka::Scene scene;
    if (scene.load(scenePath)) {
        _currentScene = std::make_unique<Haruka::Scene>(scene);
        MotorInstance::getInstance().setScene(_currentScene.get());
    }
}

// renderFrameContent() — alias para compatibilidad con código existente
void Application::renderFrameContent() {
    // El trabajo real lo hace renderFrameContentVulkan(), llamado desde renderFrame().
    // Este método existe para satisfacer referencias del editor.
}