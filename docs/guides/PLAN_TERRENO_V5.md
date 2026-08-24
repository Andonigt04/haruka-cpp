# PLAN TERRENO v5 — Quadtree sobre cube-sphere con heightmaps de nodo en GPU

> Sucesor del v4 (clipmap con teselación hardware). El v4 acertó en lo grande —generar en la
> `tese` hace que el almacenamiento sea **cero**, y por eso los chunks horneados a disco se
> descartaron (orden de TB)— pero dejó **media solución**: resuelve el campo cercano y deja el
> lejano a un mecanismo que cuesta más que la geometría que sustituye.
>
> v5 conserva el motivo del v4 (coste de disco cero) y le quita la costura: **una sola
> representación del suelo a la órbita**, con el detalle **cacheado por nodo en GPU** en vez de
> re-evaluado por vértice y por píxel en cada frame.

---

## 0. Por qué, con los números que lo justifican

Medido en sesión (2026-08-22), escena a ras de suelo:

```
present.swap (espera GPU, vsync OFF, cap 60 fps) ..... 60 ms   ← el frame
renderFrameContent (CPU) ............................. 1,65 ms
```

**El CPU es el 2,7 % del frame.** Todo lo que se optimice en CPU es ruido. El coste está en la
GPU, y el sospechoso está documentado en el propio `biome.frag`, con dos precedentes medidos:

- `reanchorToFine` — sphere-trace de **2-16 pasos POR PÍXEL**, cada paso una `harukaTerrainDetail`
  completa. Puso `present.swap` en **42 ms** una vez.
- El raymarch de sombra — **24 pasos**, y al perder una salida temprana el frame se fue a **~280 ms**.

⚠️ **Y el sphere-trace solo corre FUERA del clipmap.** Su propio comentario lo dice: *"El clipmap NO
se toca: dentro de su caja la rejilla fina ya da el relieve real"*. O sea que **los 60 ms son
literalmente el precio de la costura**: existen porque al otro lado hay una malla base con parches
de 39 km y tope de tesela 64 → quads de ~610 m, incapaz de llevar relieve, que hay que fingir por
píxel.

### Los cuatro parches de una sola frontera

| Parche | Dónde | Qué tapa |
|---|---|---|
| Sphere-trace por píxel | `biome.frag::reanchorToFine` | la malla base no lleva detalle |
| Banda de mezcla (70-95 %) | `planet.cpp` ClipParams + `clipmap.tese` | el salto de `rad·0.002` a `camD·0.012` (6×) |
| Sesgo de profundidad | pase del clipmap | tres superficies COPLANARES dibujando el mismo suelo |
| Redondeo a potencia de dos | `clipmap.tesc::edgeFactor` | que la colisión pueda compartir vértices con el render |

Existe una vista de depuración (`HARUKA_PLANET_DEBUG=9`) cuyo único propósito es saber **cuál de las
tres superficies pintó un píxel**. Una solución correcta no la necesita.

### Los dos agujeros, que son distintos

**Cerca** — la costura cae dentro de lo que miras (confirmado: la vista 9 muestra rojo *y* verde en
pantalla) y el sphere-trace cuesta los 60 ms.

**Lejos** — a partir de **119 km de cámara** (`triM = camD·0.012` cruza el early-out en 1428,5) **no
hay ningún detalle procedural**, y el relieve per-píxel ya se apagó antes (rampa `kFineFade` 40 km →
`kFineEnd` 90 km). Desde órbita el planeta es **solo el bake**: 4,9 km/téxel a 8192², 10,7 km a
4096². Y hay un **hueco de escala** que nadie cubre:

```
el bake resuelve        >= ~5-10 km   (su téxel)
la escalera de octavas acaba en λ 2857 m
                        -----------------------
sin fuente:             de ~3 km a ~10 km
```

Esa banda vacía es justo la que da forma a un continente visto desde arriba.

### Lo que NO es el problema (descartado midiendo, para no volver a mirar ahí)

- **El tamaño del quad del clipmap.** Es **4,1 px constante** a cualquier distancia ≥ 1 km — la
  métrica angular funciona. Lo más basto en pantalla es el campo *cercano* (20,6 px a 200 m, donde
  satura el tope de 32).
- **El coste de CPU del muestreo de colisión.** `terrain.rings.sample` ni aparece en el profiler
  (pico de ~30 ms al reconstruir, en un worker, ≤4 veces/s).
- **La paridad render↔colisión a los pies.** `terrain_chord_error` da **0,000000 m**.
- **La pila de anillos de colisión.** 58 560 puntos sondeados: 0 sin cubrir, 0 con solape.

> ⚠️ Nota de método. Durante la sesión que produjo este documento se apuntó **tres veces** a un
> culpable equivocado (la banda de mezcla, la escala del quad, el coste de muestreo en CPU), las tres
> por extrapolar en vez de medir. Una de las extrapolaciones erró **×18**. La regla que sale de ahí:
> *ninguna fase de este plan se justifica con un número que no esté medido en el juego corriendo.*

---

## 1. Decisiones cerradas

1. **Quadtree sobre cube-sphere.** Cada cara se subdivide por error en pantalla. **Sin marco tangente
   anclado al jugador** → sin el techo de ~20-30° de la proyección gnomónica (`sec²θ`), y la misma
   representación desde el suelo hasta la órbita.
