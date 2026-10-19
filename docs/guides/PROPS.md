# Props y hierba {#guia_props}

Poblar todo el planeta con árboles, rocas, casas, hierba y flores: el mismo árbol siempre en el mismo sitio, colocado por la ecología del campo y no por una fórmula propia, rompible y guardable. El detalle se decide por **tamaño en pantalla y milisegundos de GPU**, nunca por anillos de distancia fijos. La meta de la hierba es cubrir el suelo "cada centímetro".

## Cómo funciona

- **Capas como dato**: un planeta declara sus `propLayers` (casa, camino, árbol, roca, hierba…). Una capa es una regla de clima/forma (humedad, temperatura, pendiente, cota, zonas, expresión `when`) más qué malla instala y a qué escala.
- **El orden es la prioridad**: cada instancia reclama un radio (`claimRadius`); las capas se colocan en orden y las siguientes respetan lo reclamado. La casa se pone, el camino serpentea entre lo puesto y el árbol que caía ahí ya no sale.
- **Scatter global determinista**: cada instancia vive en una **celda del mundo** (coordenadas absolutas); su identidad y su pose salen de un hash de la celda. Sin cámara en el cálculo: cliente y servidor coinciden.
- **Dos sistemas sobre una base** (`PropSystemBase`):
  - `PropSystem` — lo que colisiona o reclama sitio: árboles, rocas, casas, cúmulos lejanos. Instancias con identidad, colliders por parte, rotura (talar, arrancar una rama) y estado guardado.
  - `GrassSystem` — el relleno sin collider (hierba, flores, líquenes): un **brote por celda** que la GPU expande en matas (`prop_burst.vert`); 2 bits de estado por mata (pisada, arrancada).
- **Cubierta densa en GPU**: `GroundScatter` siembra cada frame elementos finos sobre el suelo dibujado (`ground_scatter.comp`); `MassGround` ajusta las matas a la altura real del suelo.
- **Ley de detalle por píxeles** (`PxLod`): mientras un elemento mida ≥ `thrPx` se dibujan todos; más lejos se quedan menos y se ensanchan, así la cobertura en pantalla se conserva. El alcance sale de `thrPx`, no es un parámetro.
- **Lazo cerrado por ms** (`LoadController`): mide el tiempo de GPU del pase y mueve el detalle para quedarse dentro del presupuesto.
- **Lejanía**: cúmulos (un bosque, un pedregal, una aldea ya fundidos en una malla) sustituyen a lo individual de una celda; `ShellScatter` siembra racimos de bultos sobre una cáscara.
- **Mallas procedurales**: árboles, rocas y casas se hornean por semilla con el ProcGraph (`TreeMesh`, `PropMesh`).

## Archivos principales

| Archivo | Qué hace |
|---|---|
| `src/Haruka/World/Props/PropLayer.h` | Capas de props como dato |
| `src/Haruka/World/Props/PropLayerSpawn.h` | Colocación ordenada con claims |
| `src/Haruka/World/Props/PropScatter.h` | Scatter global por celda |
| `src/Haruka/World/Props/PropSystemBase.{h,cpp}` | Base común de sistemas |
| `src/Haruka/World/Props/PropSystem.{h,cpp}` | Árboles, rocas, casas |
| `src/Haruka/World/Props/GrassSystem.{h,cpp}` | Hierba y flores masivas |
| `src/Haruka/World/Props/GroundScatter.{h,cpp}` | Cubierta densa en GPU |
| `src/Haruka/World/Props/MassBurst.h` | Un brote por celda |
| `src/Haruka/World/Props/PxLod.h` | Detalle por tamaño en pantalla |
| `src/Haruka/World/Props/LoadController.h` | Detalle contra ms de GPU |
| `src/Haruka/World/Props/PropCollider.h` | Colliders desde el esqueleto |
| `src/Haruka/World/Props/PropCond.h` | Expresión `when` de capa |
| `src/Haruka/World/Props/ZoneShape.h` | Zonas con perímetro |
| `src/Haruka/World/Props/ShellScatter.{h,cpp}` | Racimos sobre cáscara |
| `src/Haruka/Tools/ProcGraph/TreeMesh.h` | Malla de árbol por semilla |
| `assets/shaders/prop_inst.vert` | Dibuja instancias de props |
| `assets/shaders/prop_burst.vert` | Expande brotes en matas |
| `assets/shaders/ground_scatter.comp` | Siembra la cubierta |

## Opciones

Ajustes: `foliageQuality` (densidad y alcance) y `Grass` (cubierta densa, **apagada por defecto**).

Campos de una capa en `surfaceConfig.propLayers[]`: `name`, `mesh` o `variants[]` (`mesh`, `weight`, `when`), `density`, `perCell`, `seeds`, `scaleMin/scaleMax`, `claimRadius`, `humMin/humMax`, `tempMin/tempMax`, `slopeMin/slopeMax`, `elevMinM/elevMaxM`, `feather`, `zones`, `when`, `clumpM/clumpAmount`, `densityMap`, `submerged`, `maxDistM`. Una capa sin collider ni `claimRadius` la lleva `GrassSystem`.

| Variable | Efecto |
|---|---|
| `HARUKA_NOPROPS=1` | Sin props |
| `HARUKA_PROP_BUDGET_MS=<ms>` | Presupuesto de GPU de props |
| `HARUKA_PROP_LOAD=0` | Sin lazo de ms (detalle fijo) |
| `HARUKA_PROP_DETAIL=<n>`, `HARUKA_PROP_LOD=<n>` | Fuerza detalle / LOD |
| `HARUKA_PROP_BOXES=1` | Props como cajas (depuración) |
| `HARUKA_COVER=0` | Sin cubierta densa |
| `HARUKA_COVER_DENSITY=<por m²>` | Densidad de la cubierta |
| `HARUKA_COVER_BUDGET_MS=<ms>` | Presupuesto de la cubierta |
| `HARUKA_GRASS_NEAR`, `HARUKA_GRASS_MUL` | Alcance y multiplicador de hierba |
| `HARUKA_CLUMP_BUDGET_MS=<ms>` | Presupuesto de racimos lejanos |

## Cómo usarlo

Añadir una capa de árboles a un planeta:

```json
"propLayers": [
  { "name": "pino", "mesh": "tree_pine", "density": 0.6, "claimRadius": 2.5,
    "tempMin": -10, "tempMax": 18, "humMin": 0.35, "slopeMax": 0.5,
    "when": "biome == taiga || zone == bosque" }
]
```

Romper o guardar desde el juego: `PropSystem::breakAt(centro, radio, cámara, …)` y `serializeState()` / restaurar al cargar. Para la hierba, `GrassSystem::destroyAt` (zona) o `nearest` + `destroyElement` (una flor).

Tests: `./build/haruka_tests --group World/Props` y `./build/haruka_tests_rhi vk World/Props`.
