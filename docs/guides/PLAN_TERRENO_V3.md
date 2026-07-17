# PLAN TERRENO V3 — rehacer terreno, props e islas para que sean CREÍBLES

Estado: **DISEÑO CERRADO** (2026-07-13). Sustituye al enfoque actual (suma de fBm) — no es una
mejora incremental.

## Decisiones tomadas por el usuario (2026-07-13)
1. **Cuevas: APARCADAS.** Fuera de esta fase (el diseño volumétrico queda escrito en §4 para cuando
   se retomen; la tectónica y el freático que generamos ahora son justo sus prerrequisitos).
2. **Erosión: SÍ, con HEIGHTFIELD CACHEADO como fuente de verdad.** La altura deja de ser una
   fórmula evaluable en cualquier punto y pasa a ser un CAMPO que se genera, se erosiona y se
   cachea; física, agua y props lo MUESTREAN. Esto elimina la triplicación de raíz.
3. **Alcance: la GENERACIÓN ENTERA.** Cómo se genera el terreno, la erosión, y **dónde se generan
   los props** (hoy: hash sobre una rejilla y el tipo por banda de altitud → pasa a estar dirigido
   por los campos: bioma, pendiente, humedad, caudal).

---

## 0. El principio

> **La credibilidad no sale del ruido, sale del PROCESO.**

Un paisaje real es la salida de una cadena causal: tectónica → levantamiento → erosión →
sedimentación → hidrología → clima → biomas. Cada capa EXPLICA la siguiente.

Hoy generamos con una suma de fBm (`terran_ElevKm`: continentes + colinas + montañas ridged +
lagos por seed). Eso produce, por construcción:

- **Montañas sin cordillera**: `mtnMask` es una mancha de ruido → picos dispersos, no una cadena
  con vertiente, valle y sombra de lluvia.
- **Laderas onduladas y uniformes**: el fBm suave es diferenciable en todas partes; no hay valles
  en V ni paredes escarpadas porque nada las talló.
- **Costa = contorno de ruido**: no hay deltas, ni acantilados, ni playas donde deposita.
- **Lagos falsos**: "sellos" colocados por hash, no mínimos de una cuenca real.
- **Ríos: no existen.**

Ningún cambio de color arregla eso. Hay que cambiar **qué genera la altura**.

---

## 1. Lo que el rediseño DEBE resolver (deuda estructural medida)

### 1.1 La elevación está TRIPLICADA — y la erosión hace esto insostenible
La misma fórmula vive en tres sitios que deben coincidir *exactamente*:

| Sitio | Para qué |
|---|---|
| `assets/shaders/terrain_gen.comp` | generar la malla del chunk (GPU) |
| `src/core/terrain/terrain_sampler_v2.cpp` | física / colisión / spawn (CPU) |
| `assets/shaders/water.frag` (`oceanElevKm`) | **recortar la costa per-píxel** (copiada a mano, "PORT EXACTO") |

El agua re-deriva el terreno en cada píxel para saber dónde hay mar. **El bug actual del agua
inundando la tierra vive justo ahí.**

⚠️ **Y esto es lo que bloquea la erosión**: un terreno erosionado ya NO es una función cerrada
`elev(dir)` evaluable en cualquier punto — es el resultado de un proceso iterativo con memoria.
No se puede re-evaluar en el fragment shader del agua ni en el sampler de física.

→ **La fuente de verdad debe pasar de "fórmula" a "CAMPOS PRECOMPUTADOS que todos muestrean".**
Eso elimina la triplicación de raíz, y es requisito de todo lo demás.

### 1.2 El z-buffer es inservible (medido)
`glm::perspective(fov, aspect, 0.1, 3e11)` → **todo el terreno cae entre 0.99996 y 1.0**.
Consecuencias reales, ya visibles:
- El agua no se puede ocluir por profundidad → *por eso* necesita el recorte analítico.
- Z-fighting: puntitos de terreno asomando entre el agua.

