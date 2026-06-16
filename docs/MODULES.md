# Build-time Modules

HarukaEngine is split into **compile-time modules**. Each can be turned ON/OFF so you ship
only the features you need (smaller binary, no dead code). The whole system is driven by a
single **bitmask** and a single generated header, `src/core/modules.h`.

> Source of truth: [`CMakeLists.txt`](../CMakeLists.txt) (`HARUKA_MODULE_LIST`) and
> [`cmake/modules.h.in`](../cmake/modules.h.in).

---

## The module list (FIXED order)

```cmake
set(HARUKA_MODULE_LIST
    FLUIDS      # 0: ocean + shallow-water + PBF particles (hybrid water)
    DEFORM      # 1: terrain deformation field (dig / build / flatten)
    SOFTBODY    # 2: XPBD cloth / jelly / wind
    NETWORK     # 3: DGS network client
    AUDIO       # 4: 3D audio (manager + listener + world sources + propagation)
    PARTICLES   # 5: GL_POINTS particle system
    TERRAIN     # 6: planetary terrain (default-ON; off = no planet)
    POSTFX      # 7: bloom / SSAO / HDR / IBL
    SHADOWS     # 8: shadow maps
    PHYSICS     # 9: rigid-body physics + colliders (default-ON)
)
```

**The order is the bit position**: `FLUIDS` = bit 0 … `PHYSICS` = bit 9.

---

## The bitmask: `-DMODULES=`

One character per module, in list order. `'1'` = ON, `'0'` = OFF. **Missing / short → the
absent positions default to ON.**

```bash
# Everything ON (default)
cmake -B build -DVERSION=1.0

# FLUIDS + DEFORM OFF, the rest ON (1 char per module)
cmake -B build -DVERSION=1.0 -DMODULES=0011111111
```

For the game, drive both engine and game from one place:

```bash
./[PROJECT_FOLDER]/build.sh MODULES=0011111111
```

---

## How it works (under the hood)

1. **CMake reads the mask** ([`CMakeLists.txt`](../CMakeLists.txt), `foreach` over
   `HARUKA_MODULE_LIST`). For every ON module it appends `#define HARUKA_MOD_<NAME> 1`.

2. **`modules.h` is generated** via `configure_file(cmake/modules.h.in → src/core/modules.h)`.
   It is written **into the source tree** on purpose, so the game (which includes `ENGINE/src`)
   sees the exact same switches. It's the single header every `#ifdef` keys off.

3. **An OFF module disappears two ways:**
   - **Source exclusion** — CMake drops the module's `.cpp` files from the build with
     `list(FILTER ENGINE_SOURCES EXCLUDE REGEX "...")`. Same for its shaders
     (`SHADER_SOURCES` filter). The disabled code is **never compiled or linked**.
   - **`#ifdef HARUKA_MOD_<NAME>` guards** in the *shared* code that references the module
     (headers, member pointers, call sites), so the rest of the engine still compiles. The
     guarded code provides a no-op / stub path when the module is off.

   > Rule of thumb: **inside** a module you don't need `#ifdef` (its sources aren't compiled
   > when off). You only guard the places where the **rest of the engine** touches it.

4. **Dependencies** — `set(<MOD>_REQUIRES OTHER ...)` declares that a module needs another. If
   it's ON but a requirement is OFF, CMake **auto-enables the requirement** with a warning so
   the build stays valid. (Currently none are declared — all modules are self-contained.)

5. **Engine and game must use the same mask.** Because `modules.h` lives in the source tree
   and the game includes it, both must be configured with the same `-DMODULES`. Reconfiguring
   with a different `-DMODULES` **rewrites** `src/core/modules.h`.

---

## How to add a new module

Example: a `WEATHER` module (rain / snow).

### 1. Add it to the END of the list

[`CMakeLists.txt`](../CMakeLists.txt), `HARUKA_MODULE_LIST`:

```cmake
set(HARUKA_MODULE_LIST
    FLUIDS DEFORM SOFTBODY NETWORK AUDIO PARTICLES TERRAIN POSTFX SHADOWS PHYSICS
    WEATHER)   # bit 10
```

> ⚠️ **Always append at the end.** The bitmask is positional — inserting in the middle shifts
> every existing bit, silently breaking every saved/scripted `MODULES=` string.

### 2. Put your sources under an identifiable path and exclude them when OFF

Place the module's code under e.g. `src/weather/*.cpp` and `src/renderer/weather_renderer.cpp`.
Then add an exclusion block next to the others in [`CMakeLists.txt`](../CMakeLists.txt):

```cmake
if(NOT HARUKA_MOD_WEATHER)
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "weather/.*\\.cpp$")
    list(FILTER ENGINE_SOURCES EXCLUDE REGEX "renderer/weather_renderer\\.cpp$")
endif()
```

If it ships shaders, add the same kind of block in the shader section:

```cmake
if(NOT HARUKA_MOD_WEATHER)
    list(FILTER SHADER_SOURCES EXCLUDE REGEX "/weather\\.(vert|frag)$")
endif()
```

### 3. Guard the shared code that references it

Wrap the member, the creation, the per-frame calls and any getter with the macro, and give a
no-op path when off:

```cpp
#include "core/modules.h"
...
#ifdef HARUKA_MOD_WEATHER
    std::unique_ptr<WeatherSystem> _weather;
#endif
...
#ifdef HARUKA_MOD_WEATHER
    if (_weather) _weather->update(dt);
#endif
```

### 4. (Optional) Declare dependencies

```cmake
set(WEATHER_REQUIRES PARTICLES)   # WEATHER needs PARTICLES → auto-enabled if missing
```

### 5. Build engine + game with the same mask

With no `-DMODULES`, `WEATHER` is **ON by default** (absent position = ON). To test it OFF,
pass the full mask with the last char `0`:

```bash
./[PROJECT_FOLDER]/build.sh MODULES=11111111110   # 11 chars, WEATHER off
```

### 6. Document the new bit

Update the module table in [`README.md`](../README.md) and this file.

---

## Gotchas

- **Positional bitmask** → new modules go at the **end**, never the middle.
- A missing position in the mask is **ON**, not off.
- Reconfiguring with `-DMODULES` **rewrites `src/core/modules.h`** in the source tree, so the
  engine and the game must be reconfigured together (use `[PROJECT_FOLDER]/build.sh`).
- Inside a module: no `#ifdef` needed. Only the engine code that *consumes* it gets guarded.