2. **El detalle vive en un heightmap POR NODO, generado en GPU una vez y cacheado.** La teselación
   *muestrea* esa textura; no evalúa ruido. La fidelidad la fija el téxel del nodo, no la tasa de
   teselación.
3. **Coste de disco cero.** Los nodos se generan en runtime a un pool de texturas y se descartan al
   alejarse. El argumento que mató a los chunks del v3 (*"no caben en disco, orden de TB"*) era para
   chunks **horneados**; no aplica aquí.
4. **GPU OBLIGATORIA EN EL SERVIDOR.** Decisión de Andoni. El DGS ya es una pieza a medida y las GPU
   en servidor son normales. Elimina la restricción más dura del diseño: **no hace falta un gemelo
   CPU del generador**, y cliente y servidor corren literalmente el mismo shader.
5. **Reproducibilidad por construcción, no por tolerancia.** El hash del ruido ya es aritmética
   entera pura (`uint` módulo 2³², bit-exacto por diseño) y la interpolación es polinómica. Se
   conserva esa propiedad y se protege con un test golden.
6. **La colisión lee los MISMOS téxeles que el render.** Deja de haber "dos superficies que se mide
   que coinciden": hay una. `terrain_chord_error` pasa de medir disparidad a comprobar que es 0.
7. **Se borra lo que el diseño hace innecesario**, no se deja muerto. Ver §3.

---

## 2. Lo que el v5 tiene que absorber (deuda abierta del v4)

`TODO.md:163-195` dejó **siete casillas sin marcar** desde 2026-08-06, y una es la verificación
crítica del propio v4. Empezar un v5 encima sin cerrarlas es cómo se acumulan tres sistemas de agua.
El v5 las hereda y las contesta:

- [ ] **¿Cuántos suelos hay?** — v5: **uno**, el pool de nodos. Es la respuesta por construcción.
- [ ] **¿La paridad render↔colisión se mantiene con teselación?** — v5: sí, por leer el mismo téxel.
      Cierra con F4.
- [ ] **`terrain_gen.comp` sin consumidor** — v5: **es** el generador de nodos. Se reconecta o se
      reescribe, pero deja de estar huérfano.
- [ ] **Fences + mapeo persistente sin consumidor** — v5: el pool de nodos sí hace subida asíncrona.
      Vuelven a tener call-site o se retiran del README.
- [ ] **Caché LRU + caché en disco (`core/cache/`), cero consumidores** — v5: el pool **es** una LRU
      (en VRAM, no en disco). Se reutiliza el concepto o se borra el código muerto.
- [ ] **MESO** — retirar del README y de los switches si ya no existe.
- [ ] **`drawIndexedIndirect` / `gl_DrawID`** — v5 es GPU-driven por naturaleza; decidir aquí.

---

## 3. Lo que se borra

No es limpieza opcional: si sobrevive, vuelve la costura por otra puerta.

| Se borra | Por qué deja de hacer falta |
|---|---|
| `reanchorToFine` (sphere-trace por píxel) | la geometría lleva el detalle — **esto es el arreglo de los 60 ms** |
| La malla base en el rango visible | el quadtree cubre del suelo a la órbita |
| La banda de mezcla (`blendS`/`blendE`) | no hay dos `triM` que casar |
| El sesgo de profundidad del clipmap | no hay superficies coplanares |
| `HARUKA_PLANET_DEBUG=9` | no hay tres superficies que distinguir |
| Los 11 anillos de heightfield de colisión | la colisión lee los nodos |
| El redondeo a potencia de dos por paridad | se conserva **solo** si hace falta contra T-junctions |

---

## 4. Fases

Cada fase es medible y ninguna avanza sin su número.

### F0 — Baseline honesto ✅ CERRADA  *(no toca nada)*

#### ✅ MEDIDO (2026-08-23) — con `HARUKA_FRAMELOG=1`, spawn fijo, vsync OFF, cap 60 fps

⚠️ El estado del juego EVOLUCIONA durante la toma (carga, props, clima), así que un solo número no
compara nada — es exactamente lo que TODO.md avisaba («las dos tomas no eran comparables»). Lo que sí
compara es **el mismo rango de ventanas** de 3 s desde el mismo spawn: aquí, las ventanas 3-8, que
son una meseta estable.

```
piso per-pixel 4.0 en todo (la octava fina MUERTA) ....  16,74 ms · p95 16,76  <- tope de 60 fps
piso per-pixel 1.125 en todo ..........................  ~28,9 ms             <- 34 fps
trazado 4.0 + sombreado 1.125 (lo que queda) ..........  17,77 ms · p95 16,81  <- tope de 60 fps
```

**La lección, que es de diseño y no de ajuste**: bajar el piso per-píxel costó **12 ms/frame**, y no
porque una octava sea cara — porque `reanchorToFine` la evaluaba en **cada uno de sus 16 pasos**, por
píxel. 17 evaluaciones para un detalle que solo se ve en una.

El arreglo separa dos `triM` que estaban confundidos en uno:
- **`harukaTraceTriM` (4,0)** — el trazado busca DÓNDE está la superficie; que se desvíe ±0,7 m es
  subpíxel a las distancias en las que este camino corre (>1,9 km ⇒ 0,4 px).
