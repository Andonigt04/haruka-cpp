/**
 * @file test_input.cpp
 * @brief Acciones de entrada: teclado y MANDO en la misma accion (input/input_action.h).
 *
 * Sin SDL corriendo: se construye el `InputState` a mano, que es justo lo que `Gamepad::poll`
 * rellena. Contrapruebas: sin mando abierto, los botones/ejes del estado NO cuentan; y un stick
 * dentro de la zona muerta es cero, no "casi cero".
 */
#include "test_common.h"
#include "input/input_action.h"
#include "input/gamepad.h"
#include <cmath>

using namespace Haruka::Input;

void test_input_gamepad_bindings() {
    beginTest("input_gamepad_bindings");
    bool kbd[SDL_SCANCODE_COUNT] = {};
    InputState in; in.kbd = kbd; in.gamepad = true;

    // ── Move: WASD + stick izquierdo + cruceta ──────────────────────────────────────────────
    Axis2DBinding mv{ SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D };
    mv.axisX = SDL_GAMEPAD_AXIS_LEFTX; mv.axisY = SDL_GAMEPAD_AXIS_LEFTY;
    mv.upBtn = SDL_GAMEPAD_BUTTON_DPAD_UP; mv.downBtn = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    mv.leftBtn = SDL_GAMEPAD_BUTTON_DPAD_LEFT; mv.rightBtn = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    InputAction move; move.name = "Move"; move.source = mv;

    // Stick a tope hacia ARRIBA: en SDL Y crece hacia abajo, asi que raw = −1 → +Y.
    in.axes[SDL_GAMEPAD_AXIS_LEFTY] = -1.f;
    move.evaluate(in);
    glm::vec2 v = std::get<glm::vec2>(move.value);
    CHECK(std::fabs(v.x) < 1e-6f && std::fabs(v.y - 1.f) < 1e-4f, "stick izquierdo a tope arriba = (0, +1): la Y de SDL se invierte");
    CHECK(move.isStarted(), "y la accion se ACTIVA este frame (fase started)");

    // Media inclinacion: la zona muerta se reescala, no se resta a secas.
    in.axes[SDL_GAMEPAD_AXIS_LEFTY] = -0.59f;   // (0,59 − 0,18)/(1 − 0,18) = 0,5
    move.evaluate(in);
    v = std::get<glm::vec2>(move.value);
    CHECK(std::fabs(v.y - 0.5f) < 0.01f, "a 0,59 de inclinacion el valor es 0,5 (zona muerta 0,18 reescalada)");

    // Dentro de la zona muerta: CERO exacto, no un residuo que mueva al jugador.
    in.axes[SDL_GAMEPAD_AXIS_LEFTY] = -0.12f; in.axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.12f;
    move.evaluate(in);
    CHECK(!move.isPerformed(), "un stick dentro de la zona muerta radial (0,17 de modulo) da CERO");
    CHECK(move.isCanceled(), "y la accion se cancela al soltarlo");

    // Cruceta = tecla: diagonal normalizada.
    in.axes[SDL_GAMEPAD_AXIS_LEFTY] = 0.f; in.axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.f;
    in.buttons[SDL_GAMEPAD_BUTTON_DPAD_UP] = true; in.buttons[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = true;
    move.evaluate(in);
    v = std::get<glm::vec2>(move.value);
    CHECK(std::fabs(glm::length(v) - 1.f) < 1e-4f && v.x > 0.f && v.y > 0.f, "cruceta arriba+derecha = diagonal unitaria, como W+D");
    in.buttons[SDL_GAMEPAD_BUTTON_DPAD_UP] = false; in.buttons[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = false;

    // Teclado y stick a la vez: gana el de mas modulo, ninguno anula al otro.
    kbd[SDL_SCANCODE_W] = true; in.axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.3f;   // stick flojo a la derecha
    move.evaluate(in);
    v = std::get<glm::vec2>(move.value);
    CHECK(std::fabs(v.y - 1.f) < 1e-4f && std::fabs(v.x) < 1e-6f, "con W pulsada y un stick flojo manda la tecla (mayor modulo)");
    kbd[SDL_SCANCODE_W] = false; in.axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.f;

    // ── CONTRAPRUEBA: sin mando abierto, los ejes y botones del estado NO cuentan ───────────
    in.gamepad = false;
    in.axes[SDL_GAMEPAD_AXIS_LEFTY] = -1.f; in.buttons[SDL_GAMEPAD_BUTTON_DPAD_UP] = true;
    move.evaluate(in);
    CHECK(!move.isPerformed(), "CONTRAPRUEBA: con `gamepad = false` un stick a tope y la cruceta no mueven nada");
    kbd[SDL_SCANCODE_W] = true; move.evaluate(in);
    CHECK(std::get<glm::vec2>(move.value).y > 0.99f, "...y el teclado sigue funcionando igual que antes del mando");
    kbd[SDL_SCANCODE_W] = false; in.gamepad = true;
    in.axes[SDL_GAMEPAD_AXIS_LEFTY] = 0.f; in.buttons[SDL_GAMEPAD_BUTTON_DPAD_UP] = false;

    // ── Direct: tecla, boton o gatillo por umbral ───────────────────────────────────────────
    DirectBinding cd; cd.keys = { SDL_SCANCODE_R }; cd.buttons = { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER };
    cd.axis = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; cd.axisThreshold = 0.5f;
    InputAction cast; cast.name = "Cast"; cast.source = cd;
    in.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 0.3f; cast.evaluate(in);
    CHECK(!cast.isPerformed(), "gatillo a 0,3 (< umbral 0,5): no conjura");
    in.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 0.7f; cast.evaluate(in);
    CHECK(cast.isPerformed() && cast.isStarted(), "gatillo a 0,7: conjura, y es `started` ese frame");
    in.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 0.f; in.buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = true; cast.evaluate(in);
    CHECK(cast.isPerformed() && !cast.isStarted(), "LB mantiene la accion activa sin nuevo `started`");
    in.buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = false; cast.evaluate(in);
    CHECK(cast.isCanceled(), "al soltar LB se cancela");

    // ── Axis1D: dos botones o un eje ────────────────────────────────────────────────────────
    Axis1DBinding zb{}; zb.positive = SDL_SCANCODE_EQUALS; zb.negative = SDL_SCANCODE_MINUS;
    zb.axis = SDL_GAMEPAD_AXIS_RIGHTY; zb.positiveBtn = SDL_GAMEPAD_BUTTON_DPAD_UP; zb.negativeBtn = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    InputAction zoom; zoom.name = "Zoom"; zoom.source = zb;
    in.axes[SDL_GAMEPAD_AXIS_RIGHTY] = -1.f; zoom.evaluate(in);
    CHECK(std::fabs(std::get<float>(zoom.value) + 1.f) < 1e-4f, "eje 1D a −1 → −1 (sin invertir: el llamante decide el signo)");
    in.axes[SDL_GAMEPAD_AXIS_RIGHTY] = 0.f; in.buttons[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = true; zoom.evaluate(in);
    CHECK(std::fabs(std::get<float>(zoom.value) + 1.f) < 1e-6f, "boton negativo → −1");
    in.buttons[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = false;

    // ── Ida y vuelta de la mitad del mando (lo que guarda el ini) ───────────────────────────
    {
        const auto b0 = move.padButtons(); const auto a0 = move.padAxes();
        CHECK(b0.size() == 4 && a0.size() == 2, "Axis2D publica 4 botones y 2 ejes del mando");
        std::vector<SDL_GamepadButton> b1; std::vector<SDL_GamepadAxis> a1;
        for (auto b : b0) b1.push_back(Gamepad::buttonFromName(Gamepad::buttonName(b)));
        for (auto a : a0) a1.push_back(Gamepad::axisFromName(Gamepad::axisName(a)));
        CHECK(b1 == b0 && a1 == a0, "nombre → enum → nombre devuelve los mismos botones y ejes (formato del ini)");
        // Un hueco vacio se queda en INVALID y no descoloca a los demas.
        InputAction m2 = move;
        m2.setPad({ SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_INVALID }, {});
        in.buttons[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = true; m2.evaluate(in);
        CHECK(std::get<glm::vec2>(m2.value).y < -0.99f, "tras `setPad` con huecos, `dpdown` sigue siendo ABAJO (las posiciones se conservan)");
        in.buttons[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = false;
        CHECK(Gamepad::buttonFromName("") == SDL_GAMEPAD_BUTTON_INVALID && Gamepad::axisFromName("") == SDL_GAMEPAD_AXIS_INVALID,
              "el nombre vacio es 'sin asignar'");
    }

    // ── Invertir: cambia el signo del valor FINAL, con teclas o con stick ───────────────────
    {
        InputAction m3 = move;
        m3.setInvert(true, false);
        bool ix = false, iy = false; m3.invert(ix, iy);
        CHECK(ix && !iy, "setInvert(x) se lee de vuelta");
        kbd[SDL_SCANCODE_D] = true; m3.evaluate(in);
        CHECK(std::get<glm::vec2>(m3.value).x < -0.99f, "con X invertido, la tecla D (derecha) da −1");
        kbd[SDL_SCANCODE_D] = false;
        in.axes[SDL_GAMEPAD_AXIS_LEFTX] = 1.f; m3.evaluate(in);
        CHECK(std::get<glm::vec2>(m3.value).x < -0.99f, "...y el stick a la derecha tambien");
        in.axes[SDL_GAMEPAD_AXIS_LEFTX] = 0.f;
        m3.setInvert(false, false); kbd[SDL_SCANCODE_D] = true; m3.evaluate(in);
        CHECK(std::get<glm::vec2>(m3.value).x > 0.99f, "CONTRAPRUEBA: sin invertir, D da +1");
        kbd[SDL_SCANCODE_D] = false;
    }
}
