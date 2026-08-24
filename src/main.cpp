/**
 * @file main.cpp
 * @brief Punto de entrada del runtime suelto — y **ningún target lo compila**.
 *
 * Este `main()` arranca el motor a pelo desde un `project.hrk`: lee la
 * configuración con @ref Haruka::Project y llama a
 * @ref Haruka::Core::Application::run(). Es el runtime antiguo.
 *
 * `CMakeLists.txt` lo excluye a propósito del glob de fuentes
 * (`list(FILTER ENGINE_SOURCES EXCLUDE REGEX "src/main\\.cpp$")`), porque metido en
 * `libHarukaEngine.so` hacía que la librería compartida **exportara el símbolo
 * `main`**. El punto de entrada vivo es el del juego: `template/main.cpp` (y el del
 * proyecto real), que inyecta su @ref Haruka::GameInterface antes de arrancar:
 *
 * @code
 * Application app;
 * app.setGameInterface(getGameInterface());   // los callbacks del juego
 * app.run(Haruka::AssetPaths::maps() + "menu");
 * @endcode
 *
 * Se conserva porque sigue siendo la forma de levantar el motor sin juego, y
 * porque es donde vive la portada de esta documentación.
 */

/**
 * @mainpage HarukaEngine
 *
 * C++17 runtime for space exploration at planetary scale: double-precision
 * world coordinates, procedural terrain, deferred rendering over an RHI that
 * targets **OpenGL 4.6 and Vulkan** from the same frame code.
 *
 * @section sec_docs Documentation map
 *
 * - @subpage flujo_interactivo "Flujo de ejecución interactivo" — the **Execution flow**
 *   button on every function page: what it calls, which branches it takes and which
 *   variables it writes, taken from the compiler's own AST.
 * - @subpage analisis_sistemas "Análisis de Sistemas" — architecture breakdown.
 * - @subpage diagramas_flujo "Diagramas de Flujo" — execution and data-flow diagrams.
 * - @subpage patrones_uso "Patrones de Uso" — implementation patterns and examples.
 * - @subpage guia_rapida "Guía Rápida" — quick reference and debugging tips.
 *
 * The design documents that are not part of this reference live next to the code:
 * `README.md` (build and dependencies), `TERRENO.md` (terrain contract),
 * `ROADMAP.md`, and the plans under `docs/guides/`.
 *
 * @note **This page does not transcribe call sequences.** Hand-written call lists
 * rot: an earlier version of this page named sixteen functions that no longer
 * existed anywhere in the engine. What a function does is generated from the code
 * itself — open the function and press *Flujo de ejecución*. What stays written
 * here is what a generator cannot infer: units, invariants, and the reasons.
 *
 * ---
 *
 * @section sec_boot Boot
 *
 * There are two entry points, and both end in the same call:
 *
 * - **The game** (`template/main.cpp`, and the real project's `main.cpp`) — the
 *   live one. It injects its @ref Haruka::GameInterface with
 *   `setGameInterface()` and starts at the menu scene.
 * - **The bare runtime** (`src/main.cpp`) — reads `project.hrk` and starts its
 *   `startScene` with no game module. No target compiles it; see the note at the
 *   top of that file.
 *
 * Either way the work happens in
 * @ref Haruka::Core::Application::run() "Application::run(startScene, headless)",
 * which, in order:
 *
 * 1. Resolves the shader/asset base directory from `/proc/self/exe`, so the
 *    binary runs from anywhere.
 * 2. Picks the RHI backend: the graphics setting, overridable with
 *    `HARUKA_BACKEND=vulkan|opengl`. On the GL path with a discrete GPU
 *    requested it also sets the PRIME/`DRI_PRIME` offload variables **before**
 *    the context exists — afterwards they have no effect.
 * 3. Creates the @ref Haruka::Core::Window (SDL3) or its headless variant, then
 *    the @ref Haruka::RHI::Device, published globally via `RHI::setDevice()`.
 * 4. `loadScene()` → `init(scene)` → the game module's `onInit` →
 *    `applyGraphicsSettings()`.
 * 5. Installs SIGINT/SIGTERM handlers and enters the frame loop.
 *
 * @ref Haruka::Core::Application::init() "init()" is deliberately small: it wires
 * @ref Haruka::WorldSystem, @ref PhysicsEngine, the camera and the planetary
 * system, and publishes them through `MotorInstance`. The heavy renderer
 * resources are created by the passes that need them.
 *
 * @section sec_loop The frame
 *
 * Each iteration clamps the delta to 100 ms, pumps SDL input, reacts to a resize,
 * opens a @ref Haruka::Profiler frame, ticks the game module's `onUpdate`, and
 * then runs the two halves of the frame:
 *
 * - `buildRenderQueue()` — culls the scene into the render queue, and only
 *   rebuilds it when the scene changed (`m_renderQueueDirty`).
 * - `renderFrameContent()` — the pipeline: cascade shadow passes, G-buffer,
 *   deferred PBR lighting, terrain and props, weather, water and volumetric
 *   clouds, post-process, and the ImGui overlay. It is the biggest function in
 *   the engine; its flow tree is the practical way to read it.
 *
 * @section sec_scale Scale: where the precision lives
 *
 * A float loses metres at astronomical distances, so the engine splits the two
 * worlds and never mixes them:
 *
 * - @ref Haruka::WorldSystem holds every @ref Haruka::CelestialBody in
 *   `WorldPos` (double, km) and shifts the floating origin to the camera every
 *   frame. `toLocal()` is the only sanctioned way down to the float the GPU sees.
 * - @ref Haruka::PlanetarySystem owns the planets: orbits, terrain sampling
 *   (`sampleTerrainHeight`, `sampleSurface`), water (`sampleWaterLevel`,
 *   `getSeaSurface`), weather (`weatherAt`) and ground cover. It is also the
 *   authority the physics asks through @ref Haruka::Physics::IWorldProvider.
 * - @ref PhysicsEngine integrates in double precision. Jolt does the broad
 *   phase and the solver; `HARUKA_JOLT=0` falls back to the built-in path.
 *
 * @section sec_terrain Terrain
 *
 * The surface is a cube-sphere sampled by a clipmap of 128 m patches, tessellated
 * on the GPU up to ×32. The rule that keeps it coherent is that **every LOD number
 * lives once**: `core/planet/terrain_lod.h` is the single definition, and
 * `terrain_detail.h` is the declared twin of `terrain_detail.glsl` — the height
 * function exists in two languages that are changed together, never copied.
 * A test that rewrites one of those formulas is auditing itself; see the warning
 * at the top of `terrain_lod.h`.
 *
 * @section sec_env Environment variables
 *
 * Switches the runtime reads at startup (the full list is in the source; these
 * are the ones worth knowing):
 *
 * | Variable | Effect |
 * |----------|--------|
 * | `HARUKA_BACKEND` | `vulkan` \| `opengl`, forces the RHI backend |
 * | `HARUKA_JOLT` | `0` disables Jolt and uses the built-in physics path |
 * | `HARUKA_COLLISION_WIRE` | draws the collision meshes |
 * | `HARUKA_PROF_LOG` | dumps profiler frames to a log |
 * | `HARUKA_FRAMELOG` | logs the first frames after each `init()` |
 * | `HARUKA_SHOT`, `HARUKA_SHOT_AFTER` | screenshot now / after N seconds (CI smoke tests) |
 * | `HARUKA_DIAG` | extra diagnostics on the render path |
 *
 * `--headless` runs without a visible window and is what the smoke tests use.
 */

#include "core/application.h"
#include "core/project.h"
#include "tools/error_reporter.h"

#include <iostream>
#include <stdexcept>
#include <cstring>

/**
 * @brief Entry point of the bare runtime (not compiled into any target).
 *
 * Loads `project.hrk` through `Haruka::Project`, extracts `startScene` when
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

    Haruka::Project project;
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