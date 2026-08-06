# PLAN PLANETAS MANUALES + CAPAS DE PROPS + CALIDAD CAMINABLE

> Tres features que se refuerzan y se implementan sobre el mismo pipeline (SimplePlanet + clipmap).
> **A. Planetas manuales**: el autor pinta mapas; cada zona arrastra una COLUMNA geológica (materiales
> por profundidad). **B. Capas de props**: árboles, rocas, minerales, monstruos y entidades como DATO
> de escena, colocados por función determinista. **C. Calidad caminable 4k-8k-16k con mínima
> latencia**: anillos concéntricos, doble capa de textura, streaming asíncrono de tiers.
>
> **Decisiones del autor integradas en este plan** (2026-08-01):
> - El **material y la profundidad se deciden PER-PÍXEL**: el fragment lee la textura y decide
>   qué material hay arriba y a cualquier profundidad. La elevación NO puede ser per-píxel (es
>   geometría): la decide la **segunda textura** (`elevationMap`) a resolución de vértice, y la
>   función de detalle rellena por debajo de lo que la textura puede representar.
> - Las **capas se añaden "más o menos según necesario"**: materiales, columnas, estratos y capas
>   de props son listas de longitud ARBITRARIA en el JSON, sin el tope de 16 del UBO actual.
>
> Estado del pipeline hoy: [TERRENO.md](../../TERRENO.md) (clipmap a 2 m, coste 16-26 ms/frame,
> costura por sesgo de profundidad, paridad CPU↔GPU 34 µm). Bugs e ideas sin decidir:
> [TODO.md](../../TODO.md). Lo que ya había decidido el autor y NO se reabre: sin chunks, sin
> residencia, una sola fórmula, paridad, el mapa manda, el material es dato.

## 0. Principios que este plan hereda y no viola

| Principio | Cómo lo respeta este plan |
|---|---|
| **Si nada tiene que estar cargado, nada puede faltar.** | Los estratos (§A) y los props (§B) son FUNCIONES puras de `(seed, dir)` — se evalúan donde hace falta y se tiran. No hay streaming de props ni de vetas. |
| **Lo fino se calcula, no se guarda.** | La profundidad es una función `stratumAt(dir, depth)`, no una textura 3D (§A.2). |
| **Paridad CPU↔GPU (una sola fórmula, bit a bit).** | `stratumAt` y la colocación de props van en ficheros GEMELOS como `terrain_detail` (§A.4, §B.2). |
| **El material es una regla, no un if.** | Los estratos se declaran en el JSON; el motor no conoce "humus/arcilla/veta" (§A.1). |
| **El mapa manda cuando el autor pinta.** | Mapa de zonas (superficie) + mapa de columnas opcional (profundidad) + mapas de capas de props. |
| **Per-píxel para material, per-vértice para geometría.** | Material/estratos: fragment (ya es per-píxel hoy). Elevación: la textura la decide en la malla; la función rellena el hueco. |
| **Una sola proyección cubo↔esfera.** | Todo lo nuevo muestrea equirect con las MISMAS funciones `equirectUV`/`sampleZone`. |

Nota de deuda: este plan asume la ruta SimplePlanet (la que funciona hoy). La descomposición
`Planet`/`PlanetRenderer` (TERRENO.md §8) es ortogonal; no hace falta para empezar, pero los
ficheros nuevos (strata, prop layers) deben nacer GL-free y en `src/core/planet/` para no
perpetuar el acoplamiento.

---

## PARTE A — Planetas manuales: materiales por profundidad

### A.0 Qué existe hoy, y el reparto per-píxel / per-vértice

Existe: `surface.zoneMap` (equirect, colores planos, Nearest) elige el material de SUPERFICIE y el
signo mar/tierra (`submerged`); `surface.elevationMap` (gris, bilineal) pinta la altura; los
materiales son REGLAS en `surface.materials` → UBO 12 (`terrain_material.h` +
`lib/terrain_material.glsl`). La CPU replica la selección (`zoneToMaterial` en
`planetary_system.cpp`).

Falta: **nada debajo de la superficie**. El terreno es un heightfield; cavar
(`editTerrain`/`levelTerrain`/`mesh_cut`/`ground_stamp`) no tiene información de qué hay al
descender. El corte de una excavación no enseña estratos, y un pico no puede saber si hay una veta.

Qué se decide dónde — este es el reparto que NO se negocia:

| Decisión | Dónde | Por qué |
|---|---|---|
| **Material de superficie** | **PER-PÍXEL** (fragment) | una costa a resolución de vértice saldría escalonada; el shader ya lo hace hoy (`harukaSelectMaterial`) |
| **Columna / estratos (profundidad)** | **PER-PÍXEL** (fragment) + CPU por consulta | el corte de una excavación y el pico preguntan en un PUNTO, no en una retícula |
| **Elevación** | **PER-VÉRTICE** (malla base + clipmap) | es GEOMETRÍA: no se puede decidir más fino que el triángulo; lo que la textura no puede representar lo pone la función de detalle |

### A.1 La columna geológica como DATO, elegida por textura (per-píxel)

La profundidad se decide con el MISMO patrón que el material de superficie: **una textura elige la
COLUMNA de cada píxel**, y la profundidad elige el estrato dentro de la columna. Dos texturas
pueden elegir la columna, y nunca se mezclan:

1. **La zona** (camino principal): cada material de superficie declara su columna. Pintas el mapa
   de zonas (ya existe) y eso decide el material de superficie Y las columnas a cualquier
   profundidad. Una pincelada de "bosque" = hierba arriba + arcilla + roca + la veta que decidas.
2. **Un mapa de columnas aparte** (`surface.stratigraphyMap`, equirect, paleta, opcional): cuando
   quieres que "aquí y solo aquí" la columna sea distinta de la de su zona.

Una columna es una lista de estratos **sin límite de longitud**:

```json
"columns": [
  { "color": [34, 88, 40], "name": "bosque_sobre_veta",
    "strata": [
      { "name": "humus",   "top": 0.0,   "bottom": -0.4,  "tile": "land_albedo.png" },
      { "name": "arcilla", "top": -0.4,  "bottom": -4.0,  "tile": "clay_albedo.png",  "tint": [0.55,0.4,0.3] },
      { "name": "roca",    "top": -4.0,  "bottom": -60.0, "tile": "rock_albedo.png" },
      { "name": "hierro",  "top": -60.0, "bottom": -90.0, "tile": "ore_albedo.png",   "density": 0.6 } ] },
  { "color": [20, 50, 90], "name": "fondo_marino",
    "strata": [ { "name": "sedimento", "top": 0.0, "bottom": -200.0, "tile": "sand_albedo.png" } ] }
]
```

- **`top`/`bottom` en metros** bajo la superficie (negativos hacia dentro). `tint`, `density`
  [0,1] por estrato. Un estrato sin `tile` = color plano (veta sin textura, sal, hielo).
- **El tope de 16 materiales del UBO desaparece para esto.** La tabla de columnas/estratos viaja en
  un **SSBO con cuenta en tiempo real** (binding 16; el RHI ya tiene SSBO — el clima de
  `PlanetFields` usa el binding 10). "Añadir más o menos según necesario" es añadir entradas al
  JSON, sin recompilar y sin techo fijo. El UBO 12 de la superficie se mantiene para el aspecto
  visual (tinte/grano/tile), y su recuento también puede migrar a SSBO si se quiere pasar de 16
  materiales visibles.
- El motor sigue sin saber qué materiales existen: los `tile` de los estratos van a un **segundo
  array de terreno** (unidad 16) para no tocar el array de superficie (unidad 12). Ficheros:
  `TerrainMaterialTable::strataPaths()` al lado de `albedoPaths()`.

### A.2 `stratumAt(dir, depthM)` — la profundidad como función, per-píxel

Dos ficheros GEMELOS, como `terrain_detail.h`/`terrain_detail.glsl`:

- **NUEVO** `src/core/planet/terrain_strata.h` (GL-free) + `assets/shaders/lib/terrain_strata.glsl`.

```
stratumAt(dir, depthM) → (material, density)
  1. columna del píxel = zona (Nearest) o stratigraphyMap (Nearest)  ← textura decide, per-píxel
  2. depthOffset = strataMap(dir)  (opcional, bilineal, metros)       ← §A.4
  3. recorre los estratos de ESA columna y devuelve el que cumple
     top+depthOffset > depthM > bottom+depthOffset
  4. sin columna declarada → roca (substrato global)
```

Reglas de paridad IDÉNTICAS a las ya pagadas en `terrain_detail`: todo en `float`, sin
`-ffast-math` en los ficheros del camino, hashes con aritmética entera si hace falta ruido, sin
raíces ni divisiones en el camino común. El resultado se comparte bit a bit entre el pico del
jugador (CPU) y el corte visual (GPU).

