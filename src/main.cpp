/**
 * @file main.cpp
 * @brief Entry point of the HarukaEngine runtime.
 *
 * Loads the current project metadata from `project.hrk` via @ref Haruka::Project,
 * resolves the startup scene path, and then delegates execution to @ref Application::run().
 * Any uncaught exception is reported through the engine error system and
 * causes a non-zero exit.
 */

/**
 * @mainpage HarukaEngine
 *
 * C++17 / OpenGL 4.6 runtime for space exploration, deferred rendering, and
 * procedural planetary terrain.
 *
 * The source tree is documented in parallel with the Markdown guides:
 * - `README.md`
 * - `docs/guides/ANALISIS_SISTEMAS.md`
 * - `docs/guides/DIAGRAMAS_FLUJO.md`
 * - `docs/guides/PATRONES_USO.md`
 * - `docs/guides/GUIA_RAPIDA.md`
 *
 * @section sec_overview Overview
 *
 * - `Application` owns the runtime loop and render pipeline.
 * - `SceneManager` stores the active scene as a flat collection of `SceneObject` records.
 * - `WorldSystem` maintains the floating origin and bridges double-precision CPU coordinates with GPU-local floats.
 * - `PlanetarySystem` owns orbital simulation, chunk generation, streaming, and LOD.
 * - `ObjectType` and `PlanetChunkKey` classify runtime entities and terrain chunks.
 *
 * ---
 *
 * @section sec_boot Boot sequence — `main.cpp` → `Application::run()`
 *
 * @code
 * main()
 *   ├── project.load(".")     →  read project.hrk with Project
 *   └── app.run(startScenePath)
 *         ├── create_window()          // SDL window + OpenGL 4.6 core profile
 *         ├── create_gl_context()      // SDL_GL_CreateContext + gladLoadGLLoader
 *         ├── loadScene(path)          // parse scene JSON via the scene loader
 *         ├── init(scene)              // allocate renderer/world/runtime resources
 *         └── main_loop()              // runs until SDL_EVENT_QUIT
 * @endcode
 *
 * **`create_window()`** — initializes SDL video, sets OpenGL 4.6 core profile
 * attributes, and creates the SDL3 window.
 *
 * **`create_gl_context()`** — creates the GL context via `SDL_GL_CreateContext`,
 * calls `gladLoadGLLoader(SDL_GL_GetProcAddress)` to resolve all OpenGL 4.6
 * function pointers, and enables relative mouse mode.
 *
 * **`loadScene(path)`** — deserializes a JSON scene file into the current
 * `SceneManager`. Each `SceneObject` restores its components (`Transform`,
 * `Material`, `Mesh`, `Script`, `MeshRenderer`, …) from their JSON handlers.
 * If no path is provided, the application falls back to its default bootstrap.
 *
 * **`init(scene)`** — resolves the camera and allocates renderer resources in
 * order: `GBuffer`, `HDR`, `Bloom`, `SSAO`, `Shadow`, `PointShadow`,
 * `CascadedShadowMap`, `IBL`, `GPUInstancing`, `VirtualTexturing`,
 * `ComputePostProcess`, `AssetStreamer`, `DebugOverlay`, `WorldSystem`,
 * `PlanetarySystem`, `TerrainStreamingSystem`, and physics helpers.
 *
 * ---
 *
 * @section sec_loop Per-frame loop — `main_loop()` → `renderFrame()`
 *
 * @code
 * main_loop()
 *   └── while (!quit)
 *         ├── SDL_PollEvent()          // input: mouse, resize, quit
 *         │     ├── mouse motion  →  camera.rotate()
 *         │     └── mouse wheel   →  camera.ProcessMouseScroll()
 *         ├── renderFrame()
 *         │     ├── buildRenderQueue()
 *         │     │     └── terrainStreamingSystem.update()   // async chunk load/evict
 *         │     └── renderFrameContent()
 *         │           ├── updateCascades()                  // shadow splits
 *         │           ├── cascade shadow passes  (×4)       // depth-only
 *         │           ├── geometry pass                     // fill G-buffer
 *         │           ├── lighting pass                     // deferred PBR
 *         │           ├── post-process                      // SSAO, bloom, HDR
 *         │           └── DebugOverlay::render()            // ImGui stats
 *         └── SDL_GL_SwapWindow()
 * @endcode
 *
 * **`buildRenderQueue()`** — iterates `SceneManager::getAllObjects()`, applies
 * per-layer distance culling and frustum sphere tests, then writes visible
 * pointers to the render queue. It also ticks `TerrainStreamingSystem::update()`
 * so async `generateChunk()` jobs can enqueue completed geometry.
 *
 * **`renderFrameContent()`** — drives the full render pipeline each frame:
 * cascaded shadow depth passes, G-buffer geometry pass, deferred lighting
 * (Cook-Torrance PBR + IBL + PCF shadows), SSAO, bloom bright-pass + blur,
 * HDR tone mapping, colour grading, and `DebugOverlay::render()`.
 *
 * ---
 *
 * @section sec_terrain Terrain and chunk streaming
 *
 * The planet surface is split into a **cube-sphere** grid. Each face is
 * subdivided into a configurable tile grid; each tile is one
 * `PlanetChunkKey` (face, LOD, x, y).
 *
 * **Generation** (`PlanetarySystem::generateChunk()`):
 * - A cube-sphere patch is tessellated at LOD resolution.
 * - Height is computed by layered fBm noise (`NoiseGenerator::fBm`) with four
 *   independent seeds: continents, macro, detail, warp.
 * - Vertices are displaced along the sphere normal by the height value.
 * - Normals are recalculated from the displaced positions.
 * - The job runs on a background thread via `std::async`; result is a
 *   `ChunkData` (vertices / normals / indices).
 *
 * **Streaming** (`TerrainStreamingSystem`):
 * - `WorldSystem::updateVisibleChunks()` builds the visible set from camera
 *   distance and FOV each frame.
 * - `scheduleChunkStreaming()` produces load/evict queues bounded by
 *   `maxLoadsPerFrame` / `maxEvictsPerFrame` / `maxMemoryMB`.
 * - Eviction priority: oldest `lastTouchedFrame` first, tie-broken by distance.
 * - Completed chunks register a collision proxy with `RaycastSimple` so
 *   characters can stand on freshly streamed terrain.
 * - Neighbour LOD stitching via `WorldSystem::getNeighborLods()` prevents
 *   T-junctions at LOD boundaries.
 *
 * ---
 *
 * @section sec_planet Planet and solar system
 *
 * `PlanetarySystem` owns the simulation loop and all `CelestialBody` records
 * stored in `WorldSystem`.
 *
 * @code
 * PlanetarySystem ps;
 * ps.init(scene, worldSystem);
 * ps.addStar("Sol");
 * ps.addPlanet("Earth", Units::AU, 5.972e24, Units::EARTH_RADIUS, {0.3f, 0.6f, 1.f});
 * @endcode
 *
 * - **Orbital integration** — `integrateOrbits()` advances each body with a
 *   symplectic Euler step scaled by `timeScale` (default 1000×). Positions are
 *   stored in double-precision `WorldPos` (km).
 * - **Gravity** — `calculateGravityAtPosition()` sums Newtonian contributions
 *   from all bodies. `applyPlanetaryPhysics()` applies this to the player's
 *   rigid body and adjusts the character up-vector for surface walking.
 * - **Origin shifting** — `WorldSystem::updateOrigin()` recenters the floating
 *   origin to the camera each frame, keeping single-precision render coordinates
 *   within centimetre precision at astronomical distances.
 *
 * ---
 *
 * @section sec_scene Scenes and prefabs
 *
 * A `SceneManager` is a flat collection of `SceneObject` records. Each object
 * holds:
 * - Type string and `ObjectType` enum (`MESH`, `MODEL`, `PLANET`, `STAR`, `LIGHT`, `CAMERA`, …)
 * - Transform fields (`position`, `rotation`, `scale`) in double precision
 * - A `components` map of named component handles (`Transform`, `Material`,
 *   `Mesh`, `Model`, `Script`, `MeshRenderer`, …)
 * - `parentIndex` for hierarchy (`-1` = scene root)
 *
 * **Serialization** — scene data is loaded and saved through the scene loader
 * and JSON handlers so new component types can be added without changing the
 * entry point.
 *
 * **Prefabs** — reusable scene templates are stored as JSON snapshots of a
 * `SceneObject`. They are intended for environment props, character rigs, and
 * planet templates.
 *
 * **Project** — `project.hrk` is managed through `Haruka::Project` and stores
 * the start scene path plus project metadata used by the runtime.
 *
 * @section sec_docs Documentation map
 *
 * - `README.md` for build and dependency overview
 * - `docs/guides/ANALISIS_SISTEMAS.md` for the architecture breakdown
 * - `docs/guides/DIAGRAMAS_FLUJO.md` for execution and data-flow diagrams
 * - `docs/guides/PATRONES_USO.md` for implementation patterns and code examples
 * - `docs/guides/GUIA_RAPIDA.md` for quick reference and debugging tips
 * - `docs/html/index.html` for the generated API reference
 */

#include "core/application.h"
#include "core/project.h"
#include "tools/error_reporter.h"

#include <iostream>
#include <stdexcept>

/**
 * @brief Program entry point.
 *
 * Loads `project.hrk` through `Haruka::Project`, extracts `startScene` when
 * present, and passes that path to `Application::run()`.
 *
 * If the project file is missing, the runtime starts with its default bootstrap
 * path. Any uncaught exception is reported through the engine error system and
 * causes a non-zero exit.
 *
 * @return `EXIT_SUCCESS` on clean shutdown, `EXIT_FAILURE` on fatal error.
 */
int main() {
    Application app;

    std::string startScenePath = "";

    Haruka::Project project;
    if (project.load(".")) {
        startScenePath = project.getConfig().startScene;
    }

    try {
        app.run(startScenePath);
    } catch (const std::exception& e) {
        HARUKA_MOTOR_ERROR(ErrorCode::MOTOR_INIT_FAILED,
            std::string("Uncaught exception: ") + e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}