- **`harukaPixelTriM` (1,125)** — el sombreado sí necesita la octava, y la paga UNA vez.

Coste final: **~1 ms**, 12× menos, por el mismo detalle visible.

⚠️ Y esto revalida el diagnóstico del §0 desde otro ángulo: el coste de este camino NO está en
evaluar el detalle, está en **cuántas veces por píxel** se evalúa. Es el mismo argumento que sostiene
el v5 — generar una vez y cachear, en vez de re-evaluar por píxel y por frame.

**Pendiente de F0**: las cifras a 640 m y desde órbita (esta toma es a ras de suelo), y el reparto
rojo/verde de `HARUKA_PLANET_DEBUG=9` — que ya se sabe cualitativamente (se ven los dos) pero no
cuantificado.


- `HARUKA_PLANET_DEBUG=9`: fracción de pantalla en rojo (malla base) contra verde (clipmap).
- `present.swap` con vsync OFF, quieto en un punto fijo, mirando al horizonte: a ras de suelo, a
  640 m y desde órbita.
- A/B del piso per-píxel (`harukaPixelTriM`: `1.125` ↔ `4.0`) — aísla el coste de una octava × 16
  pasos × píxel.

**Cierra con**: tres cifras de `present.swap` y el reparto rojo/verde. Sin esto no se sabe cuánto
mejora nada.

### F1 — El generador de nodos, aislado  *(no toca render ni colisión)*

El paso que decide si el proyecto vuela, y el más barato de abandonar.

- Compute que llena el heightmap de **un** nodo desde **índices enteros** (cara, nivel, i, j) con la
  `harukaTerrainDetail` que ya existe.
- ⚠️ Las coordenadas de téxel salen de enteros, **no de un marco tangente en float**. Eso elimina de
  raíz el error de 0,9302 m de separación de `dir` que mide hoy `clipmap_dir_parity`.
- Cuidado con la **contracción FMA**: `precise` donde el redondeo importe. Barato ahora, caro después.

**Cierra con**: un **test golden** (hash del contenido del nodo) + coste de generar un nodo y nodos
nuevos por segundo caminando y descendiendo.

#### ✅ RESULTADO F1 (2026-08-23) — el proyecto es viable, con una tolerancia declarada

`terrain_node.h` + `terrain_node.comp` + `terrain_node_lattice`/`_scale`/`_content` (headless) y
`testTerrainNodeGpuParity` (en `haruka_tests_rhi`, con device real).

**El direccionamiento es exacto**, sin tolerancia:

```
grueso ⊂ fino:   99 846 texeles padre↔hijo ·  0 distintos
costura:            774 texeles de arista  ·  0 distintos
T-junction:          65 gemelos            ·  0 distintos
CONTRAPRUEBA (marco tangente en float, el camino de hoy): 33/33 fallan · 0,2290 m
```

**GPU contra la referencia de CPU**, con la bisección que localiza la divergencia:

```
etapa lx (coordenada de cara)              0.000e+00   <- exacto
etapa dir.x (proyeccion Cobb)              5.960e-08   <- entra aqui
etapa entrada del ruido                    1.221e-04
diferencia final: peor 0,0224 m · media 0,0044 m
reproducibilidad en la MISMA GPU: identica bit a bit
```

**Causa localizada**: `sqrt` sobre `double` en `harukaCubeFaceToDir`. GLSL **no exige** redondeo
correcto para dobles, así que el driver no coincide con el `std::sqrt` de la CPU. No es contracción
FMA — se probó `precise` y no cambia nada — ni un gemelo divergido: `lx` sale idéntico.

**Y no bloquea el plan**, porque el v5 decide GPU también en el servidor: la identidad que hace falta
es GPU↔GPU, no CPU↔GPU. La referencia de CPU es el oráculo del test, no un camino de producción.

- Tolerancia declarada: **0,05 m**. Contexto: el motor ya vive con **0,1453 m** de disparidad
  (`clipmap_dir_parity`), así que esto es 6,5× mejor que el statu quo.
- La GPU se reproduce a sí misma bit a bit ✅ — requisito real del determinismo.
- ⚠️ **PENDIENTE Y NO CERRABLE AQUÍ**: si dos GPU DISTINTAS coinciden. Necesita otra máquina. El hash
  de referencia de este equipo es `0x32095A03` (nodo FRONT/14/4200/3100, R = 6 371 000 m).

#### ✅ COSTE F1 (2026-08-23) — medido con fence, no con el reloj alrededor del dispatch

⚠️ Un `dispatch` es asíncrono: cronometrar la llamada mide lo que tarda la CPU en encolarlo
(microsegundos), no lo que tarda la GPU. Se encolan 64 nodos y se espera la fence; el tiempo entre 64
es el coste amortizado, que además es el régimen del pool. Con calentamiento previo (la primera
ejecución paga compilación del shader y no se repite nunca en producción).

```
nodo GRUESO (nivel 8, 305 m/texel) .... 0,028 ms/nodo  ->  70 nodos nuevos por frame con 2 ms
nodo FINO   (nivel 18, 0,30 m/texel) .. 0,046 ms/nodo  ->  43 nodos nuevos por frame con 2 ms
```

