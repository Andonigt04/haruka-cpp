# Arquitectura {#guia_arquitectura}
<!-- doc: pin -->

Haruka es un motor C++26 para planetas a escala real que se recorren sin pantallas de carga: de la órbita al suelo en un mismo mundo. Se entrega como librería compartida (`libHarukaEngine.so`) que un juego o el editor cargan, y dibuja con OpenGL 4.6 o Vulkan.

## Cómo funciona

- **El juego no hereda del motor.** Rellena un `GameInterface` (tabla de callbacks: `onInit`, `onUpdate`, `onRenderWorld`, `onRenderShadow`, `onEvent`, `onShutdown`) y se lo da a `Application`.
- **`Application` es dueño de todo**: ventana, device RHI, escena, cámara, mundo, planetas, física y pases de render. Está partida en cuatro ficheros por responsabilidad: ciclo de vida, frame, assets y red.
- **Dos mundos de precisión.** Las posiciones viven en `double` (kilómetros, `WorldPos`); a la GPU solo sube `float` relativo a la cámara. `WorldSystem::shiftOrigin()` recentra el universo en la cámara cada frame y `toLocal()` es la única conversión permitida.
- **Arranque** (`Application::run(escena)`): resuelve la carpeta de assets desde el ejecutable, elige backend (ajuste o `HARUKA_BACKEND`) antes de crear la ventana, crea ventana y device, carga la escena, llama al `onInit` del juego y entra en el bucle.
- **Un frame** es una lista fija de pases en `Application::renderFrameContent()`: cómputo de terreno → cielo → escena → props → planeta → sombras → terreno y agua → juego → lluvia, fluidos, nubes → post. Ver @ref guia_render.
- **Un solo pool de hilos** (`JobSystem`) con tres colas (frame, streaming, fondo) y una cola de continuaciones que se aplican en el hilo principal. La física es la excepción: Jolt corre en un hilo para que el servidor la reproduzca exacta.
- **CPU y GPU comparten fórmulas.** Todo lo que describe el mundo (altura, olas, nubes, LOD) existe en C++ y en GLSL con las mismas cifras; los tests miden una contra la otra.
- **Módulos de compilación** apagan subsistemas enteros con una máscara de bits (@ref guia_modulos).

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/Core/Application.h` | Dueño del proceso y frame |
| `src/Haruka/Core/Application.cpp` | Arranque, carga, bucle principal |
| `src/Haruka/Core/ApplicationRender.cpp` | Cola y pases del frame |
| `src/Haruka/Core/ApplicationAssets.cpp` | Cachés de mallas y texturas |
| `src/Haruka/Core/GameInterface.h` | Callbacks que implementa el juego |
| `template/main.cpp` | Punto de entrada de ejemplo |
| `src/Haruka/World/WorldSystem.h` | Origen flotante, escala universo |
| `src/Haruka/Core/Jobs/JobSystem.h` | Único pool de hilos |
| `src/Haruka/Renderer/ResourceStore.h` | Datos CPU↔GPU con revisión |
| `src/Haruka/Tools/MathTypes.h` | Tipos `WorldPos`/`LocalPos`, unidades |
| `CMakeLists.txt` | Targets y fuentes del motor |

## Opciones

| Opción | Efecto |
|---|---|
| `HARUKA_BACKEND=vulkan\|opengl` | Fuerza el backend en esta ejecución |
| `HARUKA_JOBS=N` | Número de hilos del pool |
| `HARUKA_INI=<ruta>` | Fichero de ajustes propio (varias instancias) |
| `HARUKA_SIM_TIME=<s>` | Congela el reloj de simulación |
| `-DMODULES=…` (CMake) | Qué subsistemas se compilan |
| `-DHARUKA_BUILD_TESTS=ON` (CMake) | Compila los tests también en Release |

### Targets de CMake

| Target | Tipo | Para qué |
|---|---|---|
| `HarukaEngine` | compartida | El motor |
| `HarukaPhysics` | estática | Física, solo glm + `IWorldProvider` |
| `HarukaConstruction` | estática | Validación de construcción (cliente y servidor) |
| `haruka_simbase` | estática | Base sin GL: el suelo que comparten cliente y servidor |
| `haruka_rules` | compartida | Reglas que carga el servidor DGS |
| `haruka_tests`, `haruka_tests_rhi` | ejecutables | Tests de CPU y de GPU |
| `mapc` | ejecutable | Compila `.scene` → `.hmap` |

## Cómo usarlo

Compilar:

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j6        # -j alto se queda sin memoria
```

Engancharse desde un juego (es lo que hace `template/main.cpp`):

```cpp
#include "Haruka/Core/Application.h"
#include "Haruka/Core/GameInterface.h"

extern "C" Haruka::Core::GameInterface* getGameInterface();   // lo exporta el juego

int main() {
    Application app;
    app.setGameInterface(getGameInterface());   // onInit, onUpdate, onRenderWorld…
    app.run("scenes/main.scene");
}
```

Los includes van siempre con el prefijo de la librería: `#include "Haruka/World/Planet/Planet.h"`. Para empezar a leer un sistema, abre su guía en la barra izquierda.
