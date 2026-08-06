# Historial — qué se hizo y qué trampas dejó

Archivo de trabajo YA CERRADO, movido aquí desde `TODO.md` para que ese fichero contenga solo
**bugs abiertos y ideas**. Se conserva íntegro porque su valor no es la lista de features: son las
**trampas** que costaron sesiones enteras (el `-ffast-math`, la caché caliente, el bloque UBO que
falla al LINKAR, el orden de `view`/`projection`) y que volverían a costarlas si se borran.

El plan por versiones está en [ROADMAP.md](../ROADMAP.md). Lo que sigue abierto, en [TODO.md](../TODO.md).

> Estado congelado a **2026-07-26**, con la suite en 19437 OK · 1 fallo (`render_collision_parity`,
> que estaba en rojo a propósito; hoy pasa — ver la sección del terreno).

---

## ✅ Hecho (no re-abrir sin motivo)

- **RHI + PSO completo.** Renderers sin GL crudo (terrain, water, fluid, ibl, escena, partículas, bloom,
  sombras). Queda: 3 compute al PSO + blits del fluido (falta `blit` en el RHI).
- **Reversed-Z + far infinito + D32F.** Depth uniforme de un guijarro a un planeta.
- **Terreno V3 (F0–F10) + estabilización (jul-2026).** Tectónica→erosión→hidrología→clima→biomas→props,
  todo de UN campo. Detalle en `docs/guides/PLAN_TERRENO_V3.md`. Cerrado esta tanda:
  - **MESO OFF por defecto** (`HARUKA_MESO=1` para desarrollarlo). El relieve de cerca lo da hoy
    `midRelief` (octava en el macro, LOD-continua), que da amplitud pero NO coherencia (es ruido, no
    erosión). Ya NO está bloqueado por estabilidad del suelo — ver §"TERRENO — PAUSADO AQUÍ".
- **UN SOLO SUELO: la superficie de referencia (2026-07-25).** `reference_surface.{h,cpp}`, GL-free (va
  en `haruka_simbase` → el DGS puede evaluar el MISMO suelo que pisa el cliente). Es el terreno
  muestreado en la **retícula de vértices del LOD de referencia**, con la misma bilineal que la malla.
  La leen Jolt, props, scripts, render y tests. Antes había TRES suelos distintos y por eso se veían
  "dos terrenos".
  - **Pin del LOD del suelo** (`LODSystem::setGroundLOD`, derivado de `kGroundSpacingM = 25 m`): dentro
    del keep-radius se dibuja EXACTAMENTE ese nivel. Sin él convivían lod 11 y 12 a <400 m, y el nivel
    dibujado dependía de la resolución de pantalla, del FOV y **de los FPS** (LOD adaptativo).
  - ⚠️ `getMaxLOD()` es un TECHO (vale 20), no el nivel que se dibuja. Usarlo daba una retícula de 6 cm
    = el analítico puntual con otro nombre.
  - **Refactor**: `cube_sphere.{h,cpp}` (Cobb + inversa, GL-free; la fórmula estaba DUPLICADA) y
    `mesoEnabled()` mudado a `planet_meso.h` (salía de una cabecera con glad).
- **Paridad CPU↔GPU (2026-07-25): mediana 0.0424 → 0.0007 m.** No había ninguna rama que discrepara: los
  dos lados evaluaban en puntos separados **1 ulp de `dir`**, que sobre la Tierra son 0.38 m de
  superficie. Arreglos: (1) el ruido reduce la coordenada en **DOUBLE** (`perlin3D(dvec3)` + su copia en
  el shader; las `vec3` delegan → una sola implementación); (2) fuera las re-normalizaciones en float de
  vectores ya unitarios (5 sitios); (3) el SSBO del campo se **COPIA** en vez de re-muestrearse
  (`PlanetFields::cell()`); (4) `pow` fraccionario → formas exactas con `sqrt`.
  ⚠️ `kGenVersion` 4 → 8: **cualquier** cambio de generación lo sube. Olvidarlo hizo que un test saltara
  de 0.6 cm a 3.4 m — parecía regresión, era la caché sirviendo el mundo anterior.
- **"Me caigo al girar / collider falso" ARREGLADO (2026-07-25).** Eran TRES fallos encadenados:
  1. **La cola del streaming se TIRABA en cada recompute**: se mandaba solo `chunksToLoad` (el DIFF) y
     `setDesiredChunks` REEMPLAZA la lista → un chunk pedido y no generado desaparecía y, como luego
     salía en `chunksToKeep`, no volvía a entrar NUNCA (medido: 191 hojas sin generar con `pending=0`).
     Fix: mandar `keep ∪ load`.
  2. **Un hueco tumbaba la CARA ENTERA**: `emitSubtree` solo desciende si las 4 hijas se cubren enteras
     → el draw set caía a la RAÍZ (lod 0) y la colisión con él (~90 m por encima del terreno fino que SÍ
     estaba en GPU). Se cura al quitar los huérfanos.
  3. **La cobertura base no cabía**: `minLOD` 4 = el planeta ENTERO a lod 4 = 1536 chunks (incluido lo
     de detrás del horizonte) → caché 1276/768 MB en thrash. Fix: **`minLOD = 2`** (96 chunks). El suelo
     de LOD es COBERTURA, no detalle. ⚠️ Subir a 3 (384) si desde ÓRBITA se ve basto.
  - **+ keep-radius** (`LODSystem::setKeepRadius`, 512 m): se refina alrededor del jugador **mires donde
    mires** (override del frustum Y del horizonte; el guard-band de 12° es un halo del frustum y no cubre
    lo que tienes debajo ni a la espalda). **+ `meshHeightKmAt` toma `dir` en DOUBLE** (en float, 6371 km
    solo resuelven ~0.5 m → 6.4 cm de error sobre pendiente).
  - **Medido**: draw set 9 chunks (6 raíces) → el cut completo · LOD bajo el jugador 0 → 12 · suelo
    813.350 m (falso) → 723.233 (real) · divergencia 90.117 → **0.000 m** · caché 1276/768 → 231/768.
  - ⚠️ **`validateLOD` NO detecta esto**: con el draw set colapsado decía `OK, holes=0, overlaps=0` (las
    6 raíces cubren la esfera). Valida topología, no detalle. Por eso el bug sobrevivió a toda la suite.
- **Caché de disco ON por defecto** (`HARUKA_DISKCACHE=0` la apaga). Su motivo para estar apagada ("el
  LOD no se asienta") era el mismo bug de la cola huerfanada. ⚠️ El meso NO está en `kGenVersion` (es
  interruptor de runtime) → la caché va en **directorio por variante** (`cache/terrain/base` vs
  `.../meso`), o un run con `HARUKA_MESO=1` envenenaría la caché de los runs normales.