16 641 téxeles por nodo, o sea **~2,8 ns por téxel** en el caso fino. El coste no es uniforme entre
niveles porque cada octava está gateada por `triM`, de ahí que se midan los dos extremos.

**Lectura**: generar es barato y el pool cabe holgadamente. Para comparar — hoy se pagan **60 ms por
frame** re-evaluando el detalle por vértice y por píxel; aquí 43 nodos NUEVOS cuestan 2 ms y **se
reutilizan mientras vivan**. Ése es el argumento del v5 convertido en cifra.

**Sigue sin medirse**: cuántos nodos nuevos pide de verdad un descenso en picado (eso es F2, necesita
el selector por error en pantalla).

### F2 — El pool y el quadtree, sin dibujar ✅ CERRADA

- Selección de nodos por error en pantalla, pool en VRAM con evicción LRU, generación asíncrona.
- Presupuesto declarado: nodos residentes máximos y VRAM (orden de magnitud esperado: decenas de MB,
  a confirmar en F1).

**Cierra con**: recorrido de vuelo suelo→órbita→suelo sin quedarse sin pool ni encolar indefinidamente.

#### ✅ SELECTOR + RECORTES (2026-08-23) — el presupuesto ya es viable

`nodeShouldSplit` / `nodeBelowHorizon` / `nodeOutsideFrustum` / `nodeSelectVisible`, con
`terrain_node_select` y `terrain_node_frustum`.

```
a pie (2 m), mirando al horizonte, cota del nodo acotada:
    1 164 nodos = 73,9 MB de VRAM residente   (65,0 KB por nodo, R32F)
orbita (500 km):  403 nodos = 25,6 MB
CONTRAPRUEBA mirando al cielo: 318 nodos (x3,7 menos que al horizonte)
```

Tres hallazgos, los tres del test y no de la lectura:

1. **Sin recorte de horizonte el presupuesto se lo come la cara oculta.** Primera versión: 4 095
   nodos repartidos por toda la esfera (uno a 12 734 km, el diámetro) y el más fino bajo los pies a
   **9,7 km**, con la cámara a 2 m. Es el principio 2 de `PLAN_LOD_V3`, que ya lo decía.
2. **El recorte por sondas de esquina descartaba el suelo que se pisa** — 0 nodos a 2 m. A esa altura
   el horizonte está a 0,045° y en un nodo de nivel 0 (90°) las cinco sondas caen fuera aunque la
   zona visible esté en su INTERIOR. Falta la regla del punto sub-cámara: si cae dentro del nodo, el
   nodo se ve. Sin ella el recorte abre agujeros en el suelo.
3. **`TERRAIN_NODE_MAX_LEVEL` estaba en 20 y era tirar memoria.** El téxel de nivel 20 son 7,5 cm,
   pero la octava más fina del terreno es λ 4,5 m: por debajo de ~1,1 m/téxel el nodo guarda una
   interpolación de sí mismo. Los niveles 18-20 eran **2 732 de 5 439 nodos**, la mitad del
   presupuesto en detalle que no existe. Bajado a **17** (0,60 m/téxel), derivado de la escalera de
   ruido y vigilado por `terrain_node_scale`.

⚠️ **Y una limitación que el test cuantifica**: en el campo cercano el frustum NO recorta (5 437 de
5 439) mientras el nodo no sepa su cota. `terrainBoundM` vale 5 000 m porque el bake llega a ±4 km, y
para un nodo a 100 m `asin(5000/100)` satura en 90°. No es un defecto del algoritmo: es
incertidumbre real. Acotando la cota a ±400 m el recorte pasa a **×2,4** (2 842 → 1 164 nodos).
**El generador puede emitir el min/max de cada nodo de balde** —lo tiene delante al llenar el
heightmap— y eso vale 106 MB. Es la primera tarea de F2.

#### ✅ RANGO POR NODO (2026-08-23) — el círculo roto, y 122 MB ahorrados

`NodeRange` + `nodeFillHeights(..., &range)` + `NodeRangeFn` en el selector, con `terrain_node_range`.

```
a pie, sin rango (bound conservador +-5000 m):  2 842 nodos (180,4 MB)
a pie, con la cota REAL del nodo:                 918 nodos ( 58,3 MB)   -> x3,1 menos
```

**El rango sale gratis**: el generador ya recorre los 16 641 téxeles, solo hay que llevar el mínimo y
el máximo. Y rompe el círculo (para decidir si un nodo entra hay que acotar su cota; para conocer su
cota hay que generarlo) por HERENCIA: el hijo usa el rango del padre hasta que se genere el suyo. La
propiedad que lo permite está medida — el rango de los cuatro hijos se sale del padre **0,25 m sobre
una amplitud de 250 m (0,1 %)**, así que un margen pequeño lo cubre.

Con esto, F2 tiene el presupuesto cerrado: **58 MB residentes a pie, 25,6 MB desde órbita**, en R32F.
En R16F sería la mitad, y el rango del nodo hace que 16 bits sobren de sobra.

#### ✅ EL POOL (2026-08-23) — y el número que justifica el v5 entero

`terrain_node_pool.h` (`TerrainNodePool`), con `terrain_node_pool` y `terrain_node_pool_reuse`.

```
REUTILIZACION al andar 48 m:  1 044 de 1 169 = 89,3 %
el clipmap, con la MISMA deriva:      8 de 257 049 = 0,003 %
CONTRAPRUEBA (teletransporte medio planeta): 0,0 %
```

