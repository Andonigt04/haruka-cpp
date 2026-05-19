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
- **Double-Precision Coordinates** — Millimeter precision at astronomical scales (Half tested, at least not with a lot of objects & physics)
- **Procedural Planetary Terrain** — Cube-sphere projection with multi-octave Perlin noise (Ajustments must be done)
- **Infinite LOD System** — Quadtree-based terrain streaming with 4 detail levels
- **Async Chunk Generation** — Non-blocking terrain generation in background threads
- **Floating Origin** — Dynamic world re-centering to maintain float precision on GPU

### ⚙️ Physics & Simulation
- **Rigid Body Dynamics** — Gravity, collisions, friction
- **Planetary Gravity** — N-body gravitational forces + character ground detection
- **Broad-Phase Octree** — Efficient collision detection via spatial partitioning
- **Terrain Raycast** — Ground detection for procedural planet surfaces

### 🎮 Game Systems
- **3D Audio** — OpenAL positional audio with Doppler effects (Not fuly implemented)
- **Scene Management** — Unified entity system supporting 15+ object types
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

```bash                     # VERSION defaults to 1.0.0
cmake -B build -DVERSION=XX.XX.XX  -DHARUKA_NETWORK=ON/OFF # explicit version
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

---

## 📖 Documentation

Comprehensive architecture and implementation guides are included:

| Document | Content |
|----------|---------|
| [ANALISIS_SISTEMAS.md](docs/guides/ANALISIS_SISTEMAS.md) | Deep dive into ObjectType, SceneManager, WorldSystem, PlanetarySystem |
| [DIAGRAMAS_FLUJO.md](docs/guides/DIAGRAMAS_FLUJO.md) | Data flow diagrams, memory strategies, performance profiling |
| [PATRONES_USO.md](docs/guides/PATRONES_USO.md) | Code examples, floating origin, chunk generation, LOD system |
| [GUIA_RAPIDA.md](docs/guides/GUIA_RAPIDA.md) | Quick reference, debugging tips, configuration templates |
| [Doxygen API Docs](docs/html/index.html) | Autogenerated reference (run `doxygen Doxyfile`) |

### Generate API Documentation

Documentation generated by Doxygen.

---

## 🏗️ Architecture Highlights

Chunks are **generated asynchronously** using multi-octave Perlin noise and cached in RAM (LRU eviction when >256 MB).

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

## 📝 Status & Roadmap

**Current Version:** 0.5.0 — Deferred renderer, terrain streaming, physics foundation

---

## 📄 License



---

## 🔗 Related Projects

- **haruka** — ImGui-based editor built on top of HarukaEngine

---

**Last Updated:** 19 May 2026 | **OpenGL 4.6** | **C++17/20**

