# Terreno {#guia_terreno}

Dibujar la superficie del planeta desde la órbita hasta los pies con **una sola malla sin costuras**: un quadtree de nodos sobre una cube-sphere, generados en la GPU desde los campos horneados, con el detalle decidido por píxeles en pantalla. La altura que se dibuja y la que pisa la física tienen que ser la misma.

## Cómo funciona

- **Nodos con direccionamiento entero**: un nodo es (cara, nivel, índice). La posición de cada téxel sale de enteros con una sola división al final, así un nodo grueso cae bit a bit sobre el fino, los vecinos comparten arista exacta (sin grietas) y el resultado es determinista. No hay ancla que se mueva con el jugador: andar no invalida nada.
- **Selector por error en píxeles**: se subdivide hasta que el error de un nodo cabe en `errorPx` en pantalla, con descarte por horizonte y frustum. Ver @ref guia_herramientas para el lazo de ms.
- **Pool con LRU**: `TerrainNodePool` decide qué nodos están residentes, en qué hueco, cuál sale y cuántos se generan por frame (presupuesto por segundo). Es política pura, testeada sin GPU.
- **Generación en GPU**: `terrain_node.comp` llena el heightmap de un nodo desde el bake; `terrain_node.vert/.frag` lo dibujan desde el pool. Entre niveles se cose y se hace *morph* para que no salte.
- **Altura = bake + detalle + deformación**. `TerrainDetail` pone lo que hay entre vértices del bake; `TerrainDeform` suma lo que el jugador excava. Cada función existe en C++ y en GLSL (`lib/terrain_detail.glsl`, `lib/terrain_deform.glsl`) con las mismas cifras.
- **Las cifras de LOD viven una vez** en `TerrainLOD.h`; shaders y tests las usan, nunca las reescriben.
- **Materiales como dato**: un material es una regla sobre humedad, temperatura, pendiente y cota, más cómo se ve (matiz, grano, relieve). Viaja en un UBO; el color base lo pone el mapa de biomas.
- **Estratos**: el suelo es una columna; cavar baja la profundidad y destapa la roca que toca (`TerrainStrata`).
- **El pase de nodos es el terreno** desde 2026-08-24: no hay clipmap detrás.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/World/Terrain/TerrainNode.h` | Direcciona nodos del quadtree |
| `src/Haruka/World/Terrain/TerrainNodePool.h` | Residencia, LRU, presupuesto |
| `src/Haruka/World/Terrain/TerrainNodeGpu.h` | Une pool y compute |
| `src/Haruka/World/Terrain/TerrainNodeRenderer.h` | Pase de terreno completo |
| `src/Haruka/World/Terrain/TerrainLOD.h` | Cifras de LOD únicas |
| `src/Haruka/World/Terrain/TerrainDetail.h` | Detalle fino CPU=GPU |
| `src/Haruka/World/Terrain/TerrainDeform.h` | Excavación del jugador |
| `src/Haruka/World/Terrain/TerrainMaterial.h` | Materiales como reglas |
| `src/Haruka/World/Terrain/TerrainStrata.h` | Estratos bajo el suelo |
| `src/Haruka/World/Terrain/CubeSphere.{h,cpp}` | Proyección cubo↔esfera |
| `src/Haruka/World/Terrain/BaseField.h` | Lectura del bake (gemelo GLSL) |
| `assets/shaders/terrain_node.comp` | Genera heightmap de nodo |
| `assets/shaders/terrain_node.frag` | Sombrea el terreno |
| `assets/shaders/lib/terrain_material.glsl` | Materiales en GLSL |

## Opciones

| Opción | Efecto |
|---|---|
| Ajuste `terrainQuality`, `lodTargetPx`, `adaptiveLOD` | Detalle del terreno |
| `HARUKA_TERRAIN_V5=0` | Apaga el pase (no queda suelo) |
| `HARUKA_TERRAIN_V5_ERRPX=<px>` | Error permitido en píxeles |
| `HARUKA_TERRAIN_V5_VERTPX=<px>` | Píxeles entre vértices dibujados |
| `HARUKA_TERRAIN_V5_POOL=<n>` | Tamaño del pool de nodos |
| `HARUKA_TERRAIN_V5_DEBUG=<n>` | Vista de depuración |
| `HARUKA_TERRAIN_V5_CULL=0` | Sin descarte por frustum/horizonte |
| `HARUKA_TERRAIN_V5_MORPH`, `_STRIDE`, `_RELIEF` | Ajustes finos de morph y densidad |

Material en `surfaceConfig.materials[]`: `name`; rangos `[min, max]` en `humidity`, `tempC`, `slope`, `elevKm`, `depthKm`; `feather`, `elevFeatherKm`, `priority`; aspecto con `albedo`, `normal`, `tint`, `grain`, `detail`, `grass`; `zone` (color en el `zoneMap`), `submerged` y `role` (`cover` o `bedrock`).

## Cómo usarlo

Añadir un material sin tocar el motor, en la escena:

```json
"materials": [
  { "name": "ceniza", "tempC": [5, 60], "humidity": [0, 0.2], "slope": [0, 0.4],
    "albedo": "textures/terrain/ash_albedo.png", "tint": [0.4, 0.38, 0.36] }
]
```

Si cambias una fórmula de altura o LOD, cámbiala en el `.h` **y** en su `.glsl` gemelo, y corre la paridad:

```bash
./build/haruka_tests --group World/Terrain
./build/haruka_tests_rhi vk World/Terrain
```
