#include "window.h"
#include "stb_image.h"

#include <cstring>
#include <cstdlib>
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

    void Window::setWindowMode(int mode, SDL_DisplayID preferredDisplay) {
        if (!m_window) return;
        // Monitor efectivo: el pedido si es valido; si no (o no se pidio ninguno), el que tenga la
        // ventana; ultimo recurso, la primaria.
        SDL_DisplayID disp = preferredDisplay ? preferredDisplay : SDL_GetDisplayForWindow(m_window);
        if (!disp) disp = SDL_GetPrimaryDisplay();
        SDL_Rect b;
        switch (mode) {
            case 2: {
                // Exclusiva: SDL llena el monitor SOBRE EL QUE ESTA LA VENTANA, y una ventana ya a
                // pantalla completa NO se mueve (el WM la clava en el monitor actual; posicionar es
                // no-op y `SetWindowFullscreen(true)` otra vez también). Para cambiar de monitor hay
                // que salir, mover y volver a entrar. Si ya está en el monitor pedido, no tocar nada.
                const SDL_DisplayID curDisp = SDL_GetDisplayForWindow(m_window);
                if (curDisp != disp) {
                    SDL_SetWindowFullscreen(m_window, false);
                    if (SDL_GetDisplayBounds(disp, &b))
                        SDL_SetWindowPosition(m_window, b.x, b.y);
                }
                SDL_SetWindowFullscreen(m_window, true);
                break;
            }
            case 1: {
                SDL_SetWindowFullscreen(m_window, false);
                SDL_SetWindowBordered(m_window, false);
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
                // En modo ventana, sin monitor objetivo (preferredDisplay=0) no se mueve nada: SDL
                // recuerda donde estaba. Con monitor elegido se CENTRA en el, que es la diferencia
                // entre "salio en el monitor pedido" y "salio donde le dio la gana".
                if (preferredDisplay && SDL_GetDisplayBounds(disp, &b)) {
                    int w = 0, h = 0;
                    SDL_GetWindowSize(m_window, &w, &h);
                    SDL_SetWindowPosition(m_window, b.x + (b.w - w) / 2, b.y + (b.h - h) / 2);
                }
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

    // ── MONITORES ───────────────────────────────────────────────────────────────────────────────
    SDL_DisplayID Window::displayForName(const std::string& name) {
        if (name.empty()) return 0;   // "auto": el llamante decide (primaria / la de la ventana)
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        SDL_DisplayID hit = 0;
        if (displays) {
            for (int i = 0; i < count; ++i) {
                const char* dn = SDL_GetDisplayName(displays[i]);
                if (dn && name == dn) { hit = displays[i]; break; }
            }
            // Compositores SIN nombre (X11 sin EDID, Wayland parcial): el selector guarda la
            // etiqueta "Monitor N" de `displayLabel`. Si el nombre no existe hay que resolver la
            // etiqueta por INDICE, o elegir pantalla ahí no movería la ventana. Asume el risco
            // de reordenacion documentado en `monitorName`, que es el único que queda.
            if (!hit && name.rfind("Monitor ", 0) == 0) {
                const int idx = std::atoi(name.c_str() + 8);
                if (idx >= 0 && idx < count) hit = displays[idx];
            }
            SDL_free(displays);
        }
        return hit;
    }

    bool Window::displayListDiffers(const SDL_DisplayID* ids, int n, const std::vector<SDL_DisplayID>& cached) {
        if (n < 0) n = 0;
        if ((size_t)n != cached.size()) return true;
        for (int i = 0; i < n; ++i) if (ids[i] != cached[(size_t)i]) return true;
        return false;
    }

    int Window::displayIndexIn(const SDL_DisplayID* ids, int n, SDL_DisplayID disp) {
        for (int i = 0; i < n; ++i) if (ids[i] == disp) return i;
        return 0;
    }

    std::string Window::displayLabel(SDL_DisplayID disp) {
        if (const char* dn = SDL_GetDisplayName(disp); dn && dn[0]) return dn;
        // Algunos compositores no anuncian nombre; la etiqueta "Monitor N" al menos distingue.
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        // ⚠️ EL `break` ESTABA FUERA DEL `if`: el bucle salia SIEMPRE en la primera vuelta, asi que
        // toda pantalla sin nombre se llamaba "Monitor 0". Con dos monitores mudos el selector
        // enseñaba dos entradas iguales, `displayForName` resolvia la primera para las dos, y elegir
        // la segunda no movia nada. El indice vive ahora en `displayIndexIn`, que si se puede probar.
        const int idx = displayIndexIn(displays, count, disp);
        SDL_free(displays);
        return "Monitor " + std::to_string(idx);
    }
}
