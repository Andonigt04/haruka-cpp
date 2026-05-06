# HarukaEngine (v0.5.0)

**C++17 / OpenGL 4.6** real-time 3D engine for space exploration, planetary terrain generation, and game development.

Distributed as a shared library (`libHarukaEngine.so`) with integrated asset pipeline, physics engine, and procedural world generation.

## 🎯 Core Features

### 🖼️ Rendering
- **Deferred Shading Pipeline** — G-Buffer + light accumulation for hundreds of dynamic lights
- **PBR Materials** — Metallic/Roughness workflow with IBL (Image-Based Lighting)
- **Advanced Shadows** — Cascaded shadow maps + point lights + directional shadows
- **Post-Processing** — SSAO (Screen Space Ambient Occlusion), HDR + bloom, tone mapping
- **GPU Compute** — Frustum culling, compute post-processing via SPIR-V shaders

### 🌍 Procedural Worlds
- **Double-Precision Coordinates** — Millimeter precision at astronomical scales (millions of km)
- **Procedural Planetary Terrain** — Cube-sphere projection with multi-octave Perlin noise
- **Infinite LOD System** — Quadtree-based terrain streaming with 4 detail levels
- **Async Chunk Generation** — Non-blocking terrain generation in background threads
- **Floating Origin** — Dynamic world re-centering to maintain float precision on GPU

### ⚙️ Physics & Simulation
- **Rigid Body Dynamics** — Gravity, collisions, friction
- **Planetary Gravity** — N-body gravitational forces + character ground detection
- **Broad-Phase Octree** — Efficient collision detection via spatial partitioning
- **Terrain Raycast** — Ground detection for procedural planet surfaces

### 🎮 Game Systems
- **3D Audio** — OpenAL positional audio with Doppler effects
- **Scene Management** — Unified entity system supporting 15+ object types
- **Database Persistence** — PostgreSQL integration for player data, world state
- **WebSocket Networking** — Real-time multiplayer via `websocketpp`
- **Component Architecture** — Scripts, materials, meshes attached to entities

---

## 📋 System Architecture

```
┌─────────────────────────────────────────────┐
│         APPLICATION / EDITOR                 │
└────────────────┬──────────────────────────────┘
                 │
    ┌────────────▼──────────────┐
    │  WorldSystem (Core)       │  ← Orchestrates all subsystems
    │  - Floating Origin        │  ← Precision management
    │  - Origin shifting        │  ← Dynamic world re-centering
    └────────────┬──────────────┘
                 │
         ┌───────┴──────────┬──────────────┐
         ▼                  ▼              ▼
    ┌─────────────┐  ┌──────────────┐  ┌──────────────┐
    │ Physics     │  │ Planetary    │  │ Audio        │
    │ Engine      │  │ System       │  │ System       │
    │ (RigidBody) │  │ (Terrain)    │  │ (OpenAL)     │
    └─────────────┘  └──────────────┘  └──────────────┘
                            │
                    ┌───────┴───────┐
                    ▼               ▼
                ┌────────────┐  ┌──────────────┐
                │ LOD System │  │ Streaming    │
                │ (QuadTree) │  │ System       │
                └────────────┘  └──────────────┘
                    │               │
                    └───────┬───────┘
                            ▼
                    ┌──────────────────┐
                    │ Terrain          │
                    │ - Cache          │
                    │ - Generator      │
                    │ - Renderer       │
                    └──────────────────┘
```

### Key Abstractions

| Component | Purpose |
|-----------|---------|
| **SceneManager** | Unified entity storage with fast O(1) lookup |
| **SceneObject** | Universal entity representation (mesh, planet, light, etc.) |
| **ObjectType** | Enum-based classification (MESH, PLANET, LIGHT, CHARACTER, etc.) |
| **WorldSystem** | Coordinate precision management + subsystem orchestration |
| **PlanetarySystem** | Terrain generation, LOD, and streaming |
| **CelestialBody** | Macroscopic world entities (planets, stars, stations) |
| **PlanetChunkKey** | Unique identifier: (face, LOD, x, y) for terrain chunks |
| **ChunkData** | Generated terrain mesh data (vertices, normals, indices) |

---

## 🔧 Dependencies

### Install on Fedora

```bash
sudo dnf install cmake gcc-c++ assimp-devel openssl-devel openal-soft-devel \
    postgresql-devel gtk3-devel pkgconf-pkg-config glm-devel glslang
```

### Install SDL3 (manually)

SDL3 is not yet in Fedora repos. Choose one option:

```bash
# Option A: Vendorize (recommended)
git clone https://github.com/libsdl-org/SDL third_party/SDL
cd third_party/SDL && git checkout SDL3 && cd ../..

# Option B: Compile and install system-wide
git clone https://github.com/libsdl-org/SDL third_party/SDL
cmake -B third_party/SDL/build -S third_party/SDL -DCMAKE_BUILD_TYPE=Release
cmake --build third_party/SDL/build -j$(nproc)
sudo cmake --install third_party/SDL/build
```

### Dependency Reference

| Package | Role |
|---------|------|
| **cmake** | Build system |
| **gcc-c++** | C++17 compiler |
| **sdl3** | Window/context/input (**compile manually**, see above) |
| **glm-devel** | Vector math (vec3, mat4, dvec3 for astronomical precision) |
| **glslang** | GLSL → SPIR-V shader compilation (build-time) |
| **assimp-devel** | 3D model loading (OBJ, FBX, GLTF) |
| **openal-soft-devel** | 3D positional audio |
| **postgresql-devel** | Player/world persistence |
| **openssl-devel** | WebSocket TLS |
| **gtk3-devel** | File dialogs |
| **pkgconf-pkg-config** | Library discovery |

**Vendored:**
- `stb` — image loading
- `glad` — OpenGL functions
- `imgui` — editor UI
- `nlohmann/json` — scene serialization
- `nativefiledialog` — file dialogs

---

## 🔨 Build

### Quick Start

```bash
cmake -B build                          # VERSION defaults to 1.0.0
cmake -B build -DVERSION=   # explicit version
cmake --build build -j$(nproc)
```

### Output

```
build/
└── 0.5.0/
    ├── bin/
    │   ├── libHarukaEngine.so          ← Shared library
    │   └── shaders/
    │       ├── pbr.spv                 ← Compiled shaders
    │       ├── terrain.spv
    │       ├── shadow.spv
    │       └── ...
    └── include/
        └── Haruka/                     ← Public headers
```

### Install to Editor

```bash
cmake --install build --prefix ../haruka/build
```

This copies the library and headers to `haruka/build/0.5.0/` for linking by the editor.

### Shader Compilation

GLSL shaders in `shaders/` are compiled to SPIR-V during the build:

```bash
glslc shaders/pbr.frag -o build/0.5.0/bin/shaders/pbr.spv
glslc shaders/terrain.vert -o build/0.5.0/bin/shaders/terrain.spv
# ... etc
```

> **Requires OpenGL 4.6+** for `GL_SHADER_BINARY_FORMAT_SPIR_V` support (core since GL 4.6).

---

## 📖 Documentation

Comprehensive architecture and implementation guides are included:

| Document | Content |
|----------|---------|
| [ANALISIS_SISTEMAS.md](../ANALISIS_SISTEMAS.md) | Deep dive into ObjectType, SceneManager, WorldSystem, PlanetarySystem |
| [DIAGRAMAS_FLUJO.md](../DIAGRAMAS_FLUJO.md) | Data flow diagrams, memory strategies, performance profiling |
| [PATRONES_USO.md](../PATRONES_USO.md) | Code examples, floating origin, chunk generation, LOD system |
| [GUIA_RAPIDA.md](../GUIA_RAPIDA.md) | Quick reference, debugging tips, configuration templates |
| [Doxygen API Docs](docs/html/index.html) | Autogenerated reference (run `doxygen Doxyfile`) |

### Generate API Documentation

```bash
cd haruka-cpp && doxygen Doxyfile
xdg-open docs/html/index.html
```

---

## 🏗️ Architecture Highlights

### Floating Origin for Astronomical Precision

The engine maintains **double-precision world coordinates** on the CPU while rendering with **single-precision floats** on the GPU. This enables millimeter-accurate rendering at distances of millions of kilometers.

```cpp
// CPU: double precision (64-bit)
glm::dvec3 worldPos = glm::dvec3(1234567.89012, 9876543.21098, ...);

// GPU: float + offset correction
glm::vec3 localPos = worldSystem->toLocal(worldPos);  // Subtracts origin
gl_Position = projection * view * vec4(localPos, 1.0);
```

When the camera drifts >5000 km from the world origin, the entire universe is re-centered dynamically, keeping local coordinates small and precise.

### Procedural Terrain with Streaming

Planets use **cube-sphere projection** (6 faces) subdivided into a **Quadtree hierarchy**:

```
Planet "Earth"
├── FRONT face
│   ├── LOD 0 (max detail)  ← <100m from camera
│   │   ├── Chunk (0,0)  64×64 vertices
│   │   ├── Chunk (1,0)  64×64 vertices
│   │   └── ...
│   ├── LOD 1             ← 100-500m
│   │   ├── Chunk (0,0)  32×32 vertices
│   │   └── ...
│   └── LOD 2-3           ← 500m+
├── BACK, TOP, BOTTOM, RIGHT, LEFT faces
└── ...
```

Chunks are **generated asynchronously** using multi-octave Perlin noise and cached in RAM (LRU eviction when >256 MB).

### Unified Entity System

All objects (meshes, planets, lights, characters, audio sources) are represented as `SceneObject` with:

- **Type classification** via `ObjectType` enum
- **Transform** in double precision
- **Components** as JSON (materials, scripts, physics)
- **Hierarchy** via parent/child indices
- **O(1) lookup** by name via SceneManager hash tables

---

## 🎮 Usage Example

### Load a Scene

```cpp
#include <Haruka/core/application.h>

Haruka::Application app;
app.create_window();          // SDL3 window
app.create_gl_context();      // OpenGL 4.6
app.loadScene("my_world.json");

app.run();  // Main loop
```

### Create a Planet Programmatically

```cpp
Haruka::PlanetarySystem& planetary = app.getWorldSystem()->getPlanetarySystem();

Haruka::PlanetarySystem::Planet earth{
    .name = "Earth",
    .position = Haruka::WorldPos(0.0, 0.0, 0.0),
    .radius = 6371.0,  // km
    .terrainSettings = {
        {"seed", 42},
        {"minHeight", -200.0},
        {"maxHeight", 1200.0},
        {"layers", json::array({
            {{"freq", 0.1}, {"octaves", 4}, {"persistence", 0.6}},
            {{"freq", 1.0}, {"octaves", 3}, {"persistence", 0.5}},
            {{"freq", 4.0}, {"octaves", 2}, {"persistence", 0.4}}
        })}
    }
};

planetary.addPlanet(earth);
```

### Scene JSON Format

```json
{
  "objects": [
    {
      "name": "Earth",
      "type": "planet",
      "position": [0.0, 0.0, 0.0],
      "hasChunks": true,
      "terrainSettings": {
        "seed": 42,
        "minHeight": -200.0,
        "maxHeight": 1200.0,
        "layers": [...]
      },
      "lodSettings": {
        "distances": [100.0, 500.0, 2000.0, 10000.0]
      },
      "streamingSettings": {
        "mode": "procedural",
        "cacheSizeMB": 256
      }
    }
  ]
}
```

---

## 🚀 Performance Targets

| Metric | Target |
|--------|--------|
| Floating Origin update | <1ms per 5000 km |
| Chunk generation | <10ms per chunk (async) |
| LOD recalculation | <2ms |
| Physics update | <3ms |
| Render (1000 objects) | <8ms (60 FPS @ 120Hz display) |
| Memory (cached chunks) | <256 MB (LRU eviction) |

---

## 📝 Status & Roadmap

**Current Version:** 0.5.0 — Deferred renderer, terrain streaming, physics foundation

### ✅ Implemented
- Deferred shading + PBR materials
- Cascaded shadow maps + point shadows
- SSAO, IBL, bloom, tone mapping
- Procedural planetary terrain (Perlin noise)
- Async chunk generation & streaming
- Physics engine (rigid bodies, gravity)
- Scene management (JSON-based)
- 3D audio (OpenAL)

### 🔄 In Progress
- WebSocket multiplayer
- PostgreSQL persistence
- Advanced terrain features (erosion, water)
- GPU terrain generation (compute shaders)

### 📋 Planned
- Advanced orbital mechanics (N-body simulation)
- Biome system (desert, forest, ice)
- Character animation (skeletal meshes)
- Advanced networking (interest management)

---

## 🧪 Testing

No automated test suite yet. Manual validation steps:

1. **Build succeeds** with `-DVERSION=x.y.z`
2. **Shaders compile** without SPIR-V errors (`glslc` exit 0)
3. **Doxygen runs** to completion with zero content errors
4. **Editor loads** `libHarukaEngine.so` without missing symbols

```bash
# Quick validation
cmake --build build 2>&1 | grep -i error
doxygen Doxyfile 2>&1 | grep -i error
ldd build/0.5.0/bin/libHarukaEngine.so | grep "not found"
```

---

## 📄 License

MIT License — See [LICENSE](../LICENSE) file.

---

## 🔗 Related Projects

- **haruka** — ImGui-based editor built on top of HarukaEngine
- **Survival** — Example game project showcasing terrain & physics

---

**Last Updated:** 6 May 2026 | **OpenGL 4.6** | **C++17/20**

