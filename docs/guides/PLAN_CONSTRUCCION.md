# PLAN — Sistema de construcción (piezas free-form, rompible, validado por el DGS) + mundo por imperios

> Estado: DISEÑO v1. Decisiones de diseño del usuario CERRADAS (§0). El core (F1-F3) vive en la **librería
> de reglas** (GL-free, determinista) para que el cliente prediga y el DGS valide con el MISMO código —
> misma filosofía que la física ([[PLAN_DGS_ANTICHEAT.md]]). Contenido (imperios/ciudades/modelos .glb) =
> secciones de diseño ABIERTAS (§7). Cliente/juego en `Survival/scripts/`, engine en `haruka-cpp/`.

## ESTADO ACTUAL (2026-07-22) — apunta todo

**Core GL-free (lib `HarukaConstruction`, `src/game/construction/construction.{h,cpp}`; suite 131 OK · 0 fallos):**
- **F1** colocación: `validate(typeId,pos,orient,IWorldProvider) → Placement {Ok,UnknownType,OverlapsPiece,
  NotAnchored,WrongSupport,TooSteep}`. OBB-OBB SAT en double. NO rechaza por penetrar terreno (el relieve se
  moldea con la herramienta de terreno). `add(...,world)`.
- **F2 grafo de anclaje**: fijaciones = aristas (clavos/tornillos), `fasteners`/`grounded`/`isSupported` (BFS al
  suelo)/`structureComponent`/`relabelStructures`.
- **F2b reglas "sobre qué"**: `PieceType.supports` (terreno/pieza, bitmask) + `needsFlat` → `WrongSupport`/`TooSteep`.
- **F3 rotura**: `breakPiece(id)` → colapso en cascada de lo no soportado; `setProtected` (zona irrompible + ancla).
- **Prefabs (lote)**: `validateBatch`/`addBatch(vector<Placed>)` — colocación atómica con fijaciones internas.
- **Fixtures (regla)**: `Opening`/`FixtureKind` + `wallOpenings(...)` (dónde van puerta y ventanas en un muro) +
  `fixtureFits(op, half)`.
- **Layout de edificio**: `WallRun`/`BuildingLayout` + `buildingLayout(...)` — la estructura en datos, sin render.

**Cliente (`Survival/scripts/build_system.{h,cpp}` + `init.cpp`/`init_console.cpp`):**
- Modo construcción con **preview** (fantasma verde/rojo) + **catálogo DATA-DRIVEN desde items** (tag `construccion`
  + modelo). AABB real del `.glb`. **Sockets/SNAP** derivados del AABB. **Herramienta de terreno** (nivelar/subir/
  bajar) reusando `DeformationField`. **Prefab suelo 3×3**. **Modo camino** (puntos de referencia → hilera).
- **Controles**: V modo · F pieza/terreno/camino · C cambiar/op · B colocar/aplicar/marcar · Z suelo · X romper.

**Contenido / datos:**
- Items con esquema **tallas/variantes/bulk/proc** (`ItemDef.sizeMax/variants/bulk/proc`; loader parsea). Lista de
  prioridad poblada (tablon×16, tronco=proc log, piedra, ladrillo, metal, cuerda, tornillo/tuerca/clavo, baldosa,
  puerta×10, marco×10, ventana×6, mesa×30, etc.). **Fixtures** tag `fixture`: puerta/marco/ventana.
- **Edificios** (`building_gen`): materiales + estilo por IMPERIO + ROL del edificio → **LAYOUT en CPU y luego
  TEJIDO con las piezas del material** (ver arquitectura abajo). Tejado aún procedural con primitivas.
  CLI `building [imperio] [seed] [rol]` (`building styles` / `building roles`).
- **Ciudades** (`city_gen`): imperio + PLANO de asentamiento → parcelas medidas por huella + una calle por hueco
  (`roads`), con avenidas cada N. CLI `city [imperio] [seed] [grid] [plano]` (`city plans`).
- **TODO ESO SALE DE ARCHIVOS (2026-07-26)** — ver §"La librería en archivos" abajo.
- **Carreteras** (`roads`): aplanan el terreno, 4 tipos, API reutilizable. CLI `road`.
- **Instancing** de piezas de construcción: pase `construction_inst.vert/frag` (1 draw por modelo). ⚠️ sin verificar.

