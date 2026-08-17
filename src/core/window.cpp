#include "window.h"
#include "stb_image.h"

#include <cstring>
#include <string>
#include <filesystem>

namespace Haruka::Core {

    Window::Window(const WindowProps& props) {
        m_data.title = props.title;
        m_data.width = props.width;
        m_data.height = props.height;
        m_data.iconPath = props.iconPath;
    }

    static void applyWindowIcon(SDL_Window* win, const std::string& path) {
        if (path.empty() || !std::filesystem::exists(path)) return;
        int w = 0, h = 0, ch = 0;
        unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4);
        if (!px) { std::cerr << "[Window] icono no cargado: " << path << "\n"; return; }
        SDL_Surface* surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, px, w * 4);
        if (surf) { SDL_SetWindowIcon(win, surf); SDL_DestroySurface(surf); }
        stbi_image_free(px);
    }

    Window::~Window() { shutdown(); }

    bool Window::initHeadless() {
        if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO)) {
            std::cerr << "[SDL] Init failed: " << SDL_GetError() << std::endl;
            return false;
        }

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);

        m_window = SDL_CreateWindow(m_data.title.c_str(), m_data.width, m_data.height,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (!m_window) return false;
        return true;
    }

    bool Window::init(bool vulkanWindow) {
        if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO)) {
            std::cerr << "[SDL] Init failed: " << SDL_GetError() << std::endl;
            return false;
        }
        bool audioOk = SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO);
        if (!audioOk) {
            std::string firstErr = SDL_GetError();
            int nd = SDL_GetNumAudioDrivers();
            for (int i = 0; i < nd && !audioOk; ++i) {
                const char* drv = SDL_GetAudioDriver(i);
                if (!drv || !std::strcmp(drv, "dummy") || !std::strcmp(drv, "disk")) continue;
                SDL_SetHint(SDL_HINT_AUDIO_DRIVER, drv);
                if (SDL_InitSubSystem(SDL_INIT_AUDIO)) { audioOk = true; break; }
            }
            if (!audioOk)
                std::cerr << "[SDL] audio no disponible: " << firstErr << std::endl;
        }

        // Backend Vulkan: ventana con SDL_WINDOW_VULKAN y SIN hints/flag de GL (el VKDevice usa
        // SDL_Vulkan_CreateSurface; en SDL3 la ventana debe crearse con el flag de Vulkan). No se
        // crea ningún contexto GL: así el device Vulkan no comparte ventana con el contexto GL.
        if (vulkanWindow) {
            m_window = SDL_CreateWindow(m_data.title.c_str(), m_data.width, m_data.height,
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
            if (!m_window) {
                // ⚠️ SIN ESTO EL FALLO ERA MUDO. `SDL_WINDOW_VULKAN` hace que SDL cargue el loader
                // de Vulkan, y eso puede fallar por motivos que NADA tienen que ver con el juego:
                // capas inyectadas (RenderDoc), loader ausente, o el driver sin soporte de
                // presentación. El único mensaje que se veía era "Failed to initialize Window
                // system", que no distingue ninguno de esos casos.
                const std::string err = SDL_GetError();
                std::cerr << "[SDL] ventana VULKAN no creada: " << err << std::endl;

                // ── REINTENTO EN X11 (XWayland) ─────────────────────────────────────────────────
                //
                // RENDERDOC NO SOPORTA SUPERFICIES WAYLAND. Al inyectar su capa, la instancia deja
                // de anunciar `VK_KHR_wayland_surface` y SDL no puede crear la ventana:
                //     "Installed Vulkan doesn't implement the VK_KHR_wayland_surface extension"
                // El motor abortaba, y desde fuera parecía que "con RenderDoc arranca en OpenGL".
                //
                // Se puede arreglar desde fuera con `SDL_VIDEODRIVER=x11`, pero eso obliga a
                // acordarse de ponerlo en la configuración de lanzamiento de RenderDoc — y si se
                // olvida, el síntoma no se parece en nada a la causa. Reintentar aquí hace que
                // capturar un frame de Vulkan funcione sin configurar nada.
                //
                // Solo se reintenta si el fallo menciona la superficie: cualquier otro motivo (sin
                // loader, sin driver) no lo arregla cambiar de servidor gráfico, y reintentar a
                // ciegas escondería el error de verdad.
                if (err.find("surface") != std::string::npos ||
                    err.find("Vulkan")  != std::string::npos) {
                    std::cerr << "[SDL] reintentando con SDL_VIDEO_DRIVER=x11 (XWayland). "
                                 "Wayland + capa de captura no ofrecen VK_KHR_wayland_surface."
                              << std::endl;
                    // El subsistema de vídeo ya está arrancado con el driver anterior: el hint solo
                    // se lee al inicializarlo, así que hay que pararlo y volver a arrancarlo.
                    SDL_QuitSubSystem(SDL_INIT_VIDEO);
                    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
                    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
                        m_window = SDL_CreateWindow(m_data.title.c_str(), m_data.width,
                                                    m_data.height,
                                                    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
                        if (m_window)
                            std::cerr << "[SDL] ventana VULKAN creada sobre x11" << std::endl;
                        else
                            std::cerr << "[SDL] tampoco con x11: " << SDL_GetError() << std::endl;
                    }
                }
                if (!m_window) return false;
            }
            applyWindowIcon(m_window, m_data.iconPath);
            return true;
        }

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);

        m_window = SDL_CreateWindow(m_data.title.c_str(), m_data.width, m_data.height,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        if (!m_window) {
            std::cerr << "[SDL] ventana OPENGL no creada: " << SDL_GetError() << std::endl;
            return false;
        }
        applyWindowIcon(m_window, m_data.iconPath);

        return true;
    }

    void Window::setWindowMode(int mode) {
        if (!m_window) return;
        switch (mode) {
            case 2:
                SDL_SetWindowFullscreen(m_window, true);
                break;
            case 1: {
                SDL_SetWindowFullscreen(m_window, false);
                SDL_SetWindowBordered(m_window, false);
                SDL_DisplayID disp = SDL_GetDisplayForWindow(m_window);
                SDL_Rect b;
                if (SDL_GetDisplayBounds(disp, &b)) {
                    SDL_SetWindowPosition(m_window, b.x, b.y);
                    SDL_SetWindowSize(m_window, b.w, b.h);
                }
                break;
            }
            case 0:
            default:
                SDL_SetWindowFullscreen(m_window, false);
                SDL_SetWindowBordered(m_window, true);
                break;
        }
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(m_window, &w, &h);
        if (w > 0 && h > 0) { m_data.width = (uint32_t)w; m_data.height = (uint32_t)h; }
    }

    void Window::pollEvents(bool& running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                // En PÍXELES: `data1/2` es el tamaño lógico y con escalado del compositor no
                // coincide con el del framebuffer (ver la nota en application.cpp).
                int pw = event.window.data1, ph = event.window.data2;
                SDL_GetWindowSizeInPixels(m_window, &pw, &ph);
                m_data.width  = (uint32_t)((pw > 0) ? pw : event.window.data1);
                m_data.height = (uint32_t)((ph > 0) ? ph : event.window.data2);
            }
        }
    }

    void Window::shutdown() {
        if (m_window) SDL_DestroyWindow(m_window);
    }
}
