# Estado del motor, parte por parte

**Revisión de código a 2026-07-27.** Auditoría LEYENDO el código (no la documentación previa ni el
TODO). Cada afirmación lleva su `fichero:línea`. Tres marcas:

- 🟢 **funciona y está conectado** — hay call-site real en el camino del juego.
- 🟡 **funciona pero tiene un problema concreto** — descrito, con su consecuencia.
- 🔴 **muerto o en desuso** — compila, ocupa sitio, y nadie lo llama.
- ✅ **ARREGLADO** en la pasada del 2026-07-27 (los puntos 1-7 del §8). Se deja el apartado escrito
  con lo que había, no borrado: el problema es la mitad del valor del arreglo.

> Distinción importante que se repite abajo: *existir* no es *estar conectado*. Varias clases del
> renderer se construyen (o ni eso) y se destruyen sin que ningún pase las use. Eso NO es un bug de
> ejecución — no rompe nada — pero es lo que hacía que el README prometiera cosas que no se dibujan.

---

## 1. TERRENO — el subsistema más maduro del motor

### 1.1 La cadena, tal y como está en el código

```
PlanetGeology::generate(seed)              placas (Voronoi + warp), 30 sitios
        ↓                                   planet_geology.cpp
PlanetFields::generate(seed, geo, 256, 40)  6 caras × 256², erosión 40 iteraciones
        ↓                                   planet_fields.cpp:85
        ├── priority-flood → lagos          planet_fields.cpp:141
        ├── D8 + acumulación → caudal       planet_fields.cpp:173
        ├── stream-power + térmica          planet_fields.cpp:214
        └── computeClimate() (T°, humedad)  planet_fields.cpp:277
        ↓
    ┌───┴────────────────────────────┐
sampleTerrainV2 (CPU)          terrain_gen.comp (GPU)
terrain_sampler_v2.cpp:426     assets/shaders/terrain_gen.comp:526
    ↓                                 ↓
ReferenceSurface              malla del chunk (TerrainGenerator::generateChunk)
reference_surface.cpp:75      terrain_generator.cpp:81
= EL SUELO DEL JUEGO          = lo que se dibuja
```

La disciplina de paridad está escrita en el código y se sostiene: dirección normalizada **una sola
vez en double** (`terrain_sampler_v2.cpp:434`, `terrain_gen.comp:673`), campo muestreado en double
(`planet_fields.cpp:405`, `terrain_gen.comp:367`), raíces refinadas con Newton porque el `sqrt` de
double del driver no es IEEE (`terrain_gen.comp:108-127`), y exponentes elegidos para salir de
`sqrt` en vez de `pow` (`terrain_sampler_v2.cpp:166`, `terrain_gen.comp:604`).

**Medido hoy** (`HARUKA_DISKCACHE=0`, ejecución de `paridad`):

| ámbito | error render↔colisión | fuera de 1 cm |
|---|---|---|
| nudo a nudo (vértice GPU vs superficie de referencia) | 0.0001 m medio · 0.0003 m peor | — |
| dentro del `keepRadius` (≤400 m) | 0.0001 m medio · 0.0003 m peor | **0 / 200** |
| fuera (512 m – 3 km) | 0.731 m medio · 8.171 m peor | 132 / 200 (a propósito: ahí el LOD adelgaza) |

Suite completa: **19460 OK · 0 fallos**.

### 1.2 🟡 La superficie de referencia solo existe para UN planeta

`PlanetarySystem::sampleTerrainHeight` usa `m_refSurface` únicamente si el cuerpo más cercano es el
planeta activo (`planetary_system.cpp:1866`). En cualquier otro cuerpo cae al **analítico puntual**
(`planetary_system.cpp:1883`), que es exactamente el suelo que la superficie de referencia vino a
sustituir. Y `groundHeightKmAtDir` devuelve `false` si no hay superficie configurada
(`planetary_system.cpp:1888`) → los props no tienen suelo que consultar fuera del planeta activo.

**Consecuencia:** en la Luna y en Júpiter sigue viva la discrepancia de metros entre lo que se
dibuja y lo que se pisa (la que se midió en 7.81 m de pico en la Tierra antes de F10). No se nota
porque nadie ha aterrizado ahí todavía.

### 1.3 🟡 Los cuerpos no-terran no pasan por el camino de doble precisión

`sampleTerrainV2` normaliza en double y luego, para los perfiles moon/gas, entrega la dirección ya
**redondeada a float** (`terrain_sampler_v2.cpp:441-442`). Además, en `Survival/scenes/main.scene`
solo la Tierra lleva `gpuTerrain: true`: la Luna y Júpiter generan su malla **en CPU**, y ahí los
vértices se muestrean con `posOnSphere` en float (`terrain_generator.cpp:219, 222`).

Es coherente consigo mismo (los dos lados son CPU), pero significa que toda la infraestructura de
paridad de 1 cm cubre **un cuerpo de los tres**.

### 1.4 🟡 Un solo hoyo apaga el terreno GPU en TODO el planeta

```cpp
deformActive = (m_deform && !m_deform->empty());        // terrain_generator.cpp:144
const bool useGpu = (useV2 && !deformActive && ...);    // terrain_generator.cpp:155
```

`DeformationField::empty()` es un contador **global** (`deformation_field.h:82`), no una consulta
por chunk. En cuanto el jugador cava en cualquier punto del planeta, `useGpu` se apaga para **todos
los chunks** y la generación vuelve a CPU por vértice.

Peor: `gpuParamsFromSettings` no mira el deform (`terrain_generator.cpp:887`), así que el streaming
sigue despachando el compute y descartando su resultado → se paga el trabajo dos veces. No hay
ningún aviso; se manifiesta como "el terreno va más lento desde hace un rato".

