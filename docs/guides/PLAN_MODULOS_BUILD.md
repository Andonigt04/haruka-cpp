# Módulos del motor en compilación (MODULES=10010...)

> Estado: IMPLEMENTADO (DEFORM/SOFTBODY/FLUIDS/AUDIO). BUILD-TIME — un módulo
> deshabilitado NO se compila ni se incluye en el binario. Bitmask posicional.

## USO RÁPIDO

```
# Construir juego + engine con la MISMA máscara (recomendado):
Survival/build.sh                      # todo ON
Survival/build.sh MODULES=110111111    # SOFTBODY off
Survival/build.sh MODULES=... clean     # limpia antes

# O solo el engine:
cmake -S haruka-cpp -B haruka-cpp/build -DMODULES=110111111
```

### Tabla de bits (posición izq→der)
| pos | módulo | qué incluye | estado |
|-----|--------|-------------|--------|
| 0 | FLUIDS | océano + shallow-water + PBF + hybrid | ✅ build-time |
| 1 | DEFORM | campo de deformación de terreno | ✅ build-time |
| 2 | SOFTBODY | XPBD tela/jelly/viento | ✅ build-time |
| 3 | NETWORK | cliente DGS | usa option(HARUKA_NETWORK) aparte |
| 4 | AUDIO | audio manager | ✅ build-time |
| 5 | PARTICLES | reservado (requiere FLUIDS) | — |
| 6 | TERRAIN | terreno planetario | default-ON (off = sin planeta) |
| 7 | POSTFX | bloom/ssao/hdr/ibl | runtime (getRenderFeature*) — NO build-time |
| 8 | SHADOWS | shadow maps | runtime — NO build-time |

Posición ausente/string corto = ON. `'0'` = OFF.

### Dependencias (CMake auto-activa con warning)
- `PARTICLES_REQUIRES FLUIDS`. Añadir más en CMakeLists `set(<MOD>_REQUIRES ...)`.

### Cómo se sincronizan engine y juego
modules.h se genera en `haruka-cpp/src/core/modules.h` (NO en build/). El juego
incluye ENGINE_ROOT/src directo → ve el MISMO header → misma máscara automática.
build.sh configura ambos con un solo MODULES para que nunca diverjan.

### POSTFX/SHADOWS: por qué runtime, no build-time
Ya tienen toggles runtime (s_enableBloom/SSAO/... + getRenderFeature*) y el
pipeline los respeta. Están entrelazados (~18 usos en application.cpp + main/
settings/compute_postprocess) → build-time = mucho riesgo en el render core por
poco binario ahorrado. Decisión: apagarlos en runtime, no recompilando.

---
## (Diseño original)

## 0. Objetivo y reto

`cmake -DMODULES=10110...` → el binario solo contiene los módulos con bit 1.
Reto: el motor usa `file(GLOB_RECURSE ENGINE_SOURCES "src/*.cpp")` → compila TODO.
Y los módulos se usan cruzados (application.cpp llama a fluidos, físca, etc.).
Excluir un módulo = (a) no compilar sus .cpp + (b) `#ifdef` en cada punto de uso.

## 1. Mapa de bits (orden FIJO, documentar y no reordenar)

Posición (izq→der) → módulo → macro:
```
0: FLUIDS      → HARUKA_MOD_FLUIDS    (ocean Gerstner + shallow-water + PBF + hybrid)
1: DEFORM      → HARUKA_MOD_DEFORM    (DeformationField, edición de terreno)
2: SOFTBODY    → HARUKA_MOD_SOFTBODY  (XPBD: cloth, jelly, wind)
3: NETWORK     → HARUKA_MOD_NETWORK   (DGS client; ya existe HARUKA_NETWORK → alias)
4: AUDIO       → HARUKA_MOD_AUDIO     (audio_manager)
5: PARTICLES   → HARUKA_MOD_PARTICLES (sistema de partículas si se separa de PBF)
6: TERRAIN     → HARUKA_MOD_TERRAIN   (cube-sphere LOD; CORE? ver §4)
7: POSTFX      → HARUKA_MOD_POSTFX    (bloom, ssao, hdr, ibl)
8: SHADOWS     → HARUKA_MOD_SHADOWS   (shadow maps, cascaded)
9..N: reservados
```
Bit ausente o '0' = módulo OFF. Default (sin -DMODULES) = todos ON.
Un .h `core/modules.h` GENERADO por CMake con los `#define` activos → fuente única
de verdad para los `#ifdef`.

