# HarukaEngine

C++17 / OpenGL 4.6 real-time engine distributed as a shared library.

**Renderer** — deferred shading, PBR (metallic/roughness), cascaded shadows, SSAO, IBL, HDR, bloom, compute post-processing  
**World** — double-precision coordinates, procedural planetary terrain, async chunk streaming  
**Runtime** — physics (octree broad-phase), OpenAL 3D audio, PostgreSQL persistence, WebSocket networking

---

## Dependencies

```bash
sudo dnf install cmake gcc-c++ assimp-devel openssl-devel openal-soft-devel \
    postgresql-devel gtk3-devel pkgconf-pkg-config glm-devel glslang
```

> **`sdl3-devel` no está disponible como paquete en los repos oficiales a fecha de hoy.**
> Opciones:
> - Vendorizar: clonar SDL en `third_party/SDL` y añadirlo con `add_subdirectory`
> - Compilar manualmente:
>   ```bash
>   git clone https://github.com/libsdl-org/SDL third_party/SDL
>   cmake -B third_party/SDL/build -S third_party/SDL -DCMAKE_BUILD_TYPE=Release
>   cmake --build third_party/SDL/build -j$(nproc)
>   sudo cmake --install third_party/SDL/build
>   ```

| Package | Role |
|---------|------|
| `cmake` | Build system |
| `gcc-c++` | C++17 compiler |
| `sdl3` | Window creation, OpenGL context, input events — **compilar manualmente** (ver arriba) |
| `glm-devel` | Vector/matrix math (vec3, mat4, dvec3 for double-precision world coords) |
| `glslang` | Compiles GLSL shaders to SPIR-V (`.spv`) at build time |
| `assimp-devel` | Loads 3D model formats (OBJ, FBX, GLTF…) into engine meshes |
| `openal-soft-devel` | 3D positional audio via OpenAL |
| `postgresql-devel` | `libpq` C client — persistence layer for player data, chat, anti-cheat |
| `openssl-devel` | TLS for WebSocket connections (`websocketpp` dependency) |
| `gtk3-devel` | Native file dialogs on Linux (`nativefiledialog` backend) |
| `pkgconf-pkg-config` | Lets CMake locate installed libraries via `.pc` files |

**Vendored** (`third_party/`):

| Library | Role |
|---------|------|
| `glad` | Resolves OpenGL 4.6 function pointers at runtime against the driver loaded by SDL3 |
| `stb` | Single-header image loader (`stb_image`) used for textures and HDR environment maps |

---

## Build

```bash
cmake -B build                          # ENGINE_VERSION defaults to 1.0.0
cmake -B build -DENGINE_VERSION=2.0.0   # explicit version
cmake --build build -j$(nproc)
```

Output:

```
build/
└── 1.0.0/
    └── bin/
        ├── libHarukaEngine.so
        └── shaders/*.spv
```

GLSL shaders in `shaders/` are compiled to SPIR-V at build time via `glslc`.

---

## Install into editor

```bash
cmake --install build --prefix ../haruka/build
```

```
haruka/build/
└── 1.0.0/
    ├── bin/
    │   ├── libHarukaEngine.so
    │   └── shaders/*.spv
    └── lib64/haruka-cpp/   ← headers
```

---

## Docs

```bash
cd haruka-cpp && doxygen Doxyfile
# open docs/html/index.html
```

---

## Releases

| Version | Notes |
|---------|-------|
| 1.0.0   | Initial release — deferred pipeline, SPIR-V shaders, planetary streaming |

---

## Testing

No automated test suite yet. Manual validation steps:

- Build succeeds with `-DENGINE_VERSION=x.y.z` and output lands in the correct subdirectory
- Shaders compile without SPIR-V warnings (`glslc` exit 0)
- Doxygen runs to completion with zero content errors (`doxygen Doxyfile 2>&1 | grep -v obsolete`)
- Editor loads `libHarukaEngine.so` from `../haruka/build/1.0.0/bin/` without missing-symbol errors

---

> Requires OpenGL 4.6 (`GL_SHADER_BINARY_FORMAT_SPIR_V` — core since 4.6).