- **Tests nuevos del suelo**: `./haruka_tests giro` (gira 360° y asevera: suelo bajo el jugador ≤0.5 m,
  **suelo bajo los PROPS** —anillo de 16 puntos a 150/340 m, umbral 1 m—, se dibuja malla fina no la
  raíz, y dibujado == residente) · `./haruka_tests vuelo` (sube 30 m → 30 km; en la banda jugable ≤200 m
  el suelo no se mueve). El del ANILLO es el que ve los "dos terrenos": el jugador se corrige CADA FRAME
  y el prop solo al detectar deriva, así que el fallo se ve a 150-340 m, no bajo los pies.
  - **Estancamiento de LOD ARREGLADO**: la cascada contaba solo chunks subidos a GPU → se quedaba gruesa.
    Fix: cuenta también los EN CACHÉ (`ChunkCache::snapshotHashes`), gateado por holgura de memoria.
  - **Colisión == render**: `meshHeightKmAt` replica el morph CDLOD → el suelo ya no queda "por debajo".
  - Test guardián `./haruka_tests earthdiag` (gap malla↔analítico determinista).
- **Props por prototipos + instancing.** Índice + matriz + tinte; `GPUInstancing` al PSO.
- **Construcción F1** (ver §2): núcleo de colocación GL-free + validado + preview/selección en juego.
- **Edificios/imperios/ciudades DESDE ARCHIVOS (2026-07-26, ver §2).** `buildings/materials.json` +
  `buildings/roles/` + `empires/` + `cities/` → `BuildingLibrary`. Añadir un imperio ya no recompila.
  El caparazón sigue procedural y el layout en el core: a datos van los markers de control, no la
  generación. Tests `survival_buildings` (177 OK) — comprueban que los datos MANDAN, no que se parseen.
- **Panel de MUNDO (F3 / `world`)**: estructura por servidor → zona → grupos, lo unido como padre con (N).
- **Perf + memoria.** Frame CPU-bound domado; vigilante de memoria (caché sobre RAM total, no libre).
- **Herramientas.** `SURVIVAL_SPAWN=lat,lon` · `HARUKA_PROF_LOG=N` · consola `chunkmem`/`terrainq`.


---

# Detalle por sistema

### 1. Estilo: CEL-SHADING (anime frondoso, Genshin/SAO) — **antes de producir más arte**
DIRECCIÓN CERRADA (2026-07-20): **mood cálido-aventura con crudeza mística** (luz cálida, sombra FRÍA y algo
desaturada) · **firma = hierba/viento + cielos dramáticos + atmósfera del planeta EN CONJUNTO** · biomas con
paleta propia = lo que lo hace único (Genshin/SAO son mundos cerrados). Tablero de arte publicado (artifact).
- **v1 HECHO (2026-07-20)**: pase de **cel SUAVE + rim-light** en `final.frag` (y espejo en `construction_inst.frag`):
  terminador suave (no duro), sombra fría, rim fresnel frío, specular toon en mancha, luna suavizada. Compila +
  shaders validan. ⚠️ SIN verificar en juego → **ajustar valores con el usuario** (umbrales del terminador, color
  de sombra/rim, potencia del rim).
- **FOLLAJE v1 HECHO (2026-07-20)**: la vegetación (árboles/arbustos/hierba) la dibuja `resource_prop_renderer`
  con `prop.vert`/`prop.frag` (NO final.frag). Metí ahí: (1) **toon** cel-suave + rim frío (mismo idioma que la
  escena), (2) **follaje TRANSLÚCIDO** — las HOJAS (verde) brillan a contraluz (SSS barato), (3) **viento** en
  `prop.vert` (ondeo procedural solo de hojas, más en lo alto, fase por prop; `u_time` desde el renderer). Compila
  + glslangValidator OK. ⚠️ La pasada de SOMBRA (`prop_depth.vert`) NO ondea → sombras no siguen el vaivén (menor).
  Sin verificar en juego → **tunear** con el usuario (amplitud viento, fuerza translucidez, sombra fría).
- **HIERBA DENSA v1 HECHO (2026-07-20)**: (1) `makeGrassTuft` reescrito FRONDOSO (16-27 briznas, más altas/anchas,
  curvadas, punta más clara con el verde dominante → sigue siendo "hoja" para el shader); (2) en `resource_scatter`
  la rama de hierba emite un RACIMO (4-6 matas extra jittereadas por celda) **solo a <34 m** de la cámara (carpeta
  densa donde se ve; lejos = 1 mata, sub-píxel → sin gastar presupuesto). Toda la hierba hereda el toon+viento+
  translucidez del prop renderer. Compila. ⚠️ Sin verificar en juego → tunear (nº extra, radio 34 m, densidad con
  `foliage`); ⚠️ vigilar que el racimo cercano no se coma el cap `maxProps` (si los árboles clarean, subir el cap).
- **TOON DEL TERRENO HECHO (2026-07-20)**: `planet.frag` pasa de `ambient+0.7*ndl+shadow` a cel SUAVE + sombra
  FRÍA (mismos valores que final/prop), manteniendo shadow map (PCF), luna y niebla; sigue MATE (sin spec).
  Valida a SPIR-V. Ahora TODO el mundo comparte el idioma toon: escena + piezas instanciadas + props/follaje +
  hierba + terreno. ⚠️ Sin verificar en juego → tunear con el usuario.
- **BRUMA cálida HECHA (2026-07-20)**: `FOG_COLOR` de planet.frag y prop.frag pasa de gris dieselpunk a
  `mix(vec3(0.66,0.78,0.90), sunLightColor, 0.35)` → bruma anime que se calienta con el sol (hora del día). Validan.
- **CIELO: NUBES "REALES" — sistema de clima por FASES (2026-07-20)**. `sky.frag`:
  - **Fase 1 (volumen+movimiento) HECHA**: 2 capas a distinta escala/velocidad (parallax → profundidad, no pegote),
    auto-sombra hacia el sol (base más oscura), remap "puffy", deriva con el viento. `u_time` añadido a `SkyParams`
    (reusé el `_pad`, sin crecer el UBO; se rellena en application_render con un reloj steady_clock).
  - **Fase 2 (ciclo del planeta) HECHA**: `cloudiness` por `fbm(u_time*lento)` → despejado ↔ cubierto (frentes).
  - **Fase 2b (CINE) HECHA**: `cloudField` con DOMAIN WARP (formas billowy) + erosión; auto-sombra en 3 pasos hacia
    el sol (Beer-Lambert → gradiente luz/sombra 3D); **campo de MASA** de baja frecuencia → las nubes ESCALAN de
    tamaño por zonas (cúmulos grandes/despejado/puffs); SILVER-LINING a contraluz. Palancas en el bloque.
  - **Fase 3 (según CLIMA/humedad) HECHA**: `SkyParams` crecido a 128 B (+`vec4 weather`); se muestrea `humidity`/`tempC`
    en la cámara (`sampleSurface`) → cobertura regional (húmedo=cubierto, desierto despejado).
  - **Fase 4 (LLUVIA) HECHA**: cuando muy cubierto (ciclo+humedad) y no hiela → cielo overcast/gris + **pase de LLUVIA
    de pantalla** (`rain.vert/frag`, fullscreen sobre el composite, blend, 2 capas de gotas con viento). Comando
    `rain <0-1|auto>` (override para probar; `m_rainOverride`). PENDIENTE: suelo MOJADO (specular/oscurecer) + charcos
    + nieve (si frío) + salpicaduras; que el pase reaccione al viento real; y que la nube que da lluvia case en pantalla.
