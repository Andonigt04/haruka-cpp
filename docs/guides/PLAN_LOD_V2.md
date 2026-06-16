# Plan: Rediseño del sistema LOD / streaming (LOD v2 — "tiles")

> Estado: PROPUESTA (sin implementar). Para revisión antes de tocar código.
> Motivo: el LOD/streaming actual creció a parches y no contempló de raíz varios
> sistemas que ahora cuelgan de él (cuerpos celestes con chunks, fluidos, recursos,
> deform, colisión). El objetivo es **un solo modelo consistente** y **menos ms**.

## Decisiones (cerradas)

- **A — Identidad:** un único store (caché+renderer) con **`BodyId` en la `TileKey`**. Nunca colisionan dos cuerpos.
- **B — Capas:** **colisión = proxy del tile** (vía el sampler CPU bit-exacto); **recursos = capa de datos deterministas del tile**, pero **el render de props lo sigue haciendo el juego** (no acoplamos gameplay al motor).
- **C — Ríos:** **cauce baked en el tile** (altura/máscara) + **sim shallow-water/PBF dinámica solo en una región activa junto al jugador**.
- **D — GPU-driven COMPLETO:** la malla del terreno vive en **buffers GPU (SSBO/VBO en pool)**; el **compute genera la geometría directamente** ahí y se dibuja con **draw indirecto**. La CPU solo gestiona el quadtree, asigna slots de buffer y emite dispatch + comandos indirectos. → Implicación: **la colisión NO lee la malla GPU**; usa el **sampler CPU espejo (bit-exacto con el compute)** en los puntos de consulta. "Una sola verdad" = la **función de ruido** compartida, no el buffer.

---

## 1. Por qué rehacerlo (dolor actual, concreto)

El diseño actual nació para **un** planeta procedural y fue creciendo:

| Problema | Dónde | Síntoma |
|---|---|---|
| **La clave de chunk no tiene identidad de cuerpo** | `PlanetChunkKey {face,lod,x,y}` ([planetary_types.h:20](../../src/tools/planetary_types.h)) | Caché y renderer son **compartidos** entre Tierra/Luna/Júpiter → chunks de distinto cuerpo **colisionan en el hash**. Hoy sobrevive de chiripa. |
| **El quadtree se reconstruye entero cada recompute** | `LODSystem::updatePlanetLOD` ([lod_system.cpp](../../src/core/lod_system.cpp)) | Allocations + `balanceLeaves` + diff por planeta y por frame. Picos de ms (llegó a 924 ms con minLOD). |
| **`m_currentFrameChunks` se pisa entre planetas** | `LODSystem` | `isVisible` quedaba mal entre cuerpos (bug ya parcheado a mano). |
| **Estado de streaming antes era único y compartido** | `TerrainStreamingSystem` | Cada planeta lo pisaba; solo se generaba el último (parcheado: mapa por planeta). |
| **Contención de mutex en residencia** | `TerrainRenderer::isResident` por nodo | Parcheado con snapshot, pero es síntoma de acoplar LOD ↔ renderer. |
| **El agua va horneada en `ChunkData`** | `terrain_generator.cpp` (`waterVertices…`) | Mezcla terreno+agua+orillas+skirts en una función gigante; difícil de razonar y de dar LOD coherente al agua. |
| **Recursos por un camino aparte** | juego: `resource_scatter` + `getActivePlanet` | Árboles/rocas no comparten ciclo de vida con el chunk → no se cargan/descargan con él. |
| **Colisión re-muestrea el ruido** | `sampleHeightAt` ([terrain_generator.cpp:589](../../src/core/terrain/terrain_generator.cpp)) | Camino paralelo al de render → riesgo de divergencia malla↔colisión; y duplica coste de muestreo. |
| **Deform es un `#ifdef` incrustado en el generador** | `HARUKA_MOD_DEFORM` dentro del bucle de vértices | El parche de edición y la base procedural están entrelazados. |
| **Carga progresiva necesita recompute por frame** | parche `residencyLimited` + "cada 4 frames" | Funciona pero es un workaround sobre un árbol que se reconstruye. |

**Conclusión:** todo lo nuevo (cuerpos múltiples, agua, recursos, deform, colisión) se ha
ido colgando del LOD sin un contrato común. Un modelo de **"tile" = todas las capas de una
celda, con un ciclo de vida único** lo vuelve consistente y barato.

---

## 2. Lo que NO estaba planeado de raíz

