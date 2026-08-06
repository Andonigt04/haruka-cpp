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

    bool Window::init() {
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

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);

        m_window = SDL_CreateWindow(m_data.title.c_str(), m_data.width, m_data.height,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        if (!m_window) return false;
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
                m_data.width = event.window.data1;
                m_data.height = event.window.data2;
            }
        }
    }

    void Window::shutdown() {
        if (m_window) SDL_DestroyWindow(m_window);
    }
}