**MODELOS (2026-07-20, usuario)**: existen `SimpleDoor.glb`/`Window.glb`/`WindowGlass.glb`. El caparazón (muros/
tejado) es PROCEDURAL del material por tamaño; los MODELOS son las FIXTURES (puerta/ventana/marco) que van en los
huecos. **De momento se verifica por TESTS** (no hay render headless).

### La LIBRERÍA EN ARCHIVOS (2026-07-26, petición del autor) — markers fuera del .cpp

> *"los edificios e imperios/ciudades tienen que salir de ARCHIVOS, hoy son código"*

Antes, `building_gen.cpp` llevaba `kMaterials[]` y los cuatro `BuildingStyle` escritos a mano y `city_gen.cpp`
la rejilla de parcelas: añadir un imperio o cambiar sus materiales exigía **recompilar**, y eso no lo puede tocar
quien diseña. Ahora son datos (`Survival/scripts/data/building_data.{h,cpp}` → `BuildingLibrary`, cargada en
`registerItems()` como el catálogo de items):

| Archivo | Qué manda | Contenido |
|---|---|---|
| `assets/data/buildings/materials.json` | **de qué está hecho** | color + la **PIEZA** con la que se teje (tabla/sillar/hilada) + `use` (muro/tejado) |
| `assets/data/buildings/roles/*.json` | **el PROGRAMA** | huella, plantas, ritmo de huecos, `floorScale`, puerta, qué **fixtures** admite |
| `assets/data/empires/*.json` | **el ESTILO** | materiales de muro/tejado (por id), formas de tejado, proporciones, tallas normales, `footScale` |
| `assets/data/cities/*.json` | **el TRAZADO** | rejilla, mezcla de roles con **peso**, clase de calle + anchura, avenidas cada N, densidad |

Reglas de composición: cuando **rol** e **imperio** hablan de lo mismo (huella, plantas, ritmo) manda el **rol**
si lo declara —un templo es grande en cualquier imperio— y el imperio lo ajusta con su `footScale`; lo que el rol
calla lo pone el imperio. Sin la fixture `window` el muro se queda **macizo de verdad** (el layout no abre hueco).

Lo que **NO** cambió: el caparazón sigue siendo **procedural** (aparejado con la pieza del material) y el LAYOUT
sigue en el core GL-free y testeado. A archivos van los **markers de control** — qué está permitido y en qué
proporción —, no la generación. La carga **valida y lo canta** por log (material inexistente, tejado con material
de muro, plano con un rol que no existe, imperio incompleto → descartado).

Tests: `Survival/tests/test_buildings.cpp` (`ctest -R survival_buildings`, 159 OK) — no comprueba que el JSON se
parsee, sino que **manda**: Bram es de ladrillo con aguja porque lo dice su archivo, un templo es >1.5× la casa
del mismo imperio, un almacén sin `window` no abre ni un hueco, y la generación sigue siendo determinista.
Por eso el spawn en escena se movió a `building_spawn.cpp`: así `building_gen.cpp` es GL-free y se puede probar
sin motor.

### PANEL DE MUNDO (debug, 2026-07-26) — ver la estructura, no los objetos

`Survival/scripts/ui/world_debug.{h,cpp}`, arriba a la derecha; **F3** o consola `world`. Árbol
**SERVIDOR → ZONA → grupos**. Regla: lo que está **unido** no se lista pieza a pieza — sale el **PADRE con
(N)**: un edificio `city7_1_0 (487)` y dentro sus partes (`muro 3 (96)`, `losa 0 (12)`, `tejado (30)`); las
piezas colocadas por el jugador se agrupan por `Piece::structureId` (componentes conexas del grafo de
fijaciones). Lo suelto se lista tal cual.

⚠️ El cliente **no puede enumerar las zonas del DGS** (`ZoneHandle` es opaco y vive en el módulo de reglas):
el panel muestra el nodo conectado (`Network::serverLabel()`) o "local (sin DGS)", y dibuja la partición en
zonas calculada **en el cliente** (250 m / 1 km / 4 km) con el mismo criterio de región que usa
`serializeRegion`. El panel lo dice. Se reconstruye a **2 Hz**, no por frame.

De montarlo salió un bug real: el **vidrio de las ventanas** nacía sin `segId` y la pasada final lo adoptaba
como TEJADO → romper el tejado borraba las ventanas del edificio entero y cada cristal registraba una caja de
colisión en un vano que debe poder cruzarse. Arreglado + `BuildingPart::kind` (muro/losa/tejado, prop `bkind`).

### ARQUITECTURA de edificios (2026-07-22, DECISIÓN del usuario) — LAYOUT → PIEZAS

