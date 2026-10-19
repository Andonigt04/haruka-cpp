/**
 * @file main.cpp
 * @brief Punto de entrada del runtime suelto — y **ningún target lo compila**.
 *
 * Este `main()` arranca el motor a pelo desde un `project.hrk`: lee la
 * configuración con @ref Haruka::Core::Project y llama a
 * @ref Haruka::Core::Application::run(). Es el runtime antiguo.
 *
 * `CMakeLists.txt` lo excluye a propósito del glob de fuentes
 * (`list(FILTER ENGINE_SOURCES EXCLUDE REGEX "src/main\\.cpp$")`), porque metido en
 * `libHarukaEngine.so` hacía que la librería compartida **exportara el símbolo
 * `main`**. El punto de entrada vivo es el del juego: `template/main.cpp` (y el del
 * proyecto real), que inyecta su @ref Haruka::Core::GameInterface antes de arrancar:
 *
 * @code
 * Application app;
 * app.setGameInterface(getGameInterface());   // los callbacks del juego
 * app.run(Haruka::AssetPaths::maps() + "menu");
 * @endcode
 *
 * Se conserva porque sigue siendo la forma de levantar el motor sin juego. La
 * portada de esta documentación es `docs/guides/ARQUITECTURA.md`.
 */

#include "Haruka/Core/Application.h"
#include "Haruka/Core/Project.h"
#include "Haruka/Tools/ErrorReporter.h"

#include <iostream>
#include <stdexcept>
#include <cstring>

/**
 * @brief Entry point of the bare runtime (not compiled into any target).
 *
 * Loads `project.hrk` through `Haruka::Core::Project`, extracts `startScene` when
 * present, and passes that path to `Application::run()`. No `GameInterface` is
 * installed, so the game callbacks (`onInit`, `onUpdate`, `onRenderWorld`) never
 * fire — that is what the game's own `main()` is for.
 *
 * If the project file is missing, the runtime starts with its default bootstrap
 * path. Any uncaught exception is reported through the engine error system and
 * causes a non-zero exit.
 *
 * Supported flags:
 *   --headless  — run without a visible window (used by smoke tests / CI).
 *
 * @return `EXIT_SUCCESS` on clean shutdown, `EXIT_FAILURE` on fatal error.
 */
int main(int argc, char* argv[]) {
    Application app;
    bool headless = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0)
            headless = true;
    }

    std::string startScenePath = "";

    Haruka::Core::Project project;
    if (project.load(".")) {
        startScenePath = project.getConfig().startScene;
    }

    try {
        app.run(startScenePath, headless);
    } catch (const std::exception& e) {
        HARUKA_MOTOR_ERROR(ErrorCode::MOTOR_INIT_FAILED,
            std::string("Uncaught exception: ") + e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}