# Construcción, puertos y prefabricados {#guia_construccion}

Construir libremente (cualquier posición y orientación) pero **validado**, con el mismo código en el cliente —que predice— y en el servidor —que valida, anti-trampas—. Las estructuras son piezas distintas unidas por un grafo de anclaje, con puntos de montaje autorizados en cada asset y conjuntos ya montados (prefabricados) como dato, no como código.

## Cómo funciona

- **Validación determinista** (`ConstructionState::validate`): una pieza no solapa a otra, apoya en un soporte del tipo que admite (suelo, pieza…), no está en terreno demasiado inclinado si lo exige, y queda **anclada** (toca el suelo o una pieza). Sin GL, sin aleatorio, sin hilos: cliente y servidor dan lo mismo.
- **Grafo de anclaje**: al colocar, cada pieza se fija a las que roza; las estructuras son componentes conexas. Lo que pierde el camino al suelo deja de estar soportado.
- **Prefabricados**: una lista de piezas con su pose (`assets/data/prefabs/<nombre>.json`, o un `.glb` de Blender del que solo se leen las poses de los nodos). Un vehículo es una estructura **no anclada**: lo que lo distingue de una casa se decide al colocarlo. Las piezas siguen siendo objetos distintos (cada una con su collider), por eso una puerta gira y un impacto arranca un trozo.
- **Lotes**: un prefabricado se valida y coloca de una vez (`validateBatch`/`addBatch`).
- **Ediciones**: los cambios de un conjunto concreto (pieza quitada, movida o sustituida) se aplican sobre la copia del montaje con una sola regla (`applyPrefabEdits`).
- **Puertos autorizados**: los puntos de acople no se adivinan de la caja envolvente; los declara el asset. `Mount` (se atornilla algo), `Interact` (panel o interacción) y `Rail` (mecanismo con un grado de libertad). Una pieza de talla N pide un puerto ≥ N.
- **Motor y proyecto separados**: el motor solo entiende clase, talla y geometría del puerto. El vocabulario (`kind`: "bulkhead", "hull_bottom"…) y las reglas del juego (estanco, alimentado) son del proyecto.
- **Recortes de malla** (`MeshCut`): biselar el extremo de una pieza o hacer ingletes cortando por un plano.

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/Game/Construction/Construction.{h,cpp}` | Validación y grafo de anclaje |
| `src/Haruka/Game/Construction/MeshCut.h` | Corta mallas por planos |
| `src/Haruka/Game/Prefab/Prefab.{h,cpp}` | Prefabricados JSON/glb |
| `src/Haruka/Game/Ports/Port.{h,cpp}` | Puertos de montaje y raíles |
| `src/Haruka/Tools/PrefabEditPanel.{h,cpp}` | Editor de prefabricados |
| `src/Haruka/Tools/PreviewRender.{h,cpp}` | Miniatura 3D de piezas |
| `src/Haruka/Renderer/ScenePass.{h,cpp}` | Dibuja piezas instanciadas |
| `src/Haruka/Game/Inventory/Inventory.{h,cpp}` | Inventario genérico (items) |

## Opciones

Pieza de prefabricado (`pieces[]`): `item`, `pos` [x,y,z] en m, `rot` [w,x,y,z] (opcional), `joint` (`rigid` o `mechanism`), `name` (para que una junta la nombre).

En un `.glb`: propiedades personalizadas `item`, `joint`, `arc`; un vacío con `anchor` es punto de anclaje; un vacío "junta…" es una junta (`a`, `b`, `dof`, `limites`, `motor`, `muelle`, `amortiguacion`, `rotura`). Exportar con *Include → Custom Properties*. Piezas escaladas o espejadas se descartan con aviso.

Puerto (JSON del asset): `id`, `cls` (`mount`/`interact`/`rail`/`socket`), `kind`, `size`, `position`, `rotation`, `radius`, `rail`, `pairsWith`.

## Cómo usarlo

Validar y colocar una pieza (igual en cliente y servidor):

```cpp
using namespace Haruka::Game::Construction;
ConstructionState cs;
cs.setCatalog(catalogo);
auto r = cs.validate(typeId, pos, orient, worldProvider);
if (r == Placement::Ok) cs.add(typeId, pos, orient, worldProvider);
```

Cargar un prefabricado:

```cpp
namespace Prefab = Haruka::Game::Prefab;
Prefab::Prefab pf;
std::string file = Prefab::findPrefabFile("assets/data/prefabs", "crawler");   // .json > .glb > .gltf
if (Prefab::loadPrefab(file, pf)) { /* pf.pieces → lote para validateBatch */ }
```

Tests: `./build/haruka_tests --group Game`.