→ **Reversed-Z + far infinito.** Ahora es barato: los PSO ya centralizan el estado de depth.
Con depth real, el terreno opaco ocluye el mar SOLO, y el agua deja de re-derivar nada.

### 1.3 El cuello es el CONTEO DE CHUNKS (CPU), no la GPU
Ya medido: `lod.stream` 7.5 ms · `lod.desired` 5.6 · `terrain.evict` 4.5-6 · `drawset` 2.2 vs
**submission GL real 0.7-0.9 ms**, con la GPU ociosa ~42 ms.

→ La palanca de perf y la de look son **la misma**: geometría solo para la silueta, el depth, las
sombras y la colisión; **todo el detalle, per-píxel**. Malla más gruesa = menos chunks = arregla
el frame Y el look a la vez.

---

## 2. La nueva arquitectura de generación

Tres niveles, de global a local. La clave: **los niveles superiores se PRECOMPUTAN una vez por
planeta y se cachean como texturas; nadie re-deriva fórmulas.**

### Nivel A — GEOLOGÍA (global, una vez por planeta)
Resolución: cubemap ~6×2048² (barato, cacheable en disco con la seed).

1. **Placas tectónicas**: Voronoi esférico (~20-40 placas) + un vector de movimiento por placa.
2. **Límites** → el tipo de borde determina el relieve, y esto es lo que da **cordilleras LINEALES**
   en vez de manchas de ruido:
   - *Convergente* continental → cordillera (Himalaya). Oceánico → **fosa + arco volcánico**.
   - *Divergente* → dorsal oceánica; en continente, **valle de rift**.
   - *Transformante* → fallas, desplazamientos laterales.
3. **Uplift map** + grosor de corteza → elevación base. El continente/océano deja de ser un umbral
   de ruido: es **corteza continental vs oceánica**, que es lo que de verdad separa tierra y mar.

> Esto es EL cambio que separa "ruido" de "planeta". Todo lo demás cuelga de aquí.

### Nivel B — EROSIÓN E HIDROLOGÍA (global, sobre el mapa de A)
Sobre el mismo cubemap, iterativo en GPU (compute). Es lo que da credibilidad de verdad:

1. **Erosión hidráulica** (stream power): talla **valles en V**, gargantas, y genera
   **redes de drenaje** dendríticas. No hay atajo: es lo que el ojo reconoce como "montaña real".
2. **Flow accumulation** → **RÍOS reales**: el agua sabe dónde va, y los ríos nacen arriba y
   desembocan en el mar. Hoy no hay ríos.
3. **Lagos = priority-flood** (rellenar mínimos de la cuenca), no sellos por hash. Un lago queda
   donde la cuenca REALMENTE no drena.
4. **Erosión térmica**: ángulo de reposo → taludes y pedreras al pie de las paredes.
5. **Deposición**: llanuras aluviales, **deltas** en la desembocadura, abanicos.

Salida (los CAMPOS que todo el motor muestrea):
`elevación · flujo(caudal) · sedimento · pendiente · nivel_agua`

### ⚠️ El problema de ESCALA (y por qué hacen falta DOS niveles de erosión)
Un cubemap de 6×2048² sobre la Tierra da **~5 km por téxel**. Eso basta para cordilleras, cuencas y
grandes ríos — pero NO para el valle que ves al caminar. Y erosionar el planeta entero a 10 m/téxel
son ~10¹² téxeles: imposible.

→ **Erosión en DOS niveles:**

- **MACRO (global, una vez por planeta)**: cubemap 6×2048² (~5 km/téxel). Tectónica + erosión
  hidráulica continental. Salida: cordilleras, **cuencas de drenaje**, grandes ríos, costa real.
  Se cachea en disco con la seed. Coste: minutos una vez, no por sesión.

