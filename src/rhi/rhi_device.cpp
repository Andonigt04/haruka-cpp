#include <string>
#include "rhi/rhi_device.h"
#include "rhi/opengl/gl_device.h"

// El backend Vulkan es OPCIONAL y sigue en migración: solo se compila si el SDK está presente.
// Sin él (máquinas sin cabeceras Vulkan), la petición de Vulkan cae al fallback de OpenGL.
#if defined(__has_include)
#  if __has_include(<vulkan/vulkan.h>)
#    define HARUKA_RHI_HAS_VULKAN 1
#    include "rhi/vulkan/vk_device.h"
#  endif
#endif

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

#if defined(HARUKA_RHI_HAS_VULKAN)
    static std::unique_ptr<Device> createVK(SDL_Window* window,
                                            const std::vector<std::string>& preferredGpus)
    {
        auto vk = std::make_unique<vulkan::VKDevice>(window, preferredGpus);
        if (vk && vk->ready()) return vk;
        HARUKA_LOGE("RHI", "No se pudo crear el device Vulkan (contexto VK inválido).");
        return nullptr;
    }
#endif

    std::unique_ptr<Device> Device::create(Backend backend, SDL_Window* window,
                                           const std::vector<std::string>& preferredGpus)
    {
        // El backend OpenGL del RHI (GLDevice) es el FALLBACK universal: cualquier backend
        // solicitado que no exista o falle al inicializar cae aquí. Es el camino garantizado.
        if (backend == Backend::Vulkan)
        {
#if defined(HARUKA_RHI_HAS_VULKAN)
            if (auto vk = createVK(window, preferredGpus)) return vk;
            // El backend Vulkan sigue a medias en esta rama: si no arranca, cae a GL (RHI).
            HARUKA_LOGW("RHI", "Vulkan solicitado → fallback a OpenGL.");
#else
            HARUKA_LOGW("RHI", "Vulkan no compilado (falta el SDK) → fallback a OpenGL.");
#endif
        }

        // ⚠️ El fallback a GL IGNORA `preferredGpus`, y no es un olvido: OpenGL no expone selección
        // de adaptador. Qué GPU sirve un contexto GL lo decide el driver/SO antes de que el proceso
        // arranque. Si el usuario eligió una GPU y acabamos aquí, su elección no se aplica — por eso
        // la UI lo dice en vez de fingir que sí.
        return createGL(window);   // OpenGL del RHI: backend por defecto y fallback
    }

    // ── ENUMERAR LAS GPUs DISPONIBLES ───────────────────────────────────────────────────────────
    //
    // Consulta pura: no crea el device ni deja estado. En Vulkan se levanta una instancia TEMPORAL
    // solo para preguntar y se destruye antes de volver, así que llamarlo desde el panel de ajustes
    // no puede interferir con el device en uso.
    std::vector<Device::AdapterInfo> Device::enumerateAdapters(Backend backend, SDL_Window* window)
    {
        std::vector<AdapterInfo> out;
#if defined(HARUKA_RHI_HAS_VULKAN)
        // ⚠️ SE ENUMERA POR VULKAN AUNQUE EL BACKEND ACTIVO SEA OPENGL, y no es un descuido.
        //
        // La primera versión solo listaba por Vulkan cuando Vulkan estaba seleccionado; con OpenGL
        // devolvía `GL_RENDERER`, o sea **la GPU que ya está en uso**. En un portátil híbrido eso
        // significa que la tarjeta dedicada NO APARECE en la lista — que es justo la que el usuario
        // quiere elegir. Enumerar por Vulkan las ve todas porque `vkEnumeratePhysicalDevices` no
        // depende del contexto de dibujo.
        //
        // Que el backend activo sea GL no invalida la lista: sirve para ELEGIR (ver el offload PRIME
        // de `application.cpp`) y para saber qué hay. Si Vulkan no está, se cae a `GL_RENDERER`.
        (void)backend;
        if (vulkan::VKDevice::enumerate(window, out) && !out.empty()) return out;
        HARUKA_LOGW("RHI", "no se pudo enumerar GPUs por Vulkan; solo se informa de la de OpenGL");
#else
        (void)backend;
#endif
        // Sin Vulkan no hay enumeración posible: GL solo sabe decir en cuál está corriendo.
        AdapterInfo gl;
        const char* r = SDL_GL_GetCurrentContext() ? (const char*)glGetString(GL_RENDERER) : nullptr;
        gl.name     = r ? r : "GPU del sistema (OpenGL no enumera adaptadores)";
        gl.discrete = false;
        gl.usable   = true;
        out.push_back(std::move(gl));
        (void)window;
        return out;
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
