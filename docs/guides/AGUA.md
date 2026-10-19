# Agua {#guia_agua}

El agua tiene que verse, flotar y mojar igual en todas partes: el mar con su oleaje, los lagos donde el terreno realmente no drena, los ríos y charcos alrededor del jugador, y las salpicaduras. Lo que se ve es lo que la física toca. Aspecto anime; y los rizos, rollos y remolinos tienen que **salir de una sola física de líquidos**, no de trucos por tipo de ola.

## Cómo funciona

- **Mar**: superficie de olas evaluada en GPU (`lib/ocean_wave.glsl`) y con un **gemelo en CPU** (`OceanWave.h`) con las mismas cifras, para que la física flote sobre la ola que se ve. Un solo reloj de oleaje para todo el motor.
- **Lagos y agua quieta**: `WaterFill` corre un *priority-flood* sobre el bake de altura de todo el planeta al crear el mundo. El mar es la semilla; el agua de un téxel es `nivel − terreno`. Es función pura de la posición: no aparece un lago al caminar y dos clientes ven lo mismo. Sin mar no hay agua (una luna sale seca).
- **Agua dinámica cerca del jugador** (`FluidHost`): tres dominios en un fluido:
  - `ShallowWaterSim` — heightfield de ríos y lagos (modelo de "tuberías virtuales").
  - `PBFSolver` — partículas para salpicaduras y cascadas.
  - `HybridWater` — acopla: río→mar por nivel del mar, cascada→lago por absorción.
- **`FluidBridge`** engancha ese fluido al planeta: lecho (con el fondo de las cuevas), campo de lagos, marea, caudal de lluvia y ciclo evaporación → vapor del clima.
- **Un solo líquido 3D** (`ApicFluid`, APIC sobre rejilla MAC en GPU): gravedad + incompresibilidad + lecho. No sabe qué es una ola; los rollos y remolinos tienen que emerger. El estado vive solo en GPU.
- **Dibujo**: el agua del planeta se pinta en el pase de terreno (`terrain_node_water.*`) con `lib/ocean_shade.glsl`; las partículas con `FluidRenderer`.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/World/Water/OceanWave.h` | Oleaje en CPU (gemelo) |
| `src/Haruka/World/Water/WaterFill.h` | Lagos por priority-flood global |
| `src/Haruka/World/Water/FluidBridge.{h,cpp}` | Fluido dinámico al planeta |
| `src/Haruka/Renderer/FluidHost.{h,cpp}` | Dueño de los simuladores |
| `src/Haruka/Physics/Fluid/ShallowWater.{h,cpp}` | Ríos y lagos locales |
| `src/Haruka/Physics/Fluid/PBFSolver.{h,cpp}` | Partículas de salpicadura |
| `src/Haruka/Physics/Fluid/HybridWater.{h,cpp}` | Acopla los tres dominios |
| `src/Haruka/Physics/Fluid/ApicFluid.{h,cpp}` | Líquido 3D en GPU |
| `src/Haruka/Renderer/FluidRenderer.{h,cpp}` | Dibuja partículas de fluido |
| `assets/shaders/lib/ocean_wave.glsl` | Oleaje en GPU |
| `assets/shaders/lib/ocean_shade.glsl` | Sombreado del agua |
| `assets/shaders/terrain_node_water.frag` | Superficie del agua |
| `assets/shaders/apic_fluid.comp` | Solver APIC |

## Opciones

| Opción | Efecto |
|---|---|
| Ajuste `waterQuality` | Resolución del agua simulada |
| `HARUKA_NOWATER=1` | Sin mar |
| `HARUKA_NO_FLUID=1` | No dibuja el fluido de partículas |
| `HARUKA_OCEAN_FOAM=<f>` | Escala de la espuma |
| `HARUKA_LAKE_SHORE=<texels>` | Anchura de la orilla de lagos |
| `HARUKA_WATER_PIXEL_WAVES=<n>` | Olas por píxel en el shader |
| `HARUKA_WATER_VERTEX_PX`, `HARUKA_WATER_QUAD_MIN` | Densidad de malla del agua |
| `HARUKA_WATER_ALLNODES=1` | Agua en todos los nodos (depuración) |
| `HARUKA_WATER_CYCLE_SPEED=<f>` | Acelera evaporación/lluvia |
| Módulo `FLUIDS` | Compila o no el agua dinámica |

## Cómo usarlo

Consultar el agua desde el juego (flotar, ahogarse, mojarse):

```cpp
double cota = planets.sampleWaterLevel(worldPos);   // mar con oleaje o lago
double prof = planets.sampleWaterDepth(worldPos);   // > 0 si está bajo el agua
```

Si cambias una constante del oleaje, cámbiala en `OceanWave.h` **y** en `lib/ocean_wave.glsl`. Tests:

```bash
./build/haruka_tests --group World/Water
./build/haruka_tests_rhi vk World/Water
```
