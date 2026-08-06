# PLAN CUEVAS + ISLAS FLOTANTES — features 3D sobre el heightfield

> Refina dos sistemas 3D que NO son heightfield (el terreno base sí lo es y no puede tener
> aire debajo — ver `floating_islands.h`). **Cuevas** = módulo nuevo. **Islas flotantes** =
> refinamiento de `floating_islands.*` (hoy un blob único). Ambos: features localizadas,
> sembradas por celdas deterministas, streameadas cerca de cámara (como lagos/islas). Probar
> SIEMPRE en dev + mundo nuevo (`dev_vs_dist_hmap_trap`) y con `haruka_tests`.

## 0. Principios (decisiones cerradas con el usuario, 2026-07-05)

1. **El heightfield es el suelo; cuevas e islas son módulos 3D aparte.** El heightfield no
   representa overhangs (misma razón que las islas ya son malla propia). Nada de meter aire en
   `elev(dir)`.
2. **Determinismo total**: función pura de `(seed, celda)`. Reproducible, streameable, testeable.
3. **Cuevas = malla marching cubes + COLISIÓN ANALÍTICA.** El jugador entra y anda dentro. La
   malla es visual; la colisión muestrea el **campo de densidad** `caveDensity(worldPos)`
   (analítico, con **paridad CPU/GPU** como el heightfield). *(Decisión del usuario: opción 1.)*
4. **Todos los huecos CONECTADOS.** Garantía por post-proceso (union-find + tallado de túneles),
   no confiada al AND de los dos mapas (ver §1.3). Invariante verificado por test.
5. **Tests en `haruka_tests.cpp`** (banco del motor real, headless): determinismo, conectividad,
   paridad CPU↔GPU del campo, boca-rompe-superficie, cordura de islas. *(Decisión del usuario.)*
6. **Islas = trozos de mundo arrancados**: cima = mini-terreno real (colinas + montañas + lagos,
   reusa el vocabulario de `sampleTerrainV2`), fondo = bosque de raíces puntiagudas concentradas
   al centro. Sustituye el blob copa-plana/cono-único actual.

---

## PARTE A — CUEVAS

### A.1 Representación y marco local
Cada **sistema de cuevas** vive en una **caja local bajo un parche de superficie**, sembrado por
celda (rejilla cellular jittered sobre la esfera, como `nearestLakeCell`). Marco local en el
centro de celda `c`:
- `n` = radial (arriba) = `normalize(c)`.
- `t, b` = base tangente ortonormal determinista de `n` (misma construcción que `distToCoast`).
- `depth` = profundidad bajo la superficie a lo largo de `-n` (m). La caja cubre `depth ∈ [0, Dmax]`
  y un radio tangencial `Rc` (p.ej. Dmax ~ 80–300 m, Rc ~ 100–400 m por seed).

### A.2 Los dos mapas de estructura (tu método)
Campo de aire como **intersección de dos extrusiones 2D** (barato: 2 ruidos 2D, no un 3D):

```
air(t,b,depth) = M_top(t,b) · M_side(s,depth)  >  τ        // continuo, umbralizado (paredes suaves)
```
- `M_top(t,b)` — proyección **CENITAL** (plano tangente): huella en **planta** de los túneles.
- `M_side(s,depth)` — proyección **LATERAL** (un eje tangente `s=t` × profundidad): **perfil
  vertical** (qué niveles/estratos tienen hueco, cuánto baja).
- `τ` = umbral de porosidad (knob por seed → cuevas amplias ↔ túneles estrechos).

Genera cada mapa como **red de corredores conexa en su propio plano** (p.ej. `1 − |ridged fBm|`
umbralizado, o splines rasterizados) para MAXIMIZAR conectividad de partida — pero NO basta (§A.3).

`caveDensity(worldPos)` = campo firmado usado por malla y colisión: `>0` roca, `<0` aire.
Aproximación: `caveDensity = τ − M_top·M_side` (+ el tallado de túneles de §A.3 restado).

### A.3 Conectividad garantizada (post-proceso, NO confiado al AND)
La intersección de dos mapas conexos **no** es conexa en general → bolsas aisladas inalcanzables.
Se impone así, por sistema de cuevas, en su grid de vóxeles local:

1. Rasterizar `air` en el grid local (resolución del vóxel ~ Dmax/N).
2. **Union-find / flood-fill 26-conexo** → componentes conexas del aire.
3. Quedarse con la **componente MAYOR**. Para cada bolsa aislada: **tallar un túnel** (segmento/
   spline entre centroides, radio restado a `caveDensity`) hasta la principal. Bolsas < ε vóxeles:
   descartar (rellenar de roca) para no dejar huecos-punto.
4. Resultado: **una sola componente conexa** garantizada → invariante del test `caves_connected`.

**Alternativa (si el AND+post-paso da cuevas feas):** generar la red como **grafo de nodos +
árbol de expansión mínimo (MST)** y `air = dist_a_aristas < radio`; conectividad por construcción,
y los dos mapas quedan como **modulación de forma** encima del grafo. Mantener como plan B.

### A.4 Entradas — perforar el heightfield
El heightfield no hace el overhang de la boca. Acoplamiento cueva↔terreno = **un único mask**:
- `terrain_gen` (CPU `terrain_sampler_v2` y GPU `terrain_gen.comp`, en paridad) expone
  `caveEntranceMask(dir)`: 1 donde el volumen de aire **rompe** la superficie (`air` a `depth≈0`).
- El shader del terreno **descarta** los fragmentos dentro de la huella de la boca; la malla de
  cueva aporta techo/labio/paredes que tapan el borde. Sub-celda, per-píxel (como la costa).

### A.5 Malla (marching cubes) y streaming
- **Marching cubes / dual contouring** sobre `caveDensity` en la caja local → malla de paredes.
  Solo para sistemas de cuevas cuyo chunk esté cerca de cámara (streaming-lite, como
  `generateFloatingIslandsNear`). Cache + cota de seguridad de mallas/draws.
- Normales del gradiente de `caveDensity` (analítico o finito consistente).

### A.6 Colisión analítica (paridad CPU/GPU)
- Colisión = muestreo de `caveDensity(worldPos)` (signo) + su gradiente → resolución de
  penetración, como la colisión del terreno. **NO** se colisiona contra la malla.
- `caveDensity` debe tener **paridad CPU↔GPU** (misma fórmula de `M_top`, `M_side`, τ y el
  tallado de túneles en ambos lados) → test `caves_parity`.

### A.7 Ficheros nuevos / tocados (cuevas)
- **NUEVO** `src/core/terrain/cave_system.{h,cpp}` — siembra por celda, dos mapas, union-find +
  tallado, `caveDensity`, `caveEntranceMask`, marching cubes.
- **NUEVO** `assets/shaders/cave_density.glsl` (o funciones en `terrain_gen.comp`) — espejo GPU.
- `terrain_sampler_v2.*` / `terrain_gen.comp` — añadir `caveEntranceMask` al gate del terreno.
- `planet.frag` — discard por la boca.
- Renderer — pase de mallas de cueva (análogo a `floating_island_renderer`).

---

## PARTE B — ISLAS FLOTANTES (refinar `floating_islands.*`)

Hoy `generateIslandMesh` = **blob único**: copa achatada plana + cono dentado, 1 ruido. Se separa
en **dos superficies independientes** sobre un disco local de radio R (marco tangente en
`worldCenter`):

### B.1 Cima — mini-terreno real (no copa plana)
Reusa el vocabulario de `sampleTerrainV2` en coordenadas locales del disco `(u,v)`, `r=|(u,v)|`:
```
hTop(u,v) = domo_base(r)                         // perfil de disco (cae al borde)
          + hills     : fBm                       // colinas onduladas
          + mountains : pow(1−|ridged|, e)·mtnMask // montañas de verdad, gateadas localmente
          − lagos     : cuencas talladas + nivel de agua local
```
- Lagos → cada uno con su lámina (reusar water shell / un quad de agua por lago).
- Extensible: "diferentes cosas como montañas de momento" → `hTop` es el sitio para props,
  minerales, biomas más adelante.

### B.2 Fondo — raíces puntiagudas concentradas al centro
Sustituye el cono único por un **bosque de estalactitas** denso en el centro, ralo en el borde:
```
hBot(u,v) = − concentración(r) · campoSpikes(u,v)
concentración(r) = smoothstep(R, 0, r)^k          // pico en r=0, →0 en el aro
campoSpikes(u,v) = pow(ridged_fBm, p)             // afiladas (p alto = más punzante)
                 + unas pocas spikes grandes discretas (Voronoi)
```