- **MESO (por TESELA, bajo demanda, cacheada)**: la zona alrededor del jugador se refina a
  **~10-30 m/téxel** en teselas (p.ej. una tesela = un nodo de quadtree de LOD medio). Cada tesela:
  1. **Upsamplea** el macro (elevación + caudal entrante).
  2. Añade detalle (ridged + warp) condicionado por los campos.
  3. **Erosiona localmente** (hidráulica + térmica) con **condición de contorno**: el caudal que
     ENTRA por cada borde lo dicta el campo de flujo MACRO → el río grande sigue cruzando la tesela
     y los valles CASAN entre teselas vecinas. (Esta es la parte delicada: sin heredar el flujo, las
     teselas erosionan incoherentes y se ven las costuras.)
  4. Se **cachea** (disco + RAM, igual que ya se cachean los chunks).

**El heightfield MESO cacheado es LA FUENTE DE VERDAD.** El chunk se malla muestreándolo; la física
lo muestrea; los props se colocan sobre él. Nadie re-evalúa una fórmula.

### La consecuencia arquitectónica más gorda: el sampler de CPU
Hoy `sampleTerrainV2(dir)` responde en cualquier punto al instante (es una fórmula). Con el campo
cacheado, responder exige que **la tesela esté residente**. Impacto en física, colisión y spawn:
- Muestreo **bilineal del heightfield** de la tesela → altura y normal exactas y COHERENTES con lo
  que se dibuja (hoy pueden discrepar).
- Si la tesela no está: **fallback al macro** (upsampleado) — bueno para "¿estoy sobre tierra?" o
  colocar algo lejano, malo para colisión fina → la colisión exige tesela residente (ya se exige
  chunk residente hoy, así que el modelo encaja).
- Las teselas se piden con el mismo streaming que ya existe.

### Nivel C — DETALLE DE CHUNK (local, al generar el chunk)
El ruido sigue existiendo, pero **condicionado por los campos de B**, no libre:
- Amplitud y tipo de ruido según pendiente / caudal / sedimento (roca fracturada arriba, guijarros
  en el cauce, arena en el delta).
- **Ridged multifractal + domain warping** solo donde la geología dice roca desnuda.
- Detalle < 8 m: **NO va en la geometría** (aliasea las normales) → normal map per-píxel, como ya
  se hace.

### Clima DERIVADO (no inventado)
- Temperatura = f(latitud, altitud).
- Humedad = advección desde el mar + **sombra orográfica** (barlovento/sotavento) usando las
  cordilleras REALES de A. → **desiertos detrás de las montañas**: ese es el detalle que hace decir
  "esto es un planeta".
- **Biomas = f(temperatura, humedad, pendiente, caudal)** → tabla tipo Whittaker.
  Hoy `biomeColor()` mezcla colores por altura: eso se tira.

---

## 3. Materiales (el look que pediste)

- **Splatmapping con height-based blending** (comparar los height maps de las capas, no un `mix()`
  lineal) → la hierba crece ENTRE los guijarros. Es literalmente "tierra BAJO hierba".
- Roca donde la pendiente supera el ángulo de reposo (dato que ya da la erosión térmica).
- Sedimento/arena donde el campo de deposición lo dice (no "arena si altura < X").
- Nieve por temperatura real, con acumulación menor en pendiente fuerte.
- **Triplanar estable en espacio MUNDO** — hoy usa `FragPos` (relativo a cámara) → "swim" al moverse.
- **Quitar el especular** de `planet.frag` (`pow(dot(N,H),32) * 0.12`, ~líneas 222 y 247): el
  "borde brillante" que no pinta nada en un terreno mate.

---

## 3.4 ⚠️ AGUA BAJO TIERRA (reportado 2026-07-13) — y por qué lo arregla la hidrología

**Síntoma**: hay agua DEBAJO del terreno, y **con física** (te afecta al cavar / bajo la superficie).

