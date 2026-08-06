# PLAN LOD v3 — Error en pantalla + Oclusión + Agua dinámica

> Sucesor de `PLAN_LOD_V2.md`. v2 dejó el streaming/quadtree con BodyId, sampler espejo y
> el **decouple de render** (el render dibuja el set de hojas del LOD, no la cadena entera).
> v3 ataca la **raíz** de los artefactos a distancia y del agua: el LOD debe basarse en
> **error en pantalla** y **oclusión**, no en hacks (tope por altitud, esfera de océano,
> log depth). El agua pasa a ser un **fluido dinámico**; los chunks solo **renderizan +
> aportan física**, no deciden el comportamiento.

## 0. Principios (decisiones cerradas)

1. **LOD por ERROR EN PANTALLA**, no por distancia bruta. Un nodo se subdivide hasta que su
   error geométrico proyecta ≤ N píxeles. Acotado por la **resolución de pantalla** (≈1 vért
   por pocos píxeles visibles → ~1M vért máx a 1080p), NO por el tamaño del mundo.
2. **El coste alto NO es el detalle visible — es renderizar lo OCULTO** (cara lejana, tras
   lomas, bajo horizonte). La oclusión es lo que acota el coste. Orden: **oclusión primero**.
3. **Conservador siempre**: la oclusión solo descarta lo *demostrablemente* tapado, con
   margen. Nunca recorta lo visible. (El "recortado por los lados" de intentos previos era
   frustum/horizon mal, NO oclusión.)
4. **GPU-driven, sin readback a CPU.** Nada de `occlusion queries` clásicas (sincronizan
   GPU→CPU = stalls = el "tardaba demasiado" de antes). Hi-z + draws indirectos en GPU.
5. **Sin camuflajes**: fuera el tope por altitud, la esfera de océano y el discard analítico
   de continentes. NO log depth (esconde el problema en vez de arreglarlo).
6. **Agua = simulación dinámica** (flujo, cascadas) sobre los chunks. Los chunks dan
   geometría + colisión; la simulación decide nivel/flujo. Integrar con el sistema de fluidos
   existente (XPBD softbody + híbrido ocean/rivers/PBF — ver `fluid_softbody_system`).

## 1. Hacks actuales a eliminar (qué y por qué)

| Hack | Dónde | Por qué existe | Lo sustituye |
|---|---|---|---|
| Tope por altitud | `lod_system.cpp::altitudeMaxLOD` | evitar rendir medio planeta fino desde altura | LOD por pantalla + oclusión |
| Esfera de océano | `application_render.cpp` (pase agua) + `ocean_sphere.*` | tapar que el agua por chunks se ve a bloques lejos | LOD por pantalla correcto → chunks honestos |
| Discard analítico de continentes | `ocean_sphere.frag` (fBm + seaThreshold) | la esfera adivina la costa (depth roto a escala planetaria) | oclusión real del terreno (sin esfera) |
| (NO meter) log depth | — | "arreglar" precisión de profundidad | no hace falta si el LOD/oclusión están bien |

## 2. Fases (incremental, medible en cada paso)

### F0 — Baseline y métricas  *(no toca render)*
- Capturar perf actual: profiler (`renderFrameContent`, `terrain.draw`, `planetary.update`),
  `lodcheck` (overlaps/holes/balance), ms/frame, `res`/`mem`, en 3 escenarios: **suelo**,
  **vuelo bajo**, **órbita**.
- Definir el **presupuesto objetivo** (p.ej. ≤ X ms/frame en cada escenario) y el **error en
  píxeles** objetivo (p.ej. ≤ 2–3 px).
- **Salida**: tabla de baseline + objetivos. Sin esto no se valida nada.

### F1 — Métrica de split por error en pantalla
- Reemplazar `distSplit = dist < node.size * splitFactor` por **error proyectado**:
  `screenError(node) = (node.geomError / dist) * (viewportHeight / (2·tan(fov/2)))`; split si
  `screenError > N px`. `geomError` ≈ tamaño de detalle que el nodo NO captura (≈ node.size /
  res, o el delta vs el padre).
- **Mantener de momento** horizon+frustum cull (ya conservadores) y, como red de seguridad
  temporal, un tope de LOD MUY alto (no el agresivo actual).
- **Medir**: a igual error en píxeles, ¿desaparecen los bloques a distancia? ¿cuánto sube el
  coste sin oclusión nueva? (esperado: sube en órbita — lo arregla F3).
- **Riesgo**: en órbita sin oclusión, coste alto. Por eso F2/F3 van pegadas.

### F2 — Validar con culls EXISTENTES (lo seguro primero)
- Con F1 puesto, medir cuánto quita ya el **horizon cull** (la cara lejana = LA oclusión
  gorda) + frustum. Posible que sea casi suficiente.