**Ésa es la tesis del v5 medida.** Los nodos están fijos al mundo (índices enteros, sin ancla), así
que andar no los invalida: los que siguen a la vista son los MISMOS objetos. El clipmap reutiliza 8
de 257 049 porque su marco tangente se re-ancla y todos los nodos se mueven con él.

La política vive SEPARADA de la GPU —no hay una llamada al RHI en el fichero— y eso es deliberado:
un desalojo que tira un nodo visible da parpadeo, una generación sin tope da un tirón al descender, y
una caché sin fallback deja agujeros. Nada de eso se depura mirando la pantalla; se depura con un
test que mueve la cámara y cuenta. Con la política aparte, ese test corre headless.

Tres invariantes, verificados:
- **Presupuesto**: 10 peticiones con tope 3 → 3 encoladas.
- **LRU que no se pega un tiro en el pie**: con los huecos usados ESTE frame, publicar otro devuelve
  −1 en vez de desalojar un visible. Rechazar es "entra más tarde"; desalojar sería parpadeo.
- **Fallback por ancestro**: un nodo no residente devuelve el ancestro más profundo que sí lo esté,
  así que nunca hay un agujero — solo una zona más basta un par de frames. Con contraprueba: sin
  ancestro devuelve −1 honesto, no un hueco ajeno.

#### ✅ ADAPTADOR GPU (2026-08-23) — F2 CERRADA

`terrain_node_gpu.h` (`TerrainNodeGpu`) + `testTerrainNodePoolGpu` en `haruka_tests_rhi`. Primera vez
que las piezas corren JUNTAS:

```
pool de 256 huecos = 16,3 MB en VRAM (65,0 KB por nodo)
6 frames · 192 nodos dispatchados · residentes 192/256 · desalojos 0
8 huecos auditados contra la referencia de CPU · 0 con contenido AJENO · peor 0,0150 m
```

El pool entero es **UN** SSBO: el hueco `k` ocupa `[k·TEXELS², (k+1)·TEXELS²)`, así que generar otro
nodo solo cambia un entero del UBO — sin crear recursos, sin rebindear y sin sincronizar entre nodos,
porque los dispatch escriben en rangos disjuntos. Una sola barrera para toda la tanda.

Dos decisiones que el pegamento tenía que acertar:
- **El hueco se reserva ANTES del dispatch.** Al revés, el LRU podría reasignar el hueco entre medias
  y el nodo acabaría con el contenido de otro sitio — en pantalla, un trozo de terreno ajeno.
- **El rango se publica inválido** y el selector tira de la herencia del padre. Conocer el min/max
  real exige leer el contenido, y un readback por nodo mataría la asincronía. Cuando el compute lo
  calcule por su cuenta (reducción o atómicos) se puede apretar.

Lo que este test ve y ninguno de los anteriores podía: **que el hueco que el pool asigna es el hueco
que la GPU llena**. Un desfase de índice no rompe ningún test unitario y en pantalla sale como
terreno de otro sitio; se audita comparando cada hueco residente contra la referencia de CPU del nodo
que el pool dice tener ahí.

---

**F2 CERRADA.** Lo siguiente es F3 (render sobre nodos), y con ella el borrado de `reanchorToFine` —
que es el arreglo de los 60 ms.

### F3 — Render sobre nodos

- La teselación muestrea el heightmap del nodo. **Se borra `reanchorToFine`.**
- Stitching sin grietas entre nodos de nivel distinto (faldas o casado de niveles por arista).

**Cierra con**: `present.swap` contra el baseline de F0. Es el número que justifica todo el plan.

#### ✅ F3 COBRA (2026-08-23) — 10 ms, y el frame se clava en el tope

A/B en el juego, mismas ventanas de 3 s desde el mismo spawn:

```
ventana:      3      4      5      6      7      8      9     10
clipmap   26.01  26.61  27.51  27.54  27.11  30.74  23.27  25.22
v5        58.37  19.08  16.75  16.75  16.75  16.75  16.75  16.85
```

Tras el pico de carga (58 ms llenando el pool), **v5 se clava en 16,75 ms — el tope de 60 fps** —
mientras el clipmap va a 23-31 ms. O sea que sobra GPU, que es lo contrario de donde se empezó.

##### Y cómo se llegó: cuatro medidas, tres hipótesis mías equivocadas

La primera versión salió **2× PEOR** (53 ms contra 26). Lo que lo arregló, en orden:

```
53 ms  primera version (1 011 nodos, 33 M triangulos)
24 ms  densidad de dibujo DESACOPLADA del nivel  -> 2,1 M triangulos
24 ms  un draw instanciado en vez de 1 008       -> sin efecto
21 ms  proyeccion en float en vez de double      -> el sqrt fp64 valia ~3 ms
20 ms  saltarse tambien la MALLA BASE
16,75  (medida limpia A/B)
```

**El bug de fondo era conceptual**: `errorPx` decide cuánto DETALLE necesita el heightmap y otra cosa
distinta decide cuántos TRIÁNGULOS se dibujan de él. Estaban acoplados —un vértice por téxel— así que
un téxel de 1 px daba **un vértice por píxel**. Separarlos (`HARUKA_TERRAIN_V5_VERTPX`, 4 px por
defecto, que es lo que dibuja el clipmap) fue 33 M → 2,1 M de triángulos.