### A.3 La elevación la decide la SEGUNDA textura (`elevationMap`)

Con la escala real de partida (radio ~6 371 km → circunferencia ~40 000 km):

| Recurso | Resolución efectiva | Qué puede representar |
|---|---|---|
| Malla base | un vértice cada **39,1 km** | la forma del planeta, nada más |
| `elevationMap` a **4096²** | **9,8 km/téxel** | 4× más fino que la malla: continentes y cordilleras |
| `elevationMap` a **16384² (16k)** | **2,4 km/téxel** | costas y cordilleras finas, lo que el autor pinte sale donde lo pinte |
| Función de detalle (octavas) | 2 m en el clipmap | el relieve que se camina — lo único que no se puede guardar (255 TB) |

**Compromiso de diseño**: cuando el planeta trae `elevationMap`, la elevación la decide ESA
textura, per-vértice, en `heightFn = sampleElevMap(dir)` — las placas se ignoran (el autor no
puede ver el campo que modula su pintura). El detalle por debajo de lo que la textura puede
representar lo pone la función. Entre los dos: el autor pinta la FORMA, el motor rellena el
DETALLE que se pisa.

### A.4 Manual por mapa: el desfase de estratos

Para que la profundidad sea MANUAL y no solo declarada, un mapa opcional:

```json
"strataMap": "depth_field.png",   // equirect GRIS, bilineal (como elevationMap)
"strataOffsetRange": [-20.0, 40.0]  // metros a los que corresponde 0 y 255
```

Cada píxel gris desplaza los límites de la columna en ese punto: "aquí la veta sale a −10 m, allí
a −40". Es el mismo truco que `elevationMap` con la altura: un canal, no un mapa por banda. **Sin
él**, las columnas van planas (lo común). El desfase entra en `stratumAt` (§A.2 paso 2). Si se
quiere "aquí NO hay arcilla" puntualmente, se modela como densidad 0 en un rango — no hace falta
un mapa nuevo.

### A.5 El corte excavado se ve (GPU)

Cuando `editTerrain`/`mesh_cut` deja una pared, esa pared no es un objeto aparte: es el heightfield
ya bajado, con pendiente casi vertical. El fragment shader ya tiene `slope` y `vClimate.x`
(elevación). La profundidad bajo la superficie ORIGINAL en ese píxel es:

```
depthM = (R + baseH_orig + detalle_orig) − (R + baseH_cur + detalle_cur)
```

- `baseH_orig` sale del MISMO `baseFieldTex` que ya usa el clipmap (`sampleBase`, unidad 15) — la
  CPU deja la altura sin editar en el campo y aplica los edits aparte (`m_heightEdits`), así que la
  original es consultable sin tocar el contrato.
- Cuando `slope` > umbral Y `depthM` > ~0,2 m, `biome.frag` mezcla hacia `stratumAt(dir, depthM)`.
  El relieve de la excavación es geométrico (ya existe); solo la VISTA del estrato es nueva.
- Es un coste PER-PÍXEL en el camino de materiales; se hace en un pase/triplanar aparte para no
  encarecer el camino de superficie.

### A.6 Minerales y pico

- El estrato con `density` < 1 es la veta: `stratumAt` devuelve la densidad; el pico del jugador
  (o el sistema de recolección) extrae según ella. Determinista: la MISMA semilla → la MISMA veta.
- Las vetas siguen a los estratos, y los estratos a las columnas que pinta el autor: "hierro aquí"
  se decide una vez, no píxel a píxel.

---

## PARTE B — Capas de props y entidades

### B.0 Qué existe hoy

`GPUInstancing` (prototipo + transformación + tinte, un draw por tipo) está migrado y funciona.
`SpawnSystem` gestiona puntos de spawn/respawn por tipo (MONSTER, NPC…). `TerrainSample` ya declara
"el bioma decide dónde van los props". `m_heightEdits`/`ReferenceSurface` dan el suelo. Falta: **la
capa de AUTORÍA** — pintar dónde hay árboles, rocas, vetas, monstruos — y la colocación sobre el
terreno.

### B.1 `propLayers` como DATO de escena

Nuevo bloque en `surface` del planeta (raw JSON, como `materials` — el motor no sabe qué protos
existen):

