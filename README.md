# HarukaEngine (v1.0.0)

## 🚧 Work in Progress / Active Development

**C++17 / OpenGL 4.6** real-time 3D engine for space exploration, planetary terrain generation, and game development.

Distributed as a shared library (`libHarukaEngine.so`) with integrated asset pipeline, physics engine, and procedural world generation.

<img src="image.png" />

## ✨ What makes Haruka unique

Most engines render a *level*. Haruka renders a **solar system at true scale** and lets you walk on it — the same code path takes you from orbiting a star to a pebble at your feet, with no loading screens and no fake skybox.

- **🪐 Real-scale planets, millimetre precision.** Earth sits ~1 AU (1.5×10⁸ m) from the sun at its real radius (6 371 km). Rendering is **camera-relative double precision**: the world is re-centred on the camera every frame so GPU floats never lose precision, no matter how far from the origin you are. You can fly from interplanetary space down to the grass without a seam.

- **⚡ GPU-compute terrain, streamed grueso→fino.** The planetary heightfield is generated on the **GPU via compute shaders** and harvested **asynchronously with GL fences** — the CPU never blocks on the GPU. The render thread does almost nothing: chunk vertex directions are **derived on the GPU** (not built and uploaded), the readback lands in **persistently-mapped buffers**, and the copy + mesh assembly happen on **worker threads**. A cube-sphere **quadtree LOD** streams chunks **progressively coarse→fine**: a node only subdivides once its parent is resident, so you **never see a black hole** while detail loads — the planet fills in from low-poly to crisp. An optional **LOD floor** keeps the *entire* planet resident at a base detail so the far side is always there.

- **🌋 Terrain by process, not by noise.** Elevation isn't a noise formula — it's the **output of a simulation** run once per seed: **plate tectonics** (Voronoi plates, noise-warped boundaries, convergence → orogeny, continental margins) feeds **hydrology and erosion** (priority-flood lakes, D8 flow accumulation, stream-power incision, thermal talus). The eroded field is the **single source of truth**, shared by the terrain compute shader, the physics sampler and the water shader — so mountains have *drainage*, rivers run *downhill into* lakes and the coast is simply where elevation = 0. A parity test keeps the CPU and GPU heightfields within centimetres of each other, because a mismatch means you walk through the ground.

- **🌗 Movable celestial bodies.** Planets and moons **orbit** (day/night from the sun, moonlight, sun–moon tides). Terrain chunks are stored **planet-local**, so a body can move every frame **without regenerating a single chunk** — the renderer just re-anchors them. Moons, gas giants and home worlds are **the same kind of procedural, streamed body**, declared entirely in the **scene file** (a `terran` / `moon` / `gas` generation profile), not in game code.

- **🌊 Hybrid water & living sky.** Oceans (Gerstner waves), inland rivers/lakes and PBF fluid particles share one water system. A **procedural atmosphere** (gradient sky + sun disc + stars that emerge at night and in space, fading to black with altitude) replaces any cubemap. Wind, tides and a cratered moon are all driven by the same celestial mechanics.

- **🛠️ Deformable, editable worlds.** A deformation field lets you **dig, build and flatten** the procedural surface (sphere or oriented-box footprint), with the affected chunks re-streamed live — terrain that's both generated *and* author/player-editable.

- **🔊 One solver, two senses.** The **propagation** system finds the path A→B **around walls and through openings** (placed colliders + terrain occlusion). It's shared by **audio** (sound bends around corners; AI literally "hears" the player for stealth) **and** by **curving projectiles** (serpentine spells follow the same route).

- **🧩 Modular by bitmask.** Fluids, soft-body, networking, audio, particles, terrain, post-FX, shadows and physics are **compile-time modules** — strip what you don't need to a lean binary; the game is built with the same mask.

> In short: a **source-available, single-`.so` engine purpose-built for seamless planetary-scale worlds** — double-precision origin, GPU-streamed procedural terrain, orbiting editable planets, and a propagation model that unifies stealth audio and curving spells.

---

## 🎯 Core Features