> *"creas un layout sin render cpu y cantidad de pisos tamaño etc y después según pones las tablas de maderas"*

Un edificio **no** es un `.glb` escalado ni un montón de cajas. Se construye en **dos pasos**:

1. **LAYOUT** — `Haruka::Construction::buildingLayout(floors, W, D, wallT, floorH, door…, win…)` →
   `BuildingLayout { floors, footprint, floorHeight, vector<WallRun> }`, con
   `WallRun { a, b (extremos en planta), baseY, height, thickness, isFrontGround, openings }`.
   Es **la estructura en DATOS: en CPU, SIN render**, determinista y **cubierta por tests** (core GL-free).
   Los huecos salen de `wallOpenings` (puerta solo en el frontal de planta baja; ventanas en el ritmo).
2. **PIEZAS** — `building_gen` **teje** cada `WallRun` con la unidad del material (`Piece {len, height, bond}`:
   tabla de madera 2.0×0.30, sillar 1.2×0.55, hilada de ladrillo 1.0×0.20, paño de yeso 1.5×0.80…):
   hiladas desde la base, **traba** a media pieza en alternas, recorte **a ras** de los extremos y del techo, y
   **partido en los huecos** (jambas limpias sin necesidad de vanos en la geometría). Vidrio en las ventanas.
   Color con veteado por pieza. **De aquí sale la credibilidad**: el muro está *aparejado*, no escalado.

Coste: casa de madera ~300 piezas; bloque de piedra de 4 plantas ~1200. Una ciudad de 20 → decenas de miles de
`SceneObject`. **El pase de instancing es lo que lo sostiene** (y lo que permitiría bajar el tamaño de pieza
hasta ver el ladrillo uno a uno).

**INSTANCING de PRIMITIVAS (2026-07-22)**: el pase instanciado solo cogía `RenderKind::Model` con `.glb` y prop
`construction` — las piezas tejidas son PRIMITIVAS (`cube`) con prop `building`, o sea que cada tabla era un
draw. Ahora `application_render.cpp` acumula también las primitivas con prop `construction`/`building` en
`primInstances` (clave = tipo de primitiva) y las dibuja con el mismo PSO instanciado (bind VBO/EBO de la
primitiva + `GPUInstancing::render`) → **1 draw por primitiva**. Además el buffer de instancias es FIJO (20 000,
`addInstance` DESCARTA en silencio al pasarse), así que ambos caminos dibujan **por lotes** del tamaño del
buffer: una ciudad ya no pierde piezas.

**VERIFICADO EN JUEGO (2026-07-22) — se dibuja, con 3 fallos corregidos:**
1. **`GL_INVALID_OPERATION` en `glDrawElementsInstanced`**: `getPrimitiveMesh`/`getOrLoadModelCached` crean el
   recurso PEREZOSAMENTE y al hacerlo dejan el VAO a **0** → el draw salía con el VAO del pipeline
   desbindeado. Es la MISMA trampa que ya documenta el pase de escena. Arreglado re-bindeando el PSO
   *después* de resolver la malla. **Probable causa también de las piezas BLANCAS**: con el VAO suelto, los
   atributos de instancia (color/matriz) se leen de donde sea → color y normales basura.
2. **Cúpulas "platillo volante"**: las bases de las primitivas NO son iguales — el cubo es UNIDAD
   (`scale` = tamaño completo) pero la esfera es de **RADIO 1** (`scale` = radio). `spawnBuilding` escalaba
   ambas por `semiejes×2` → las esferas salían del DOBLE. (El aviso ya estaba anotado y era cierto.)
3. **Z-fighting en las losas**: la losa de planta llegaba a ras de huella y los muros se apoyan SOBRE esa
   línea (medio espesor a cada lado) → se interpenetraban. Ahora la losa va retranqueada `wT/2`.
   Además las hiladas se reparten UNIFORMES en la altura del muro: la última ya no puede quedar de grosor
   casi nulo (escala ~0 → matriz singular → `inverse()` NaN en el shader → pieza blanca).

⚠️ **PENDIENTE de mirar en la próxima pasada**: sin COLISIÓN (las piezas son `SceneObject` sin collider; va
con Jolt), tejados aún con primitivas (no tejidos), y confirmar que el blanqueo se fue con el fix del VAO.

### CIUDADES: las calles van por el HUECO entre edificios (2026-07-22, usuario)

> *"las carreteras deberían de ser en una ciudad según el gap de edificio a edificio"*