### 1.5 ✅ `getWorldParamsCached` era una caché de UN hueco

**Lo que había:**

```cpp
static bool valid; static uint32_t cachedSeed; static WorldGenParams cached;  // :30-42
```

`deriveWorldParams` calibra el umbral de mar con **2048 muestras de fBm de 6 octavas**
(`terrain_sampler_v2.cpp`). Con dos planetas de seeds distintas, cada llamada alternada
recalcaba esa calibración entera, bajo un mutex que comparten todos los hilos worker — o sea, no
solo repetía el trabajo: lo serializaba.

Era literalmente el mismo fallo que ya se había identificado y arreglado para el campo. Aquí quedó
sin arreglar.

**Arreglado**: `std::unordered_map<uint32_t, WorldGenParams>` (`terrain_generator.cpp:37`). Cada seed
se calibra una vez.

### 1.6 ✅ El campo del planeta se generaba y se guardaba DOS veces

**Lo que había:** dos cachés independientes de `PlanetFields` para la misma seed.

| dónde | quién lo usaba |
|---|---|
| `fieldsFor()` — `terrain_sampler_v2.cpp` | el sampler de CPU (colisión, props, DGS) |
| `fieldsInstance()` — `gpu_heightfield.cpp` | el SSBO que sube a la GPU |

Cada una ejecutaba `PlanetGeology::generate` + `PlanetFields::generate(seed, geo, 256, 40)` y retenía
6 arrays de `6·256²` floats → **~9 MB y varios segundos por seed, duplicados**.

Coincidían porque la generación es determinista. Pero eran una fuente de verdad duplicada justo en el
eje donde el proyecto ha decidido que no puede haber dos, y bastaba una futura no-determinación (un
`std::sort` con empates, un paralelismo, un cambio de orden de flotantes) para que malla y colisión
se separaran **sin que ningún test lo viera**, porque los dos lados del test también leían cachés
distintas.

**Arreglado**: queda **una** caché, `fieldsFor` (`terrain_sampler_v2.cpp:246`), que vive en la unidad
GL-free — la que arrastra glad depende de ella, no al revés. `fieldsInstance` y el alias
`planetFieldsFor` se han eliminado; `gpu_heightfield.cpp` y `terrain_generator.cpp` piden el campo
ahí. El test que comparaba las dos cachés se ha retirado en vez de dejarlo verde por construcción:
comparaba A con A. La garantía pasó de runtime a **estructural** (un solo accesor), y el eje lo sigue
cubriendo la paridad extremo a extremo GPU↔CPU.

### 1.7 🟡 Los lagos y el caudal se calculan ANTES de erosionar

En `PlanetFields::generate`:

1. priority-flood → `m_water` (línea 167-168)
2. D8 + acumulación → `acc`, `down` (líneas 173-196)
3. **erosión, que modifica `m_elev` durante 40 iteraciones** (líneas 214-250)

`m_water` y `m_flow` no se vuelven a calcular después. La cota de cada lago es la del terreno **sin
erosionar**, mientras el suelo bajo él ha bajado. El sampler se protege con
`waterKm > 0 && waterKm > elev` (`terrain_sampler_v2.cpp:567`), así que no hay agua "flotando" por
encima del terreno, pero la lámina puede cubrir más superficie que la cuenca real.

Es la misma familia de fallo que la "lámina de agua falsa" que se acaba de eliminar del shader
(`terrain_gen.comp:640-654`), y el motivo de mencionarlo es ese: **el agua era el único eje que
ningún test miraba**. Merece una medición antes que una corrección.

### 1.8 🟡 El pin del disco de suelo va apagado por defecto

`LODSystem::groundDiscKeys` devuelve vacío salvo con `HARUKA_GROUNDPIN=1`
(`lod_system.cpp:113-132`). El comentario explica el porqué, y es un buen porqué: encenderlo rompió
el juego dos veces con la suite en verde, porque toca **residencia** y ahí un fallo no se ve como
"menos detalle" sino como "no hay terreno".

Lo que se pierde está acotado y medido en el propio comentario: con la caché estrangulada a 32 MB, la
malla dibujada puede separarse del suelo hasta **162 m**. Con memoria normal coinciden.

Sigue activo, y es lo que de verdad importaba, el pin del **LOD** del suelo
(`setGroundLOD`, `planetary_system.cpp:1847`): bajo tus pies hay una sola retícula.

### 1.9 ✅ Código muerto en el camino del terreno — borrado

**En el compute (`assets/shaders/terrain_gen.comp`) — ~150 líneas sin call-site:**

| qué | por qué estaba muerto |
|---|---|
| `geologyElevKm` (~60 líneas) | desde F3 la elevación base y la orogenia vienen horneadas en el campo |
| `gHashU/gHashC/gH01/gVHash/gVNoise/gWarp` | solo las usaba `geologyElevKm` |
| `nGradD`, `perlin3D_d`, `fBmGrad` | la normal se hace por diferencias finitas sobre 4 vecinos |
| `dirToFaceUV` (versión float) | todo el camino usa la de double |
| `ssf` | se fue con la rama legacy de lagos |
| `carveKm` + `riverW` | calculadas por vértice y nunca restadas (ídem en CPU) |
| `vec3 dir` en `terranElevKm` | copia en float que solo miraba la geología |

⚠️ **Esto tenía una consecuencia real, no solo cosmética,** y es lo único de la limpieza que además
quita una llamada GL potencialmente inválida. Con `geologyElevKm` muerta, los uniforms
`u_plateCount` y `u_geoContBaseKm..u_geoMarginRad` (locations 20-25) no quedaban activos tras el
link — pero `gpu_heightfield.cpp` los seguía subiendo en **cada dispatch**, y además generaba y subía
un SSBO de placas que nadie leía. El propio fichero explicaba dos párrafos más arriba por qué se
habían quitado los uniforms 2/5/6/7/8/9: *"Subir a una location sin uniforme activo es
GL_INVALID_OPERATION en drivers estrictos"*. Era exactamente el mismo caso.