**Causa**: el océano es una **lámina continua a nivel del mar en todo el planeta**. No existe el
concepto de "el agua está solo donde hay cuenca": *todo* lo que quede por debajo del nivel del mar
es agua, incluido lo que está bajo roca. La física consulta "¿estoy bajo el nivel del mar?" → sí →
agua. Visualmente ya no se ve (el terreno la ocluye por depth, F0), pero **sigue ahí**.

**No se arregla con un parche visual.** Lo arregla F3: la hidrología da un **nivel freático** y un
**nivel de agua por cuenca** — el agua deja de ser "una esfera a radio R" y pasa a ser un CAMPO
(dónde hay agua y a qué cota), que es lo que deben consultar tanto el render como la física.
Mientras tanto: la física debería exigir, además de "bajo el nivel del mar", que **no haya roca por
encima** (que el punto esté en la columna de agua, no dentro del terreno).

*(Nota: este mismo nivel freático es un prerrequisito de las cuevas kársticas — ver §4.)*

---

## 3.45 ⚠️ EL AGUA SIGUE FACETADA (reportado 2026-07-13, tras F1)

La espuma ya está bien (per-píxel), pero **la superficie del mar se ve en triángulos**.

**Causa**: es el MISMO error que la espuma, pero en la geometría. El oleaje Gerstner se evalúa
**per-vértice** sobre la rejilla del océano (192²) y se interpola: las olas cortas (7–41 m) NO caben
en esa malla, así que lo que ves son las facetas de la rejilla, no olas.

**Fix (mismo principio: geometría per-vértice, sombreado per-píxel)**: la malla se queda **solo con
el oleaje largo** (la mar de fondo, que sí resuelve y que da la silueta del horizonte), y todo el
detalle corto pasa a una **normal per-píxel** — evaluando las olas cortas en el fragment (o con un
normal map de olas animado). La geometría deja de intentar representar lo que no puede.

*(Es exactamente el §0: la malla solo tiene que ser buena para la SILUETA.)*

---

## 3.46 ✅ MAR BAJO EL TERRENO — ARREGLADO (2026-07-13, con F3)

**La causa era una pregunta mal hecha.** La física preguntaba *"¿estoy bajo el nivel del mar?"*
(`isOcean = terrainH < 0`), y `terrainH` **incluye lo que cavas** → cualquier hoyo lo bastante
profundo se convertía en océano, con física de nadar, en mitad de un continente.

**El arreglo** (posible solo con los campos de F3): la pregunta correcta es **"¿HAY agua aquí?"**.
El motor lo responde con el campo hidrológico (`PlanetarySystem::sampleWaterLevel`): océano donde el
terreno **PROCEDURAL** (no el excavado) está bajo el mar, y lago donde la cuenca no drena. Un agujero
que cavas tierra adentro **no está conectado al mar** → no se llena.
*(De paso, los LAGOS dejaron de ser "sellos" por hash y pasaron a ser las cuencas endorreicas reales
del priority-flood.)*

*Texto original del bug, por si vuelve:*

## 3.46-bis ⚠️ (histórico) SIGUE HABIENDO MAR BAJO EL TERRENO (confirmado tras F0/F1)

Ya no se VE (el terreno lo ocluye por depth), pero **sigue existiendo y sigue teniendo física**.
Ver §3.4: la causa no es de render, es que el océano es una lámina a radio R en TODO el planeta.

**Lo arregla F3**: el agua deja de ser "una esfera a radio R" y pasa a ser un CAMPO (dónde hay agua
y a qué cota: mar, lago, río) que consultan render y física. Mientras tanto, el parche honesto es
que la física exija, además de "bajo el nivel del mar", que **no haya roca encima**.

---

## 3.5 PROPS — dónde se generan (parte del alcance)

**Hoy** (`Survival/scripts/resources/resource_scatter.cpp`): rejilla + hash determinista, y el TIPO
se elige por **banda de altitud** (`sampleTerrainHeight`). No mira pendiente, ni bioma, ni agua, ni
suelo. Resultado: un bosque idéntico repartido por igual, árboles en cornisas imposibles y minerales
que aparecen "porque estás a tal altura". Es colocación por ruido, no por **ecología**.

