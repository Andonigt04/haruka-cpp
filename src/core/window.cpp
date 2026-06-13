#include "window.h"

namespace Haruka::Core {

    Window::Window(const WindowProps& props) {
        m_data.title = props.title;
        m_data.width = props.width;
        m_data.height = props.height;
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
        if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            std::cerr << "[SDL] audio no disponible: " << SDL_GetError()
                      << " (sin micro/efectos)" << std::endl;
        } else {
            int ri = 0, ro = 0;
            if (SDL_AudioDeviceID* a = SDL_GetAudioRecordingDevices(&ri)) SDL_free(a);
            if (SDL_AudioDeviceID* a = SDL_GetAudioPlaybackDevices(&ro))  SDL_free(a);
            std::cerr << "[SDL] audio OK — entradas: " << ri << ", salidas: " << ro << std::endl;
        }

        // Configuración de OpenGL
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);

        m_window = SDL_CreateWindow(m_data.title.c_str(), m_data.width, m_data.height, 
                                   SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        
        if (!m_window) return false;

        m_glContext = SDL_GL_CreateContext(m_window);
        if (!m_glContext) return false;

        if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
            return false;
        }

        glViewport(0, 0, m_data.width, m_data.height);
        glEnable(GL_DEPTH_TEST);

        return true;
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