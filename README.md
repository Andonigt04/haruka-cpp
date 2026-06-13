# HarukaEngine (v6)

**C++17 / OpenGL 4.6** real-time 3D engine for space exploration, planetary terrain generation, and game development.

Distributed as a shared library (`libHarukaEngine.so`) with integrated asset pipeline, physics engine, and procedural world generation.

## Examples



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
- **3D Audio** — OpenAL listener (camera) + **world sound sources** (persistent positional emitters) + one-shot SFX, with **terrain occlusion** (a source behind a hill is muffled). `propagation(from,to)` is a shared 0..1 query used for playback **and** logically (AI "hears" the player → stealth). Device selection in-settings (input/output, SDL3). Full propagation through openings (portal pathfinding) is planned.
- **Voice input (optional)** — Mic capture (SDL3) + **Vosk** offline speech-to-text with a restricted grammar; used by the game for voice-cast abilities. Compiles without Vosk (stub).
- **i18n / Locale** — `assets/lang/<code>.json` key→string, `TR("key")` resolution, base-language fallback. Languages auto-detected; modular per-language assets (`assets/voice/<code>`, `assets/audio/<code>`).
- **Terrain editing** — Deformation field (dig / build / **flatten/level**, sphere or oriented-box footprint) over procedural terrain, with chunk re-streaming.
- **Particles** — GL_POINTS additive radiant system (trails, impacts).
- **Settings** — Persisted graphics/audio/controls/language; rebindable input actions; in-game panel (tabs).
- **Component Architecture** — Scripts, materials, meshes attached to entities

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
| **vosk-api-devel** | *(optional)* offline speech-to-text for voice input. Without it the voice module compiles as a stub. |
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
└── 6/
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

### Modules (build-time)

The engine is split into optional modules toggled by a bitmask. CMake generates
`src/core/modules.h` (`#define HARUKA_MOD_<NAME> 1`) and excludes the sources of
disabled modules. Order of the bitmask = the list below.

| Bit | Module | Purpose |
|-----|--------|---------|
| 0 | `FLUIDS` | ocean + shallow-water + PBF particles (hybrid water) |
| 1 | `DEFORM` | terrain deformation field (dig / build / flatten) |
| 2 | `SOFTBODY` | XPBD cloth / jelly / wind |
| 3 | `NETWORK` | DGS network client |
| 4 | `AUDIO` | 3D audio: `audio_manager` (API + listener + world sources + propagation) → `audio_loader` (buffers) + `audio_system` (OpenAL backend) |
| 5 | `PARTICLES` | particle system |
| 6 | `TERRAIN` | planetary terrain (default-ON; off = no planet) |
| 7 | `POSTFX` | bloom / SSAO / HDR / IBL |
| 8 | `SHADOWS` | shadow maps |
| 9 | `PHYSICS` | rigid-body physics + colliders (default-ON) |

```bash
# All modules ON (default)
cmake -B build -DVERSION=6

# Custom mask (1 char per module, in list order): e.g. fluids+deform off
cmake -B build -DVERSION=6 -DMODULES=0011111111
```

> ⚠️ Configuring with `-DMODULES=...` rewrites `src/core/modules.h` in the source
> tree (via `configure_file`). The game (`Survival`) must be built with the **same**
> mask — its `build.sh` keeps engine + game in sync from one place.

### Shaders (engine vs game)

Engine shaders live in `haruka-cpp/shaders/` and are compiled to `.spv` (+ GLSL copy) at
build time. **Game-specific shaders live in the game repo** (e.g. `Survival/shaders/`) and are
compiled by the game's build into the same runtime `shaders/` dir, coexisting by name. Each
shader is compiled **once** by its owner — the game copies (does not recompile) the engine's
shaders. So adding a game shader never touches the engine.

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

**Current Version:** 6 — Deferred renderer, terrain streaming, physics foundation

---

## 📄 License



---

## 🔗 Related Projects

- **haruka** — ImGui-based editor built on top of HarukaEngine

---

**Last Updated:** 19 May 2026 | **OpenGL 4.6** | **C++17/20**