**Rediseño: los props los dicta el mismo campo que el terreno.**

| Prop | Regla (campos que consulta) |
|---|---|
| Árboles | `humedad` alta · `temperatura` en rango · `pendiente` < ~30° · `caudal` bajo (no en el cauce) · `sedimento` (suelo) presente. **Densidad y especie del BIOMA**, no de la altitud. |
| Sotobosque / hierba | Igual pero tolera más pendiente; se apaga en roca desnuda. |
| Rocas sueltas / pedreras | Justo donde la **erosión térmica** depositó talud (¡el campo ya lo dice!) y al pie de paredes. |
| Juncos / vegetación de ribera | `caudal` alto o borde de lago → **el río se ve porque la vegetación lo acompaña**. |
| Minerales / vetas | A lo largo de **fallas y fracturas** de la tectónica, y en el arco volcánico. Deja de ser "por altitud" y pasa a tener una razón geológica. |
| Playa / dunas | Donde el campo de **deposición** costera lo dice. |

**Distribución**: sustituir la rejilla por **blue-noise / Poisson-disk** determinista por hash
(anclado al mundo, mismo resultado en cliente y servidor) → sin patrón de rejilla visible y con
separación mínima creíble entre troncos.

**Regla de oro**: un prop nunca se coloca con una fórmula propia — **pregunta al campo**. Así el
mundo es coherente: hay bosque donde llueve, pedrera donde se desmorona la pared y juncos donde
pasa el río.

---

## 4. Cuevas — ⏸️ APARCADAS (diseño guardado)

⚠️ **Un quadtree de heightfields NO puede representar una cueva**: una altura por dirección → no
hay techos ni voladizos. Requiere representación **volumétrica**. No hay atajo honesto.

**Diseño creíble** (las cuevas reales tienen una causa):
- **Kársticas**: disolución en caliza, siguiendo **fracturas** (que ya tenemos: las fallas de A) y
  el **nivel freático** (que ya tenemos: la hidrología de B). Galerías horizontales al nivel del
  agua, simas verticales por fractura.
- **Tubos de lava** en zonas volcánicas (arcos de A).

**Representación**: grafo de la red (nodos + túneles) generado desde fracturas y freático → se
evalúa como **SDF 3D**; el chunk que interseca la red pasa a ser un **chunk híbrido** mallado con
**dual contouring** (preserva aristas). Encaja con lo que ya hay: el pool ya admite topologías
distintas, y la generación ya es compute en GPU.

Coste: es la pieza más cara del plan. Decisión ⚠️: **volumétrico real** vs **cuevas prefab**
(mallas colocadas como props, entrada recortada) vs **aparcar**.

---

## 5. Islas flotantes

Hoy: blobs fusionados en un draw, sin LOD, sin morph, sin colisión real.
Para que sean creíbles hay que **justificarlas en el mundo** (son fantasía: la credibilidad viene
de la coherencia interna, no de la física):
- Una **causa** visible: mineral levitante aflorando en la cara inferior (y que sea el recurso que
  el juego premia con ir allí).
- **Erosión invertida** por debajo (la roca se desprende hacia abajo) → silueta rota, no un blob.
- **Cascadas** que caen del borde y se deshacen en niebla — vende la escala como nada.
- Cara superior con el MISMO sistema de biomas (clima por altitud) → no un objeto pegado.
- **LOD/impostor** a distancia + colisión.

---

## 6. Fases (orden obligado por las dependencias)

