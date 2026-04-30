# Haruka Engine

Haruka Engine is a C++17 Vulkan real-time engine with:

- deferred rendering + PBR
- editor/runtime/server targets
- procedural planetary tooling
- integrated post-processing and lighting stack

---

## Setup

Download Dev dependecies
```bash
  sudo dnf install cmake gcc-c++ assimp-devel openssl-devel openal-soft-devel postgresql-libs gtk3-devel pkgconf-pkg-config glm-devel glad-devel vulkan-loader vulkan-headers vulkan-tools sdl3-devel
```

---

## Architecture Overview

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

## Build Requirements

### Toolchain

- CMake >= 3.14
- C++17 compiler (GCC/Clang/MSVC)

### Required libraries

Resolved by CMake via `find_package`/`pkg-config`:

- OpenSSL
- OpenAL
- PostgreSQL client (`libpq`)
- GTK3 (for native file dialog/editor integration)

### Third-party sources

The repo expects `third_party/` sources (GLM, stb, etc.).
You can bootstrap the folder with [setup_deps.sh](setup_deps.sh).

---

## Build

Build folder
```bash
mkdir -p build
```

Normal cmake setup
```bash
cmake ..
```

In the build folder indicate, where do you want to store the project for IDE use.
```bash
cmake --install . --prefix ../../haruka/build
```

For compilation.
```bash
make -j${nproc}
```

Or if you want to install it into the SO.
```bash
make install
```
---

## Key Runtime Pipelines

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

## Project Layout

Top-level folders:

- [src/](src/) → engine/editor/server source
- [template/](template/) → project template skeleton
- [docs/](docs/) → engineering and documentation standards

---

## Notes

- `Release` is the default build type if not explicitly set.
- CMake copies the shader directory into the build output.
- `HarukaEngineLib` is built as a shared library and reused by client/editor.

For deeper internal docs, see [docs/project_stl_standard.md](docs/project_stl_standard.md) and [docs/project_module_map.md](docs/project_module_map.md).
