/**
 * @file main.cpp
 * @brief Entry point of the HarukaEngine runtime.
 *
 * Reads `project.hrk` (JSON) from the working directory to resolve the
 * startup scene path, then delegates execution to @ref Application::run().
 * Any uncaught exception is reported through the engine error system and
 * causes a non-zero exit.
 */

/**
 * @mainpage HarukaEngine
 *
 * C++17 / OpenGL 4.6 real-time engine distributed as `libHarukaEngine.so`.
 *
 * **Renderer** — deferred shading, PBR (metallic/roughness), cascaded shadows, SSAO, IBL, HDR, bloom, compute post-processing
 * **World** — double-precision coordinates, procedural planetary terrain, async chunk streaming
 * **Runtime** — physics (octree broad-phase), OpenAL 3D audio, PostgreSQL persistence, WebSocket networking
 *
 * ---
 *
 * @section sec_boot Boot sequence — `main.cpp` → `Application::run()`
 *
 * @code
 * main()
 *   ├── open "project.hrk"  →  read startScene path
 *   └── app.run(startScenePath)
 *         ├── create_window()          // SDL_Init + SDL_CreateWindow
 *         ├── create_gl_context()      // SDL_GL_CreateContext + gladLoadGLLoader
 *         ├── scene.load(path)         // parse scene JSON → SceneObject array
 *         ├── init(scene)              // allocate all GPU/subsystem resources
 *         └── main_loop()              // runs until SDL_EVENT_QUIT
 * @endcode
 *
 * **`create_window()`** — calls `SDL_Init(SDL_INIT_VIDEO)`, sets OpenGL 4.6 core
 * profile attributes, and creates the SDL3 window.
 *
 * **`create_gl_context()`** — creates the GL context via `SDL_GL_CreateContext`,
 * calls `gladLoadGLLoader(SDL_GL_GetProcAddress)` to resolve all OpenGL 4.6
 * function pointers, and enables relative mouse mode.
 *
 * **`scene.load(path)`** — deserializes a JSON scene file into the flat
 * `Scene::objects` array. Each `SceneObject` restores its components
 * (`Transform`, `Material`, `Mesh`, `Script`, …) from their `fromJson` pairs.
 * If no path is provided, an empty `"DefaultScene"` is used.
 *
 * **`init(scene)`** — resolves the camera (from `GameInterface` or the first
 * Camera-type scene object), then allocates all renderer resources in order:
 * `GBuffer`, `HDR`, `Bloom`, `SSAO`, `Shadow`, `PointShadow`,
 * `CascadedShadowMap`, `IBL`, `GPUInstancing`, `VirtualTexturing`,
 * `ComputePostProcess`, `AssetStreamer`, `DebugOverlay`, and
 * `WorldSystem::initComputeShaders()`.
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
 * **`buildRenderQueue()`** — iterates `Scene::objects`, applies per-layer
 * distance culling and frustum sphere tests, then writes visible pointers to
 * `g_sceneRenderQueue`. Also ticks `TerrainStreamingSystem::update()` which
 * dispatches async `generateChunk()` jobs and injects completed geometry.
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
 *   `PlanetarySystem::ChunkData` (vertices / normals / indices).
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
 * A `Haruka::Scene` is a flat array of `SceneObject` records. Each object holds:
 * - Type string and `ObjectType` enum (`Mesh`, `Light`, `Camera`, `Planet`, …)
 * - Transform fields (`position`, `rotation`, `scale`) in double precision
 * - A `components` map of named `ComponentPtr` (`Transform`, `Material`,
 *   `Mesh`, `Model`, `Script`, `MeshRenderer`, …)
 * - `parentIndex` for hierarchy (−1 = scene root)
 *
 * **Serialization** — `Scene::saveToFile()` / `loadFromFile()` round-trip the
 * full object graph to JSON. Each component type provides `toJson` / `fromJson`
 * so new types are automatically included.
 *
 * **Prefabs** — `Haruka::savePrefab()` snapshots one `SceneObject` (with all
 * its components) to a standalone JSON file. `loadPrefab()` restores it as a
 * ready-to-insert record. Prefabs are the intended reuse unit for environment
 * props, character rigs, and planet templates.
 *
 * **Project** — `project.hrk` is a JSON file generated by the IDE that groups
 * scene paths, asset root, shader override directory, and export settings.
 * The engine reads it at startup to locate the start scene.
 */

#include "core/application.h"
#include "core/json.hpp"
#include "core/error_reporter.h"
#include <iostream>
#include <fstream>
#include <stdexcept>

/**
 * @brief Program entry point.
 *
 * Boot sequence:
 * 1. Open `project.hrk` and read `"startScene"` if present. (proyect.hrk is an file generated by the IDE for when exported or developed with the IDE, it contains metadata about the project)
 * 2. Pass the scene path to Application::run() which owns the main loop.
 * 3. On uncaught exception, report via HARUKA_MOTOR_ERROR and return
 *    `EXIT_FAILURE`.
 *
 * @return `EXIT_SUCCESS` on clean shutdown, `EXIT_FAILURE` on fatal error.
 */
int main() {
    Application app;

    std::string startScenePath = "";
    // Leer project.hrk si existe
    std::ifstream f("project.hrk");
    if (f.is_open()) {
        nlohmann::json j;
        f >> j;
        if (j.contains("startScene")) {
            startScenePath = j["startScene"].get<std::string>();
        }
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