`city_gen` trazaba una rejilla de paso FIJO (parcela 16 m) y dos calles en cruz. Con huellas de hasta 14 m
(Karsk) la calzada urbana (6 m) **pisaba los edificios y les aplanaba el terreno por debajo**. Ahora la ciudad
se traza desde las huellas REALES: (1) se generan los edificios y se miden, (2) cada columna/fila se separa lo
que pide el más grande de la suya **más la banda de calle** (`calzada 6 + 2×acera 1.5`), (3) hay **una calle por
hueco**, con su eje EXACTAMENTE en el centro de la banda → no pisa ninguna huella. Los waypoints se siembran
cada ~12 m para que la calzada siga el relieve en vez de aplanar un plano.

**PENDIENTE (por orden):**
1. **Fixtures (render)**: colocar SimpleDoor/Window/WindowGlass en el transform de cada `Opening` del layout
   (`BuildingPart.model` + `spawnBuilding` con `modelPath` YA existen; hoy la ventana es un cristal-caja y la
   puerta queda como vano). Lo verifica el usuario en juego.
2. **F1c**: exponer `validatePlacement`/`validateBatch` como VERBO del módulo DGS (ABI).
3. **F3 por FUERZA** (Jolt: carga > `breakThreshold` → romper constraints). Hoy el disparador es manual (X).
4. **Carreteras**: superficie/visual + **roller** (tierra) + que el generador las use mejor; spline suave.
5. **Camino**: yaw (girar según el tramo), spline (curvas), escaleras.
6. **Instancing**: verificar en juego; **mesh-cut** (recorte real de geometría) aparcado en `mesh_cut.h`.
7. **Items naturales proc** (log/branch/rock/ore): malla procedural por semilla + icono seed 1 (aparcado, ver memoria).

## 0. Decisiones CERRADAS (usuario, 2026-07-18)

1. **Colocación FREE-FORM** — pieza en cualquier posición/orientación con snap a superficie (NO rejilla
   estilo Minecraft). Da las "formas dinámicas" que el usuario quiere.
2. **La lógica vive en la LIBRERÍA DE REGLAS** — colocar/romper son **verbos que el DGS valida**. Free-form
   NO significa cliente-autoritativo: el servidor comprueba cada colocación con el mismo código.
3. **Rotura = GRAFO POR PIEZA con fallo PROPAGADO** (NO islas rígidas) — cada ladrillo es un cuerpo con
   constraints rompibles a sus vecinos; romper las anclas de abajo transfiere carga → cascada → colapso.
4. **Todo rompible SALVO zonas designadas** — zona protegida = ancla con umbral ∞ (irrompible).
5. **Piezas = modelos 3D `.glb`** (aún NO existen) descompuestos; el descompositor extrae la geometría.
6. **Mundo**: tierra dividida por **imperios** → **ciudades** cada X distancia, **pueblos** cada Y, con
   **arquitectura por imperio** y **edificios de forma dinámica**.

⚠️ **Dependencia estética**: el usuario quiere cambiar la **estética de terreno/texturas** antes de dar por
buena la construcción (los edificios se ven sobre el terreno). No bloquea F1-F3 (física/lógica), pero SÍ el
pulido visual y el mundo (F5). Ver §8.

## 1. Principio: el core es DETERMINISTA y vive en la lib de reglas

Igual que la física, la construcción NO puede ser solo cliente: en multijugador el servidor es autoritativo.
Así que el **estado + placement + rotura** son GL-free, deterministas y viven en el módulo del proyecto que
el DGS carga con `dlopen` y que el cliente linka para predecir.

```
CLIENTE (HarukaEngine, GL)                         SERVIDOR (DGS, genérico)
  render de piezas (instancing)                      dlopen(reglas.so)
  preview + snap de colocación                       valida cada verbo
  predicción local ──────────────┐           ┌────── autoritativo, MISMO código
                                 ▼           ▼
        LIBRERÍA DE REGLAS (GL-free, determinista, timestep fijo)
        ├─ ConstructionState: piezas + grafo de anclajes (serializable → replicable)
        ├─ validatePlacement(pieceType, transform, world) → ok | motivo de rechazo
        ├─ validateBreak / applyImpulse(punto, fuerza) → propaga fallo por el grafo
        └─ simula por-pieza vía HarukaPhysics/Jolt (bodies + constraints rompibles)
```

