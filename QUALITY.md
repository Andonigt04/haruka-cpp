# Code and testing standards

These are the rules the codebase follows. Most come from bugs that actually happened; the reason
is given where it is not obvious.

## Language and toolchain

- **C++26**, compiled with GCC 16 or newer. `HarukaEngine` requires `cxx_std_26` publicly, so
  anything that links it compiles as C++26 too.
- **No `-ffast-math`.** Floating-point contraction is off globally (`-ffp-contract=off`, see
  `cmake/HarukaBuildFlags.cmake`). With fast-math the CPU↔GPU parity tests fail only in Release
  builds, while staying green in the configuration people usually test.

## Naming and layout

| Element | Style | Example |
|---|---|---|
| Directories | PascalCase | `src/Haruka/World/Planet/` |
| Files | PascalCase, named after their main type | `PlanetarySystem.h` / `PlanetarySystem.cpp` |
| Namespaces | Mirror the directory, one to one | `src/Haruka/World/Planet/` → `Haruka::World::Planet` |
| Acronyms | Upper case, in files and types | `RHIDevice.h`, `VKSwapchain`, `TerrainLOD.h` |
| Types | PascalCase | `TerrainNodeRenderer` |
| Functions and methods | camelCase | `renderFrameContent()` |
| Members | `m_` prefix | `m_lakeWin` |
| Constants | `k` prefix | `kMaxRemeshPerFrame` |
| Module macros | `HARUKA_MOD_<NAME>` | `HARUKA_MOD_FLUIDS` |

## Headers and includes

- Headers use `.h` and start with `#pragma once`.
- A `.cpp` file includes **its own header first**. That proves the header compiles on its own.
  The one allowed exception is a file whose whole body is gated by a module macro, which must
  include `Core/Modules.h` first.
- Include engine headers with the `Haruka/` prefix, by their path from the source root
  (`"Haruka/World/Planet/Planet.h"`), never with `../`. The prefix is the library's name, as with
  `<Jolt/...>` or `<SDL3/...>`: it says where a header comes from and keeps it from colliding with a
  game's own `Core/` or `UI/`.
- All `#include` lines go at the top of the file, before any code or `using` directive.
- No `using namespace` in headers.
- A type lives in the namespace of its directory: `src/Haruka/Core/Scene/SceneManager.h` declares
  `Haruka::Core::Scene::SceneManager`. A forward declaration goes in the namespace of the type it
  declares, not of the file that needs it. Helper namespaces inside a module (`detail`, `BakeUtil`,
  anonymous ones) hang under the directory's namespace.
- No back-compat aliases at global scope (`using Haruka::X::Y;` or `using Y = Haruka::X::Y;` outside
  any namespace). Write the qualified name, or a `using` inside a `.cpp`.
- Inside a `#define`, write fully qualified names (`Haruka::Core::log`): a macro expands wherever it
  is used.

## Types and resources

- Use `enum class`. Plain `enum` is allowed only for bit masks combined with `|` and for indices
  into arrays. In both cases a comment says why. Moving to `enum class` uncovered a call that passed
  enum values into the wrong constructor parameters; the implicit conversion had hidden it.
- Ownership is RAII: no raw `new`/`delete`, no `malloc`/`free`, no `strcpy`/`sprintf`. Use
  `std::unique_ptr`, `std::vector`, `std::string`, `std::snprintf` with the buffer size.
- Shared data read by other threads is published as a whole (for example
  `std::atomic<std::shared_ptr<const T>>`), never written field by field under a reader.

## Threads

- Physics (Jolt) runs on one thread on purpose: the authoritative server must reproduce it
  exactly. Do not parallelise it.
- **Do not create threads.** No `std::thread`, `std::async` or private pools. Submit work to
  `Haruka::Core::Jobs::jobs()` (`src/Haruka/Core/Jobs/JobSystem.h`) with the right priority. Use `parallelFor`
  for data-parallel loops, and `postToMain` for anything that must touch the GPU or the scene.
  `std::async(std::launch::async)` creates a new OS thread per call; the job system's test measures
  64 concurrent threads with it against the pool's fixed cap.
- A long-lived thread with its own loop (audio capture, for example) is created with
  `spawnDedicated`, so it is named and counted.
- **CPU and GPU copies of the same data go through `ResourceStore`** (`src/Haruka/Renderer/ResourceStore.h`).
  Do not invent another revision counter, upload budget or `destroy` call.
- GPU resources are created and destroyed on the render thread only.

## Tests

- **Every behaviour change comes with a test**, in the CPU suite (`tests/`, `haruka_tests`) or,
  if it needs a GPU, in the RHI bench (`haruka_tests_rhi`). Put it in the `tests/` folder that mirrors
  the code it tests; the folder is its group and its CTest test. Shaders used only by tests go in
  `tests/Shaders/`, never in `assets/shaders/`.
- **Every test has a counter-test.** Show that the check fails when the thing it guards is
  broken: the same check against a deliberately wrong input, or the old behaviour. A test that
  would also pass on an empty implementation proves nothing.
- **Test against real data where it exists.** For example, the `.glb` prefab test loads a file
  that Blender exported, not one written by hand.
- Don't make a test pass by loosening its threshold. If a threshold is wrong, explain in the pull
  request why the new value is right.

## Measuring

- **Measure; don't assert.** Claims about performance or precision need a number, how it was
  obtained, and the conditions: backend, GPU, build type, power source. `HARUKA_FRAMELOG=1` gives
  frame-time statistics; the profiler (`HARUKA_PROF_LOG`) gives per-stage times.
- Measure in an optimised build (`RelWithDebInfo` or `Release`). Numbers from `Debug` are
  meaningless here.
- Say what was **not** verified. It is part of the result.

## Comments

Comment the **why**: the constraint, the bug that motivated the code, the number that was
measured. Don't comment what the next line already says. Keep comments in the language the file
already uses (much of the codebase is commented in Spanish).
