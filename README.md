# HarukaEngine (v1.0.0)

## 🚧 Work in Progress / Active Development

**C++17 / OpenGL 4.6** real-time 3D engine for space exploration, planetary terrain generation, and game development.

Distributed as a shared library (`libHarukaEngine.so`) with integrated asset pipeline, physics engine, and procedural world generation.

<img src="image.png" />

## ✨ What makes Haruka unique

Most engines render a *level*. Haruka renders a **solar system at true scale** and lets you walk on it — the same code path takes you from orbiting a star to a pebble at your feet, with no loading screens and no fake skybox.

- **🪐 Real-scale planets, centimetre agreement.** Earth sits ~1 AU (1.5×10⁸ m) from the sun at its real radius (6 371 km). Rendering is **camera-relative double precision**: the world is re-centred on the camera every frame so GPU floats never lose precision, no matter how far from the origin you are. You can fly from interplanetary space down to the grass without a seam — and within the play radius, **what you see and what you collide with agree to 0.3 mm** (measured, see [Terrain status](#-terrain-status)).

- **⚡ GPU-compute terrain, streamed grueso→fino.** The planetary heightfield is generated on the **GPU via compute shaders** and harvested **asynchronously with GL fences** — the CPU never blocks on the GPU. Chunk vertex directions are **derived on the GPU** (not built and uploaded), the readback lands in **persistently-mapped buffers**, and the copy + mesh assembly happen on **worker threads**. A cube-sphere **quadtree LOD** streams chunks **progressively coarse→fine**: a node only subdivides once its parent is resident, so you **never see a black hole** while detail loads.

- **🌋 Terrain by process, not by noise.** Elevation isn't a noise formula — it's the **output of a simulation** run once per seed: **plate tectonics** (Voronoi plates, noise-warped boundaries, convergence → orogeny, continental margins) feeds **hydrology and erosion** (priority-flood lakes, D8 flow accumulation, stream-power incision, thermal talus), and that in turn drives **climate** (temperature by latitude + lapse rate, humidity advected from the sea with **orographic rain shadow**). The eroded field is the **single source of truth**, shared by the terrain compute shader, the physics sampler, the water shader and prop placement — so mountains have *drainage*, rivers run *downhill into* lakes, deserts sit *behind* mountain ranges, and the coast is simply where elevation = 0.

- **🧭 One ground, not three.** The surface the game runs on is the **reference surface** (`reference_surface.h`): the terrain sampled on the *same vertex lattice the renderer draws*, with the same bilinear filter. Physics, props, scripts, tests and the renderer all read it. It is **GL-free** and lives in `haruka_simbase`, so the authoritative server can evaluate the exact ground the client walks on. Before it there were three different "grounds" and you could watch the player fall through one of them.

- **🌗 Movable celestial bodies.** Planets and moons **orbit** (day/night from the sun, moonlight, sun–moon tides). Terrain chunks are stored **planet-local**, so a body can move every frame **without regenerating a single chunk**. Moons, gas giants and home worlds are declared entirely in the **scene file** (a `terran` / `moon` / `gas` generation profile), not in game code.

- **🌦️ Weather as world state.** Fronts with position and drift travel the planet as a **pure function of (seed, time, direction)** — deterministic, GL-free, server-verifiable. Precipitation is *derived from cloud cover*, so "it rains only where there are clouds" is an invariant, not a coincidence. A **zenith mask** pass means it doesn't rain under a roof, and a **granular layer** (snow / sand / mud) sits *above* the reference surface with footprints that slow you down — without touching the ground the collision reads.

- **🔊 One solver, two senses.** The **propagation** system finds the path A→B **around walls and through openings** (placed colliders + terrain occlusion). It's shared by **audio** (sound bends around corners; AI literally "hears" the player for stealth) **and** by **curving projectiles** (serpentine spells follow the same route).

- **🧩 Modular by bitmask.** Fluids, soft-body, networking, audio, particles, terrain, post-FX, shadows and physics are **compile-time modules** — strip what you don't need to a lean binary; the game is built with the same mask.

> In short: a **source-available, single-`.so` engine purpose-built for seamless planetary-scale worlds** — double-precision origin, GPU-streamed procedural terrain, one authoritative ground shared by client and server, orbiting editable planets, and a propagation model that unifies stealth audio and curving spells.

---

## 🎯 Core Features

> Honesty note: this list is what is **wired and running**. Several classes exist in the tree
> without an active call site (deferred G-Buffer, cascaded/point shadows, IBL, SSAO pass, virtual
> texturing, compute post-processing) — they are listed under
> [Not wired](#-present-in-the-tree-but-not-wired), not here. See
> [docs/ESTADO_MOTOR.md](docs/ESTADO_MOTOR.md) for the per-subsystem audit with file:line references.

### 🖼️ Rendering
- **RHI abstraction** (`src/rhi/`) — Device/Context + **pipeline state objects**; the scene renderers issue no raw GL calls. OpenGL is the one backend; the seam for a Vulkan one is in place (`src/rhi/vulkan/` is still empty).
- **Reversed-Z with an infinite far plane** — depth as `near/dist`, `glClipControl(ZERO_TO_ONE)`, D32F. Precision is uniform from a pebble to a planet; a conventional 0.1 / 3e11 range collapsed all terrain depth into the last 0.004% of the buffer.
- **Forward shading + stylised pass** — `simple.vert` + `final.frag` (soft cel + rim light), one directional shadow map, and a **zenith sky-exposure mask** used by the weather system.
- **Post-Processing** — HDR, bloom (bright-pass + blur), tone mapping, FXAA, render-scale.
- **Pooled terrain draws** — chunks share vertex pools and are drawn with `drawIndexedIndirect` (`gl_DrawID` indexes a per-draw SSBO), so there are no per-chunk uniforms or VAO rebinds.
- **GPU instancing** — construction pieces and props (index + matrix + tint), through the PSO layer.

### 🌍 Procedural Worlds
- **Double-Precision Coordinates** — camera-relative; tested to AU distances.
- **Geology → hydrology → climate → terrain** — plate tectonics (`planet_geology`) drives an eroded cube-sphere field (`planet_fields`: lakes, rivers, stream-power + thermal erosion, temperature and orographic humidity), cached once per seed and sampled by CPU *and* GPU.
- **Cube-sphere Quadtree LOD** — screen-space error driven, with a **keep radius** (the terrain around you is fine *whichever way you look*) and a **pinned ground LOD** so a single lattice exists under your feet.
- **Reference surface** — the authoritative, camera-independent ground (see above). GL-free.
- **Async Chunk Generation** — GPU dispatch harvested via **GL fences** into persistently-mapped buffers; copy + mesh assembly on a worker pool (never blocks the frame); LRU RAM cache + versioned disk cache (**on by default**; `HARUKA_DISKCACHE=0` disables it).
- **MESO tiles** *(opt-in: `HARUKA_MESO=1`)* — on-demand ~5 km tiles (128², radius-derived) re-eroded locally with **inherited flow + margin** so tile borders don't seam; built on workers. Off by default as a **cost/benefit decision**, not a stability one — it no longer moves the ground (`giro` cold: 0.000 m) and no longer breaks the 1 cm requirement.
- **Floating Origin** — per-frame world re-centering on the camera.

### ⚙️ Physics & Simulation
- **Jolt Physics v5.6.0** — mature rigid-body engine (git submodule), wrapped behind `HarukaPhysics` via PIMPL so the game never sees Jolt. Compiled only when the `PHYSICS` module is on and the submodule is present; otherwise the in-house solver takes over.
- **GL-free physics library** — `HarukaPhysics` depends only on glm + `IWorldProvider`. That's the prerequisite for running the *same* movement validation on client and authoritative server.
- **Planetary Gravity** — radial gravity, float-safe local frame, `CharacterVirtual` controller, ground mesh built from the reference surface with async refresh.
- **Broad-Phase Octree** + terrain raycast.

### 🎮 Game Systems
- **Weather** (`core/weather_system.*`) — deterministic fronts, cloud cover → precipitation with depth, zenith mask (no rain under cover), rain/snow/sand.
- **Granular ground layer** (`core/ground_layer.*`) — snow / sand / mud accumulation and footprints *above* the reference surface; a sink scalar the character controller reads, not geometry.
- **Construction** (`game/construction/`) — free-form placement validated by the DGS, per-piece graph with propagated failure (Jolt), instanced rendering.
- **3D Audio** — OpenAL listener (camera) + world sound sources + one-shot SFX. Device selection in-settings (SDL3).
- **Propagation / nav** (`core/propagation.*`) — path A→B around walls and through openings + terrain occlusion; `through()` returns a 0..1 throughput (playback **and** logical: AI "hears" the player → stealth), `path()` returns the waypoints. Shared by audio and by curving spells.
- **i18n / Locale** — `assets/lang/<code>.json`, `TR("key")`, base-language fallback.
- **Terrain editing** — deformation field (dig / build / flatten, sphere or oriented-box footprint) with chunk re-streaming.
- **Particles** — GL_POINTS additive radiant system (trails, impacts) + a dedicated precipitation renderer.
- **Settings** — persisted graphics/audio/controls/language; rebindable input actions; in-game panel.
- **Component Architecture** — scripts, materials, meshes attached to entities.

### 🔻 Present in the tree but **not wired**
`GBuffer` / deferred pipeline · `CascadedShadowMap` · `PointShadow` · `IBL` · `LightCuller` ·
`ComputePostProcess` · `VirtualTexturing` · the SSAO **pass** (the class allocates buffers;
Details and file:line in [docs/ESTADO_MOTOR.md](docs/ESTADO_MOTOR.md).

---

## 🔧 Dependencies

### 1. Vendored — committed in the tree (nothing to download)

| Path | Library | Role |
|------|---------|------|
| `third_party/glad`            | GLAD            | OpenGL 4.6 function loader |
| `third_party/json`            | nlohmann/json   | scene & save serialization |
| `third_party/stb`             | stb_image       | image loading |
| `external/dgs`                | DGS client      | optional networking (`HARUKA_NETWORK`) |

### 2. Git submodules — one command

```bash
git submodule update --init --recursive
```

| Path | Library | Role |
|------|---------|------|
| `third_party/imgui`            | Dear ImGui      | editor / debug UI |
| `third_party/nativefiledialog` | nativefiledialog| file dialogs |
| `third_party/JoltPhysics`      | Jolt Physics    | rigid bodies (only built if `PHYSICS` is on) |

> Without the Jolt submodule the build still works: `HarukaPhysics` falls back to the in-house solver.

### 3. System libraries — one install command

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

### 4. SDL3 — download & build manually

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

---

## 🔨 Build

### Quick Start

```bash
git submodule update --init --recursive
cmake -B build -DHARUKA_NETWORK=ON      # or OFF
cmake --build build -j$(nproc)
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
| 4 | `AUDIO` | 3D audio: `audio_manager` → `audio_loader` + `audio_system` (OpenAL) |
| 5 | `PARTICLES` | particle system |
| 6 | `TERRAIN` | planetary terrain (default-ON; off = no planet) |
| 7 | `POSTFX` | bloom / HDR / post chain |
| 8 | `SHADOWS` | shadow maps |
| 9 | `PHYSICS` | rigid-body physics + colliders + Jolt (default-ON) |

```bash
# All modules ON (default)
cmake -B build

# Custom mask (1 char per module, in list order): e.g. fluids+deform off
cmake -B build -DMODULES=0011111111
```

> ⚠️ Configuring with `-DMODULES=...` rewrites `src/core/modules.h` in the source
> tree (via `configure_file`). The game (`Survival`) must be built with the **same**
> mask — its `build.sh` keeps engine + game in sync from one place.

> Weather, ground layer, construction and the DGS rules module do **not** have their own bits yet;
> they ride along inside `TERRAIN` / `PHYSICS` / `NETWORK` or are ungated.

### Shaders (engine vs game)

Engine shaders live in `haruka-cpp/assets/shaders/` and are compiled to `.spv` (+ GLSL copy) at
build time into the runtime `assets/shaders/` dir. **Game-specific shaders live in the game repo**
and the game's build compiles them into the **same** dir, coexisting by name. Each shader is
compiled **once** by its owner. (Runtime path resolution: `AssetPaths::shaders()`, identical in dev
and release.)

### Environment switches

| Variable | Effect |
|---|---|
| `HARUKA_DISKCACHE=0` | disable the versioned chunk disk cache (**on** by default) |
| `HARUKA_MESO=1` | enable the ~5 km eroded meso tiles |
| `HARUKA_GROUNDPIN=1` | pin the ground disc in the cache (see [terrain status](#-terrain-status)) |
| `HARUKA_PROF_LOG=N` | dump the profiler tree to stderr every N frames |
| `HARUKA_FIELD_DBG=1` | print the elevation range of the field uploaded to the GPU |

---

## 📖 Documentation

| Document | Content |
|----------|---------|
| [ESTADO_MOTOR.md](docs/ESTADO_MOTOR.md) | **Per-subsystem audit**: what's wired, what's broken, what's dead code (file:line) |
| [PLAN_TERRENO_V3.md](docs/guides/PLAN_TERRENO_V3.md) | The terrain redesign: tectonics → erosion → climate → biomes → props |
| [PLAN_LOD_V3.md](docs/guides/PLAN_LOD_V3.md) | Screen-space LOD, keep radius, ground pin |
| [PLAN_DGS_ANTICHEAT.md](docs/guides/PLAN_DGS_ANTICHEAT.md) | Authoritative server, per-project rules via `dlopen` |
| [PLAN_CONSTRUCCION.md](docs/guides/PLAN_CONSTRUCCION.md) | Free-form building, validated placement |
| [ANALISIS_SISTEMAS.md](docs/guides/ANALISIS_SISTEMAS.md) | ObjectType, SceneManager, WorldSystem, PlanetarySystem |
| [MODULES.md](docs/MODULES.md) | The module bitmask |
| Doxygen | run `doxygen Doxyfile` → `docs/html/index.html` |

---

## 🏗️ Architecture Highlights

Chunks are **generated asynchronously on the GPU** from the planet's eroded geological field (not
from raw noise), assembled on a worker pool and cached in RAM (LRU eviction, budget auto-sized from
system RAM) with a versioned disk cache.

### Scene JSON Format

```json
{
  "objects": [
    {
      "name": "Earth",
      "type": "Planet",
      "position": [0.0, 0.0, 0.0],
      "hasChunks": true,
      "terrainSettings": {
        "config": {
          "seed": 42,
          "genVersion": 2,
          "gpuTerrain": true,
          "profile": "terran",
          "chunkSize": 96,
          "reliefStrength": 1.0
        }
      },
      "lodSettings":       { "distances": [100.0, 500.0, 2000.0, 10000.0] },
      "streamingSettings": { "mode": "procedural", "cacheSizeMB": 256 }
    }
  ]
}
```

> `genVersion: 2` selects the geology→erosion pipeline. The legacy `config.layers` block (v1 noise
> stacking) is still parsed but **no longer read by any planet** — see
> [ESTADO_MOTOR.md §1.9](docs/ESTADO_MOTOR.md).

---

## 📝 Engine status

**v1.0 — playable.** A real-scale solar system streams and renders end to end: you can orbit, fly
in, land and walk, dig the ground, swim, build, and the planet stays consistent between what you
*see* and what you *collide with*. Below is an honest account.

### ✅ Working (on by default)

| Area | State |
|------|-------|
| Rendering | Forward + stylised pass + one directional shadow + post chain, through the **RHI/PSO** layer |
| Depth | **Reversed-Z + infinite far plane** — no z-fighting between terrain and water at any scale |
| Terrain | GPU compute generation from the **eroded geological field** (tectonics → hydrology → erosion → climate) |
| Ground | **One** reference surface, camera-independent and GL-free, read by physics, props, scripts and tests |
| Streaming | Async LOD recompute on a worker; GPU-derived vertex grids; persistently-mapped readback; worker pool for mesh assembly; disk cache on |
| Water | Ocean (Gerstner) + lakes/rivers + PBF particles; signed-depth clipping so the shoreline is exact per-pixel |
| Weather | Deterministic fronts; precipitation implies cloud cover by construction; zenith mask; granular layer + footprints |
| Physics | Jolt (rigid bodies, `CharacterVirtual`), radial gravity, octree broad-phase, terrain raycast |
| Tests | **19 460 OK · 0 failures** (`haruka_tests`) — CPU↔GPU parity, water parity, LOD invariants, streaming settle, ground stability on turn/climb, orbit closure |

### 🎯 Terrain status

Measured today with `HARUKA_DISKCACHE=0` (cold cache — the case of a player starting a new world):

| Scope | mean | worst | outside 1 cm |
|---|---|---|---|
| Node-by-node (GPU-baked vertex vs reference surface) | 0.0001 m | 0.0003 m | — |
| Inside the keep radius (≤ 400 m) | 0.0001 m | 0.0003 m | **0 / 200** |
| Outside (512 m – 3 km) | 0.731 m | 8.171 m | 132 / 200 |

The outer band is **by design**: past the keep radius the LOD thins out. The 1 cm requirement is
about the ground you actually stand on.

What the number cost, and what protects it: the direction is normalised **once, in double**, on both
sides; the field is sampled in double and **copied** (never re-sampled) into the SSBO; the driver's
double `sqrt` is refined with Newton because it is only ~1e-8, not IEEE; and the height path is
compiled with `-fno-fast-math -ffp-contract=off` because `-ffast-math` would break parity **only in
Release**, with the tests still green.

Open items on the terrain, none of them blocking — full detail in
[ESTADO_MOTOR.md §1](docs/ESTADO_MOTOR.md):

- the reference surface only exists for the **active planet**; other bodies fall back to the
  point-analytic sampler
- a single dig disables the GPU terrain path **planet-wide**
- lake levels are computed **before** erosion runs and never recomputed
- the ground-disc cache pin is **off by default** — it touches residency, and there a mistake looks
  like "there is no terrain", not like "less detail"

### 🚧 Next

1. **Validate in-game** what this cycle closed: weather, spellcasting, construction-from-files,
   world panel. The suite is green; a human has not confirmed them yet.
2. **Materials** — per-material textures (buildings, construction pieces and items share them) and a
   height/slope-blended splatmap. Today a stone tile is a flat-coloured cube.
3. **Procedural organic meshes** by seed (`log`/`branch`/`rock`/`ore`) — today they draw as boxes.
4. **Cleanup pass** — the dead code inventoried in ESTADO_MOTOR.md (~150 lines of dead compute
   shader, the v1 generator, the unwired renderer classes, `main` in the `.so`).
5. **Vulkan backend** — the RHI/PSO seam is done; this is a backend, not a rewrite. It would *not*
   fix a CPU-bound frame.
6. **Parked:** caves and floating islands — postponed until the surface is credible.

---

## 📄 License

**Haruka Source-Available License v1.0** — © 2026 Andoni García Torres. See [LICENSE](LICENSE.md).

Free to view, study, modify, and use for **non-commercial** purposes; **commercial use needs
written permission**. Any improvement built using the engine is **licensed back** to the author
(grant-back). Third-party components (DGS, Dear ImGui, GLAD, GLM, SDL3, OpenAL, Jolt, …) keep their
own licenses.

---

## 🔗 Related Projects

- **Survival** — the game built on HarukaEngine (same module mask)
- **haruka** — ImGui-based editor built on top of HarukaEngine

---

**Last Updated:** 27 July 2026 | **OpenGL 4.6 (RHI: Vulkan-ready)** | **C++17/20**