Dos hipótesis mías que la medida refutó:
- **"Son los 1 008 draw calls"** — el instanciado no cambió nada (24,07 vs 24,24). No era driver.
- **"Es el `sqrt` en doble precisión"** — sí, pero solo ~3 ms de 24, no el grueso.

Y una que sí: **el v5 se estaba SUMANDO a la malla base**, no sustituyéndola. Solo se anulaba el
clipmap, así que `biome.frag` seguía corriendo su sphere-trace por píxel — el coste que el v5 viene a
quitar. Ésa es la mitad del ahorro.

##### Lo que queda antes de poder borrar el clipmap

- **El pico de 58 ms al cargar el pool.** Hay presupuesto (43 nodos/frame) pero el primer frame pide
  1 000: hay que repartirlo mejor o precargar niveles gruesos.
- **El sombreado real** (biomas, triplanar). Hoy es una direccional, así que la comparación de coste
  es de GEOMETRÍA — el número subirá cuando el sombreado sea el de verdad.
- ~~**La costura entre CARAS del cubo**~~ — hecha el 2026-08-24, ver abajo.
- **Mirarlo**. Nadie ha juzgado todavía si se VE bien.

#### 🟡 F3 EN CURSO (2026-08-23) — el nodo se dibuja y cae donde debe

`terrain_node.vert` / `.frag` + `testTerrainNodeRender`. Camino PARALELO, opt-in: no sustituye al
clipmap todavía. Tocar el camino de dibujo de golpe es donde están todas las cicatrices del repo.

```
el nodo ocupa el hueco 3 de 4            (la lista de libres es LIFO: hay que PREGUNTAR el hueco)
rejilla compartida: 16 641 vertices · 32 768 triangulos  (la MISMA para todos los nodos)
posicion del vertice: GPU vs referencia CPU -> peor 0,0185 m
  (hoy, render vs colision: 0,1453 m de clipmap_dir_parity)
```

**Lo que el vertex shader NO hace es la noticia**: no evalúa ni una octava de ruido. La altura sale
de un fetch al pool. `clipmap.tese` la evalúa por vértice y por frame, y `biome.frag` otra vez por
píxel dentro del sphere-trace.

**Una geometría para todos los nodos**: la rejilla es la misma (enteros `u,v`), y lo único que
distingue a un nodo de otro es su UBO. Un vertex buffer, un index buffer, y `drawIndexed` por nodo.

La normal sale de diferencias del propio heightmap (dos lecturas más), no del gradiente del ruido.

#### ✅ COSTURA ENTRE NIVELES (2026-08-23) — sin faldas, exacta

`nodeStitch` / `nodeStitchedDir` + su gemelo en `terrain_node.vert`, con `terrain_node_stitch`.

```
vertice impar del borde vs la recta del vecino grueso:
  SIN coser: 0,5533 m  <- la grieta
  COSIDO:    0,000e+00 m
salto de DOS niveles (stride 4): 0,000e+00 m
vertices pares movidos: 0 · interiores movidos: 0
```

⚠️ **La solución habitual son FALDAS** —geometría extra colgando del borde para tapar el hueco— y
aquí no hacen falta. Como el vértice grueso cae **bit a bit** sobre el fino (F1), el vértice que
sobra se puede COLOCAR en la recta que une a sus dos vecinos que sí existen en el grueso. No queda
hueco que tapar: la arista fina describe exactamente la misma recta que la gruesa.

Faldas habría significado geometría extra, un borde que asoma en pendientes fuertes y un parámetro de
"cuánto cuelga" que nadie sabe justificar. Esto no tiene parámetros. **Es F1 cobrando**: la exactitud
del direccionamiento no era purismo, era lo que permite coser sin inventar geometría.

Detalle que importa: se interpolan las DIRECCIONES y **no** se re-normaliza. La arista del vecino
grueso es una recta entre sus dos vértices, no un arco; normalizar devolvería el vértice a la esfera
y reabriría la grieta, más pequeña pero grieta.

**Lo que falta de F3, y no es poco:**
- **Integrarlo en el frame** y **borrar `reanchorToFine`**. Ése es el arreglo de los 60 ms y hasta
  que no esté, F3 no ha cobrado.
- **El sombreado real** (biomas, triplanar). Hoy el fragmento es una direccional a propósito, para
  poder medir el coste de la GEOMETRÍA contra el camino que sustituye.

### F4 — Colisión sobre los mismos nodos

- `HeightFieldShape` de Jolt directamente sobre el téxel del nodo (una rejilla regular es lo que Jolt
  quiere; mapea 1:1, sin conversión).
- Se retiran los 11 anillos.

**Cierra con**: `terrain_chord_error` da 0 **por construcción**, con contraprueba de que el test
seguiría detectando una divergencia si la hubiera.

### F5 — Definición desde órbita

Dos problemas distintos, y el segundo probablemente pesa más:

- **Geometría**: octavas por encima de la escalera actual (λ ~6 km y ~12 km) para romper la suavidad
  bilineal del bake, vivas a distancia orbital.
