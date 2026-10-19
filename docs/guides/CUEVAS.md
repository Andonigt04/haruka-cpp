# Cuevas e islas {#guia_cuevas}

Lo que el heightfield no puede representar —cuevas, islas flotantes y lo que pica el jugador— vive en **un campo volumétrico** uniforme del planeta. Una sola forma de tocar el mundo, sin costuras, la misma para el render y para la colisión.

## Cómo funciona

- **Chunks bajo demanda**: un chunk es un cubo de 128 m con 32³ vóxeles (4 m), indexado por (cara del cubo, i, j) y una capa radial `k`. Dos jugadores que miran el mismo sitio hablan del mismo chunk sin coordinarse.
- **Una cueva es aire que se quita; una isla es roca que se pone.** Las dos son el mismo gesto que el pico del jugador (`VoxWorld::stroke`), puestas de antemano.
- **Las cuevas las definen mapas en disco**, no una fórmula: `planta` (huella vista desde arriba) y `perfil` (qué profundidades tienen hueco) se pueden pintar a mano; `vol` es el volumen resuelto con la conectividad ya garantizada. Todo en `bakes/` con clave hash. La CPU y la GPU leen los mismos téxeles.
- **Sin costuras por construcción**: todos los vóxeles salen de la misma definición global, y cada chunk se mallea (surface nets) con un vóxel de delantal del vecino.
- **Recorte del terreno**: `VoxRenderer` mantiene una ventana que dice al terreno dónde no dibujar suelo (la boca de una cueva).
- **Física con la misma malla**: los chunks remallados se mandan a Jolt; cerca del jugador se usan **ladrillos finos** (celdas de 4 m subdivididas a decímetros) en el job system, para render y colisión a la vez.
- **Ediciones persistentes**: los trazos se guardan por chunk (carpeta `voxEdits` del mundo o de la partida).
- **Pincel del mundo** (`WorldEditPanel`): panel de ImGui en el motor —para editor y juego— con formas de pincel, deshacer y creación de cuevas e islas.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/World/Vox/VoxWorld.{h,cpp}` | Campo volumétrico por chunks |
| `src/Haruka/World/Vox/CaveSystem.{h,cpp}` | Cuevas desde mapas horneados |
| `src/Haruka/World/Vox/IslandSystem.{h,cpp}` | Islas flotantes |
| `src/Haruka/World/Vox/VoxFineBricks.{h,cpp}` | Malla fina cercana |
| `src/Haruka/World/Vox/VoxSystem.{h,cpp}` | Paredes de cueva a Jolt |
| `src/Haruka/Renderer/VoxRenderer.{h,cpp}` | Dibuja paredes, recorte |
| `src/Haruka/World/Terrain/BakeUtil.h` | Retícula y hash compartidos |
| `src/Haruka/Tools/WorldEditPanel.{h,cpp}` | Pincel de cuevas e islas |
| `assets/shaders/lib/vox_cut.glsl` | Recorte del terreno en GPU |

## Opciones

| Opción | Efecto |
|---|---|
| `voxEdits` en `surfaceConfig` | Carpeta de ediciones del mundo |
| `HARUKA_VOX_SAVE_DIR=<dir>` | Carpeta de ediciones si la escena no da una |
| `HARUKA_CAVE_AT=lat,lon` | Pone una cueva al arrancar |
| `HARUKA_ISLAND_AT=lat,lon` | Pone una isla flotante al arrancar |
| `HARUKA_VOX_FINE=0` | Sin ladrillos finos |
| `HARUKA_VOX_DRAW=0\|2` | No dibujar / dibujar sin depth |
| `HARUKA_VOX_NOCULL=1` | Sin descarte de chunks |
| `HARUKA_VOX_DEBUG=<n>` | Vista de depuración del recorte |
| `HARUKA_VOX_DUMP`, `HARUKA_VOX_CUT_PNG` | Vuelca mallas / ventana de recorte |

## Cómo usarlo

Probar una cueva sin editor:

```bash
HARUKA_CAVE_AT=12.5,-3.2 ./juego
```

Picar o rellenar desde el juego:

```cpp
VoxWorld& vox = /* del planeta activo */;
vox.stroke(worldPos, 1.5f, /*remove=*/true);    // esfera de 1,5 m de aire
```

Para editar a mano, abre el pincel del mundo (`WorldEditPanel`) o pinta los PNG `cave_<clave>_planta.png` y `cave_<clave>_perfil.png` en `bakes/`; el volumen se rehornea si falta.

Tests: `./build/haruka_tests --group World/Vox`.