- Afinar márgenes de horizon/frustum (ya mejorado para chunks grandes) → **cero recorte de
  visible**. Verificar visualmente: nada falta por los lados.
- **Decisión**: si el coste ya entra en presupuesto → F4 (no hace falta hi-z aún). Si no → F3.

### F3 — Oclusión local hi-z (GPU-driven, solo si F2 no basta)
- Hi-z: pirámide de profundidad jerárquica del depth (pase previo o frame anterior).
- Cull de chunks por su **AABB contra el hi-z** en un **compute pass** → draws **indirectos**.
  Cero readback CPU.
- **Conservador**: margen + manejo de latencia de 1 frame (re-test / depth pre-pass del frame
  actual) → nunca descarta algo que acaba de hacerse visible de forma persistente.
- **Medir**: ahorro real (chunks descartados) vs coste del hi-z. Verificar **0 huecos** al
  girar rápido (el fallo clásico).
- Encaja con la decisión D de v2 (**GPU-driven SSBO + indirect**).

### F4 — Quitar la esfera de océano + discard analítico
- Con F1–F3, el agua por chunks ya se ve correcta a distancia (sub-píxel) → fuera la esfera y
  el discard analítico de `ocean_sphere.frag`. Fuera el corte `kWaterNearRadius`.
- El agua vuelve a ser **solo chunks** (honesta, sigue la costa exacta del terreno).
- **Medir**: costa exacta a toda distancia, sin moteado ni diamantes, sin esfera.

### F5 — Capa de fluido dinámico
- El agua deja de ser geometría estática a nivel del mar: **simulación** (nivel, flujo,
  cascadas en desniveles) sobre los chunks. Los chunks **renderizan** la lámina que la sim
  decide y **aportan colisión**; no deciden el nivel.
- Integrar con `fluid_softbody_system` (ocean Gerstner / rivers shallow-water / PBF).
- **Asíncrono** al terreno (la sim corre aparte; el chunk solo muestra el estado actual).

## 3. Métricas de validación (por fase)
- **Perf**: ms/frame y filas del profiler en suelo/vuelo/órbita, vs baseline F0.
- **Correcto**: `lodcheck` → overlaps/holes/balance = 0 en estado estable.
- **Sin recorte**: inspección visual — nada falta por los lados al girar (cazar over-cull).
- **Sin pop**: girar rápido no deja huecos > 1 frame (latencia hi-z controlada).

## 4. Riesgos y mitigaciones
- **Over-cull (faltan trozos)** → conservador + margen; validar visualmente cada fase.
- **Stalls (lento)** → nada de occlusion queries con readback; todo GPU-driven indirecto.
- **Latencia hi-z (pop al girar)** → re-test / depth pre-pass; margen temporal.
- **Coste F1 sin oclusión** → no soltar F1 a producción sin F2/F3 medidos.

## 5. Estado
- **F0**: hecho. Medido: F1 cuesta **~nada** (~0.3ms) vs baseline en los 3 escenarios. El
  **horizon cull existente ya hace la oclusión gorda** → el split por pantalla solo subdivide
  lo visible (acotado por píxeles). Overlaps solo en body 1 (Luna lejana, irrelevante).
- **F1**: hecho y **POR DEFECTO** (`m_screenSpace=true`). Mata el tope por altitud. `lodscreen
  off` vuelve a baseline para comparar. Detalle lejano mejor confirmado por el usuario.
- **F2**: hecho DE VERDAD. Corrección: el horizon cull del RENDER NO abarata el RECOMPUTE del
  LOD (`updatePlanetLOD` subdividía la esfera entera = pico de **179ms** en CPU al volar). Fix:
  **frustum + horizon cull DENTRO de `recursiveProcess`** (`m_cullPlanes` vía `setCullMatrix`,
  1 frame de desfase) → solo refina lo VISIBLE → `planetary.update` **179ms → 0.95–8ms**. El
  minLOD floor cubre el resto a grueso (nada desaparece). **F3 (hi-z local) NO hace falta.**
- **F4**: hecho. Fuera la **esfera de océano** (draw + corte 12km + discard analítico). El agua
  es SOLO chunks a toda distancia (fina por F1). Costa EXACTA sin depender del depth:
  `waterParams.y` (profundidad) **con signo** + `water.frag` descarta `Depth ≤ 0` → recorte en
  el contorno del nivel del mar, sub-celda. Skirts del agua reducidos (40m→6m, no asoman).
  Olas confirmadas de cerca (sub-píxel lejos = correcto). `ocean_sphere.*` = código muerto.
  **F2.b (extra)**: balance 2:1 NO cascadea fuera del frustum (`leafVisibleForBalance`) → mató
  la explosión de chunks en el borde del frustum (pico 188ms → 0.5–0.8ms).
- **F5** (fluido dinámico): pendiente.