### 🖼️ Rendering
- **RHI abstraction** (`src/rhi/`) — Device/Context + **pipeline state objects**; the renderers issue no raw GL calls. OpenGL is one backend; the seam for a Vulkan one is already in place.
- **Reversed-Z with an infinite far plane** — depth as `near/dist`, `glClipControl(ZERO_TO_ONE)`, D32F. Precision is uniform from a pebble to a planet; a conventional 0.1 / 3e11 range collapsed all terrain depth into the last 0.004% of the buffer.
- **Deferred Shading Pipeline** — G-Buffer + light accumulation for hundreds of dynamic lights
- **PBR Materials** — Metallic/Roughness workflow with IBL (Image-Based Lighting)
- **Advanced Shadows** — Cascaded shadow maps + point lights + directional shadows
- **Post-Processing** — SSAO (Screen Space Ambient Occlusion), HDR + bloom, tone mapping
- **GPU Compute** — Frustum culling, compute post-processing via SPIR-V shaders
- **Pooled terrain draws** — chunks share vertex pools and are drawn with `drawIndexedIndirect` (`gl_DrawID` indexes a per-draw SSBO), so there are no per-chunk uniforms or VAO rebinds

### 🌍 Procedural Worlds
- **Double-Precision Coordinates** — Millimeter precision at astronomical scales (camera-relative; tested to AU distances)
- **Geology → hydrology → terrain** — plate tectonics (`planet_geology`) drives an eroded cube-sphere field (`planet_fields`: lakes, rivers, stream-power + thermal erosion), cached once per seed and sampled by CPU *and* GPU
- **MESO tiles** *(opt-in: `HARUKA_MESO=1`)* — on-demand ~5 km tiles (128², radius-derived) re-eroded locally with **inherited flow + margin** so tile borders don't seam; built on a worker, never on the render thread
- **Cube-sphere Quadtree LOD** — Deep, distance-driven detail (root face → fine leaves) with **progressive coarse→fine streaming** and an optional whole-planet LOD floor
- **Async Chunk Generation** — GPU dispatch harvested via **GL fences** into persistently-mapped buffers; copy + mesh assembly on a worker pool (never blocks the frame); LRU RAM cache + optional disk cache
- **Floating Origin** — Per-frame world re-centering on the camera to keep GPU float precision
- **CPU↔GPU parity, tested** — the physics sampler and the terrain compute shader must agree (median error ~2 cm); the mesh and the collision are the same surface

### ⚙️ Physics & Simulation
- **Rigid Body Dynamics** — Gravity, collisions, friction
- **Planetary Gravity** — N-body gravitational forces + character ground detection
- **Broad-Phase Octree** — Efficient collision detection via spatial partitioning
- **Terrain Raycast** — Ground detection for procedural planet surfaces

### 🎮 Game Systems
- **3D Audio** — OpenAL listener (camera) + **world sound sources** (persistent positional emitters) + one-shot SFX. Device selection in-settings (input/output, SDL3).
- **Propagation / nav** (`core/propagation.*`) — finds the path A→B **around walls** (placed OBB colliders) and through openings + **terrain occlusion**; `through()` returns a 0..1 throughput (playback **and** logical: AI "hears" the player → stealth), `path()` returns the waypoints. **Shared** by audio (sound routes around walls) and by **curving spells** (serpenteante projectiles follow the same path).
- **Voice input (optional)** — Mic capture (SDL3) + **Vosk** offline speech-to-text with a restricted grammar; used by the game for voice-cast abilities. Compiles without Vosk (stub).
- **i18n / Locale** — `assets/lang/<code>.json` key→string, `TR("key")` resolution, base-language fallback. Languages auto-detected; modular per-language assets (`assets/voice/<code>`, `assets/audio/<code>`).
- **Terrain editing** — Deformation field (dig / build / **flatten/level**, sphere or oriented-box footprint) over procedural terrain, with chunk re-streaming.
- **Particles** — GL_POINTS additive radiant system (trails, impacts).
- **Settings** — Persisted graphics/audio/controls/language; rebindable input actions; in-game panel (tabs).
- **Component Architecture** — Scripts, materials, meshes attached to entities

