#include <string>
#include "rhi/rhi_device.h"
#include "rhi/opengl/gl_device.h"
// #include "rhi/vulkan/vk_device.h"   // F5: cuando exista el backend Vulkan

#include <cstdio>
#include "core/logger.h"

namespace Haruka::RHI
{
    // Crea el device del backend GL. Devuelve null si el contexto no quedó listo.
    static std::unique_ptr<Device> createGL(SDL_Window* window)
    {
        auto gl = std::make_unique<opengl::GLDevice>(window);
        if (gl && gl->ready()) return gl;
        HARUKA_LOGE("RHI", "No se pudo crear el device OpenGL (contexto GL inválido).");
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
            HARUKA_LOGW("RHI", "Vulkan solicitado → fallback a OpenGL.");
        }

        return createGL(window);   // OpenGL del RHI: backend por defecto y fallback
    }

    static Device* g_device = nullptr;
    static bool    g_torndown = false;   // true tras setDevice(nullptr): no resucitar el device

    Device* device()
    {
        // Red de seguridad: si nadie fijó un device pero hay un contexto GL activo, crea uno
        // del RHI (backend OpenGL) adoptando ese contexto. Así los wrappers del renderer pueden
        // asumir que device() NUNCA es null (dentro de un contexto GL) → sin ramas de GL-directo.
        // El juego llama setDevice() en el arranque, así que esta rama casi nunca se usa.
        //
        // OJO con g_torndown: tras el cierre NO se puede resucitar un device. Los destructores de
        // recursos (Mesh, Texture...) llaman a device()->destroy(); si se ejecutan DESPUÉS de que
        // el device muera (cachés estáticas, miembros destruidos más tarde) y aquí creásemos uno
        // nuevo, estaríamos tocando GL sobre un contexto agonizante. Devolver null es lo correcto:
        // esos destructores ya comprueban `if (Device* dev = device())` y se vuelven no-op, y los
        // objetos GL ya los liberó el destructor del device al barrer sus pools.
        if (!g_device && !g_torndown && SDL_GL_GetCurrentContext())
        {
            // Fuga intencionada: NO se destruye al salir (el contexto GL ya no existiría →
            // glDelete* sobre un contexto muerto = crash). El SO recupera la memoria al terminar.
            g_device = new opengl::GLDevice(nullptr);
            HARUKA_LOGI("RHI", "device() sin setDevice previo → GLDevice del RHI creado bajo demanda.");
        }
        return g_device;
    }

    // setDevice(nullptr) = CIERRE: el global deja de apuntar al device y no se vuelve a crear
    // ninguno. Llamarlo JUSTO ANTES de destruir el device — si no, g_device queda COLGANDO y el
    // primer destructor de recurso que corra después llama un virtual sobre memoria liberada
    // ("pure virtual method called").
    void setDevice(Device* d)
    {
        g_device = d;
        if (!d) g_torndown = true;
    }

    namespace opengl { std::string& shaderIncludeDir(); }   // definido en gl_device.cpp

    void setShaderIncludeDir(const std::string& dir)
    {
        std::string d = dir;
        if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
        opengl::shaderIncludeDir() = d;
    }
}
