# Haruka Engine

C++17 OpenGL 4.6 real-time engine built as a shared library (`libHarukaEngine.so`).

- Deferred rendering + PBR (metallic/roughness)
- SPIR-V shaders compiled at build time from GLSL
- Procedural planetary tooling
- Post-processing: SSAO, IBL, HDR, bloom, tone mapping, cascaded shadows

---

## Architecture

Source modules:

- `src/core` — application shell, scene model, camera, common systems
- `src/renderer` — rendering pipeline, GPU resources, shadows, post-process
- `src/game` — gameplay systems, planet generation, character systems
- `src/network` — websocket/socket client-server abstractions
- `src/database` — PostgreSQL persistence layer
- `src/physics` — octree, rigid-body simulation, raycast helpers
- `src/audio` — OpenAL integration
- `src/io` — asset streaming and loading utilities

GLSL shaders live in `shaders/` and are compiled to SPIR-V at build time via `glslc` or `glslangValidator`.

---

## Dependencies

```bash
sudo dnf install cmake gcc-c++ assimp-devel openssl-devel openal-soft-devel \
    postgresql-devel gtk3-devel pkgconf-pkg-config glm-devel sdl3-devel glslang
```

Also requires `glad` and `stb` under `third_party/`

---

## Build

```bash
cmake -B build                        # defaults to ENGINE_VERSION=1.0.0
cmake -B build -DENGINE_VERSION=2.0.0 # explicit version
cmake --build build -j$(nproc)
```

Output layout inside the build directory:

```
build/
└── 1.0.0/
    └── bin/
        ├── libHarukaEngine.so
        └── shaders/
            ├── simple.vert.spv
            ├── pbr.frag.spv
            └── ...
```

Headers stay in source at `src/` and are referenced directly during editor builds.

---

## Install into Haruka Editor

To deploy the engine for the editor without a system-wide install:

```bash
cmake --install build --prefix ../haruka/build
```

Result:

```
haruka/build/
└── 1.0.0/
    ├── bin/
    │   ├── libHarukaEngine.so
    │   └── shaders/*.spv
    └── lib64/
        └── haruka-cpp/   ← headers and sources
```

The editor's CMake will auto-discover this layout.

---

## Rendering Features

- Deferred shading (G-buffer geometry + lighting passes)
- PBR material workflow (metallic/roughness)
- Directional and point-light shadows, cascaded shadow maps
- SSAO
- IBL (irradiance + prefilter + BRDF LUT)
- HDR and tone mapping
- Bloom
- Compute-shader post-processing path

---

## Notes

- Requires OpenGL 4.6 (core SPIR-V support via `GL_SHADER_BINARY_FORMAT_SPIR_V`).
- `Release` is the default build type.
- `ENGINE_VERSION` controls both the build output subdirectory and the install path.
