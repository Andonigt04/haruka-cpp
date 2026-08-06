#pragma once

#include <SDL3/SDL.h>
#include <string>
#include <iostream>

namespace Haruka::Core {

    struct WindowProps {
        std::string title;
        uint32_t width;
        uint32_t height;
        std::string iconPath;

        WindowProps(const std::string& t = "Haruka Engine", uint32_t w = 1280, uint32_t h = 720,
                    const std::string& icon = "")
            : title(t), width(w), height(h), iconPath(icon) {}
    };

    class Window {
    public:
        Window(const WindowProps& props);
        ~Window();

        bool init();
        /** @brief Creates a hidden SDL window for headless / CI operation. */
        bool initHeadless();
        void shutdown();
        void pollEvents(bool& running);

        SDL_Window* getNativeWindow() const { return m_window; }
        uint32_t getWidth()           const { return m_data.width; }
        uint32_t getHeight()          const { return m_data.height; }

        uint32_t setWidth(uint32_t w) { m_data.width = w; return m_data.width; }
        uint32_t setHeight(uint32_t h) { m_data.height = h; return m_data.height; }

        void setWindowMode(int mode);

    private:
        SDL_Window* m_window = nullptr;

        struct WindowData {
            std::string title;
            uint32_t width, height;
            std::string iconPath;
        } m_data;
    };
}