- **Albedo**: desde 500 km un continente se lee por el color —bioma, nieve, ríos, zonas áridas—
  antes que por el relieve. Hoy el planeta es de **un verde uniforme**. Aunque la geometría sea
  perfecta, sin variación de material seguirá pareciendo una bola lisa.

**Cierra con**: captura desde órbita contra la de F0.

---

## 5. Riesgos declarados

| Riesgo | Mitigación |
|---|---|
| Es un rewrite del camino con más cicatrices del repo | F1 y F2 no tocan render ni colisión; se abandona barato |
| Grietas entre nodos de nivel distinto (T-junctions) | el problema clásico del quadtree; faldas o casado por arista, decidir en F3 |
| Deriva entre GPUs del fleet | el hash ya es bit-exacto; test golden en F1 + `precise` contra contracción FMA |
| El pool se queda corto al descender rápido | presupuesto y política de evicción medidos en F2, no supuestos |
| La v4 queda a medio migrar como quedó la v3 | §2: el v5 hereda su checklist y la contesta |

## 6. Lo que este plan NO resuelve

- **El muro de precisión del float.** Siguen haciendo falta orígenes locales.
- **El color desde órbita**, salvo por lo que se haga en F5 — y es otro sistema (biomas/materiales).
- **El coste del raymarch de sombra** (24 pasos por píxel). Es independiente del quadtree y sigue
  ahí después de F3.


#### ✅ COSTURA ENTRE CARAS DEL CUBO (2026-08-24) — derivada, no cableada

`nodeNeighbourLevels` hacía `continue` cuando el vecino caía fuera de la cara, así que **a lo largo
de las doce aristas del cubo nadie cosía**: el vecino quedaba como "mismo nivel" y las T-junctions
abrían grieta.

La adyacencia entre caras son 24 combinaciones (6 caras × 4 aristas), cada una con su rotación y su
posible reflejo. **No se ha escrito ninguna tabla**: `nodeNeighbourAcrossFace` la deriva de la
geometría que ya estaba probada — punto medio de la arista, empujón hacia afuera, ida y vuelta por
`cubeFaceToDir` / `dirToCubeFaceClosed` (par exacto a 1e-8, test `cube_sphere_inverse`). La rotación
y el reflejo vienen incluidos sin haberlos escrito.

⚠️ **El empujón va en 3D, no en coordenadas de cara.** La primera versión sumaba un cell a `lx`/`ly`.
A nivel 1 un cell vale 1,0, así que pedía `cubeFaceToDir(-1.5, -0.5)` — muy fuera del dominio del
spherify — y **las 24 esquinas del cubo salían todas a la misma cara**. Medido: 46 de 1488 cruces sin
simetría, todos de nivel 1. La versión buena perturba la dirección: `normalize(em + (em − dc)·½)`.

El cosido del shader **no necesita la orientación del vecino**: interpola sobre su propia arista con
su propio (u,v) y una zancada que sale de la diferencia de nivel, y eso es invariante a que el vecino
tenga la arista girada o del revés. Lo único que hace falta de él es el nivel.

Medido (`test_terrain_node_face_seam`):

    simetria de la relacion (niveles 1..5)   1488 / 1488 cruces
    la arista compartida, en metros          0.0000 m  (el nodo mide 625 471 m de lado)
    cosido cruzando cara, vecino mas grueso  arista = 1  (antes: 0, no cosia)
    CONTRAPRUEBA sin vecino dibujado         arista = 0
    CONTRAPRUEBA nodos interiores            0 dicen cruzar

**El oráculo es la SIMETRÍA**, y es lo bueno de este caso: si A cruzando su arista da B, B tiene que
devolver A. Una tabla mal escrita —o un empujón mal dimensionado, como el primero— no puede cumplir
eso por casualidad en las 24 combinaciones. El segundo oráculo es en metros: la arista compartida
tiene que estar en el mismo sitio del espacio vista desde las dos caras.

⚠️ **SIN VERIFICAR EN PANTALLA.** Esto demuestra que el cosido recibe el nivel correcto en las doce
aristas; no que la grieta haya desaparecido a la vista. Falta mirarlo con `HARUKA_TERRAIN_V5=1`.

#### ⚠️ EL NODO NO TENÍA CONTINENTES (2026-08-24)

Al ir a montar el sombreado real salió algo más de fondo: **`terrain_node.comp` calculaba solo
`harukaTerrainDetail`**. El clipmap compone su superficie como `R + baseH + detalle·atenuación` con
recorte al nivel del mar; el pase de nodos dibujaba `R + detalle` a secas.

|                          | clipmap                          | v5 (antes)   |
|--------------------------|----------------------------------|--------------|
| superficie               | `R + baseH + detalle·atenuación` | `R + detalle`|
| recorte al nivel del mar | sí (`h = −baseH`)                | no           |
| `baseH` (bake)           | **±4 km**                        | ausente      |

O sea que el v5 dibujaba **otro planeta**: sin continentes, sin océanos y sin costa, y a otra altitud
que el clipmap — un escalón en la transición entre los dos. Y el sombreado dependía de esto: la
selección de material usa `vClimate.x`, que **es** `baseH` (define la banda de arena de la costa).

El test de paridad de F1 no lo veía porque corre **sin bake**, o sea por la rama sin muestreo.