**Arreglado**: fuera las dos mitades a la vez — la geología del shader y su subidor. Las locations
20-25 y el binding 4 quedan libres.

**En el sampler de CPU (`terrain_sampler_v2.cpp`):** `fieldElevD`, `geologyElevD`, `powD`, `mixD`,
`regionalElevKm` — ninguna con call-site. Borradas. Dos merecen nota:

- `powD` se conserva como **comentario**: no hay `pow` genérico en el camino de altura *a propósito*
  (`std::pow` y el de GLSL difieren ~1e-5 relativo = ~1.5 cm sobre el término de montaña). Dejar la
  función invitaba a usarla.
- `regionalElevKm` era la función que la rama legacy usaba como **nivel de agua del lago** — la
  "lámina de agua falsa". Borrarla cierra la puerta por la que entró.

**Parámetros de mundo que ya no leía nadie:** `lakeDensity`, `lakeMaxProb`, `coastBandKm`,
`coastWidth` (`terrain_sample.h`), y en `GpuHeightfield::Params` además `continentFreqA`,
`seaThreshold`, `voronoiDensity`. Se propagaban por tres sitios para no llegar a ningún `glUniform`.
Fuera los campos y los tres rellenos.

**El generador v1 entero:** `TerrainGenerator::calculateHeight` (~90 líneas) + su `sstep`,
`s_detailScale`, y **todo el esquema `layers`** (struct `TerrainLayerSettings`, parseo, serialización,
validación, y los tres bloques del `main.scene` del juego). v2 es ya el único generador procedural
(el modo `Manual` no era v1 y sigue igual), así que `genVersion` deja de elegir nada en
`terrain_generator`; el validador de escena ahora **avisa** si una escena declara `genVersion < 2`.

> ⚠️ Un detalle que solo salió al compilar: el gate que decidía "este objeto de la escena es un
> PLANETA" era `!terrainSettings->layers.empty()`. Quitar `layers` sin tocarlo habría dejado la
> escena **sin ni un planeta**. Ahora pregunta si el objeto declara config de terreno.

**`assets/shaders/planet_generation.comp`:** generador de planeta anterior a F3, sin referencias.
Borrado con el resto de shaders huérfanos (§2.4).

### 1.10 ✅ Comentario de cabecera del compute, falso

`terrain_gen.comp:9` decía *"NOTA: aún SIN lagos (la CPU los talla); por eso es opt-in y
experimental"*. Los lagos salen del campo desde F3 y este es el camino de producción de la Tierra.
Corregido.

### 1.11 🟡 `mesoElevD` cuesta 5 muestreos aunque el meso esté apagado

`terrain_sampler_v2.cpp:66-78`: un muestreo para el valor y **cuatro más** por diferencias finitas
para el gradiente. Con el meso apagado los cinco caen al macro (`elevKmIfResident` → `m_macro->sample`),
y los cuatro del gradiente solo alimentan la normal. En el camino de colisión, que solo quiere la
altura, son cuatro bilineales de más por consulta.

### 1.12 🔴 / ⏸️ Aparcado pero vivo

`floating_islands.{h,cpp}` + `floating_island_renderer` siguen compilándose y llamándose desde
`planetary_system.cpp:401`, con la feature aparcada por decisión de diseño.

---

## 2. RENDER — lo que se dibuja de verdad

### 2.1 🟢 El camino real

Es un **forward renderer** con pase de estilo: `simple.vert` + `final.frag`
(`application_render.cpp:567, 580-582`), reversed-Z con far infinito
(`window.cpp:94` `glClipControl(ZERO_TO_ONE)`, comparación GEQUAL por PSO), y encima:

- terreno con pools y `drawIndexedIndirect` (`terrain_renderer.cpp`), 1479 líneas, PSO completo
- agua (Gerstner + lagos + profundidad con signo), cielo procedural, precipitación (`precip.*`)
- **una** sombra direccional (`Shadow`, `application_render.cpp:902-912`) — el shader lo pone el
  juego (`Survival/assets/shaders/shadow_depth.*`)
- máscara cenital de exposición al cielo (`application_render.cpp:941`) — la pieza que hace que no
  llueva bajo techo
- bloom (`bloom_extract`/`bloom_blur`), HDR + `post_present`, render-scale y FXAA
- `GPUInstancing` para piezas de construcción (`application_render.cpp:815`), ground stamps
  (huellas), preview 3D de items

### 2.2 🔴 Lo que existe en `application.h` y NUNCA se construye

| miembro | dónde se declara | estado |
|---|---|---|
| `_gBuffer` | `application.h`, creado en `application.cpp:230` | **se crea y se destruye; jamás se bindea** → no hay deferred |
| `_cascadedShadow` | `application.h:421` | nunca construido |
| `_pointShadow` | `application.h:413` | nunca construido |
| `_ibl` | `application.h:410` | nunca construido |
| `_computePostProcess` | `application.h:419` | nunca construido |
| `_virtualTexturing` | `application.h:423` | solo aparece en `.reset()` (`application.cpp:328`) |
| `LightCuller` | `light_culler.{h,cpp}` | ninguna instancia en todo el árbol |

Estas son las líneas del README viejo que no se sostenían: *"Deferred Shading Pipeline — G-Buffer +
light accumulation for hundreds of dynamic lights"*, *"Cascaded shadow maps + point lights"*,
*"PBR Materials with IBL"*, *"GPU Compute — Frustum culling"*.