---

## 🔧 Dependencies

Haruka needs three groups of dependencies. **Two of them you already have** the moment you
clone the repo — only the system libraries and SDL3 require a download.

### 1. Vendored — *already in the repo* (nothing to download)

These ship **committed** inside the source tree (they are **not** git submodules, so a plain
`git clone` is enough — no `git submodule update`):

| Path | Library | Role |
|------|---------|------|
| `third_party/glad`            | GLAD            | OpenGL 4.6 function loader |
| `third_party/imgui`           | Dear ImGui      | editor / debug UI |
| `third_party/json`            | nlohmann/json   | scene & save serialization |
| `third_party/stb`             | stb_image       | image loading |
| `third_party/nativefiledialog`| nativefiledialog| file dialogs |
| `external/dgs`                | DGS client      | optional networking (`HARUKA_NETWORK`) |

> ✔️ No action required — CMake picks these up from the tree.

### 2. System libraries — one install command

These come from your distro's package manager.

```bash
# Fedora
sudo dnf install cmake gcc-c++ pkgconf-pkg-config \
    glm-devel glslang \
    assimp-devel openal-soft-devel libzstd-devel \
    openssl-devel postgresql-devel gtk3-devel
```

```bash
# Debian / Ubuntu (equivalents)
sudo apt install cmake g++ pkg-config \
    libglm-dev glslang-tools \
    libassimp-dev libopenal-dev libzstd-dev \
    libssl-dev libpq-dev libgtk-3-dev
```

*Optional:* `vosk-api` for offline voice input — without it the voice module compiles as a
stub, so you can skip it.

### 3. SDL3 — download & build manually

SDL3 isn't in most distro repos yet. Pick one option:

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

### System-library reference

| Package (Fedora) | Role |
|---------|------|
| **cmake** | Build system |
| **gcc-c++** | C++17 compiler |
| **pkgconf-pkg-config** | Library discovery |
| **sdl3** | Window/context/input (**compile manually**, see above) |
| **glm-devel** | Vector math (vec3, mat4, **dvec3 for astronomical precision**) |
| **glslang** | GLSL → SPIR-V shader compilation (build-time; `glslc` also works) |
| **assimp-devel** | 3D model loading (OBJ, FBX, GLTF) |
| **openal-soft-devel** | 3D positional audio |
| **libzstd-devel** | Save / asset compression codec |
| **postgresql-devel** | Player/world persistence |
| **openssl-devel** | WebSocket TLS |
| **gtk3-devel** | File dialogs |
| **vosk-api-devel** | *(optional)* offline speech-to-text for voice input — without it the voice module compiles as a stub |

> The vendored libraries (`glad`, `imgui`, `nlohmann/json`, `stb`, `nativefiledialog`, `dgs`)
> are listed in **§1 above** and need no install.

---

## 🔨 Build

### Quick Start

```bash                     # VERSION defaults to 1.0
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
cmake -B build -DVERSION=1.0

# Custom mask (1 char per module, in list order): e.g. fluids+deform off
cmake -B build -DVERSION=1.0 -DMODULES=0011111111
```

> ⚠️ Configuring with `-DMODULES=...` rewrites `src/core/modules.h` in the source
> tree (via `configure_file`). The game (`Survival`) must be built with the **same**
> mask — its `build.sh` keeps engine + game in sync from one place.

### Shaders (engine vs game)

Engine shaders live in `haruka-cpp/assets/shaders/` (the engine's own `assets/` root — also
where engine-provided **default** textures/fonts would go) and are compiled to `.spv` (+ GLSL
copy) at build time into the runtime `assets/shaders/` dir. **Game-specific shaders live in the
game repo** and the game's build compiles them into the **same** `assets/shaders/` dir,
coexisting by name. Each shader is compiled **once** by its owner. So adding a game shader
never touches the engine. (Runtime path resolution: `AssetPaths::shaders()` → `assets/shaders/`,
identical in dev and release.)

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

