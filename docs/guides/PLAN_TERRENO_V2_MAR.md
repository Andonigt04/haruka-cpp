# Plan: generación de terreno v2 ESTABLE CON EL MAR

Objetivo: cerrar la saga de la costa/agua (~17 iteraciones de tiles/diamantes, agua-sobre-tierra,
flicker/panales, discos, llano tostado, rectángulos azules) con un rediseño *mask-first* donde el
mar es estable a cualquier LOD. Ver memoria `perf_terrain_chunksize_rootcause` (historial) y
`terrain_gen_v2_plan` (pipeline completo). Probar SIEMPRE en dev + mundo nuevo (`dev_vs_dist_hmap_trap`).

## Diagnóstico raíz
Todos los fallos = **la lámina de agua y la línea de costa están cuantizadas a la celda de la malla
del chunk**, y "¿mar o tierra?" va acoplado a la altura. A LOD grueso submuestrea → tesela/parpadea.
En continentes planos, `c≈c0` sobre un área ancha → contorno difuso → checkerboard de chunks mar/tierra
(los "rectángulos azules") + llano tostado sin relieve.

## 3 pilares de estabilidad
1. **Decisión mar/tierra = `landMask`, MUY baja freq, suave, SEPARADA de la altura.** Baja freq →
   idéntica a cualquier LOD → estable. Clamp de ambos lados (tierra ≥ +ε, mar ≤ −ε) mata islotes
   sub-celda que flipan (flicker). *(Hecho iter 17.)*
2. **Costa nunca cuantizada → contorno per-píxel exacto.** La costa = curva `elev(dir)=0` evaluada
   por píxel (`oceanElevKm`, ya portado). Malla de agua con margen, el shader recorta al contorno
   exacto → sin borde poligonal a ningún LOD. *(Hecho; falta quitar el gate por preset — en Low se
   apaga y por eso salen rectángulos. Debe ser siempre-on.)*
3. **Costa con RELIEVE garantizado → `distToCoast`, no el valor de `c`.** El llano sale porque el
   relieve se gatea por el VALOR de `c` (~constante en llanos). Solución: gatear por **`distToCoast`**
   (distancia mundo a la línea `landMask=0.5`), un campo con gradiente REAL siempre → la tierra
   SIEMPRE sube desde la orilla y el lecho SIEMPRE baja, por muy plano que sea `c`. Rompe el llano.

## Micro-sistema `distToCoast` (parity-safe)
`distToCoast(dir)` ≈ distancia MUNDO (con signo: >0 tierra, <0 mar) a la costa:
```
gmag = |∇_tangente c|   // magnitud del gradiente tangencial del ruido de continente
distToCoast_dir = (c - c0) / max(gmag, eps)     // en unidades de dir (esfera unidad)
distToCoast_world = distToCoast_dir * planetRadius
```
**Gradiente por DIFERENCIAS FINITAS con el MISMO epsilon en los 3 sitios** (terrain_gen.comp,
terrain_sampler_v2.cpp, water.frag) → paridad por construcción (autodiff en CPU vs finito en GPU
divergiría en la costa → colisión rota). Base tangente ortonormal determinista de `dir`; 4 muestras
extra de `c` (±t1, ±t2). Coste: +4 fBm/punto en gen (una vez por vértice) y en la franja de costa
del fragment (gateado). Aceptable.

Uso: gatear la subida de tierra y la bajada del lecho por rampas sobre `distToCoast_world`
(p.ej. `smoothstep(0, 1500m, distToCoast)` para el relieve costero de tierra; análogo negativo para
el lecho). Reemplaza el parche "colinas por landMask".

## Fases (verificables, dev + mundo nuevo, lodcheck=0 overlaps/holes)
1. **`distToCoast` finito + gate del relieve costero por él** (rompe el llano). ← EMPEZAR AQUÍ.
2. **Contorno per-píxel siempre-on** (quita gate por preset) → mata rectángulos en Low.
3. **Unificar lámina de agua** (nivel como parámetro: océano=0 global / lago=L local, un solo camino).
4. **(Opcional, mayor esfuerzo)** normales analíticas (gradiente del ruido) → mata pinchos/aliasing
   de raíz; sustituiría el finito de distToCoast por analítico (más barato y exacto).

## Estado
- Fase 1 (distToCoast) ✅ — costa con relieve (ladera al mar), confirmado por captura.
- Fase 2 (pxCoast siempre-on) ✅ — agua recortada per-píxel al contorno también en Low (antes rectángulos).
- "Terreno desaparece al mover cámara" ✅ ARREGLADO — el LOD refina solo en frustum (F2) y solo
  recalculaba al TRASLADAR → al ROTAR la zona revelada quedaba con el set viejo (grueso/vacío). Fix
  (planetary_system.cpp): recompute también cuando la vista cambia (Σ|Δ cull VP| > 0.02); el recompute
  es ~1ms sin allocs → asumible. Pendiente validar por el usuario.
- Fase 3 (unificar lámina agua) ✅ RESUELTO con **superficie única de océano** (`WaterRenderer::renderSingleOcean`,
  DEFAULT; toggle `oceanshell off` = por-chunk). Rejilla a nivel del mar que sigue a la cámara (densa
  cerca, hasta el horizonte), recorte de costa per-píxel (`u_oceanSurface=1` → `oceanElevKm` en todos los
  píxeles) + oclusión por depth del terreno. Aprobado por el usuario desde órbita ("perfecto", sin teselas).
  Cull agua↔terreno: el agua por-chunk (fallback) usa el set POST-CULL del terreno (m_drawnByPlanet).
  DEFERIDO (el usuario dijo "después"): oleaje ROTO (las olas Gerstner usan plano tangente anclado a cámara
  → se rompen lejos en la superficie grande; necesita param por-vértice o esférico); costa ABRUPTA (sombreado
  fijo profundidad 50m, sin degradado somero); no llega al LIMBO en órbita (rejilla tangente cubre ~22°,
  no el hemisferio → param esférica o cap). Optimización pendiente: cull de geometría (colapsar vértices
  sobre tierra por landMask grueso) para no evaluar per-píxel sobre tierra.
- Fase 4 (normales analíticas) — pendiente (mata pinchos + reemplaza el finito de distToCoast).
- OBJETIVO (usuario): agua = FLUIDO DINÁMICO + splash. Estado F5:
  - **F5.0 ✅** (ya estaba) — sim acoplada al océano: world.cpp fija `m_hybrid->seaLevelAlongUp = pR-|ancla-pC|`
    por frame → ríos desembocan, cavar junto a la costa se llena.
  - **F5.1 ✅ primer cut** — el océano único SIGUE a la sim cerca del jugador. Callback engine↔juego
    (`WaterRenderer::setFluidSampler` ← `PlanetarySystem::setWaterFluidSampler` ← World). Por vértice del
    mar: si `m_water->containsWorld`, desplaza el radio por `(surfaceAlongUpAtWorld − seaLevelAlongUp)·blend`
    (fade en el borde del parche). En mar abierto delta≈0 (sim=nivel mar); dinámico en costa/ríos/cavar.
    PENDIENTE validar en runtime (la sim solo corre en PLAYING). Si el fade/escala chirría, ajustar.
  - F5.2 (cascadas PBF sobre el planeta) — el emisor de rebose ya existe (world.cpp overflow); verificar.
  - F5.3 (doble superficie sim↔océano) — evitar z-fight de la lámina shallow-water con el océano único.
  - Splash: fase 1 (objetos→mar) hecha; falta jugador + olas/cascadas.