Añade **verbos nuevos al ABI del módulo** (`dgs_game_module`): `validatePlacement`, `validateBreak`. Es la
evolución natural de `validateMove` → `validateAction` → construcción. El DGS sigue sin conocer "ladrillos":
solo llama al verbo y el módulo valida (ver [[dgs-generico-opaco]], blobs opacos).

## 2. Modelo de datos (mínimo, en la lib de reglas)

```cpp
struct PieceType {                 // catálogo, cargado del .glb vía el descompositor (§5)
    uint16_t id;
    ConvexHull hull;               // colisión (del descompositor; NO la malla de render)
    std::vector<AnchorPoint> anchors;  // puntos/áreas por donde se conecta a otras piezas
    float mass, breakThreshold, buildCost;
    // (render: el cliente mapea id → malla .glb; el servidor NO necesita la malla)
};
struct Piece {
    uint64_t id; uint16_t typeId;
    glm::dvec3 position; glm::dquat orientation;   // free-form, double (escala planetaria)
    uint32_t structureId;                          // a qué estructura pertenece
    enum { Anchored, Free, Broken } state;
};
struct Anchor { uint64_t a, b; float threshold; }; // arista del grafo (∞ = zona protegida)
struct ConstructionState {                         // AUTORITATIVO, serializable
    std::unordered_map<uint64_t, Piece> pieces;
    std::vector<Anchor> anchors;                   // grafo
    // índice espacial (para placement/vecindad): rejilla hash o BVH
};
```

El estado se **serializa** (persistencia MongoDB del DGS como blob opaco) y se **replica** por deltas.

## 3. Física por-pieza + rotura propagada (Jolt) — y cómo hacerla viable

- Cada `Piece` = cuerpo **Jolt Dynamic** con RESISTENCIA (ver [[jolt-fisica]]: NUNCA `Static`; estático =
  masa alta + ancla). Cada `Anchor` = **constraint rompible** de Jolt entre dos piezas (o pieza↔terreno).
- **Fallo propagado**: al superar el umbral de un constraint, Jolt lo rompe; la carga se redistribuye →
  otros constraints superan su umbral → cascada. El colapso EMERGE de la simulación, no se scriptea.
- **Zona protegida** = constraint con umbral ∞ (Jolt no lo rompe nunca).

⚠️ **Coste** (el usuario eligió lo caro a sabiendas). Estrategia para que miles de piezas no maten el frame:
1. **Sleeping de Jolt**: una estructura estable DUERME → coste ≈ 0 en reposo.
2. **Fusionar-en-reposo / fragmentar-al-impacto**: mientras duerme, la estructura se trata como **UN cuerpo**
   (o un shape compuesto); al recibir un impacto que supere el umbral, se **re-expande a piezas** y se simula
   el colapso por-pieza SOLO ahí y SOLO esas piezas. Da el colapso realista *cuando importa* sin pagarlo el
   99% del tiempo. NO cambia el modelo (sigue siendo grafo por pieza), solo su representación en reposo.
3. **Presupuesto**: nº de piezas activas acotado por frame; el resto espera (como el streaming de terreno).

**Determinismo** (cliente=servidor): Jolt en `JobSystemSingleThreaded` + timestep fijo (ya es así en
[[jolt-fisica]]). La rotura debe dar el MISMO resultado en ambos lados → sin aleatoriedad no sembrada.

## 4. Colocación free-form + validación

**Cliente (preview):** raycast desde la cámara → punto en el terreno o sobre una pieza existente; la pieza se
previsualiza ahí, orientable; snap suave a la superficie (normal del terreno vía `meshHeightKmAt` +
gradiente, o a un `AnchorPoint` cercano de otra pieza). Verde = válido, rojo = rechazado.

**`validatePlacement(pieceType, transform, world)` (lib de reglas, cliente Y servidor):**
- **Terreno**: la pieza NO penetra la malla (muestrea `sampleTerrainHeight` en la huella del hull).
- **Solape**: el hull no interseca otra pieza ni OBB de objeto (props/entidades) — narrow-phase de Jolt.
- **Anclaje**: la pieza TOCA el suelo o un `AnchorPoint` de una pieza ya anclada (si no, no se sostiene →
  rechazo, o se coloca "suelta" y cae, según regla).
- **Alcance/recursos/permiso**: distancia al jugador, coste, y si la zona permite construir (imperio/claim).
- Devuelve `ok` o un **motivo** (para el feedback rojo del cliente).

Cliente y servidor llaman a la MISMA función → la predicción del cliente coincide con la validación del DGS.