Arreglado en los dos lados a la vez (`terrain_node.comp` y `nodeFillHeights`, que ahora acepta un
proveedor de altura base inyectado para que `terrain_node.h` siga sin depender del planeta ni de la
GPU). Medido con un campo base sintético de ±3 km (`testTerrainNodeBaseField`):

    referencia                    2162.7 .. 2212.7 m
    Vulkan, GPU <-> CPU           0.0007 m
    CONTRAPRUEBA sin bake         hasta 2177.2 m de diferencia  (no es un no-op)

⚠️ **PENDIENTE, y con consecuencia real: en OpenGL el sampler del compute devuelve 0**, así que ahí
el nodo sigue saliendo sin bake. Descartado: el `.spv` está al día, la subida de texturas en capas de
GL es correcta, y el mismo `binding = 15` funciona en GL desde el TESE del clipmap. Lo que **nunca se
ha ejercido** en este RHI es compute + sampler en OpenGL — ningún otro `.comp` del proyecto muestrea
texturas. Es un gap del RHI, no del terreno.

**Sigue sin hacer: el sombreado real** (bioma/triplanar). El núcleo de `biome.frag` son ~280 líneas
(690–968) que habría que extraer a `lib/` en vez de copiarlas — ver el inventario de shaders.

#### F5 — SOMBREADO REAL: la cifra que faltaba (2026-08-24)

Incremento acotado a propósito: extraer el núcleo a `lib/`, enchufarlo al pase de nodos y **medir**,
antes de comprometerse con el resto.

**Lo medido** (`testTerrainNodeShadeCost`), a 800 m, 506 nodos, 1,0 M triángulos, **1920×1080**, con
`lod` y `lodNrm` ≈ 1 (o sea triplanar de albedo Y de normales a pleno — el caso peor):

    OpenGL    luz plana   5.46 ms/frame  ->  sombreado real   5.49 ms/frame   (+0.04 ms · x1.01)
    Vulkan    luz plana  10.35 ms/frame  ->  sombreado real  11.21 ms/frame   (+0.86 ms · x1.08)

**Sombrear de verdad cuesta 0,86 ms/frame en Vulkan** (un 8 %) y nada medible en OpenGL, en el caso
peor. A 1,0 M de triángulos sobre 2,0 M de píxeles el pase está atado por GEOMETRÍA y el fragment
cabe casi entero en la sombra de eso.

⚠️ **LA PRIMERA MEDIDA DE VULKAN ERA EL VSYNC.** Daba 16,61 → 16,68 ms, o sea 1/60 clavado: medía la
presentación, no la GPU, y se leía como "sombrear es gratis". Ahora `HARUKA_NO_VSYNC=1` pide MAILBOX
(o IMMEDIATE si no lo hay) y **el test detecta solo el caso**: si el tiempo por frame cae a menos del
3 % de un múltiplo de 16,67 ms, imprime `MEDIDA INVALIDA` en vez de dar la cifra por buena.

⚠️ **La primera versión midió en la ventana del banco (256×256) y dio +0,01 ms también** — pero por
la razón equivocada: 32 000 píxeles para 0,6 M de triángulos. Habría sido una cifra tranquilizadora y
falsa. La contraprueba obligatoria es que **el 100 % de los píxeles cambian** al encender el
sombreado: sin ella, un `uShade.w` que no llegara al shader habría dado el mismo +0,01 y se habría
leído como "sombrear es gratis".

**Dos librerías nuevas, para no duplicar:**
- `lib/base_field.glsl` — el muestreo del bake con bilineal a mano. Tenía DOS copias literales
  (`clipmap.tese` y `terrain_node.comp`); la tercera la habría hecho insostenible.
- `lib/terrain_shade.glsl` — material por clima + triplanar + normal map, con los samplers como
  parámetros para que cada pase ate lo que tenga.

✅ **`biome.frag` YA usa la librería: hay UNA copia, no dos.** −176 líneas / +57. Lo que se quedó
fuera es lo que su `main` necesita después del bloque y que no se puede recomputar barato: `slope`
(calculado ANTES de que el normal map mueva `n`), `bUV`, `shoreF` (función pura de `elev`) y
`zoneRGB` (una muestra, solo para una vista de depuración). `biomeCol` y `matIdx` salen de la
librería por out-param porque recomputarlos exigiría recorrer otra vez la tabla de materiales.

Que la extracción es fiel lo sostienen los tests que dibujan terreno de verdad: `terreno: responde a
la luz`, `el marchador de sombras`, `props` y `mar` siguen pasando en los dos backends.

**Dos fallos del RHI destapados de camino:**
- `NodeDraw` declarado distinto en vértice y fragmento: **OpenGL rechaza el enlace**, Vulkan no dice
  nada (enlaza por etapa). Cualquier campo nuevo va en los dos.
- Barrera sobre una imagen **D24S8** con `aspectMask` solo de profundidad: la spec exige incluir el
  stencil. Arreglado con `depthAspectOf()`. Nadie lo había visto porque **ningún test dibujaba a un
  render target offscreen** hasta ahora.
- ⚠️ Y uno SIN arreglar: el test de coste deja el backend en un estado que hace fallar al siguiente
  que lee píxeles del framebuffer por defecto. Ni el viewport, ni destruir el target, ni un frame de
  restauración lo explican. Está puesto EL ÚLTIMO como mitigación, no como arreglo.