### 2.3 🔴 SSAO es un hueco

`SSAO` reserva FBO, textura de ruido y kernel (`ssao.h:16-52`), y el ajuste viaja al shader
(`application_render.cpp:666`). Pero `bindForWriting`, `bindForReading` y `getSSAOTexture` **no
tienen ni un call-site** fuera de la propia clase. No hay pase que genere oclusión: el ajuste de la
UI no puede hacer nada.

*(Nota: el otro punto de deuda que arrastraba el TODO, "los RT no siguen el resize", **ya está
arreglado**: `recreateFBOs` recrea gbuffer/hdr/bloom/ssao (`application.cpp:218-244`) y `_postScene`
se recrea al cambiar de tamaño (`application_render.cpp:372-375`).)*

### 2.4 ✅ Shaders sin referencia (se compilaban a `.spv` en cada build) — borrados

`bloom_composite.frag` · `deferred_geom.{vert,frag}` · `deferred_light.frag` · `frustum_cull.comp` ·
`ibl.frag` · `instancing.{vert,frag}` · `light_cube.frag` · `parallax.frag` ·
`planet_generation.comp` · `planet_pooled.vert` · `point_shadow.{vert,frag,geom}` ·
`rain.{vert,frag}` (sustituido por `precip.*`) · `shadow.{vert,frag}` (el juego trae el suyo) ·
`skinned.{vert,frag}` · `ssao.frag` · `tonemapping.frag`

Y tres artefactos sueltos commiteados en `assets/shaders/`: **`comp.spv`, `frag.spv`, `vert.spv`**.

**23 shaders + 3 `.spv`** fuera. Con `shadow.*`/`point_shadow.*` borrados, el filtro
`if(NOT HARUKA_MOD_SHADOWS)` del `CMakeLists.txt` quedaba vacío y también se ha quitado. Se han
borrado con ellos los dos `unique_ptr<Shader>` que nunca se construían y cuyo único rastro era el
`.reset()` del destructor: `_pointShadowShader` y `_instancingShader`.

### 2.5b ✅ Terreno → RHI, paso 1: los 16 uniforms sueltos son ahora un UBO

`gpu_heightfield.cpp` tenía **35 llamadas GL crudas**. Dos tercios ya tenían verbo en el RHI
(`createBuffer`/`updateBuffer`, `bindStorageBuffer`, `dispatch`); lo que faltaba era: uniformes,
fences y barriers. Este paso cierra el primero — el de más valor y menos riesgo.

**Por qué los uniformes primero, y no por estética.** Los 16 eran `layout(location = N) uniform`, y
una location **solo está activa si el enlazador ve que el uniforme se usa**. Es exactamente el fallo
del §1.9: al morir `geologyElevKm`, sus seis uniforms dejaron de estar activos y el subidor siguió
escribiendo en ellos — `GL_INVALID_OPERATION` en drivers estrictos, silencio en los benévolos. Ese
modo de fallo **no existe en un bloque**: los campos existen porque el bloque existe, y si sobra uno
se ve leyendo la struct. Además `glUniform1i(location, …)` no tiene equivalente en Vulkan y
`bindUniformBuffer` sí, así que es el paso que de verdad avanza hacia el backend que falta (§3).

⚠️ **El UBO es POR SLOT, y eso es corrección, no optimización.** Un `glUniform` es estado del
PROGRAMA y el dispatch lo captura AL EMITIRSE, así que un solo juego valía para los 16 dispatches en
vuelo. Un UBO lo lee el shader cuando EJECUTA: con un único buffer, escribirlo para el chunk
siguiente cambiaría los parámetros de los que aún no han corrido — leerían cara, LOD y chunk
equivocados, o sea malla en otro sitio. El anillo de slots ya garantiza que un slot no se reutiliza
hasta cosecharlo, que es justo la vida que el UBO necesita.

El espejo C++ ↔ GLSL va con `static_assert` de tamaño: std140 con solo escalares int/float empaqueta
denso, así que struct y bloque coinciden byte a byte. Un desajuste ahí no daría error de compilación
ni de link — daría terreno en otro sitio.

**Verificado**: la paridad no se mueve **ni un dígito** (nudo a nudo 0.0001 / 0.0003 m; 0/200 fuera de
1 cm; `parity` mediana 0.0000, p99 0.001, max 0.007; agua 0/4096). Suite **19460 OK · 0 fallos**;
motor y juego compilan. Queda **1** llamada GL cruda de las 16: el `glBindBufferBase` del UBO, que
cae cuando el programa pase de `ComputeShader` propio a `PipelineHandle` del RHI (paso 2).

> Pendiente, por orden: **paso 2** dispatch + bindings (mecánico, el RHI ya tiene los verbos) y
> **paso 3** fences y barriers — el delicado: ahí vive el `SlotGuard` con el aviso de que soltar un
> slot dos veces es catastrófico, y añadir fences al RHI es diseño de interfaz (GL usa `GLsync`,
> Vulkan semáforos/timeline), no un renombrado. Los pasos 2 y 3 tocan el camino de residencia: antes
> de ellos, el invariante de la pirámide (§8.4) debería dejar de ser opcional.

### 2.5 🟡 Queda GL crudo fuera del RHI

El RHI/PSO está completo para los renderers de escena, pero siguen con GL a pelo:
`compute_postprocess.cpp` (`glUseProgram`/`glDispatchCompute`, líneas 147-202),
`compute_shader.cpp:34-39`, `virtual_texturing.cpp:150`, los blits del fluido
(`fluid_renderer.cpp:106, 177-194`, falta `blit` en el RHI) y el pase de sombra, que fija estado
global a mano (`application_render.cpp:918-921`) precisamente porque el PSO anterior dejó reversed-Z.

