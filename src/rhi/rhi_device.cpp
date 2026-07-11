#include "rhi/rhi_device.h"
#include "rhi/opengl/gl_device.h"
// #include "rhi/vulkan/vk_device.h"   // F5: cuando exista el backend Vulkan

#include <cstdio>

namespace Haruka::RHI
{
    // Crea el device del backend GL. Devuelve null si el contexto no quedó listo.
    static std::unique_ptr<Device> createGL(SDL_Window* window)
    {
        auto gl = std::make_unique<opengl::GLDevice>(window);
        if (gl && gl->ready()) return gl;
        std::fprintf(stderr, "[RHI] No se pudo crear el device OpenGL (contexto GL inválido).\n");
        return nullptr;
    }

    std::unique_ptr<Device> Device::create(Backend backend, SDL_Window* window)
    {
        // El backend OpenGL del RHI (GLDevice) es el FALLBACK universal: cualquier backend
        // solicitado que no exista o falle al inicializar cae aquí. Es el camino garantizado.
        if (backend == Backend::Vulkan)
        {
            // F5: cuando exista el backend Vulkan, intentarlo y devolverlo si arranca:
            //   auto vk = std::make_unique<vulkan::VKDevice>(window);
            //   if (vk && vk->ready()) return vk;
            //   std::fprintf(stderr, "[RHI] Vulkan falló al inicializar → fallback a OpenGL (RHI).\n");
            std::fprintf(stderr, "[RHI] Vulkan solicitado → fallback a OpenGL (RHI).\n");
        }

        return createGL(window);   // OpenGL del RHI: backend por defecto y fallback
    }

    static Device* g_device = nullptr;
    Device* device()             { return g_device; }
    void    setDevice(Device* d) { g_device = d; }
}
