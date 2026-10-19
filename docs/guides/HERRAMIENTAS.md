# Herramientas y pruebas {#guia_herramientas}
<!-- doc: pin -->

Medir antes de optimizar y probar antes de dar algo por bueno: perfilador de CPU y GPU, panel de cifras, trazas que se encienden sin recompilar, y dos bancos de tests (CPU y GPU) donde cada prueba de paridad lleva su contraprueba para demostrar que puede fallar.

## Cómo funciona

- **Perfilador de CPU**: `HARUKA_PROFILE("nombre")` al principio de un ámbito acumula su tiempo por frame. Los ámbitos se anidan en árbol (`renderFrameContent → planetary.update → …`). Un árbol por hilo.
- **GPU**: `HARUKA_GPU_SCOPE("nombre")` mide un pase con timestamps (ver @ref guia_rhi).
- **Detalle por ms**: los sistemas caros (terreno, props, hierba, nubes) no usan distancias fijas: un lazo cerrado (`LoadController`) mide sus ms de GPU y ajusta el detalle para quedarse en su presupuesto.
- **Panel de depuración** (`DebugOverlay`): arriba a la derecha, fps y percentiles de frame, memoria, red, coordenadas, CPU/GPU y API gráfica.
- **Errores y arranque**: `ErrorReporter` da informes estructurados por subsistema (`HARUKA_ERROR`…); `StartupReport` resume al arrancar qué sistemas fallaron o se saltaron.
- **Logs**: por nivel; las trazas por frame de investigaciones cerradas están apagadas salvo `HARUKA_DIAG=1`.
- **Tests**: `tests/` refleja `src/Haruka/`; cada carpeta es un grupo y cada fichero registra sus tests (`HARUKA_REGISTER_TESTS`). Los de GPU (`*GPU.cpp`) van a `haruka_tests_rhi`. Una prueba que solo mira "no petó" no distingue lo correcto de lo que no hace nada: por eso cada paridad trae contraprueba.
- **ProcGraph**: grafo de nodos para generación procedural (ruido, clima, texturas, mallas de árbol y roca), usado por el editor y por el horneado de props.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/Tools/Profiler.h` | Perfilador de CPU en árbol |
| `src/Haruka/RHI/RHIGPUScope.h` | Tiempos de GPU por pase |
| `src/Haruka/World/Props/LoadController.h` | Detalle contra ms medidos |
| `src/Haruka/Tools/DebugOverlay.{h,cpp}` | Panel de cifras en pantalla |
| `src/Haruka/Tools/ErrorReporter.{h,cpp}` | Errores estructurados |
| `src/Haruka/Tools/StartupReport.h` | Resumen de arranque |
| `src/Haruka/Core/Logger.{h,cpp}` | Logs y nivel |
| `src/Haruka/Tools/ProcGraph/ProcGraph.h` | Grafo procedural de nodos |
| `tests/Common/TestCommon.h` | Utilidades de tests |
| `tests/RHI/RHITestMain.cpp` | Banco de tests de GPU |
| `cmake/HarukaTests.cmake` | Targets y grupos de CTest |
| `tools/smoke_test.sh` | Arranque headless de humo |

## Opciones

| Opción | Efecto |
|---|---|
| `HARUKA_FRAMELOG=1` | Media, p95 y peor frame cada 3 s |
| `HARUKA_PROF_LOG=N` | Árbol del perfilador cada N frames |
| `HARUKA_PROF_SPIKE_MS=<ms>` | Vuelca el árbol si un frame supera eso |
| `HARUKA_PROFILE_DUMP=<s>` | Árbol medio del frame al log cada s segundos |
| `HARUKA_MEM_LOG=N` | Memoria cada N frames |
| `HARUKA_DIAG=1` | Trazas de diagnóstico por frame |
| `HARUKA_LOG_LEVEL=<nivel>` | Verbosidad del log |
| `HARUKA_SHOT=<png>` + `HARUKA_SHOT_SEC` / `HARUKA_SHOT_AFTER` | Captura automática |
| `HARUKA_CAM_PITCH=<grados>` | Fija la inclinación de cámara (capturas) |
| `-DHARUKA_BUILD_TESTS=ON` | Tests también en Release |
| `-DHARUKA_ALLOW_DEBUG=ON` | Permite compilar en Debug |

## Cómo usarlo

Medir un tramo:

```cpp
#include "Haruka/Tools/Profiler.h"
void MiSistema::update() {
    HARUKA_PROFILE("MiSistema::update");
    // …
}
```

```bash
HARUKA_PROF_LOG=120 HARUKA_FRAMELOG=1 ./juego
```

Correr los tests:

```bash
./build/haruka_tests                         # toda la CPU
./build/haruka_tests --group World/Terrain   # un grupo (carpeta)
./build/haruka_tests_rhi vk                  # GPU con Vulkan (gl | vk | all)
ctest --test-dir build -L cpu -j8            # por carpetas, en paralelo
ctest --test-dir build --rerun-failed --output-on-failure
```

Un test nuevo va en la carpeta de `tests/` que refleja su código, y si compara CPU con GPU lleva su contraprueba.