```json
"propLayers": [
  { "name": "bosque_pintado",
    "map": "forest_map.png", "color": [80, 160, 60],        // equirect, paleta, Nearest
    "protos": ["pine_a", "pine_b", "oak"],
    "density": 0.012, "cellSize": 8.0,                      // props/m²  · celda de hash
    "slope": [0.0, 0.45], "avoidWater": true, "alignToNormal": true },
  { "name": "veta_cobre",                                    // mineral (junta con PARTE A)
    "map": "ore_map.png", "color": [160, 90, 20],
    "stratum": "cobre", "minDepth": -40.0, "maxDepth": -10.0,
    "density": 0.6 },
  { "name": "monstruos_norte", "map": "monster_map.png", "color": [200, 0, 0],
    "entity": "goblin", "max": 40, "cellSize": 4.0 } ]
```

- Cada capa es **mapa O regla**, nunca las dos (misma decisión que placas vs mapa): o pintas la
  región, o la capa se decide por bioma/clima. Dos fuentes mezcladas = "no está donde la pinté".
- `cellSize` separa la colocación: dentro de cada celda, el hash decide cuántos y cuáles.

### B.2 Colocación como FUNCIÓN determinista (sin residencia)

Mismo contrato que el detalle del terreno: **los props no se guardan, se evalúan**.

```
propDecider(layer, cell, seed) → { proto, n, scale, yaw, tint }   // hash entero, bit-exacto
pos = sampleHeight(dir) + normal·(alto del proto)                  // el MISMO suelo que pisa el jugador
```

- Hash ENTERO como `detailHash` (`terrain_detail.h`): reproducible entre cliente y servidor con la
  sola seed del planeta → el DGS ve el mismo mundo. El `ReferenceSurface`/`sampleTerrainHeight`
  (paridad 34 µm) da el asiento → **los props no flotan ni se hunden** (el fallo ya pagado).
- Nada que pueda faltar: no hay cola de carga de props, ni caché, ni estado de residencia.

### B.3 Render e instancias

- Cada frame se evalúan las capas de props dentro de un radio (cull por celda), se llenan
  `GPUInstancing` por (tipo, LOD) y se dibujan en un pase tras el terreno (con el mismo
  reversed-Z/sesgo). Variedad por tinte/escala del hash — sin geometría nueva.
- **Coste CPU**: evaluar celdas por frame. Presupuesto inicial: radio 600 m + cull de frustum. Si
  el coste pasa del presupuesto (bosques densos), la colocación se mueve a **compute** (un dispatch
  escribe el buffer de instancias con el mismo hash — paridad con la CPU del hash y del asiento).
  Es la única parte del plan donde el coste podría escalar; por eso tiene presupuesto y fallback.

### B.4 Entidades / monstruos

- La capa con `entity` NO instancia la entidad: marca la región. `SpawnSystem` (ya existe) elige
  los puntos dentro de la región con la seed (determinista) y respeta `max`.
- Las capas conviven con los SceneObjects de spawn actuales (el editor ya tiene las capas Props /
  Monsters / Spawns): el mapa es la capa de mundo; los objetos son la capa de autoría puntual.

### B.5 Tests (patrón `haruka_tests`, GL-free donde se pueda)

- `props_determinism`: mismo `(seed, dir, layer)` → mismo `propDecider`.
- `props_seat`: la altura de asiento de un prop == `sampleHeight` en ese punto (paridad).
- `props_region`: un punto fuera de la zona pintada → la capa no actúa.
- `strata_determinism` y `strata_parity` (§A.4): mismo corte de excavación en CPU y GPU.

---

## PARTE C — Caminar a 4k-8k-16k con mínima latencia

Dónde está el hoy, medido en el repo:

| Pieza | Hoy | Techo para caminar |
|---|---|---|
| Geometría | clipmap 2 m en ~4 km; luego salto a 611 m sin octavas finas → **línea visible** (TERRENO.md "Lo que falta") | anillos: 2 m → 8 m → 32 m hasta ~64 km |
| Textura de material | tile `src/` **512²** a tiling 100 m → **~0,2 m/téxel** (los PNG de partida son 512², no 4096² como decía el techo de TERRENO §6.7) | doble capa: 100 m + capa fina a 2 m (0,5 mm/téxel) |
| Carga | tiers hd/4k/8k/16k **síncronos** en `loadTextureArray`; `texRes` evalúa biomas/macro en CPU al arrancar (4096² ≈ 13 min) | tier bajo inmediato + tiers altos en segundo plano |
| Coste por vértice | 15 evaluaciones de ruido (5 octavas × altura + 2 normales) | saltar octavas con peso 0 por `triM` |
| Detalle pintado único | no existe | VT (`virtual_texturing.{h,cpp}` está vacío, sin call-sites) |