## 6. OPTIMIZACIÓN RAM + VRAM (ordenado por ROI: impacto alto / esfuerzo bajo primero)

Hechos verificados en el código:
- `ChunkData.colors` NO se usa en el render del terreno → se genera y cachea para nada.
- Los índices son **topología FIJA por `res`** (`i, i+1, i+res+1…`) → todos los chunks del
  mismo `res` tienen el MISMO índice (hoy duplicado miles de veces).
- Atributos subidos como `float32` vec3/vec2 (pos, normal, morphTarget, morphNormal, uv).

### Progreso (2026-06-23)
- ✅ **#1 índice compartido por res** (1 EBO, no por chunk) — RAM+VRAM.
- ✅ **#4 normales + morphNormals + uv empaquetadas** (`INT_2_10_10_10` + half-float) → **−20 B/vértice**
  (~36% RAM+VRAM). Seguro, sin cambio de shader.
- ⏳ **#4 posiciones** (16-bit SNORM ×3 relativo a bbox, GL_SHORT/normalizado) → RE-APLICADO con la
  **corrección de la sospecha (a)**: la bbox ahora se calcula sobre **vértices Y morphTargets**
  (terrain_generator.cpp), así ningún morphTarget se recorta al morphar de cerca (era el fallo de
  "mirar al suelo"). `bbCenter` se suma al `u_chunkOffset` en CPU a doble precisión; el shader
  (planet.vert, `u_bbRadius` loc 13) escala el SNORM. vertices/morphTargets se LIBERAN tras empaquetar
  (nadie más los lee; física = muestreo analítico). −6 B/vértice extra (pos) sobre los −20 de normales+uv.
  PENDIENTE: que el usuario confirme mirando al suelo de cerca + cine/órbita. Si aún falla → era (b)
  offset, o el cambio de TERRENO (hipótesis del usuario), no el empaquetado.

### Orden de ataque (mejor ROI arriba)
| # | Optimización | Impacto | Esfuerzo | Memoria |
|---|---|---|---|---|
| 1 | **Índice compartido por `res`** (1 EBO para todos, no por chunk) | Alto | Bajo | RAM + VRAM |
| 2 | **Quitar `colors`** del ChunkData (verificar física/colisión antes) | Medio | Bajo | RAM |
| 3 | **Cap de cache menor** (`chunkmem`, o auto <25%) — palanca | Directo | Trivial | RAM |
| 4 | **Empaquetar atributos**: normal/morphNormal `INT_2_10_10_10` (4B vs 12), uv half (4 vs 8), pos/morph 16-bit relativo a chunkCenter (6 vs 12) | Alto | Medio | RAM + VRAM |
| 5 | **F3 oclusión hi-z**: no subir/refinar terreno tapado por lomas (dentro del frustum) | Medio | Medio | RAM + VRAM |
| 6 | **Purga de VRAM**: quitar mallas no dibujadas; mantener la cadena en cache RAM | Alto | Medio* | VRAM |
| 7 | **Regenerar en vez de cachear** (el compute GPU es rápido) → cambiar RAM por cómputo | Alto | Medio | RAM |
| 8 | **`morphTarget` como delta** (target−pos es pequeño → empaqueta mejor) o flag+computar | Medio | Medio | RAM + VRAM |
| 9 | **GPU-driven**: 1 SSBO grande + `glMultiDrawElementsIndirect` en vez de N VAOs | Alto | Grande | RAM + VRAM |

*Purga de VRAM necesita el fix **gate-via-cache**: el descenso del LOD debe consultar la
ChunkCache (RAM), no la residencia GPU — si no, purgar mallas rompe el descenso (intento previo
falló por esto). Ver el incidente en la sesión 2026-06-23.

### Lista RAM (ChunkCache) — por ROI
1. Índice compartido (no cachear índices). 2. Quitar `colors`. 3. Cap menor. 4. Empaquetar lo
cacheado. 5. Regenerar-vs-cachear. 6. `morphTarget` delta. 7. (GPU-driven reduce duplicación).

### Lista VRAM (mallas GPU) — por ROI
1. Índice compartido (1 EBO). 2. Empaquetar atributos. 3. F3 oclusión. 4. Purga de VRAM
(gate-via-cache). 5. `morphTarget` delta. 6. GPU-driven SSBO + indirect.

## 7. Otros pendientes
- **Pulir agua a media distancia**: aún se ve algo "por chunks" (parches) — cobertura/morph del
  agua a LOD intermedio. Cosmético, no crítico.
- ~~Limpiar `ocean_sphere.*`~~ — HECHO (shaders borrados + creación/miembros fuera).
- Decisiones 0.1–0.6 cerradas con el usuario (2026-06-23): error en pantalla, oclusión
  primero, conservador, GPU-driven sin readback, sin log depth, agua = sim.
