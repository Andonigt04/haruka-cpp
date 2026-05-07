#pragma once

#include <SDL3/SDL.h>
#include <glad/glad.h>
#include <string>
#include <iostream>

namespace Haruka::Core {

    struct WindowProps {
        std::string title;
        uint32_t width;
        uint32_t height;

        WindowProps(const std::string& t = "Haruka Engine", uint32_t w = 1280, uint32_t h = 720)
            : title(t), width(w), height(h) {}
    };

    class Window {
    public:
        Window(const WindowProps& props);
        ~Window();

        bool init();
        void shutdown();
        void swapBuffers();
        void pollEvents(bool& running);
        
        // Getters
        SDL_Window* getNativeWindow() const { return m_window; }
        SDL_GLContext getContext()    const { return m_glContext; }
        uint32_t getWidth()           const { return m_data.width; }
        uint32_t getHeight()          const { return m_data.height; }

        uint32_t setWidth(uint32_t w) { m_data.width = w; return m_data.width; }
        uint32_t setHeight(uint32_t h) { m_data.height = h; return m_data.height; }

    private:
        SDL_Window* m_window = nullptr;
        SDL_GLContext m_glContext = nullptr;

        struct WindowData {
            std::string title;
            uint32_t width, height;
        } m_data;
    };
}