1. **Varios cuerpos** con chunks a la vez (Tierra streamed + Luna streamed + gas), **móviles** (orbitan).
2. **Agua** como capa con su propio LOD (océano a nivel mar, lagos finos, ríos dinámicos, partículas PBF).
3. **Recursos** (árboles/rocas/menas) atados al chunk: aparecen/desaparecen con él, deterministas por celda.
4. **Deform** (cavar/construir) como overlay que sobrevive al streaming.
5. **Colisión/física** que use EXACTAMENTE la geometría que se ve (no un re-muestreo).
6. **Presupuesto de frame**: que generar/subir nunca produzca picos (los 700/924 ms).

---

## 3. Objetivos de diseño

- **Consistencia**: un único concepto (`Tile`) y un único contrato de generación/streaming que TODAS las capas usan.
- **Menos ms**:
  - Quadtree **persistente** (no se reconstruye; se hace split/merge incremental).
  - **Sin contención**: el LOD no consulta al renderer por nodo; trabaja sobre su propio estado.
  - **Presupuesto por frame** para subidas/dispatch (cero picos).
  - **GPU-driven** donde toca (altura/normal/agua ya en compute; extender a máscaras de recursos).
- **Aislamiento por cuerpo**: cada cuerpo es independiente; nada se pisa.
- **Una sola verdad de altura**: render, colisión, recursos y agua leen la MISMA fuente.

---

## 4. Arquitectura nueva (LOD v2)

### 4.1. Identidad y namespacing
```cpp
struct BodyId { uint16_t v; };                 // cada cuerpo celeste
struct TileKey { BodyId body; uint8_t face, lod; uint32_t x, y; }; // hash incluye body
```
Un cuerpo NUNCA colisiona con otro. La caché y el renderer pasan a ser **por cuerpo** (o
un solo store con `TileKey` que incluye `body` — ver Decisión A).

### 4.2. El `Tile` = todas las capas de una celda
Un tile se genera, cachea, sube y descarga **como una unidad**. Contiene:
```
Tile {
  TileKey key;
  // BASE (GPU compute → worker assembly):
  mesh terrain (vértices rel. a tileCenter, normales, UV, morphTargets CDLOD)
  // CAPAS:
  mesh  water      (océano/lago, con waterline coherente por LOD)   // opt
  collision        (la MISMA malla, o un proxy grueso garantizado-igual)
  resources[]      (instancias deterministas: árbol/roca/mena, sembradas por la celda)
  // ESTADO:
  flags: hasOcean, hasLake, isDeformed(dirty), residentGPU, ...
}
```
> Hoy ya está medio así (`ChunkData` lleva agua), pero ad hoc. La idea es hacerlo un
> **contrato explícito por capas**, cada una opcional y con su propio sub-LOD si hace falta.

### 4.3. Quadtree PERSISTENTE (no se reconstruye)
- Cada cuerpo mantiene su árbol vivo entre frames.
- Cada frame: un paso **incremental** que decide `split`/`merge` por nodo según
  distancia-a-superficie + altitud (cap) + **suelo mínimo del cuerpo activo**.
- `split` solo si el nodo ya está residente (carga progresiva grueso→fino, ya validada),
  pero **sin reconstruir** el árbol — solo se añaden/quitan hojas.
- Sin `m_currentFrameChunks` global: el árbol ES el estado; `isVisible` = "es hoja del árbol".

### 4.4. Pipeline GPU-driven (Decisión D)
La geometría **no vuelve a la CPU**. Hay un **pool de buffers GPU** (SSBO/VBO grandes); cada
tile residente ocupa un **slot** (rango de vértices/índices).
```
quadtree (CPU) decide hojas
  → asigna slot de buffer a cada tile nuevo
  → [GPU compute] escribe DIRECTO en el slot: posiciones, normales, UV, morph, agua
  → construye el buffer de comandos de dibujo (uno por tile visible)
  → [draw INDIRECTO] dibuja todos los tiles visibles en 1–pocas llamadas
```
- La CPU solo: recorre el árbol, asigna/libera slots, lanza dispatches y emite el indirecto.
- **Frustum/horizon cull**: idealmente en GPU (compute compacta el buffer de comandos), o en
  CPU sobre las bounding spheres de cada slot.
- **Colisión y recursos** (no pueden leer el buffer GPU): usan el **sampler CPU espejo**,
  bit-exacto con el compute (mismas constantes Perlin/Voronoi). El `RaycastSimple` consulta
  altura por punto; el scatter de recursos siembra por celda con ese mismo sampler. La
  consistencia la garantiza la **función compartida**, no un buffer.
- **Agua**: su propia geometría en el pool (otro rango), generada por el mismo compute con el
  nivel de agua; waterline por-fragmento.