## 5. Descompositor: `.glb` → `PieceType`

- **Carga**: `Renderer::Model` (assimp, ya soporta `.glb`) → `Mesh` (verts/índices).
- **Extracción**: por cada pieza del modelo → **convex hull** (colisión barata y estable en Jolt) + AABB +
  detección de **puntos de anclaje** (caras planas / marcadores por convención de nombres en el .glb, p.ej.
  nodos `anchor_*`). Guarda el `PieceType` en el catálogo.
- **Render**: el cliente instancia la MALLA original del `.glb` (`gpu_instancing` / patrón de
  `resource_prop_renderer`: índice de prototipo + matriz + tinte, ~80 B/pieza). El servidor NO carga mallas.
- **Herramienta**: un paso offline (o en carga) que tuesta los `.glb` a catálogo. ⚠️ Convención de nombres de
  nodos en el .glb para anclajes/sub-piezas = decisión de contenido (§7).

## 6. Roadmap por fases (fundamento primero; cada fase: compila + tests verdes)

- **F1 — Pieza + colocación** *(prototipo con primitivas, sin .glb aún)*: `PieceType`/`Piece`/`ConstructionState`
  en la lib de reglas; descompositor mínimo (caja/cilindro primitivos); preview + snap a terreno;
  `validatePlacement` (terreno + solape + anclaje). Render instanced de una pieza. **Verificable**: colocar
  una pieza válida sobre el terreno esquivando obstáculos; test de `validatePlacement` (legal→ok, penetra/
  solapa→rechazo), idéntico en lib estática y `.so`.
- **F2 — Ensamblaje**: grafo de anclajes; varias piezas → estructura; Jolt las simula (constraints).
  **Verif.**: una pared de piezas se sostiene anclada; se serializa/deserializa igual.
- **F3 — Rotura por resistencia**: umbrales + fallo propagado + zonas protegidas + fusionar/fragmentar (§3).
  **Verif.**: un impacto derrumba la estructura en cascada; la zona protegida aguanta; determinista.
- **F4 — Edificios de forma dinámica**: reglas de generación (gramática/plantillas) que componen edificios
  con el kit. Depende del catálogo de piezas `.glb` (§7).
- **F5 — Mundo**: imperios dividen la tierra → ciudades cada X, pueblos cada Y, instanciando edificios F4.
  Sistema grande aparte; depende de F4 y de la estética (§8).

## 7. Contenido — DECISIONES ABIERTAS (a rellenar con el usuario)

- **Modelos `.glb`**: catálogo de piezas (bricks, vigas, tornillos, muros, techos…). Convención de nodos
  para anclajes/sub-piezas. *(no existen aún)*.
- **Arquitectura POR IMPERIO**: estilos visuales/estructurales por facción (materiales, siluetas, reglas de
  edificio). Define los `PieceType` y las gramáticas de F4.
- **Ciudades / capitales / pueblos**: layout (murallas, distritos, plaza, puntos de interés), densidad,
  distancias (X ciudades, Y pueblos), y qué las distingue (capital vs ciudad vs pueblo).
- **Reglas de claim/permiso**: quién puede construir/romper dónde (imperio, propiedad, zonas protegidas).

## 8. Dependencias y riesgos

- **Estética terreno/texturas (usuario)**: cambio pendiente que condiciona el look de F4/F5. No bloquea la
  lógica F1-F3. Apuntar como tarea previa al mundo. Ver [[terreno-v3.md]] (texturizado por pendiente/biomas).
- **Coste del grafo por pieza**: mitigado por §3 (sleep + fusionar/fragmentar), pero es el riesgo de perf #1.
- **Determinismo cliente↔servidor de la rotura**: cualquier no-determinismo (orden de iteración, float) hace
  divergir predicción y validación. Timestep fijo + `JobSystemSingleThreaded` + sin RNG sin sembrar.
- **Free-form + anti-cheat**: validar colocación arbitraria es más difícil que una rejilla. `validatePlacement`
  debe ser robusta (penetración, solape, anclaje, alcance) o se cuela construcción ilegal.

## 9. Verificación global

- `haruka_tests` verde tras cada fase (+ tests nuevos: `validatePlacement`, serialización del estado,
  determinismo de la rotura).
- `validatePlacement`/`validateBreak` dan el MISMO resultado con la lib estática (cliente) y con el `.so` (DGS).
- En juego: colocar/romper con `SURVIVAL_SPAWN` fijo para comparar builds.