### 2.6 ✅ `MeshLOD` y `MeshOptimizer` — borrados

`mesh_lod.{h,cpp}` no lo incluía nadie salvo su propio `.cpp`; `mesh_optimizer` solo lo incluía
`mesh_lod.h`. Los dos, muertos. `skeletal_animation.{h,cpp}` (SkinnedMesh), igual. Los seis ficheros
fuera.

### 2.7 🟡 Objetos de escena sin LOD ni culling

Los objetos de escena se dibujan a detalle completo a cualquier distancia. Con el instancing ya
migrado, es el sitio natural para darles cull + LOD.

---

## 3. RHI — la costura para Vulkan

🟢 `Device`/`Context` + PSO, con un backend: `src/rhi/opengl/` (`gl_device.cpp`, `gl_context.cpp`).
Buffers con memoria tipada (`Static`/`Dynamic`/`Readback` con mapeo persistente) — es lo que hizo que
cosechar el terreno pasara de `glGetBufferSubData` (sincroniza) a `memcpy`.

🔴 `src/rhi/vulkan/` **está vacío**. La costura existe, el backend no. Falta también `blit` en la
interfaz (lo pide el fluido).

---

## 4. FÍSICA

🟢 **Jolt v5.6.0** integrado como submódulo (`third_party/JoltPhysics`), envuelto por `HarukaPhysics`
en PIMPL para que el juego nunca vea Jolt (`CMakeLists.txt:398-427`). Se compila solo si el módulo
`PHYSICS` está activo y el submódulo presente; si no, cae al solver propio
(`physics_engine.cpp`, guardas `HARUKA_HAS_JOLT`).

🟢 `HarukaPhysics` es una librería **sin GL**: solo depende de glm + `IWorldProvider`
(`physics/world_provider.h`). Ese es el requisito para pasarla al DGS y validar el movimiento con el
mismo código en cliente y servidor.

🟡 El parche de colisión en frontera fría cuesta ~43 ms; está mitigado por el refresco asíncrono (no
bloquea el frame), pero sigue ahí.

🟡 `tools/jolt_demo.cpp` y `tools/physics_demo.cpp` aparecen **borrados** en el working tree sin
commitear.

---

## 5. MÓDULOS DE COMPILACIÓN

🟢 Bitmask de 10 módulos (`CMakeLists.txt:126-137`), `src/core/modules.h` generado, fuentes de los
módulos apagados excluidas del build. Funciona y es de las cosas mejor cerradas del proyecto.

🟡 `PARTICLES` está anotado como *"reserved"* en la lista (`CMakeLists.txt:132`) aunque el sistema de
partículas existe y se dibuja. Anotación desactualizada.

🟡 Los subsistemas nuevos — **clima, capa granular, construcción, DGS** — no tienen bit propio: van
dentro de `TERRAIN`/`PHYSICS`/`NETWORK` o directamente sin gate.

---

## 6. SISTEMAS DE JUEGO (en el motor)

| sistema | estado |
|---|---|
| **Clima** (`weather_system.{h,cpp}`) | 🟢 función pura de (seed, tiempo, dirección), GL-free, en `haruka_simbase`. La precipitación sale de la cobertura por construcción. 🔴 **sin validar en juego** |
| **Capa granular** (`ground_layer.{h,cpp}`) | 🟢 nieve/arena/barro POR ENCIMA de la superficie de referencia (no la tocan) + huellas por lista acotada. 🔴 **sin validar en juego** |
| **Construcción** (`game/construction/`) | 🟢 núcleo GL-free, validado por el DGS, instancing. 🔴 **sin validar en juego** |
| **Propagación** (`core/propagation.*`) | 🟢 un solo solver para audio (sonido que rodea paredes, IA que "oye") y hechizos serpenteantes |
| **Inventario / items** | 🟢 aspecto = forma + material, preview 3D a FBO. 🟠 falta **textura** por material (hoy color plano) y la **malla procedural orgánica** por semilla (hoy se dibujan como cajas) |
| **Audio** (`audio/`) | 🟢 OpenAL, listener + fuentes de mundo + one-shot |
| **i18n** (`core/locale.*`) | 🟢 |
| **Voz (Vosk)** | 🔴 **retirada.** ✅ Limpiadas las referencias huérfanas de `renderer/model.h` y `ui/settings_panel.cpp`. (El sub-namespace `Haruka::Renderer` nació de una colisión con la clase `Model` que exportaba Vosk; el namespace se queda —es correcto igualmente— pero ya no se justifica por una librería que no está) |
| **Cinematics** | 🟡 existe y lo usa el juego (`Survival/scripts/game_state.h:33`) |
| **DGS / red** (`external/dgs`, `net/default_rules.cpp`) | 🟢 módulo de reglas por proyecto vía `dlopen`; ABI v4 con estado autoritativo por zona |

---

## 7. EMPAQUETADO

✅ **`libHarukaEngine.so` exportaba `main`.** `src/main.cpp` define el `main()` del runtime antiguo y
el glob de CMake lo metía en la librería compartida sin excluirlo:

```
$ nm -D build/libHarukaEngine.so | grep -w main
00000000001da25e T main
```

El ejecutable del juego define el suyo y ganaba (símbolo fuerte en el ejecutable), así que no rompía
nada — pero era un símbolo `main` público en una `.so`, con el punto de entrada de un runtime que ya
nadie arranca así. Ningún `add_executable` lo usaba.

**Arreglado**: `list(FILTER ENGINE_SOURCES EXCLUDE REGEX "src/main\.cpp$")`. Verificado tras
reconstruir: `nm -D` ya no lo encuentra.