| Fase | Qué | Por qué va aquí |
|---|---|---|
| **F0 — Cimiento** | ✅ **HECHO Y VALIDADO EN JUEGO (2026-07-13)**. Reversed-Z + far infinito + D32F. Profundidad del terreno: de `0.99996..1.0` a `0..0.000115` en float32. **Cerró el bug del agua** (el mar tapaba el terreno): no era el recorte de costa, era el z-buffer. | Requisito de todo. |
| **F1 — Agua sin re-derivar terreno** | ✅ **HECHO (2026-07-13)**, pendiente de validar el mar en juego. `water.frag`: 385 → 206 líneas (**−157 de Perlin/fBm portado a mano**). La profundidad del agua sale del DEPTH BUFFER: reversed-Z ⇒ `dist = near / z` → `espesor = distFondo − distAgua` (además es lo físicamente correcto: la absorción va por el camino del rayo). Fuera del UBO: seed, seaThreshold, continentFreqA, invRadius, coastWidth, pxCoast, oceanSurface. Se copia el depth de escena con un blit (leer el depth ATADO al FBO activo sería feedback loop). **Espuma PER-PÍXEL hecha**: lo per-vértice (Jacobiano de Gerstner) ya solo da la MÁSCARA y el detalle lo pone un ruido per-píxel anclado al mundo → se acabaron las manchas/triángulos blancos (era una señal de alta frecuencia metida en un canal de baja frecuencia). | Rompe la triplicación por el lado del agua. |
| **F2 — Tectónica** | ✅ **HECHO Y EN EL JUEGO (2026-07-13)**. `planet_geology.{h,cpp}`: placas Voronoi con bordes deformados por ruido, corteza continental/oceánica, límites convergente/divergente/transformante → cordilleras, fosas, arcos volcánicos, dorsales, rifts. Portado a `terrain_gen.comp` (las PLACAS las sube C++ → los datos tienen una sola fuente; solo se duplica la fórmula, cubierta por el test de paridad, **0.04 m de error mediano CPU↔GPU**). El nivel del mar ya es `elev = 0` (fuera `seaThreshold`). Test: **>90% del relieve alto está en los LÍMITES de placa** (cordilleras, no manchas). | La base de todo lo creíble. |
| **F3a — Erosión + hidrología MACRO** | ✅ **HECHO (2026-07-13)**, *pendiente de conectar al mundo*. `planet_fields.{h,cpp}`: cube-sphere 6×N², relleno de depresiones (**priority-flood** → LAGOS reales donde la cuenca no drena, no por hash), rutado D8 + acumulación de caudal (**RÍOS**, que no existían), erosión hidráulica (**stream power** `E = K·A^m·S^n` → valles en V y redes dendríticas) y térmica (ángulo de reposo → taludes). Test: ríos = **cauces** (1.9% de la superficie), no manchas. | Lo que de verdad hace "real". |
| **F3b — Conectar los campos + MESO** | ⏳ **SIGUIENTE.** Subir el campo a GPU (textura) y que el compute y el sampler de CPU lo MUESTREEN en vez de re-derivar → **el heightfield pasa a ser LA FUENTE DE VERDAD** y muere la duplicación. Luego el nivel MESO (teselas ~10-30 m/téxel con **caudal heredado del macro**, que es el riesgo #1). | |
| **F4 — Consumidores del campo** | Mallado del chunk · **sampler CPU (física/colisión/spawn) muestrea el heightfield** · agua lee el nivel del campo | Es lo que ELIMINA la elevación triplicada. Sin esto, F3 no sirve de nada. |
| **F5 — Clima y biomas** | Sombra orográfica (usa las cordilleras REALES) · Whittaker (temp, humedad, pendiente, caudal) | Depende de F2/F3. |
| **F6 — Props dirigidos por el campo** | Reglas ecológicas (§3.5) + blue-noise | Depende de F5 (necesita bioma/humedad/caudal). |
| **F7 — Materiales** | Splatmap height-blend (tierra BAJO hierba) · triplanar estable en mundo · fuera especular | Depende de F5 (el bioma manda el material). |
| **F8 — Malla más gruesa** | Subir `targetPx`; detalle 100% per-píxel | Arregla el cuello de CPU (conteo de chunks). Se solapa con F7. |
| **F9 — Islas flotantes** | Causa (mineral levitante) · erosión invertida · cascadas · LOD · colisión | Independiente: puede ir en paralelo con cualquier fase. |
| ~~Cuevas~~ | ⏸️ **Aparcadas** | Se retoman con F2/F3 hechas (fracturas + freático son sus prerrequisitos). |

**Riesgo alto asumido**: F2-F4 cambian la altura de TODO el mundo → invalidan la caché de chunks,
mueven la costa y pueden dejar el spawn/las bases bajo el agua. **Hay que regenerar el mundo.**

**Riesgo técnico #1 (el que puede morder)**: la **coherencia entre teselas MESO**. Si la erosión
local no hereda el caudal entrante del macro, los valles no casan en los bordes y se ven costuras.
Es la parte a prototipar antes que nada dentro de F3.

**Riesgo técnico #2**: la física pasa a depender de teselas residentes. Hoy ya depende de chunks
residentes, así que el modelo encaja — pero hay que cuidar el fallback al macro para consultas
lejanas (spawn, IA), que no necesitan precisión fina.

---

# 🔴 F10 — LA ALTURA TIENE QUE SALIR DE LA MALLA (la deuda que queda) — 2026-07-14

**Síntoma que lo destapó**: props FLOTANDO sobre el terreno (y, en su día, caer a través del suelo).

**Causa (no es un ajuste, son DOS fuentes de verdad)**: la malla y el sampler de CPU no aplican el
MESO en los mismos sitios.
- La **malla** (GPU) usa meso solo en chunks FINOS (espaciado < 200 m). Un chunk grueso no puede
  representar detalle de 40 m: entre dos de sus vértices hay 200 m de rampa.
- El **sampler de CPU** (props, colisión) usa meso **siempre que la tesela esté residente**, sin
  mirar qué chunk cubre ese punto.
→ Donde el terreno se dibuja grueso pero la tesela existe, la CPU cree que el suelo está hasta **60 m
más arriba** de donde la malla lo pinta. El prop se queda colgado.

Se ha ido tapando tres veces (subir el cupo de teselas, hundir el pie de los props, el fade del
borde). Todas son parches sobre el mismo agujero.

## El arreglo: la FÍSICA MUESTREA LA MALLA QUE SE DIBUJA

1. **No hay que guardar nada nuevo**: `ChunkData::vertices` ya es la rejilla `(res+1)²` en orden,
   relativa a `chunkCenter`. Elevación de un vértice = `|chunkCenter + v| − radio`.
2. **`PlanetarySystem::meshHeightKmAt(dir)`**:
   - dirección → cara + uv,
   - de FINO a GRUESO: construye la clave del chunk en cada LOD y para en el primero **RESIDENTE**
     (que es, por construcción, el que el renderer dibuja — ya existe `isResident`/`residentHashes`),
   - bilineal sobre su rejilla → **la altura que VES**.
3. **Respaldo analítico** donde no hay chunk residente (spawn, IA, consultas lejanas): allí no hay
   malla con la que chocar, el sampler actual vale.
4. **Consumidores**: colisión del jugador, `getTerrainHeightAt`, colocación de props. El sampler
   analítico SIGUE siendo la verdad para lo que no es altura (bioma, humedad, caudal).

## ⚠️ El único trozo no trivial
Los vértices se generan con el cubo→esfera de **Cobb** (`getLocalPosition`), que **no se invierte en
forma cerrada**. Para saber en qué celda cae una dirección hay que invertirlo (Newton, 2-3 pasos) o
aceptar un error de fracción de celda. Es la parte a prototipar primero.

## Lo que esto cierra de raíz
Props flotando · caer a través del suelo · colisión desalineada. Y **el MESO deja de ser un riesgo**:
da igual que una tesela se evicte, porque ya nadie re-deriva la altura por su cuenta.
