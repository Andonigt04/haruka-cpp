# Haruka Engine

Haruka Engine is a C++17 Vulkan real-time engine with:

- deferred rendering + PBR
- editor/runtime/server targets
- procedural planetary tooling
- integrated post-processing and lighting stack

The repository builds three main applications from the same codebase:

- `HarukaEditor` (tools and authoring)
- `HarukaEngine` (runtime client)
- `HarukaServer` (dedicated server with database support)

---

## Setup

Download Dev dependecies
```bash
  sudo dnf install cmake gcc-c++ glfw3-devel assimp-devel openssl-devel openal-soft-devel postgresql-libs gtk3-devel pkgconf-pkg-config glm-devel glad-devel vulkan-loader vulkan-headers vulkan-tools sdl3-devel
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

## Rendering Features

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


### Shader Asset Overview

| Shader File                | Propósito / Descripción breve                                      |
|----------------------------|-------------------------------------------------------------------|
| bloom_blur.frag            | Desenfoque gaussiano para el efecto bloom (horizontal/vertical)    |
| bloom_composite.frag       | Composición de imagen base y bloom, con ajuste de intensidad       |
| bloom_extract.frag         | Extracción de regiones brillantes para bloom                      |
| brdf_lut.frag              | Generación de tabla BRDF LUT para IBL/PBR                         |
| brdf_lut.vert              | Fullscreen quad para BRDF LUT                                     |
| deferred_geom.vert/.frag   | Paso de geometría para G-buffer (pos, normal, albedo, etc.)       |
| deferred_light.frag        | Paso de iluminación diferida, sombras, SSAO, IBL, etc.            |
| equirect_to_cubemap.*      | Conversión de mapas HDR equirectangulares a cubemap               |
| frustum_cull.comp          | Culling de cuerpos por frustum y LOD en compute                   |
| ibl.frag                   | Lighting IBL (irradiancia, prefilter, BRDF LUT)                   |
| instancing.vert/.frag      | Renderizado de instancias con color y transformaciones por objeto  |
| irradiance_convolution.*   | Cálculo de irradiancia para IBL (cubemap convolución)              |
| light_cube.frag            | Sombreado procedural de cubos de luz y terreno                    |
| parallax.frag              | Sombreado PBR con parallax mapping                                |
| pbr.frag                   | Sombreado PBR estándar (metallic/roughness, IBL, luces)           |
| planet_generation.comp     | Generación procedural de planetas en compute                      |
| point_shadow.vert/.geom/.frag | Sombra de punto omnidireccional (cubemap shadow mapping)      |
| prefilter_env.vert/.frag   | Prefiltrado de entorno para IBL (roughness mip chain)             |
| screenquad.vert            | Fullscreen quad para post-procesos                                |
| shadow.vert/.frag          | Sombreado de mapas de sombra (direccional)                        |
| simple.vert/.frag          | Sombreado PBR/Phong simple, materiales y luces múltiples           |
| ssao.frag                  | Ambient occlusion en pantalla (SSAO)                              |
| tonemapping.frag           | Mapeo de tonos HDR y mezcla de bloom                              |


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
cmake --install . --prefix ../../haruka/
```

For compilation.
```bash
make -j${nproc}
```

Or if you want to install it into the SO.
```bash
make install
```

Generated binaries:

- `build/libHarukaEngine.so`

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
- [shaders/](shaders/) → GLSL shaders compile to SPV
- [template/](template/) → project template skeleton
- [docs/](docs/) → engineering and documentation standards

---

## Notes

- `Release` is the default build type if not explicitly set.
- CMake copies the shader directory into the build output.
- `HarukaEngineLib` is built as a shared library and reused by client/editor.

For deeper internal docs, see [documents/project_stl_standard.md](docs/project_stl_standard.md) and [documents/project_module_map.md](docs/project_module_map.md).