## 2. CMake: exclusión de fuentes

1. Parsear `MODULES` (string de 0/1) → por cada bit, set `HARUKA_MOD_<X>=ON/OFF`.
2. Reemplazar el GLOB ciego por **listas por módulo**:
   ```
   set(SRC_FLUIDS src/renderer/water_renderer.cpp src/physics/fluid/*.cpp ...)
   set(SRC_DEFORM src/core/terrain/deformation_field.cpp)
   ...
   set(SRC_CORE   <todo lo demás>)
   ```
   (GLOB por carpeta de módulo donde sea limpio; lista explícita donde el módulo
   está disperso.)
3. `target_sources` solo añade `SRC_<X>` si su bit está ON.
4. `configure_file` genera `core/modules.h` con los `#define HARUKA_MOD_<X>` activos
   + `target_compile_definitions` para los `#ifdef` en headers.

## 3. Guards en el código (los puntos de uso cruzado)

Cada sitio que instancia/llama a un módulo se envuelve. Inventario de puntos
(de la auditoría):
- `application.cpp/h`: `_waterShader`/water pass (FLUIDS), `getDeformationField`/
  `editTerrain`/`rebuildTerrain` (DEFORM), bloom/ssao/hdr/ibl (POSTFX), shadow (SHADOWS).
- `planetary_system.{h,cpp}`: m_waterRenderer (FLUIDS), m_deform (DEFORM).
- `world.cpp` (Survival): m_water/m_pbf/m_hybrid (FLUIDS), m_xpbd/softbody (SOFTBODY).
- Red: ya usa `#ifdef HARUKA_NETWORK` → renombrar a HARUKA_MOD_NETWORK (alias temporal).
Patrón: el código que usa un módulo OFF debe compilar a no-op o ausencia limpia
(punteros null, early-return), NUNCA dejar símbolos colgando.

## 4. Qué es CORE (no modulable)

No todo puede ser módulo sin volverse inarrancable:
- CORE siempre: window, camera, scene, renderer base, shader, input, settings,
  math, profiler, save system, **terreno** (es el mundo; TERRAIN como módulo es
  arriesgado — marcar opcional pero default-ON y avisar que apagarlo = sin planeta).
- Survival depende de algunos módulos: si compilas sin FLUIDS, world.cpp no debe
  referenciar agua. El juego también necesita sus propios guards.

## 5. Riesgos / honestidad

- Es invasivo: toca CMake + decenas de `#ifdef`. Alto riesgo de romper el build en
  combinaciones raras (FLUIDS off pero el juego lo llama). Hay que probar varias
  máscaras, no solo "todo ON".
- Combinaciones inválidas: algunos módulos dependen de otros (¿PBF sin FLUIDS?).
  Definir dependencias y que CMake las fuerce/avise.
- El juego (Survival) y el motor comparten la máscara → el .so y el exe deben
  compilarse con el MISMO MODULES o habrá símbolos faltantes en runtime.
- Mantenimiento: cada sistema nuevo debe declararse en un módulo desde el día 1.

## 6. Fases
1. Infra CMake: parsear MODULES, generar core/modules.h, sin excluir nada aún
   (todos los #define ON) → no rompe nada, prepara el terreno.
2. Un módulo piloto acotado: DEFORM (1 archivo, pocos usos) → exclúyelo, prueba
   MODULES con ese bit a 0, valida que compila y arranca sin él.
3. Extender a FLUIDS, SOFTBODY (los grandes), con sus guards en world.cpp.
4. NETWORK (renombrar macro), AUDIO, POSTFX, SHADOWS.
5. Dependencias entre módulos + validación de máscaras.

Arrancar por la Fase 1+2 (infra + piloto DEFORM) valida el enfoque con bajo riesgo.

## 7. Estado actual relevante
- GLOB_RECURSE en haruka-cpp/CMakeLists.txt:71. Survival GLOB en su CMakeLists.
- HARUKA_NETWORK ya existe como option → modelo a seguir para los demás.