🟡 `.gitmodules` declara **tres submódulos** (`imgui`, `nativefiledialog`, `JoltPhysics`). El README
afirmaba lo contrario ("no son submódulos, basta `git clone`"); corregido en esta pasada.

---

## 8. Resumen de lo que tocaría hacer, por coste/beneficio

**Barato y sin riesgo (limpieza) — ✅ HECHO (2026-07-27):**
1. ✅ Borrar del compute la geología muerta + los ruidos que solo ella usaba, y **dejar de subir el
   SSBO de placas y los uniforms 20-25** (§1.9) — el único punto de esta lista que además quita una
   llamada GL potencialmente inválida.
2. ✅ Borrar `calculateHeight` v1 + `s_detailScale` + `layers` de la escena (§1.9).
3. ✅ Borrar `mesh_lod`, `mesh_optimizer`, `skeletal_animation`, los shaders sin referencia y los
   `*.spv` sueltos (§2.4, §2.6).
4. ✅ Excluir `src/main.cpp` del glob de la `.so` (§7).
5. ✅ Quitar las referencias a Vosk que quedaron (§6).

**Barato y con efecto medible — ✅ HECHO (2026-07-27):**
6. ✅ `getWorldParamsCached` → caché por seed, como ya se hizo con el campo (§1.5).
7. ✅ Unificar las dos cachés de `PlanetFields` en una (§1.6).

> **Verificación de los puntos 1-7.** Motor y juego (`Survival`) compilan limpios. Suite completa
> **19460 OK · 0 fallos**. Paridad **en frío** (`HARUKA_DISKCACHE=0`), idéntica a la de antes de la
> pasada: nudo a nudo 0.0001 m medio / 0.0003 m peor; dentro del `keepRadius` **0/200** fuera de 1 cm.
> `parity` GPU↔CPU: mediana 0.0000 m, p99 0.001, max 0.007; nivel de agua 0/4096 fuera de 1 cm.
> `nm -D libHarukaEngine.so | grep -w main` ya no devuelve nada.
> 🔴 **Falta validarlo EN JUEGO** — nada de esto ha cambiado un número, pero el punto 2 tocó el gate
> que decide qué objeto de la escena es un planeta, y eso solo se ve arrancando.

### 8.1 🟡 `ground_stable_on_fast_turn` es INTERMITENTE (hallazgo de esta pasada)

Al pasar la suite en frío salió **1 fallo**: `ground_stable_on_fast_turn`, aserción *"al volver del
cielo SIGUE habiendo malla dibujada bajo el jugador"* (`test_render_gl.cpp:2008`).

**No es determinista y no depende de la caché de disco.** Medido con el MISMO binario:

| | fallos | `sinMallaC` observado |
|---|---|---|
| en frío (`HARUKA_DISKCACHE=0`), 7 ejecuciones | 3 | 0, 0, 0, 0, 1, 2, 2 |
| en caliente, 5 ejecuciones | 2 | 0, 0, 0, 1, 2 |

El test mira al cenit 4 s y vuelve al suelo, y exige `sinMallaC == 0` sobre **540 frames** muestreados
con `sleep_for(3 ms)` por frame: cuenta los frames en los que, en el instante de preguntar, el
streaming asíncrono aún no ha re-emitido la hoja bajo el jugador. Fallan 1 o 2 de 540. Es una carrera
de **reloj de pared** en el test, no una propiedad del motor — el resto de invariantes del mismo test
(LOD mínimo dibujado = 12, salto máximo 0.000 m) salen perfectos en todas las ejecuciones.

⚠️ **No se ha podido establecer un baseline pre-pasada**: `HEAD` no compila por sí solo (el árbol
tenía trabajo sin commitear necesario — `reference_surface.cpp` no encuentra `mesoEnabled`/`fieldsFor`),
así que no hay forma limpia de ejecutar el test "antes". Lo que sí se puede afirmar: falla igual en
caliente que en frío, y los siete cambios de esta pasada **quitan** trabajo del camino de streaming
(menos uniforms por dispatch, un SSBO menos, una generación de campo menos, menos ops por vértice);
ninguno añade latencia. Queda anotado como pendiente de decidir: o el test se hace robusto
(tolerancia, o esperar a que asiente antes de medir) o hay una condición real de residencia detrás.
Un test que falla 1 de cada 3 veces no informa de nada mientras siga así.

### 8.2 🟡 `SIN MALLA` en juego: NO es el presupuesto de caché (medido)

En una sesión real del juego el vigilante de superficies imprimió `SIN MALLA NI DIBUJADA NI
RESIDENTE` de forma sostenida, correlacionado con el vigilante de memoria estrangulando la caché
(3072→2304→1728→1296 MB con ~1.6 GB libres de sistema) y con `sinGenerar` pasando de 0 a decenas por
nivel en el volcado del draw set. La hipótesis obvia era "el techo cae por debajo del conjunto de
trabajo y entra en thrash".

**Se ha medido en vez de suponerlo.** Nuevo diagnóstico `ground_vs_cache_budget`
(`./haruka_tests presupuesto`): mide primero el conjunto de trabajo **caminando**, y luego barre
techos derivados de él, caminando y muestreando cada frame — sin `settle()`, que es justo lo que
`memcut` hace y lo que esconde el efecto. Primer resultado (Tierra, seed 123158, chunkSize 96):

```
conjunto de trabajo CAMINANDO con techo holgado: 262 MB  (gpuChunks=318)
ratio   techo     used  gpu   sinMalla  (sinDatos)   peorDiv   lodMin
2.00     524      271  352      0/360   (   0)       0.00 m    12
1.25     327      271  350      0/360   (   0)       0.00 m    12
1.08     282      271  354      0/360   (   0)       0.00 m    12   <- el régimen del juego
0.75     196      195  314      0/360   (  63)       0.25 m    12
0.50     131      177  153      0/360   (   0)     136.53 m     5
```