### C.1 Geometría: anillos concéntricos

Sustituir el clipmap único de 31×31×128 m por **tres rejillas fijas en coordenadas de cámara**,
misma técnica (se reorientan cada frame, nunca se regeneran):

| Anillo | Parche | Triángulo | Alcance | Octavas que le tocan (Nyquist) |
|---|---|---|---|---|
| 0 | 128 m | 2 m | ~4 km | todas (22 m entera, 4,5 m al 12 %) |
| 1 | 512 m | 8 m | ~16 km | hasta 22 m |
| 2 | 2048 m | 32 m | ~64 km | hasta 111 m |

- La costura entre anillos usa el MISMO truco que hoy (`mix(clipM, meshM, smoothstep)`): cada
  anillo converge su `triM` al del anillo siguiente en su borde exterior → **las dos superficies
  coinciden donde una acaba y la otra sigue** (sin escalón, sin nueva maquinaria de cosido).
- El sesgo de profundidad existente (gana la fina) se mantiene; los anillos se dibujan de grueso a
  fino y la más fina vence donde se solapan.
- Esto quita la línea donde acaba el relieve de 2 m — el defecto visual más gordo del clipmap hoy.

### C.2 Material: doble capa de textura

- La capa gruesa (tiling 100 m, la actual) + una **capa fina de detalle** (tiling ~2 m, tile 4096² →
  0,5 mm/téxel) que se enciende SOLO en el anillo 0 (por distancia a la cámara), mezclada en
  `biome.frag` con la macro-variación existente + una variación fina nueva para romper la
  repetición de 2 m (sin esto, el tile de 2 m canta).
