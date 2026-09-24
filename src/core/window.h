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

        /** @brief Crea la ventana visible. `vulkanWindow` → SDL_WINDOW_VULKAN (sin perfil GL); si
         *         false → perfil OpenGL 4.6 core (backend GL). El flag lo decide el backend RHI
         *         pedido ANTES de crear la ventana (settings → imgui.ini → RenderBackend). */
        bool init(bool vulkanWindow = false);
        /** @brief Creates a hidden SDL window for headless / CI operation. */
        bool initHeadless();
        void shutdown();
        void pollEvents(bool& running);

        SDL_Window* getNativeWindow() const { return m_window; }
        uint32_t getWidth()           const { return m_data.width; }
        uint32_t getHeight()          const { return m_data.height; }

        uint32_t setWidth(uint32_t w) { m_data.width = w; return m_data.width; }
        uint32_t setHeight(uint32_t h) { m_data.height = h; return m_data.height; }

        void setWindowMode(int mode, SDL_DisplayID preferredDisplay = 0);

        // ── MONITORES ──────────────────────────────────────────────────────────────────────────
        // Helpers estaticos sobre lo que SDL ve AHORA. `displayForName` devuelve 0 cuando el nombre
        // guardado ya no existe (monitor desenchufado, EDID cambiado) — el llamante se cae a la
        // primaria en vez de apuntar a una pantalla equivocada. Resuelve ADEMÁS las etiquetas
        // "Monitor N" que `displayLabel` genera cuando el compositor no anuncia nombres.
        static SDL_DisplayID     displayForName(const std::string& name);
        static std::string       displayLabel(SDL_DisplayID disp); // nombre, o "Monitor N" si no hay

    private:
        SDL_Window* m_window = nullptr;

        struct WindowData {
            std::string title;
            uint32_t width, height;
            std::string iconPath;
        } m_data;
    };
}