### 4.5. Streaming con presupuesto
- Cola por cuerpo, **near→far** (ya existe).
- **Budget por frame**: máx. X ms (o X tiles) de dispatch + máx. Y subidas GPU/frame.
  Lo que no entra, el siguiente frame. → **estructuralmente sin picos**.
- Descarga diferida + "stale hasta cubrir" (ya existe) → sin huecos.

### 4.6. Cuerpos móviles
- Tiles **body-local** (ya hecho). El render suma el centro actual del cuerpo (órbita) →
  mover un cuerpo NO regenera nada. Primer-ciudadano, no parche.

### 4.7. Capas especiales
- **Océano/lago**: sub-malla del tile, waterline por-fragmento (terreno ocluye), lagos solo a LOD fino (ya validado).
- **Ríos / shallow-water + PBF**: NO son estáticos del tile. Son una **región activa** anclada al jugador, alimentada por el height del tile (acumulación de flujo). Viven aparte del quadtree (sim, no streaming). El tile solo aporta el cauce/altura.
- **Deform**: overlay separado de la base. Un tile editado lleva flag `dirty` y se regenera componiendo base+overlay. El overlay persiste aunque el tile se descargue.

---

## 5. Qué hace que sean MENOS ms

1. **Quadtree persistente** → se acaban las allocations + `balanceLeaves` + diff por frame (el grueso de los 924 ms).
2. **El LOD no toca el renderer por nodo** → sin contención de mutex (el snapshot deja de ser un parche).
3. **Presupuesto por frame** → subidas/generación amortizadas, sin spikes (700/924 ms imposibles por diseño).
4. **Una sola verdad de altura** → colisión deja de re-muestrear (menos CPU) y se elimina el riesgo de regenerar por divergencia.
5. **Recursos atados al tile** → no se re-siembran por movimiento; se cargan una vez con la celda.
6. **GPU-driven** del height/normal/agua (ya) + máscara de recursos → menos CPU.

---

## 6. Plan de migración (incremental, sin big-bang)

El motor tiene que seguir funcionando en cada paso:

1. **F1 — Identidad por cuerpo.** `BodyId` en `TileKey` + hash; un solo store que lo respeta. *(Arregla la colisión latente; base de todo.)*
2. **F2 — Quadtree persistente.** Split/merge incremental sobre árbol vivo (sin rebuild). Mantener la API de `LODUpdate` para no romper el streaming aún. *(Gran recorte de ms; bajo riesgo.)*
3. **F3 — Sampler espejo bit-exacto** CPU↔compute como módulo aislado; colisión (`RaycastSimple`/`sampleHeightAt`) y scatter pasan a usarlo. *(Una sola verdad; desacopla colisión de la malla.)*
4. **F4 — Pool de buffers GPU + draw indirecto** (Decisión D). El compute escribe geometría directo en slots; render por indirecto. Primero solo terreno del cuerpo home; validar paridad visual contra el render actual. *(El cambio grande de rendimiento.)*
5. **F5 — Agua como rango del pool** (océano/lago) con waterline por-fragmento. Retirar el horneado de agua del generador monolítico.
6. **F6 — Recursos como capa de datos del tile** (deterministas por celda vía sampler espejo); el juego sigue renderizando props, pero suscrito al ciclo de vida del tile.
7. **F7 — Presupuesto de frame** en dispatch + asignación de slots. *(Mata picos por diseño.)*
8. **F8 — Deform como overlay** desacoplado (base procedural + overlay editable que persiste al streaming).
9. **F9 — Ríos baked + región PBF dinámica** junto al jugador, alimentada por el height del tile.

Cada fase es testeable sola. **F1+F2** ya dan el grueso de la mejora de ms con riesgo bajo;
**F4** es el salto grande (pipeline GPU-driven) y conviene aislarlo y validar paridad.

---

## 7. Riesgos

- **GPU-driven (F4) es el riesgo principal**: pool de buffers + draw indirecto + (idealmente) cull en compute es complejo de depurar. Mitigación: aislarlo tras un flag, **mantener el render actual en paralelo** y **validar paridad visual** antes de cambiar.
- Tocar la clave de chunk invalida saves/`.hmap` que la usen → **versionar el formato**.
- El quadtree persistente cambia el orden de eventos load/unload → re-validar el "stale hasta cubrir".
- Sampler espejo CPU↔compute: si divergen (constantes, orden de float), colisión/recursos no cuadran con lo que se ve → test de paridad permanente como **gate** (no un pico de 700 ms al arrancar).

## 8. Pendiente de decidir (no bloquea F1–F3)

- **Target de hardware** (escritorio/GPU moderna vs portátiles flojos) → fija presupuestos por defecto y `minLOD`. Cerrarlo antes de F7.