**El suelo NO desaparece a ningún ratio**, ni siquiera con la mitad de memoria que el conjunto de
trabajo. Lo que hace la presión de memoria es otra cosa: el LOD dibujado **colapsa 12 → 5** y la
malla dibujada se separa **136 m** de la superficie de referencia (coherente con los 162 m que §1.8
midió con la caché a 32 MB). Degradación grácil, no desaparición.

→ Conclusión: el `SIN MALLA` del juego **no lo explica el techo de la caché**. Los sospechosos que
quedan son la **inanición de GENERACIÓN** (`sinGenerar` alto: los chunks ni existen, que es distinto
de "existen y no se han subido") y el **draw set**. Es el mismo eje del §8.1.

⚠️ **Límite de la medición**: el banco headless tiene un conjunto de trabajo de 262 MB / 318 chunks
frente a los ~1200 MB / ~2000 chunks del juego, y un solo planeta frente a tres compartiendo caché.
No es una reproducción completa — dice que el ratio por sí solo no basta, no que el juego esté sano.
Para cerrar eso está `HARUKA_CACHEMB=N`, que fija el techo y **congela el vigilante de memoria**: sin
él, dos sesiones del juego en la misma máquina no comparan lo mismo.

### 8.3 🟢 Instrumento de la COLA DE GENERACIÓN (`HARUKA_STREAMDBG=1`)

El volcado del draw set dice **cuántas** hojas faltan por generar (`sinGenerar`); no dice **por qué**,
y las razones son bugs distintos con arreglos distintos. `TerrainStreamingSystem::PumpStats` cuenta
los motivos DENTRO del bucle real del `pump` (no en una pasada aparte, que se desincronizaría del que
decide) y `PlanetarySystem::dbgDumpStreamingQueue()` los vuelca. El juego lo imprime junto al draw set
(`vigilarTerreno`); el banco headless, en cada escalón de `presupuesto`.

Lo que separa, y por qué importa cada uno:

| campo | qué significa |
|---|---|
| `FALLIDAS` | ⚠️ `m_failedChunks`: **no se reintentan JAMÁS**. El draw set dibujará un ancestro para siempre |
| `NO ALCANZADAS` | el bucle cortó (sin slot de compute / cupo de concurrencia) antes de mirarlas: caudal, no trabajo perdido |
| `enVuelo` | ya se están generando — sano |
| `starvedMaxAge` | pasadas que lleva la hoja más vieja deseada-y-sin-generar. **El campo que decide** |

⚠️ `starvedMaxAge` alto es AMBIGUO por sí solo, y la primera versión del veredicto se equivocó por
eso: con la caché **en su techo** significa que el conjunto de trabajo no cabe (lo generado se evicta
antes de que llegue el resto — capacidad), y con la caché **holgada** significa que nadie la despacha,
que sí es un bug. El volcado imprime la ocupación y solo dice "trabajo huérfano" en el segundo caso.
Un diagnóstico que grita se deja de mirar.

**Apagado por defecto**: los contadores sueltos son gratis, pero las edades y las "no alcanzadas"
obligan a barrer la cola que queda tras el corte llamando a `hasChunk` por hoja — justo el trabajo por
frame que este subsistema lleva años quitándose de encima (`pump.scan` a 10 ms al encender el meso).

### 8.4 🟡 `SIN MALLA` es INANICIÓN DE **SUBIDA** (RAM→GPU), no de generación — DIAGNOSTICADO

> **Gravedad, acotada por el autor (2026-07-28):** en juego **el terreno se ve estable**; estos
> mensajes salen mientras CARGA. O sea, no es "el terreno está roto" sino "tarda en aparecer" — y la
> ventana en la que tarda es justo la que el guardián reporta. Se arregla por **tiempo de carga y
> estabilidad al moverse**, no por corrupción. Por eso 🟡 y no 🔴.

Sesión del juego con el techo clavado y el instrumento encendido
(`HARUKA_CACHEMB=2048 HARUKA_STREAMDBG=1`). El instrumento descarta la generación de forma limpia:

- **`FALLIDAS = 0 (acum 0)` en TODA la sesión.** Ninguna hoja marcada como no-reintentable.
- **`starvedMaxAge` ≤ 92 pasadas** en el peor momento, y 0 la mayor parte del tiempo. No hay trabajo
  huérfano: lo que se desea se despacha.
- Los cortes que salen son `sinSlotGpu=1` / `presupDisco=17`, o sea caudal normal, y se recuperan.

Y la prueba que lo cierra — pasada 494, con el guardián imprimiendo `SIN MALLA` en ese mismo momento:

```
[cola] pasada=494 · deseadas=1953 · sinGenerar=0
[cola]   NO ALCANZADAS=0 · cortes: sinSlotGpu=0 maxEnVuelo=0 presupDisco=0
[cola]   INANICIÓN: la hoja más vieja lleva 0 pasadas · 0 llevan >60
[cola]   caché 1787/2048 MB (con holgura)
```

**Cero por generar, nada estancado, memoria de sobra — y aun así no hay suelo dibujado.** El draw set
de ese mismo instante dice dónde está el atasco:

```
lod 10: deseadas=184  enGPU=  9  dibujadas=  9  | faltan: enCache=175 sinGenerar=0
lod 12: deseadas=167  enGPU=  0  dibujadas=  0  | faltan: enCache=167 sinGenerar=0
              …con  res=2598 chunks en RAM  y  gpu=564  ·  pend=0
```