- Es exactamente la pieza que TERRENO §6.7 marca como pendiente ("para agacharse y ver el grano
  falta una segunda capa, no subir la resolución del tile").
- La capa fina es OPCIONAL por ajuste (grano fino = coste de muestreo); sin ella el planeta se ve
  como hoy.

### C.3 Latencia: streaming ASÍNCRONO de tiers

`AssetStreamer` (`src/io/asset_streamer.{h,cpp}`) **está implementado y sin un solo call-site**.
`virtual_texturing` igual. Esta es su oportunidad:

1. **Arranque instantáneo**: el planeta se construye con el tier bajo (`hd` 2048²) síncrono — el
   terreno aparece en milisegundos. El bloqueo actual de `loadTextureArray` con el tier alto se
   elimina.
2. **Promoción de tier en segundo plano**: `AssetStreamer` descodifica (stbi) y baja los tiers
   4k/8k/16k FUERA del hilo GL; el hilo principal sube la textura al completarse (cola diferida,
   una por frame). El jugador no nota el arranque ni el cambio.
3. **`texRes` (biomas + macro)**: generar el tier 512² al arrancar (segundos) y REGENERAR 2048²/
   4096² en segundo plano, intercambiando la textura al terminar. Hoy 4096² bloquea ~13 min.
4. **VRAM acotada**: presupuesto con `MemoryBudget`/`Cache`; si el tier alto no cabe, se queda el
   que cabe. Un 16k de 8 materiales RGBA8 ≈ 1 GB — solo Ultra y solo si el presupuesto lo permite.

### C.4 Coste por vértice

Los anillos solo pagan si el coste BAJA con la distancia: saltar las octavas cuyo `octaveWeight`
es 0 (con `triM` alto, 3 de las 5 octavas dan 0) en vez de evaluarlas y multiplicar por 0. Es la
optimización que TERRENO §8 anota ("hay margen obvio"): 15 evaluaciones → ~9 en el camino cercano
y ~6 en los anillos lejanos. Se hace con cuidado de NO cambiar el resultado donde el peso no es 0
(paridad de la física intocable; la CPU sigue evaluando SIEMPRE el caso más fino).

### C.5 Detalle pintado ÚNICO — virtual texturing (Fase 2, opcional)

Donde el autor pinta algo único (carretera, ciudad, huella de dragón), la doble capa no sirve
(repetición). `virtual_texturing.{h,cpp}` da el detalle ~1 cm LOCALMENTE sin exabytes: el feedback
de páginas que mira la cámara alimenta la carga asíncrona (presupuesto `maxResidentPages`). No
bloquea: la página ausente se muestra con el tier de abajo hasta que llega. Fase 2 porque lo que
falta primero es que el planeta manual (PARTE A) pueda REPRESENTAR lo que el autor pinte.

### C.6 Verificación a pie

El listón de calidad se fija a **1,7 m de altura de ojo** (lo que TERRENO §8 pide y nadie ha
hecho): relieve que se camina sin hervir, grano visible al agacharse, sin línea de anillo, sin
saltos de carga al volar. Cada fase de C se cierra mirándolo a esa altura, no en órbita.

---

## FASES — incrementales y verificables (dev + mundo nuevo; `haruka_tests` verde)

| Fase | Qué | Verificación |
|---|---|---|
| **F0** | Baseline: coste actual (vértices/frame, arranque, VRAM) | profiler + sonda offscreen |
| **F1** | PARTE A dato: `columns` en el JSON, tabla de columnas/estratos → SSBO 16, array de terreno de estratos (unidad 16) | `strata_determinism`; JSON de prueba |
| **F2** | PARTE A función: `terrain_strata.{h,glsl}` + `strataMap` + desfase | `strata_parity` (CPU↔GPU, corte de excavación) |
| **F3** | PARTE A visual: pared excavada pinta estrato (fragment, `depthM` reconstruida) | render de un hoyo: capas visibles y alineadas con `stratumAt` |
| **F4** | PARTE B dato: `propLayers` + `propDecider` GL-free + `sampleHeight` como asiento | `props_determinism` · `props_seat` · `props_region` |
| **F5** | PARTE B render: instancias por (tipo, LOD) con cull por celda; capas `entity` → `SpawnSystem` | render: bosque/roca/monstruos sin flotar; presupuesto de CPU medido |
| **F6** | PARTE C.1: anillos 1 y 2 (512 m / 2048 m), costura por `triM` convergente | a pie y en vuelo: sin línea donde acaba el 2 m |
| **F7** | PARTE C.2: capa fina a 2 m en el anillo 0 (opcional por ajuste) | agachado: grano de verdad, sin rejilla de repetición |
| **F8** | PARTE C.3: streaming asíncrono de tiers (`AssetStreamer` + cola diferida GL) + `texRes` diferido | arranque < 1 s con 16k disponible; volar sin stutter |
| **F9** | PARTE C.4: saltar octavas con peso 0 | paridad de física intacta (test existente) + coste medido |
| **F10** | PARTE C.5 (opcional): VT en la unidad 16/17 para detalle pintado único | 1 cm donde el autor pinta, sin exabytes |

Cada fase deja el juego EN EL ESTADO ANTERIOR + lo nuevo: no hay regresión posible entre pasos.

---

## RIESGOS Y TRAMPAS YA CONOCIDAS (no volver a pagarlas)

- **Paridad de `stratumAt`**: misma clase de fallo que "tres suelos" y "dos climas". Gemelos GLSL/C++
  con `-fno-fast-math`, hashes enteros, sin `mix` ambiguo. El test `strata_parity` es el candado.
- **La pared excavada y el campo**: `baseH_orig` se lee del campo SIN editar; los edits viven en
  `m_heightEdits`. Mezclarlos en la misma textura rompería la altura original que pinta el estrato.
- **El clipmap lee el campo a mano (`texelFetch`)**: si `strataMap`/nuevos mapas entran en el
  clipmap, se interpolan A MANO, nunca con el filtrado de GL (pesos de 8 bits → escalón).
- **Coste por vértice de los anillos**: sin §C.4, triplicar el clipmap triplica el peor caso
  (26 ms → ~70 ms). Por eso C.1 y C.4 van juntos en el orden de fases.
- **Props por frame**: si el coste CPU de `propDecider` se sale de presupuesto, hay que moverlo a
  compute (§B.3). Se mide en F5, no se asume.
- **Instancing y el suelo**: los props usan SIEMPRE `sampleHeight`/`ReferenceSurface`; un prop
  flotando es un bug de asiento, no de dibujo.
- **El tier `src/` es 512²**: el techo de TERRENO §6.7 asumía 4096². La capa fina (§C.2) es lo que
  da el grano; generar tiers altos es necesario pero no suficiente.
- **`terrain_material.glsl` layout**: añadir `strata` NO toca el UBO de materiales (binding 12, no
  hay espacio libre): la tabla de columnas/estratos va a un SSBO con cuenta (binding 16, ya hay
  SSBOs en el pipeline). Cualquier cambio al layout rompe el shader inline del SimplePlanet y
  `planet.frag` a la vez (el bug de linkado del TODO.md).
