# Haruka Engine

Haruka Engine is a C++17 OpenGL 4.6 real-time engine with:

- deferred rendering + PBR
- editor/runtime/server targets
- procedural planetary tooling
- integrated post-processing and lighting stack

The repository builds three main applications from the same codebase:

- `HarukaEditor` (tools and authoring)
- `HarukaEngine` (runtime client)
- `HarukaServer` (dedicated server with database support)

---

## 1) Architecture Overview

Main source modules:

- `src/core` → application shell, scene model, camera, common systems
- `src/renderer` → rendering pipeline, GPU resources, shadows, post-process
- `src/editor` → editor panels, command history, viewport tooling
- `src/game` → gameplay systems, planet generation, character systems
- `src/network` → websocket/socket client-server abstractions
- `src/database` → PostgreSQL persistence layer
- `src/physics` → octree, rigid-body simulation, raycast helpers
- `src/audio` → OpenAL integration
- `src/io` → asset streaming and loading utilities

The engine library target is `HarukaEngineLib` and is linked by runtime/editor.

---

## 2) Rendering Features

Haruka includes:

- Deferred shading (`GBuffer` geometry + lighting passes)
- PBR material workflow (metallic/roughness)
- Directional and point-light shadows
- Cascaded shadow maps
- SSAO
- IBL (irradiance + prefilter + BRDF LUT)
- HDR and tone mapping
- Bloom
- Virtual texturing
- Compute-shader post-processing path

Core shader assets are in [shaders/](shaders/).

---

## 3) Build Requirements

### Toolchain

- CMake >= 3.14
- C++17 compiler (GCC/Clang/MSVC)

### Required libraries

Resolved by CMake via `find_package`/`pkg-config`:

- OpenGL
- GLFW3
- Assimp
- OpenSSL
- OpenAL
- PostgreSQL client (`libpq`)
- GTK3 (for native file dialog/editor integration)

### Third-party sources

The repo expects `third_party/` sources (GLM, GLAD, GLFW, stb, etc.).
You can bootstrap the folder with [setup_deps.sh](setup_deps.sh).

---

## 4) Build and Run

Typical Linux flow:

```bash
mkdir -p build
cd build
cmake ..
make -j"$(nproc)"
```

Generated binaries:

- `build/HarukaEditor`
- `build/HarukaEngine`
- `build/HarukaServer`
- `build/ChatTest`

Run examples:

```bash
cd build
./HarukaEditor
```

Build options from CMake:

- `BUILD_EDITOR` (ON by default)
- `BUILD_RUNTIME` (ON by default)
- `BUILD_SERVER` (ON by default)

---

## 5) Key Runtime Pipelines

### Terrain/Planet workflow

1. Configure seed and terrain layer parameters.
2. Generate base geometry (GPU when available, CPU fallback otherwise).
3. Optionally split into chunks for editing.
4. Persist chunk deltas and reload them on scene reopen.

### Deferred render workflow

1. Geometry pass fills G-buffer attachments.
2. Lighting pass accumulates light contribution.
3. Post passes apply SSAO/HDR/bloom/tone mapping.

---

## 6) Project Layout

Top-level folders:

- [src/](src/) → engine/editor/server source
- [shaders/](shaders/) → GLSL shaders
- [projects/](projects/) → sample/working projects
- [template/](template/) → project template skeleton
- [documents/](documents/) → engineering and documentation standards

---

## 7) Notes

- `Release` is the default build type if not explicitly set.
- CMake copies the shader directory into the build output.
- `HarukaEngineLib` is built as a shared library and reused by client/editor.

For deeper internal docs, see [documents/project_stl_standard.md](documents/project_stl_standard.md) and [documents/project_module_map.md](documents/project_module_map.md).