- **PALETA/ACORDE POR BIOMA HECHO (2026-07-20)**: `planet.frag` tiñe la LUZ (0.35) y la BRUMA (0.5) con un acorde
  derivado del clima ya calculado (`tempC`/`humidity`): eje temperatura frío-azul↔cálido-dorado + humedad
  seco-polvo↔verde-lush. Sutil, no recolorea el albedo. Valida. Solo TERRENO (props no muestrean clima → paso aparte).
- SIGUIENTE: **bloom + color grade** (el "glow" anime) → **agua TOON** (water_renderer) → nubes en movimiento
  (u_time en SkyParams) → acorde en props → contorno solo en personajes.

### 2. Mundo habitado: CONSTRUCCIÓN + CAMINOS + asentamientos
Plan: `docs/guides/PLAN_CONSTRUCCION.md`. Estado F1 hecho (colocar/validar/preview una pieza). Siguiente:

- **✅ ARQUITECTURA — construcción SACADA de físicas (2026-07-19).** `construction.{h,cpp}` movido a
  `src/game/construction/` y a su **propia lib GL-free `HarukaConstruction`** (PIC), fuera de `HarukaPhysics`.
  No es física: es **validación** determinista (para el DGS). La linkan `HarukaEngine` y `haruka_simbase`.
  Verificado GL-free (`nm` sin GL/SDL) + 6/6 tests. Include: `"game/construction/construction.h"`.
- **Kit de PIEZAS**: beams (vigas), paredes, suelo/techo, remates… Kit modular con SOCKETS/puntos de
  anclaje (no modelar 100 paredes: combinar ~15 piezas). AABB real del `.glb` para la colisión (hecho en F1b-3).
- **F2 — PREFABS + FIJACIONES.** Además de beams sueltos, colocar un **suelo/estructura COMPLETO pre-hecho de
  beams** como UNA pieza (prefab = mini-grafo ya ensamblado, un solo placement). **Clavos/tornillos** = las
  ARISTAS del grafo de anclaje que unen piezas (romperlas propaga carga → colapso); el prefab trae sus aristas
  internas resueltas. ⚠️ Render de piezas hoy **ESTÁTICO** (1 SceneObject/pieza, por-objeto); un prefab de N
  beams NO pueden ser N draws → **instancing/malla compuesta** por estructura (ver punto de instancing abajo).
  - **F2a HECHO (2026-07-19)**: grafo de anclaje en `ConstructionState` (fijaciones=aristas, `isSupported` por
    BFS al suelo, `structureComponent`, `relabelStructures`). GL-free, determinista. Es la base de F3 (cortar
    aristas → recomputar soporte → cae lo no soportado) y de agrupar/mover estructuras.
  - **Catálogo DATA-DRIVEN desde items HECHO (2026-07-19)**: las piezas salen del `ItemRegistry` (item con tag
    `construccion` + modelo `.glb`); `typeId` estable por orden de id. items.json: foundation/block_stone/table_wood.
  - **Reglas "sobre qué" HECHO (2026-07-19)**: `PieceType.supports` (terreno/pieza) + `needsFlat` → `validate`
    da `WrongSupport`/`TooSteep`. Reglas desde tags del item (`apilable`, `llano`). 17/17 tests.
  - **Sockets/SNAP (b) HECHO (2026-07-19)**: sockets derivados del AABB (cara arriba + 4 lados); la pieza
    engancha al socket más cercano a la mira (`kSnapRadius`). Cliente-only (snap→validar); el DGS no ve sockets.
    PENDIENTE: (a) override de sockets por pieza (`.glb`/item) para piezas no-caja o semánticas.
