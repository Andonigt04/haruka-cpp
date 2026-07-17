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

    // Carga un PNG (RGBA) como icono de la ventana. Silencioso si el archivo no existe.
    static void applyWindowIcon(SDL_Window* win, const std::string& path) {
        if (path.empty() || !std::filesystem::exists(path)) return;
        int w = 0, h = 0, ch = 0;
        unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4); // fuerza RGBA
        if (!px) { std::cerr << "[Window] icono no cargado: " << path << "\n"; return; }
        SDL_Surface* surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, px, w * 4);
        if (surf) { SDL_SetWindowIcon(win, surf); SDL_DestroySurface(surf); }
        stbi_image_free(px);
    }

    Window::~Window() {
        shutdown();
    }

    bool Window::init() {
        if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO)) {
            std::cerr << "[SDL] Init failed: " << SDL_GetError() << std::endl;
            return false;
        }
        // Audio OPCIONAL (micro de voz + efectos + enumeración de dispositivos en Config).
        // No debe tumbar el arranque si no hay tarjeta de sonido (servidor/headless).
        bool audioOk = SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO);
        if (!audioOk) {
            // El driver por defecto falló → prueba cada driver compilado explícitamente
            // (pipewire/pulseaudio/alsa…). Arregla el caso "SDL eligió un driver que no va".
            std::string firstErr = SDL_GetError();
            int nd = SDL_GetNumAudioDrivers();
            for (int i = 0; i < nd && !audioOk; ++i) {
                const char* drv = SDL_GetAudioDriver(i);
                if (!drv || !std::strcmp(drv, "dummy") || !std::strcmp(drv, "disk")) continue;
                SDL_SetHint(SDL_HINT_AUDIO_DRIVER, drv);
                if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
                    audioOk = true;
                    std::cerr << "[SDL] audio: driver por defecto falló, usando '" << drv << "'\n";
                }
            }
            if (!audioOk) {
                std::cerr << "[SDL] audio no disponible: " << firstErr << " (sin micro/efectos)\n"
                          << "[SDL] drivers compilados (" << nd << "):";
                for (int i = 0; i < nd; ++i) std::cerr << ' ' << SDL_GetAudioDriver(i);
                std::cerr << "\n[SDL]   solo dummy/disk → SDL3 sin backends (reinstala "
                             "pipewire/pulseaudio/alsa -devel y recompila SDL3).\n";
            }
        }
        if (audioOk) {
            int ri = 0, ro = 0;
            if (SDL_AudioDeviceID* a = SDL_GetAudioRecordingDevices(&ri)) SDL_free(a);
            if (SDL_AudioDeviceID* a = SDL_GetAudioPlaybackDevices(&ro))  SDL_free(a);
            std::cerr << "[SDL] audio OK (" << (SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?")
                      << ") — entradas: " << ri << ", salidas: " << ro << std::endl;
        }

        // Configuración de OpenGL
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);

        m_window = SDL_CreateWindow(m_data.title.c_str(), m_data.width, m_data.height, 
                                   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        
        if (!m_window) return false;
        applyWindowIcon(m_window, m_data.iconPath);   // icono de la ventana (si existe el PNG)

        m_glContext = SDL_GL_CreateContext(m_window);
        if (!m_glContext) return false;

        if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
            return false;
        }

        glViewport(0, 0, m_data.width, m_data.height);
        glEnable(GL_DEPTH_TEST);

        // REVERSED-Z: el rango de profundidad del clip pasa de [-1,1] (convención GL) a [0,1]
        // (convención D3D/Vulkan). Sin esto, la mitad del rango del depth buffer se desperdicia y
        // la matriz reversed-Z de la cámara NO funciona. glm emite [0,1] por GLM_FORCE_DEPTH_ZERO_TO_ONE.
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

        return true;
    }

    void Window::setWindowMode(int mode) {
        if (!m_window) return;
        switch (mode) {
            case 2: // Fullscreen (pantalla completa de SDL — desktop fullscreen)
                SDL_SetWindowFullscreen(m_window, true);
                break;
            case 1: { // Borderless: sin borde cubriendo el escritorio (windowed fullscreen)
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
            case 0: // Windowed: ventana normal con borde
            default:
                SDL_SetWindowFullscreen(m_window, false);
                SDL_SetWindowBordered(m_window, true);
                break;
        }
        // Sincroniza el tamaño REAL en píxeles → m_data + viewport (por si el evento
        // de resize aún no ha llegado este frame).
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(m_window, &w, &h);
        if (w > 0 && h > 0) {
            m_data.width  = (uint32_t)w;
            m_data.height = (uint32_t)h;
            glViewport(0, 0, w, h);
        }
    }

    void Window::pollEvents(bool& running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                m_data.width = event.window.data1;
                m_data.height = event.window.data2;
                glViewport(0, 0, m_data.width, m_data.height);
            }
            // Aquí puedes añadir el dispatch a tu EventManager
        }
    }

    void Window::swapBuffers() {
        SDL_GL_SwapWindow(m_window);
    }

    void Window::shutdown() {
        if (m_glContext) SDL_GL_DestroyContext(m_glContext);
        if (m_window) SDL_DestroyWindow(m_window);
    }
}