### B.3 Malla y colisión
- Rejilla del disco superior (`hTop`) + rejilla inferior (`hBot`), cosidas en el aro `r=R`
  (ambas → 0). Normales por gradiente o por promedio de caras (como ahora).
- Colisión = muestreo de `hTop`/`hBot` (analítico). **Sin** paridad GPU (la isla es malla propia,
  no compute).
- Mantener `generateFloatingIslandsNear` (streaming + cota `MAX_ISL`) tal cual; solo cambia la
  construcción de la malla.

### B.4 Ficheros tocados (islas)
- `src/core/terrain/floating_islands.{h,cpp}` — `generateIslandMesh` → `hTop`/`hBot` + lagos.
- `floating_island_renderer.*` — si se añaden lagos/material de cima.

---

## FASES (incrementales, verificables; dev + mundo nuevo; `haruka_tests` verde)

### F0 — Baseline
- Confirmar que islas actuales siguen bien; capturar coste de `generateFloatingIslandsNear`.

### F1 — Islas: cima mini-terreno + raíces centradas  *(reusa código, resultado visible ya)*
- `hTop` (colinas + montañas) y `hBot` (raíces concentradas). Sin lagos aún.
- **Medir**: silueta correcta, sin agujeros en el cosido del aro; coste de malla acotado.

### F2 — Islas: lagos en la cima
- Cuencas + lámina de agua local. **Medir**: agua no se derrama por el borde.

### F3 — Cuevas: campo `caveDensity` + dos mapas (solo CPU, sin malla)
- `cave_system.cpp`: siembra por celda, `M_top`, `M_side`, τ, caja local. Sin conectividad aún.
- **Test** `caves_determinism`: mismo `(seed,celda)` → mismo campo.

### F4 — Cuevas: conectividad (union-find + tallado)
- Grid local, flood-fill, componente mayor, tallado de túneles, descarte de bolsas < ε.
- **Test** `caves_connected`: el aire de un sistema = **1 sola componente conexa** (invariante duro).

### F5 — Cuevas: marching cubes (malla) + streaming cerca de cámara
- Malla de paredes; pase de render; cache + cota de draws. **Medir**: sin fugas de mallas, coste
  acotado al volar.

### F6 — Cuevas: perforar el heightfield (bocas)
- `caveEntranceMask` en CPU **y** GPU (paridad); discard en `planet.frag`.
- **Test** `caves_entrance`: la boca coincide con el aire a `depth≈0`; **Test** `caves_parity`:
  `caveEntranceMask`/`caveDensity` CPU↔GPU coinciden (mediana del error acotada, como el parity
  del heightfield).

### F7 — Cuevas: colisión analítica
- Muestreo de `caveDensity` en la colisión del jugador. **Verify**: entrar y andar dentro (no se
  cae ni atraviesa). Test de cordura: un punto marcado aire tiene `caveDensity<0` y roca `>0`.

## Tests a añadir a `haruka_tests.cpp` (patrón existente: `static void test_*`, `g_pass/g_fail`, filtro `want`)
- `test_caves_determinism()` — puro CPU (como `test_sampler_determinism`).
- `test_caves_connected()` — union-find sobre el grid → nº componentes de aire == 1.
- `test_caves_parity()` — CPU↔compute-GPU de `caveDensity`/`caveEntranceMask` (junto a
  `test_cpu_gpu_parity`, ya hay contexto GL + compute cargable).
- `test_caves_entrance()` — la boca ⇔ aire a `depth≈0`.
- `test_islands_sanity()` — `hTop>hBot` dentro del disco; cierre en el aro `r=R`; determinismo.
- Enganchar en `main()`: filtros `caves` / `islands` en el `want(...)` (línea ~470).

## Riesgos
- **AND no conexo** → mitigado por el post-paso (F4) + plan B (MST). El test es la red de seguridad.
- **Divergencia CPU/GPU del campo** (colisión rota) → misma fórmula en ambos + `caves_parity`.
- **Coste de marching cubes al volar** → streaming-lite + cache + cota, como islas.
- **Boca del heightfield mal recortada** (borde poligonal) → discard per-píxel + labio de la malla.

## Estado
- Diseño cerrado (2026-07-05). Decisiones del usuario: cuevas MC + colisión analítica; tests en
  `haruka_tests`; empezar por escribir este plan. Implementación: pendiente (F1 = empezar aquí).