- **TERRENO = HERRAMIENTA APARTE (decisión 2026-07-19), no auto-deform.** Las piezas SOLO se apoyan (quité el
  rechazo por penetración del core: `PenetratesTerrain`/`kPenetrateTol` fuera). El relieve se moldea con una
  herramienta de terreno en modo build (nivelar/subir/bajar/**roller** para tierra) que reusa el `DeformationField`
  (mismo que la tecla G): `app->editTerrain`/`levelTerrain`/`levelTerrainBox`. Los brushes son estado del mundo
  (se guardan) y `DeformationField` es GL-free → el DGS valida contra el mismo terreno deformado.
  - **HECHO (2026-07-19)**: herramienta de terreno cableada en modo build. **F** alterna pieza↔terreno; en
    terreno **C** cicla nivelar/subir/bajar y **B** aplica bajo la mira (`app->levelTerrain`/`editTerrain`).
    PENDIENTE: radio del pincel por RUEDA (hoy fijo 3 m) y el **roller** (va con la clase ROAD).
- **DOS MODOS de colocación HECHO (2026-07-19)** (herramienta cíclica con **F**: pieza → terreno → camino):
  1. **Básico**: un punto → coloca la pieza (con snap a sockets + preview verde/rojo).
  2. **Puntos de referencia (Path)**: **B** marca puntos; entre dos puntos coloca una HILERA de la pieza
     seleccionada a lo largo del tramo (proyectada al suelo, sigue el relieve); encadena **dobleces**. Reusa
     `validateBatch`/`addBatch`. PENDIENTE: **yaw** (alinear la pieza con la dirección del tramo — hoy +Y
     radial, sin girar), curva suave (spline en vez de polilínea) y **escaleras** (escalonar la altura).
- **ROADS / TRACK** (clase aparte). **Base HECHA (2026-07-19)**: `Survival/scripts/roads.{h,cpp}` — `RoadSystem`
  con API `createRoad(waypoints, tipo, material)` REUTILIZABLE por el generador; aplana el terreno a lo largo del
  trazado (`levelTerrainBox` por tramo, anchura por tipo). Comando CLI `road start|mark|build <tipo>|list`. **4
  tipos**: principal 12 / secundaria 8 / urbana 6 / general 5 m. PENDIENTE: superficie/visual de la vía (hoy solo
  aplana), **roller/apisonadora** para tierra, spline suave (sigue valles/collados), y que el GENERADOR lo invoque.
- **Asentamientos por IMPERIO**. **HECHO (2026-07-19)**: (paso 1) generador de EDIFICIOS por MATERIAL + estilo de
  imperio (`building_gen`): lista de materiales, markers por imperio (materiales/formas de tejado/tamaños-plantas
  normales), forma procedural por semilla; CLI `building [imperio] [seed]`. (paso 2) generador de CIUDADES
  (`city_gen`): un imperio → rejilla de parcelas + cruz de calles (`RoadSystem`); CLI `city [imperio] [seed] [grid]`.
  Items con esquema de tallas/variantes/bulk/proc.
  **(2026-07-26) TODO ESO SALE YA DE ARCHIVOS**, no de tablas en el .cpp: `assets/data/buildings/materials.json`
  (color + la PIEZA con la que se teje), `buildings/roles/*.json` (el PROGRAMA: huella, plantas, ritmo de huecos,
  fixtures), `empires/*.json` (el ESTILO: materiales por id, formas de tejado, proporciones, tallas) y
  `cities/*.json` (el TRAZADO: mezcla de roles con peso, clase/anchura de calle, avenidas cada N, densidad).
  Cargados por `BuildingLibrary` (`scripts/data/building_data.{h,cpp}`), que VALIDA y lo canta por log. CLI
  `building [imperio] [seed] [rol]` · `city [imperio] [seed] [grid] [plano]` (+ `building styles|roles`,
  `city plans`). Tests `survival_buildings` (159 OK): comprueban que los datos MANDAN, no que se parseen.
  PENDIENTE: **el usuario hace los modelos base de casa por ROL** (van en los huecos como fixtures);
  instancing de edificios a escala; mallas procedurales de items naturales (proc).
- **PANEL DE MUNDO (debug) HECHO (2026-07-26)**: `Survival/scripts/ui/world_debug.{h,cpp}`, arriba a la
  derecha, **F3** o consola `world`. Árbol **SERVIDOR → ZONA → grupos** con la regla del autor: lo que está
  UNIDO sale como **PADRE con (N)** — `city7_1_0 (487)` y dentro `muro 3 (96)` / `losa 0 (12)` / `tejado (30)`;
  las piezas del jugador se agrupan por `Piece::structureId` (componentes conexas del grafo de fijaciones).
  ⚠️ El cliente **no puede enumerar las zonas del DGS** (`ZoneHandle` es opaco): muestra el nodo conectado
  (`Network::serverLabel()`) y la partición en zonas que calcula ÉL (250 m/1 km/4 km) — lo dice en el panel.
  Se reconstruye a **2 Hz**, no por frame (recorrer 25 000 objetos con consultas JSON por frame es el coste
  que se quitó del bucle de dibujo). PENDIENTE: que el DGS exponga la propiedad de zonas (entonces el árbol
  deja de ser una estimación); recursos (scatter procedural) no salen porque no son SceneObjects.
- **BUG ARREGLADO (2026-07-26) — el vidrio de las ventanas era del TEJADO**: nacía sin `segId` y la pasada
  final (la que adopta lo suelto como tejado) se lo quedaba → romper el tejado borraba las ventanas del
  edificio ENTERO y cada cristal registraba una caja de colisión en un vano que debe poder cruzarse. Ahora va
  con el `segId` de SU muro + `BuildingPart::kind` (muro/losa/tejado, prop `bkind`) para poder nombrar la
  parte. Test `parentesco` en `survival_buildings` (177 OK); comprobado que falla si se revierte.
- **Rotura estructural (F3) HECHO (2026-07-19)**: `breakPiece(id)` → colapso en cascada de lo no soportado;
  `setProtected` = zona irrompible + ancla del mundo. Cliente: tecla **X** rompe la pieza apuntada. 23/23 tests.
  PENDIENTE: rotura por FUERZA (Jolt, cuando la carga supera `breakThreshold` → constraints rompibles / islas
  rígidas). Ver [[jolt-fisica]] Fase 4 y `PLAN_CONSTRUCCION.md`. Hoy el disparador es manual; falta el físico.
- **Prefabs (colocación por lote) HECHO (2026-07-19)**: core `validateBatch`/`addBatch`; cliente tecla **Z**
  coloca un suelo 3×3 de la pieza seleccionada de una. 29/29 tests. Hoy 1 SceneObject por parte.
- **Instancing de piezas — IMPLEMENTADO (2026-07-19), FALTA VERIFICAR EN JUEGO.** Pase instanciado: los
  SceneObjects con prop `construction` se acumulan por modelo `.glb` y se dibujan con `GPUInstancing` (1 draw
  por modelo) en el MISMO render pass/depth que la escena. Shaders nuevos `construction_inst.vert/.frag`
  (fork de simple.vert/final.frag: model+color por instancia, misma luz y UBO per-frame). PSO `m_constInstPSO`
  (binding0=Vertex pos/normal, binding1=instancias vía `appendInstanceLayout`). `Model::drawInstancedRHI`.
  ⚠️ NO verificable headless → probar en juego (ver notas). Riesgos: offset de posición (origin-shift),
  luz distinta, o el fantasma de preview (también lleva prop construction → se instancia, con su tinte).

### 3. SISTEMA DE CLIMA — CERRADO A→E (2026-07-25/26). Falta VALIDARLO EN JUEGO

**El bug que reportó el autor** ("la lluvia sale donde sea, no es lluvia que cae de las nubes") tenía
DOS causas independientes, y las dos están arregladas:
1. **La lluvia y las nubes eran dos relojes distintos.** La CPU decidía la lluvia con
   `0.5+0.5·sin(t·0.05)` y `sky.frag` decidía SUS nubes con `fbm(t·0.008)`. No hay motivo por el que
   dos fórmulas así coincidan, y no coincidían: llovía con cielo despejado y salían nubarrones secos.
2. **La lluvia era un filtro de pantalla**, no lluvia. `rain.vert/frag` pintaba rayas sobre la imagen
   ya compuesta: sin profundidad ni posición → caía dentro de las cuevas, tras los muros y bajo los
   tejados.

- **✅ FASE A — el clima lo posee el MUNDO** (`core/weather_system.{h,cpp}`, GL-free, en
  `haruka_simbase` como la superficie de referencia → el DGS puede evaluarlo). 14 frentes recorren el
  planeta (cada uno gira sobre su propio eje, radios 0.25–0.72 rad); la cobertura en un punto es la
  suma de los que lo tapan, modulada por la humedad del CAMPO. **Función pura de (seed, tiempo,
  dirección)**: sin estado acumulado → dos clientes y el servidor ven la misma tormenta compartiendo
  solo el reloj (`PlanetarySystem::simulationTime()`, el mismo que las órbitas de Kepler).
  - **La precipitación SALE de la cobertura** (`smoothstep(kPrecipCover=0.62, 0.94, cover)`): sin nube
    encima da exactamente 0. Es un invariante del sistema, no un ajuste, y tiene test.
  - Lluvia o NIEVE según la temperatura del campo (`kSnowTempC = 1 °C`).
  - Consola: `weather [escala]` (estado aquí + acelerar los frentes) · `rain <0-1|auto>` sigue valiendo,
    y ahora el override sube TAMBIÉN la nube que lo justifica (si no, reintroducía el bug a mano).
- **✅ FASE B — la precipitación existe en el MUNDO** (`renderer/precipitation_renderer.{h,cpp}` +
  `precip.vert/frag`). Gotas y copos son GEOMETRÍA dentro del pase de escena, con el depth del mundo:
  lo que está delante las tapa. Sin buffers ni trabajo de CPU por frame — la posición de cada gota sale
  de un hash de `gl_VertexID`, y la rejilla se ENVUELVE alrededor del observador anclada al mundo (el
  módulo se hace en DOUBLE en CPU: a 6.4e6 m un float no tiene esa fracción y la lluvia se quedaría
  pegada al jugador). El pase fullscreen de lluvia se ha ELIMINADO.
  - La NIEVE es la misma precipitación con otra forma/velocidad/deriva (copo lento que el viento
    arrastra ×0.9; la gota, ×0.35).
  - ⚠️ **Ancho mínimo de ~1.4 px**: una gota real es sub-píxel y NO se rasteriza (medido: 0 píxeles).
    El tamaño en mundo se sube al que ocupa ~1.4 px a esa distancia → deja de depender de la resolución.
  - ⚠️ Dos trampas que costaron el primer intento, ambas comentadas en el código: el UBO per-frame
    declara **`view` ANTES que `projection`** (al revés compila pero no dibuja nada), y
    `glClear(GL_DEPTH_BUFFER_BIT)` **obedece a `glDepthMask`** (el PSO lo deja en false → el clear del
    test no escribía y medía dos veces el mismo caso).
- **✅ VIENTO ÚNICO**: un solo vector del clima mueve las nubes (`sky.frag`), inclina la precipitación
  y ondea el follaje (`prop.vert`, que además ondea HACIA el viento en vez de en ejes fijos).
- **✅ Fuera la nieve por TEMPERATURA del terreno** (`planet.frag`): el suelo no cambia de material
  porque haga frío. La nieve pasa a ser una CAPA que se acumula encima (fase D).
- **Tests**: `./haruka_tests weather` (frentes deterministas + "sin nube no llueve" + los frentes se
  mueven + otra seed = otro clima) · `./haruka_tests precip` (PÍXELES: la lluvia se dibuja Y la geometría
  delante la oculta — la propiedad que el pase de pantalla no podía tener).

- **✅ FASE C — MÁSCARA DE EXPOSICIÓN AL CIELO** (pase ortográfico CENITAL, ±48 m a 1024² = 9.4 cm/téxel).
  Es el shadow map con la "luz" en el cénit: dice qué tienes ENCIMA. Reusa el hook `onRenderShadow` del
  juego (props, edificios, piezas), así que no hay casters nuevos que registrar, y solo se dibuja cuando
  precipita o el suelo sigue mojado. **No es el shadow map del sol**: la sombra viaja durante el día, la
  zona cubierta no — por eso son dos máscaras y no una.
  - **No llueve bajo cubierto**, y se decide POR GOTA (`precip.vert` la muestrea): en el borde de un
    alero llueve fuera y no dentro, en la misma imagen. Un interruptor global no puede hacer eso.
  - **El suelo se moja con silueta SECA bajo cualquier collider** (`planet.frag`): oscurecer + saturar
    (el 80 % del efecto), especular + fresnel del agua, y CHARCOS donde el agua puede quedarse (llano +
    `flow` del campo hidrológico). PCF 3×3 en la máscara → el borde de lo seco es difuso, no un recorte.
  - **Salpicaduras** (anillos que se abren) solo mientras CAE, solo a cielo abierto y solo cerca (a 40 m
    un anillo de 20 cm es sub-píxel y solo daría aliasing).
  - **El mojado se INTEGRA** (`Application::m_groundWetness`): ~30 s de chaparrón para empaparse, ~4 min
    para secarse. Por eso el suelo sigue oscuro y brillante un rato después de escampar, en vez de
    apagarse con la última gota. Cuando exista `GroundLayer` será un canal suyo.
  - Fuera del alcance de la máscara se asume EXPUESTO (el mundo es cielo abierto; el error se va a donde
    no se mira de cerca).
  - ⚠️ Trampa nueva: el bloque UBO debe declararse **IDÉNTICO** en vértice y fragmento — añadirlo solo en
    uno da "definitions of uniform block do not match" **al LINKAR**, no al compilar, y el pase queda mudo.
  - **Test**: dentro de `./haruka_tests drawn` — seco 79.1 → mojado 48.6 → mojado BAJO CUBIERTO 64.1
    (vuelve hacia seco). Y en `precip`: 0 px bajo techo · 13367 px a cielo abierto.
- **✅ FASE D — LA CAPA GRANULAR con MATERIA + HUELLAS** (`core/ground_layer.{h,cpp}` GL-free +
  `renderer/ground_stamp_renderer` + `ground_stamp.vert/frag`).
  - **La nieve es una CAPA, no un bioma**: se acumula ENCIMA de lo que haya (tierra, hierba, roca) y
    cuaja según (a) la máscara CENITAL —bajo el alero no nieva, igual que no llueve—, (b) la PENDIENTE
    (en la pared resbala) y (c) un ruido de borde para que la línea no sea un recorte. `m_snowAccum`
    se integra: cuaja nevando y **FUNDE con la temperatura** — no es "hace frío ⇒ suelo blanco".
  - **La MATERIA gobierna el comportamiento**, y es la feature: nieve (huella profunda y persistente,
    te hundes 14 cm y andas al 62 %), arena (huella somera que se DESMORONA en ~35 s), barro (queda y
    brilla), ceniza. Vida, curva de desvanecido, hundimiento y agarre salen del material.
  - **Las huellas son una LISTA acotada de pisadas**, no una retícula persistente: cada frame se
    re-proyectan en una ventana cenital de ±24 m. Eso elimina de raíz el scroll toroidal (desplazar el
    contenido de la textura al moverse) y hace que re-centrar sea gratis. Tope de 384 pisadas ≈ 290 m
    de rastro: es un rastro, no un historial.
  - **La huella se ve por su SOMBRA, no por su color**: sobre nieve blanca no hay contraste de albedo.
    El pase escribe *hundido* (R) y *reborde levantado* (G) — el material desplazado tiene que ir a
    algún sitio — y `planet.frag` perturba la NORMAL con su gradiente. Blend ADITIVO: pisar dos veces
    el mismo sitio hunde más (con alfa, la pisada nueva sustituiría a la vieja).
  - **Se NOTA al andar**: `Character::getGroundSpeedFactor/getGroundSink`. El factor se aplica dentro
    de `getSpeed()` para que lo respeten todos los caminos (andar/correr/agachado) sin poder olvidarse.
    El jugador estampa **por ZANCADA** (0.85 m), no por frame — si no, sale una banda continua.
  - ⚠️ **NO va sobre `DeformationField`**, y la nota vieja que decía lo contrario era un error caro:
    sus brochas invalidan y regeneran chunks (rebuild por pisada) y moverían la SUPERFICIE DE
    REFERENCIA, el invariante cerrado a 6 mm. La capa va POR ENCIMA: albedo+normal en el shader y un
    ESCALAR de hundimiento para el controlador. Hundirse 8 cm no exige que el suelo baje 8 cm.
  - **Test**: `./haruka_tests huellas` — la materia manda (a media vida: nieve 0.88 vs arena 0.25),
    cola acotada, la pisada marca dentro de su radio y NO fuera, `update()` retira lo caducado.

  - **ARENA (2026-07-25)**: la segunda capa, y NO depende del clima del día — el desierto es de arena
    llueva o no. Sale de la ARIDEZ del campo (`humidity`, mismos umbrales 0.08–0.26 en shader y CPU) y
    de la ORILLA, ambas limitadas por la pendiente (en la pared no se sostiene un manto suelto). La
    nieve, si la hay, manda encima. `PlanetarySystem::groundCoverAt()` es la versión CPU de lo que
    `planet.frag` calcula por píxel: **tienen que decidir lo mismo**, o dejarías huella donde no se ve
    capa. La huella en arena es AL REVÉS que en nieve: el hueco saca material húmedo de debajo (más
    OSCURO) y el reborde es arena seca removida (más CLARO); y el manto suelto tiene rizos de viento
    que la pisada BORRA — por eso una huella en duna se lee tan bien.
  - ⚠️ **Bug que cazó el test (2026-07-25)**: `update()` tiraba huellas caducadas SOLO por el frente
    de la cola, asumiendo que caducan en orden de llegada. Con dos materiales es FALSO (arena 35 s vs
    nieve 180 s): al cruzar de la duna a la nieve, las de arena muertas quedaban atrapadas detrás de
    una de nieve viva, seguían proyectándose cada frame y ocupaban sitio en la cola acotada.

**Lo que queda:**
1. **VALIDARLO EN JUEGO** — nada de esto se ha visto jugando; qué mirar, en §0. Mandos: `weather
   [escala]` (estado aquí + acelerar los frentes) y `rain <0-1|auto>`.
2. **Los PROPS no se mojan ni se cubren de nieve** (solo el terreno): misma `u_skyMask`, falta
   cablearla en `prop.frag`. Es lo que más se va a notar tras una lluvia — un árbol seco en un bosque
   empapado canta.
3. **Que el clima se OIGA** (lluvia/viento por bioma; la propagación de sonido ya existe) y que afecte
   a la jugabilidad más allá de la capa (visibilidad, agarre, temperatura del jugador).
4. **Frentes en el HUD/minimapa y sincronía con el DGS** (el estado ya es replicable: seed + reloj).

### 4. SPELLCASTING + IDIOMA ANTIGUO — gramática, manifestación y LENGUA hechas (2026-07-25/26)

**Decisiones del autor (2026-07-25)**: la gramática llega a **raíz + MODIFICADOR + forma** (sin
objetivo ni condicionales, de momento) y la canalización se queda **instantánea** (tier por ruedita).

- **✅ LA GRAMÁTICA** (`assets/data/magic/language/modifiers.json` + `SpellData::modifierByPrefix` + el parser de
  `SpellSystem::resolve`). Un modificador es un INFIJO entre la raíz y el sufijo: `kaer·ul·ith` =
  saeta de fuego INTENSA. Se encadenan (hasta 4) y sus efectos se ACUMULAN por producto.
  - 3 ejes, uno por pregunta que el jugador se hace: **intensidad** (menor/mayor/supremo), **alcance**
    (cerca/lejos), **duración** (breve/duradero) y **área** (amplio/preciso). 9 infijos.
  - **Regla de diseño: si un infijo no cambia un NÚMERO, no debe existir.** Nada decorativo. Por eso
    `ResolvedCast` gana `rangeMult` y `durationMult`, ejes que antes no tenían dónde vivir.
  - **11 elementos × 19 formas × (9+1) = 2090 hechizos** con un solo infijo, y se encadenan.
  - **Lectura inequívoca**: gana el infijo MÁS LARGO (`ulrak` = supremo, no `ul` + basura). Sin eso,
    un hechizo supremo saldría torcido en silencio.
  - **La GLOSA** (`ResolvedCast::gloss` = "fire · mayor · lejos · bola") se muestra al lanzar: un
    idioma que no te enseña cómo te ha entendido no se puede aprender jugando.
  - ⚠️ **La precisión se mide contra lo NO reconocido**, no contra el largo de la palabra: si no,
    encadenar modificadores (palabras legítimamente largas) penalizaría igual que hablar mal — la
    gramática castigaría usarla. Tiene test.
  - El **vocabulario** crece solo: 2101 palabras (raíz, raíz+forma y raíz+UN modificador+forma). Se
    para en un modificador a propósito: con dos pasaría a ~17.000, y eso solo tenía sentido cuando
    alimentaba a un reconocedor de voz (ahora retirado, ver abajo). Hoy sirve de glosario.
- **✅ LA MANIFESTACIÓN** (`abilities/spell_manifest.{h,cpp}`). Había 19 formas y **solo se
  manifestaba una**: todo se mapeaba a `Motion{Saeta,Lanza,Bola,Serpenteante}` y salía un proyectil,
  así que un MURO de fuego volaba como una flecha y el domo, la niebla, el meteorito, el orbital, la
  cadena y el parpadeo hacían exactamente lo mismo. Ahora el `kind` de la forma decide QUÉ aparece:
  - **instantáneas**: proyectil (con el abanico de la ley L3) · multi (enjambre) · cono (ráfaga) ·
    caida (meteorito que nace ARRIBA y cae) · area (golpe inmediato con caída por distancia) ·
    cadena (salta perdiendo fuerza) · utilidad (parpadeo = blink real).
  - **persistentes** (`SpellField`): estatico (muro) · zona (niebla/domo) · orbita · **vortice**. Son los que hacen
    que el eje de DURACIÓN signifique algo — sin campos que duren, "duradero" no multiplica nada.
    Se dibujan con PARTÍCULAS (encaja con el toon, cuesta poco, no hay que inventar geometría).
  - `strike()` está factorizado: el área y la cadena pegan EXACTAMENTE como el proyectil (daño por
    zonas + effects + reacciones). Si no, el mismo elemento reaccionaría distinto según la forma.
  - El pulso de daño de los campos va a 4 Hz, no por frame: atarlo al framerate haría que pegara el
    triple a 180 fps que a 60.
  - **TORNADO DE VERDAD (2026-07-26)**: `torbellino`/`tornado` eran `kind:"area"` = un golpe
    instantáneo con nombre bonito. Ahora `vortice`: **persiste, AVANZA y ARRASTRA** hacia su eje con
    componente hacia ARRIBA — lo que define un tornado no es que empuje, es que te LEVANTA mientras
    te traga. El arrastre va por FRAME (no en el pulso de 4 Hz): una succión a tirones se sentiría
    como empujones. Se dibuja como columna en espiral que se estrecha abajo, no como un anillo.
- **✅ Tests**: `Survival/tests/test_language.cpp` → `./build/bin/survival_tests assets/data/magic/language/`
  (**53 OK**; `ctest` lo lanza solo). Cubre: los infijos mueven los números · se encadenan (producto, no "el último gana") ·
  el más largo gana · decir de más tuerce pero no falla · encadenar NO penaliza · la glosa · el
  vocabulario · **que TODA forma de `forms.json` tenga manifestación** (`spell_kinds.h`) — el fallo
  silencioso de añadir un `kind` nuevo al dato y que caiga al proyectil por defecto — y la LENGUA
  (frase completa, caso, tiempo, imperativo, negación, pregunta, y que el elemento sea la MISMA
  palabra al hablar que al conjurar; si eso falla, hay dos idiomas y el lore se cae).
- **✅ EL IDIOMA ES UNA LENGUA, no un teclado (2026-07-26, petición del autor)**
  (`abilities/language.{h,cpp}` + `magic/language/{lexicon,grammar}.json`). Hasta aquí solo servía
  para dar órdenes al mundo: con eso no se lee un grimorio, ni una lápida, ni habla un NPC.
  - **LA IDEA QUE LO SOSTIENE: las piezas se comparten.** Las raíces de elemento SON los sustantivos
    (`kaer` = fuego, se conjure o no) y los modificadores SON los adjetivos (`ul` = grande). Se leen
    de `SpellData`, **no se copian**: añadir un elemento da un sustantivo el mismo día. Duplicarlos
    sería garantizar que un día digan cosas distintas.
  - **Consecuencia de lore, hecha código**: conjurar es hablar la lengua en IMPERATIVO con el verbo
    ELIDIDO. Por eso `kaeruloro` no lleva caso ni tiempo: no es una frase *sobre* el fuego, es una
    orden *al* fuego. `ParsedPhrase::isIncantation` distingue las dos cosas.
  - Aglutinante y **SOV**: casos por sufijo (`-ka` sujeto · `-ta` objeto · `-ni` a/para · `-sa` en ·
    `-mo` de · `-du` desde), tiempo en el verbo (`-a` presente · `-o` pasado · `-e` futuro · `-u`
    IMPERATIVO · `-ai` condicional), `nu-` niega, `-ru` pregunta (al final, coherente con el verbo al
    final), `-or` plural. 81 palabras. Consola: `speak <frase>` glosa y traduce · `speak words`.
  - ⚠️ La misma sílaba puede ser caso o tiempo (`-a`): desambigua la CLASE de la raíz — por eso el
    léxico guarda clase y no solo significado.
- **✅ DATOS REORGANIZADOS (2026-07-26)**: todo lo mágico bajo `assets/data/magic/` — `language/`
  (elements · forms · modifiers · lexicon · grammar) · `effects/` (effects · reactions) · `items/`
  (foci · magic_cores · grimoires · runes · seals · motions) · `abilities.json`. Estaban los diez
  sueltos en `data/` junto a recetas y monstruos.
- **✅ Dato muerto**: `SpellDef`/`m_spells` ya no existen (solo queda la nota histórica en un comentario).

**Lo que queda:**
1. **Validar EN JUEGO** (yo no ejecuto el juego): que el muro se lea como muro, que el meteorito caiga
   donde apuntas, que el parpadeo no te meta en el suelo. Consola: `spell <incantacion>` muestra la
   glosa y todos los números; `spell list` lista raíces, infijos y sufijos.
2. **El jugador no es un `Combatant`** (solo lo son los monstruos): por eso `caster` llega null y los
   hechizos no pueden excluirle ni curarle. Está explícito en `SpellManifest::Params`; el día que se
   registre, empieza a funcionar solo.
3. **`bolsillo`** (utilidad de almacenaje) necesita el sistema de inventario espacial. Avisa en vez de
   fingir que funciona.
4. **Forma ESCRITA**: runas/grimorios como objetos del mundo (`runes_creation`, `spell_books`).
5. **VOZ RETIRADA (2026-07-26, decisión del autor)**: fuera el reconocedor propio (Vosk,
   `abilities/voice_input.*`, la opción `VOICE` de CMake y `assets/voice/`). Motivo del autor: el STT
   del juego ya hace ese trabajo y el chat de voz por proximidad no necesita una gramática de
   conjuros. La incantación se **ESCRIBE**: `CastPhase::Listening` → `Typing`, y el cuadro muestra la
   **GLOSA EN VIVO** mientras tecleas (elemento · modificador · forma) + aviso si la incantación es
   imperfecta — que enseña el idioma mejor de lo que lo hacía el dictado. `SpellSystem::vocabulary()`
   y `LanguageSystem::vocabulary()` siguen generando las palabras por si vuelve un reconocedor.
6. **El DGS tendrá que validar un hechizo como VERBO** — ahora que el flujo está cerrado, ya se puede
   diseñar esa validación sin que cambie debajo. Ver [[dgs-generico-opaco]].

### 5. Vida: cuerpos procedurales + audio
- Monstruos con generación de cuerpo (esqueleto→miembros, IK); el BIOMA decide qué aparece. En testing.
- Audio ambiental por bioma; la propagación (sonido rodea paredes) ya existe.

### 6. Jugabilidad
- Interacción física, mínima UI (grimorios = objetos del mundo). HUD: chat, grupo, inventario.
- Progresión — solo cuando existan materiales/recursos de verdad.


---

---

## 🟢 TERRENO — EL 1 CM CUMPLIDO (2026-07-26). Y el "desaparece a veces" era AGUA FALSA

`render_collision_parity` dentro del keepRadius: **0.0001 m de media · 0.0003 m el peor · 0/200 fuera
de 1 cm** (venía de 0.0060 / 0.0177 / 34). Suite **19438 OK · 0 FALLOS** en frío y en caliente, y
también en una build **Release** (que es donde podía romperse en silencio, ver abajo). Detalle
completo y las trampas en memoria [[terreno-v3]].

**1. "El terreno desaparece a veces" = lagos-sello LEGACY que solo existían en la GPU.**
`terrain_gen.comp` conservaba de antes de F3 una rama de lagos por voronoi+hash que `sampleTerrainV2`
ya no tiene. Tallaba 60 m de cuenca solo en la malla y —lo grave— fijaba el nivel del lago en
`regionalElevKm` (una función del ruido de continentalidad, sin relación con la altura real) hasta
**1 km**, y para TODA la celda del hash (**~53 km de lado**). Con el terreno a 0.4 km eso es una lámina
de agua plana **600 m por encima del suelo**: el terreno desaparece debajo. Solo se emitía en chunks
finos → aparecía y desaparecía al refinar. Ahora el agua sale del CAMPO, igual que en la CPU.
⚠️ **Ningún test miraba el AGUA** — toda la escalera medía alturas. Nuevo guardián en `cpu_gpu_parity`
(hoy 0.0000 m), verificado no vacuo.

**2. El residuo de 6 mm NO era "ruido de float32 repartido"** (esa conclusión anterior era falsa): el
`sqrt`/`inversesqrt` de **double** del driver (Mesa/ACO) es ~**1e-8**, no IEEE, y ese desvío de la
dirección se multiplica por la escala del ruido. Arreglado con Newton sobre el recíproco
(`rsqrtExact`). ⚠️ **No rematar con Herón**: la división de double también es aproximada y empeora 50×.
**Regla: en el camino que casa con la CPU, en double solo `*`, `+` y `−`; raíces y divisiones, con
Newton.**

**3. `-ffast-math` era una mina.** Release compila con `-O3 -march=native -flto -ffast-math`, que
autoriza reasociar y fundir en FMA → la paridad se rompería **solo en Release**, con los tests (sin
`BUILD_TYPE`) en verde. Los ficheros del camino de altura llevan ya `-fno-fast-math -ffp-contract=off`.

**Lo que queda del terreno** (nada de esto bloquea): el meso sigue OFF por defecto — ya no mueve el
suelo (`giro` en frío 0.000 m) y ahora tampoco rompe el 1 cm, así que la decisión es solo si su detalle
compensa su coste. Y fuera del keepRadius el error sigue siendo de decímetros a metros **a propósito**
(ahí el LOD adelgaza); el 1 cm es el requisito donde se juega.

## ⏸️ (histórico) TERRENO — PAUSADO AQUÍ (2026-07-25, decisión del autor). Estado y qué queda

Detalle completo en memoria [[terreno-v3]]. Resumen para retomar sin re-derivar nada:

**El suelo es UNO solo y ya no se mueve.** La superficie de referencia (`reference_surface.{h,cpp}`)
sustituyó a los TRES suelos que había. `giro` con meso ON **en frío** pasó de 118.889 m a **0.000 m**
(props 150.524 → 0.000). El meso ya NO mueve el terreno.

**⚠️ Dos premisas de este archivo eran FALSAS y están corregidas:**
- **La "pirámide de mips" NO hace falta.** Se derivaba de "la colisión lee la malla dibujada", y eso ya
  no es cierto. Además, contra el analítico PUNTUAL el 1 cm es inalcanzable con pirámide o sin ella
  (medido: 7.81 m de pico — una malla lineal a 23 m/vértice no reproduce detalle de 40 m/téxel). Lo que
  hacía falta era que ambos lados leyeran **la misma retícula**.
- **Los 42.783 m del geomorph eran CACHÉ CALIENTE.** En frío daba 118.889 m. **Medir el terreno con
  `HARUKA_DISKCACHE=0`**, que es el caso del jugador que estrena mundo.

**Lo único que falta para el 1 cm**: `render_collision_parity` da **0.0060 m de media y 0.0177 m el
peor** dentro del radio de juego (venía de 0.5989 / 5.09). El test queda EN ROJO a propósito: es el
requisito del autor, no un umbral cómodo.
- Ya no depende de LOD, cámara, streaming ni meso. Es **ruido de float32 repartido** entre términos
  (peor, en m: montañas 0.131 · midRelief 0.052 · campo 0.048 · lecho marino 0.047), cada uno de unos
  pocos ulps sobre su amplitud = el suelo de evaluar la misma fórmula en dos procesadores.
- Bajar de ahí = llevar más de la cadena de términos a **double en los dos lados**, y eso hay que
  MEDIRLO en `terrain.draw` antes de comprometerse (fp64 en GPU va a ~1/32 de rate). La alternativa
  razonable es aceptar ~2 cm de pico con 6 mm de media.
- **Más visible que eso: los outliers de `parity` (max 37.5 m, 3/4096).** Están en las RAMAS de después
  de la suma (recorte de costa `landMask>0.5` y lagos), no en los términos. Ir a por esto primero.

**Herramienta que dejó la sesión, y su lección**: tres hipótesis razonadas a ojo salieron NEUTRAS
seguidas. Lo que resolvió fue instrumentar: `u_dbgTerm` (shader) + `terrainDebugTerms()` (CPU) →
`./haruka_tests parity` compara **término a término**. Así se cazó que el SSBO del campo se
RE-MUESTREABA en vez de copiarse (0.58 m de pico él solo).

**Si se retoma el MESO**: ya no está bloqueado por estabilidad del suelo. El criterio de aceptación
sigue escrito en `mesoEnabled()`: `HARUKA_MESO=1 HARUKA_DISKCACHE=0 ./haruka_tests giro` ≤ 0.01 m
(hoy: 0.000 m). Lo que falta es decidir si su detalle aporta lo suficiente frente a su coste.

## 🧹 Cosmético / ⏸️ Aparcado

- **Namespaces**: clases a mover a sub-namespaces por carpeta. File-by-file.
- **Código muerto**: `MeshLOD`, `SoftbodyRenderer` sin call-site.
- **Cuevas** e **islas flotantes**: aparcadas hasta que la superficie sea creíble (prerrequisito ya hecho).
- **MESO**: desbloqueado (ya no mueve el suelo) pero SIN encender — decisión pendiente de si su detalle
  compensa su coste. Ver §"TERRENO — PAUSADO AQUÍ".