Chunks are **generated asynchronously on the GPU** from the planet's eroded geological field (not from raw noise), assembled on a worker pool and cached in RAM (LRU eviction, budget auto-sized from system RAM) with an optional versioned disk cache.

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

## 📝 Engine status

**v1.0 — playable and stable.** A real-scale solar system streams and renders end to end: you can
orbit, fly in, land and walk, dig the ground, swim, and the planet stays consistent between what you
*see* and what you *collide with*. Below is an honest account of what is done, what is opt-in and
what is not there yet.

### ✅ Working (on by default)

| Area | State |
|------|-------|
| Rendering | Deferred + PBR + shadows + post-FX, all through the **RHI/PSO** layer (no raw GL in the renderers) |
| Depth | **Reversed-Z + infinite far plane** — no z-fighting between terrain and water at any scale |
| Terrain | GPU compute generation from the **eroded geological field** (tectonics → hydrology → erosion) |
| Streaming | Async LOD recompute on a worker; GPU-derived vertex grids; persistently-mapped readback; worker pool for mesh assembly; pinned coarse pyramid (never evicted → no holes) |
| Water | Ocean (Gerstner) + lakes/rivers + PBF particles; depth-based thickness and foam, reading the scene depth instead of re-deriving terrain |
| Physics | Rigid bodies, planetary gravity, octree broad-phase, terrain raycast |
| Tests | **61/61 green** (`haruka_tests`) — includes CPU↔GPU parity, LOD invariants (no holes / no overlaps), streaming settle, orbit closure |

### 🧪 Opt-in (works, off by default)

| Flag | What it does | Why it's off |
|------|--------------|--------------|
| `HARUKA_MESO=1` | ~5 km eroded tiles under the player: relief you actually *walk on* (+64 m over the macro field, ≤2 m seam at tile borders) | Suite is green with it, but it hasn't been through a long play session yet |
| `HARUKA_DISKCACHE=1` | Persist generated chunks to disk (versioned, 4 GB cap, background writer) | With a populated cache the streaming doesn't settle in 3 tests — root cause not yet found |
| `HARUKA_PROF_LOG=N` | Dump the profiler tree to stderr every N frames | Diagnostics only |

### 🎯 Performance

Frame cost is dominated by *genuine* work (GPU uploads inside their budget), not by waste. Measured
in-game over ~1 500 frames while flying and turning:

| Scope | p50 | p90 | p99 |
|-------|-----|-----|-----|
| `planetary.update` (LOD + streaming) | 5.6 ms | 10.6 ms | 18 ms |
| `pump.harvest` (GPU readback) | 0 ms | 0.8 ms | 3.0 ms |
| `scan.dispatch` (chunk dispatch) | 0 ms | 0.3 ms | 0.5 ms |

Frames over 30 ms: **4 in 1 338**. The remaining p90 is the chunk-upload time-box (5 ms/frame by
design — spending it is the point, it's how new terrain reaches the GPU without stalling).

### 🚧 Next

1. **Climate → biomes → props** — orographic rain shadow, Whittaker biomes, and vegetation/resources placed *by the field* (a forest where there's water and soil), not by uniform noise.
2. **Materials** — height-blended splatmap so slope actually switches texture.
3. **Vulkan backend** — the RHI/PSO seam is done; this is now a backend, not a rewrite. (Note: it would *not* fix a CPU-bound frame — the bottleneck is game logic, not draw submission.)
4. **Parked:** caves and floating islands — deliberately postponed until the surface is credible.

---

## 📄 License

**Haruka Source-Available License v1.0** — © 2026 Andoni García Torres. See [LICENSE](LICENSE.md).

Free to view, study, modify, and use for **non-commercial** purposes; **commercial use needs
written permission**. Any improvement built using the engine is **licensed back** to the author
(grant-back). Third-party components (DGS, Dear ImGui, GLAD, GLM, SDL3, OpenAL, …) keep their
own licenses.

---

## 🔗 Related Projects

- **haruka** — ImGui-based editor built on top of HarukaEngine

---

**Last Updated:** 14 July 2026 | **OpenGL 4.6 (RHI: Vulkan-ready)** | **C++17/20**
