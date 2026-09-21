#include "input/gamepad.h"
#include "core/logger.h"
#include <cstdlib>
#include <cstring>

namespace Haruka::Input {

Gamepad& Gamepad::get() { static Gamepad g; return g; }

void Gamepad::ensureInit() {
    if (m_init) return;
    m_init = true;
    if (const char* e = std::getenv("HARUKA_GAMEPAD")) m_disabled = (e[0] == '0');
    if (m_disabled) return;
    if (!SDL_WasInit(SDL_INIT_GAMEPAD) && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        HARUKA_LOGW("Mando", "SDL_INIT_GAMEPAD fallo: %s (sin mando)", SDL_GetError());
        m_disabled = true;
    }
}

void Gamepad::openIfNeeded() {
    if (m_pad) {
        if (!SDL_GamepadConnected(m_pad)) {
            HARUKA_LOGI("Mando", "desconectado: %s", m_name.c_str());
            SDL_CloseGamepad(m_pad); m_pad = nullptr; m_name.clear();
        }
        return;
    }
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n && !m_pad; ++i) {
        m_pad = SDL_OpenGamepad(ids[i]);
        if (m_pad) {
            const char* nm = SDL_GetGamepadName(m_pad);
            m_name = nm ? nm : "mando";
            HARUKA_LOGI("Mando", "abierto: %s (%s)", m_name.c_str(),
                        SDL_GetGamepadStringForType(SDL_GetGamepadType(m_pad)));
        }
    }
    if (ids) SDL_free(ids);
}

void Gamepad::poll(InputState& in) {
    ensureInit();
    in.gamepad = false;
    if (m_disabled) return;
    openIfNeeded();
    if (!m_pad) return;
    in.gamepad = true;
    for (int a = 0; a < (int)SDL_GAMEPAD_AXIS_COUNT; ++a) {
        const Sint16 raw = SDL_GetGamepadAxis(m_pad, (SDL_GamepadAxis)a);
        // −32768..32767 → −1..1 (los gatillos ya vienen en 0..32767 → 0..1).
        in.axes[a] = (raw < 0) ? (float)raw / 32768.f : (float)raw / 32767.f;
    }
    for (int b = 0; b < (int)SDL_GAMEPAD_BUTTON_COUNT; ++b)
        in.buttons[b] = SDL_GetGamepadButton(m_pad, (SDL_GamepadButton)b);
}

const char* Gamepad::buttonName(SDL_GamepadButton b) { const char* s = SDL_GetGamepadStringForButton(b); return s ? s : ""; }
const char* Gamepad::axisName(SDL_GamepadAxis a)     { const char* s = SDL_GetGamepadStringForAxis(a);   return s ? s : ""; }
SDL_GamepadButton Gamepad::buttonFromName(const char* n) { return (n && *n) ? SDL_GetGamepadButtonFromString(n) : SDL_GAMEPAD_BUTTON_INVALID; }
SDL_GamepadAxis   Gamepad::axisFromName(const char* n)   { return (n && *n) ? SDL_GetGamepadAxisFromString(n)   : SDL_GAMEPAD_AXIS_INVALID; }

} // namespace Haruka::Input