Los chunks **están generados y en RAM**; lo que no ocurre es la **SUBIDA a GPU**. Es, palabra por
palabra, el fallo que el propio `hasUploadBacklog()` describe en su comentario: *"`pend=0`,
`sinGenerar=0` y aun así `faltan: enCache=60..100` en CADA nivel… con agujeros en todos los niveles,
`emitSubtree` no puede descender y el planeta entero se dibuja con los ~80 chunks del nivel base"*.
La mitigación existe (la ventana de catch-up se mantiene abierta con backlog de subida) pero **no
basta**, y el instrumento explica por qué al mirar el otro extremo:

**La generación va ordenada por distancia; la subida NO.**

| etapa | orden | dónde |
|---|---|---|
| generación | `std::sort` por distancia² a la cámara, cercano→lejano | `planetary_system.cpp` (`byDist`) |
| **subida** | **orden de hash** de `m_currentFrameChunks`, y `chunksToKeep` ANTES que `chunksToLoad` | `lod_system.cpp` → `processLODUpdate` |

Con `beginUploadFrame(256, 5.0)` el presupuesto real es el de TIEMPO: a 2-3 ms por subida son ~2
chunks por frame. Con ~1500 hojas esperando en RAM y orden de hash, la hoja bajo los pies del jugador
sale en una posición aleatoria de la cola → espera ~750 frames (**~12 s**) de media. Mientras tanto
hay agujeros en todos los niveles, así que el draw set no puede ni descender ni pararse en un
ancestro: bajo el jugador no se dibuja NADA. Encaja con la curva del log (cobertura 8%→100% durante
~30 s) y con que el síntoma desaparezca al llegar a `DIBUJADAS=1941 sinLlegar=0` y vuelva en cuanto
te mueves.

**❌ INTENTO DE ARREGLO, REVERTIDO (2026-07-28).** Se ordenaron las dos listas cercano→lejano en
`LODSystem::updatePlanetLOD`, para que la SUBIDA priorizara igual que la GENERACIÓN. **Rompe el
dibujado**, y la razón es la parte que faltaba del modelo:

> **CERCA = LOD PROFUNDO.** Ordenar por distancia es ordenar de FINO a GRUESO. Pero el draw set
> necesita la PIRÁMIDE: `emitSubtree` solo puede pararse en un nivel que esté EN GPU. Subiendo primero
> lo más fino, los niveles INTERMEDIOS se quedan vacíos y el dibujado no puede ni descender hasta las
> hojas finas ni pararse en un ancestro → no dibuja nada.

Medido en juego (Tierra, techo fijo 2048 MB, durante la carga):

```
lod  5: deseadas=140  enGPU=  0   dibujadas=0   ← agujero en el medio
lod  8: deseadas=180  enGPU=  7   dibujadas=0
lod 12: deseadas=167  enGPU=170   dibujadas=0   ← lo fino, entero en GPU y sin poder usarse
drawSet=4 de 1941 hojas · suelo dibujado a 4.6 km del real
```

El orden de HASH, por accidente, repartía las subidas entre TODOS los niveles y la pirámide siempre
tenía dónde apoyarse. Era mejor por una razón que no estaba escrita en ninguna parte. Revertido; el
razonamiento queda como comentario en `lod_system.cpp` para que no se reintente igual.

**Si se vuelve a atacar**: la prioridad correcta no es la distancia sola — tiene que respetar la
pirámide (un ancestro antes que sus descendientes), o subir por NIVELES de grueso a fino y dentro de
cada nivel por distancia.

⚠️ **Y lo que más pesa de este episodio: la verificación no valía.**

1. La suite dio **19460 OK · 0 fallos** con el motor roto, dos veces seguidas (con el comparador caro
   y con el orden equivocado). Es el mismo agujero del §1.8 — *"encenderlo rompió el juego dos veces
   con la suite en verde"*.
2. El banco `presupuesto` daba la MISMA tabla antes y después, y llegó a mostrar el cambio como una
   mejora (136.53 m → 0.00 m de divergencia en el escalón apretado). Pide 96-264 hojas frente a las
   1941 del juego: no forma la pirámide profunda donde está el problema.
3. Lo único que detectó las dos regresiones fue **ejecutar el juego**.

De paso salió un defecto de rendimiento propio: la primera versión calculaba la distancia DENTRO del
comparador de `std::sort` (~36.000 proyecciones cubo→esfera por planeta y recálculo), que por sí sola
dejaba el draw set sin actualizar. El patrón correcto —decora-ordena-desdecora— estaba diez líneas más
allá, en `byDist`.

---

**Primer hallazgo del banco**, y no es el que se buscaba: a ratio **2.00 y 1.25** —con la caché
holgada, 271/524 MB— una pasada midió `peorDiv` de **102.84 m** con `lodMin = 0`. O sea, la malla
dibujada bajo el jugador cayó a la **raíz de la cara**. Es la firma de "draw set a la raíz", no la de
falta de memoria: ocurrió con el presupuesto MÁS generoso. No es reproducible en todas las
ejecuciones (la pasada anterior dio 0.00 m / lod 12 en esos mismos escalones), lo que lo emparenta
con el intermitente del §8.1. Es el hilo del que tirar.

**Requiere decidir (pendiente):**
8. Superficie de referencia para **todos** los cuerpos, no solo el activo (§1.2) — es lo que hace
   falta antes de que se pueda aterrizar en la Luna.
9. Que cavar no apague el terreno GPU del planeta entero (§1.4).
10. Medir si la cota de los lagos, calculada antes de erosionar, produce láminas de más (§1.7).
11. SSAO: implementar el pase o quitar la clase y el ajuste (§2.3).
12. Objetos de escena: cull + LOD (§2.7).
