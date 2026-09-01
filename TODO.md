# TODO — bugs, verificaciones e ideas

**Qué hay aquí**: lo que está **roto**, lo que está **sin mirar** y lo que está **sin decidir**.
Nada de plan por versión.

| Este fichero | Otro fichero |
|---|---|
| Bugs abiertos · verificaciones pendientes · ideas sin decidir. | [ROADMAP.md](ROADMAP.md) — el plan por versión de los tres proyectos. |
| | [docs/HISTORIAL.md](docs/HISTORIAL.md) — lo cerrado, con las trampas que costaron sesiones. |

**Suite**: `./haruka_tests` → **26 083 OK · 0 FALLOS** · RHI: `./haruka_tests_rhi` →
**308 OK · 0 FALLOS** en los dos backends (2026-08-26).
⚠️ **Los tiempos dependen del ÁRBOL**: `Survival/build` está en `RelWithDebInfo` y la suite CPU entera
tarda **~95 s**; `haruka-cpp/build` está en **`Debug`** y ahí no termina en 260 s. Mirar
`CMAKE_BUILD_TYPE` antes de citar un tiempo. ⚠️ El rojo de `terrain_quality_mapping` que
arrastraba desde el 2026-08-18 **no era un bug del código**: el test esperaba `Low` desde antes de que
la deduplicación de capas (9 → 4 imágenes, 1,6 GB → 716 MB) hiciera asequible `Medium` por defecto.
⚠️ **Para medir tiempos, `HARUKA_NO_VSYNC=1`**: con FIFO cualquier ms/frame se clava en 16,67 y mide
la presentación, no la GPU. El test de coste del sombreado lo detecta y avisa, los demás no. Juego:
`Survival/build/bin/survival_tests assets/data/magic/language/` → **53 OK**.
⚠️ **`build.sh` NO construye los binarios de test**: `cmake --build build --target haruka_tests haruka_tests_rhi -j16`.
*⚠️ El README sigue diciendo 19 460 y este fichero decía 20 135: los dos estaban obsoletos. El
recuento de arriba es el de la última ejecución real, no una estimación.*

---

## ✅ Sesión del 2026-08-25 — los pinchos del terreno, cerrados

Andoni los reportó y sobrevivieron a nueve causas medidas y descartadas. **La causa era de precisión y
ningún test podía verla: todos calculan en double.** Lo destapó mirar el VS Output en RenderDoc
(los pinchos estaban en la GEOMETRÍA) y medir el período de la estría sobre el PNG.

| qué | antes | ahora |
|---|---|---|
| **Dirección del vértice en FLOAT** (`terrain_node.vert` usaba `harukaCubeFaceToDirF`, el compute la de double) | **0,2148 m = 36 % del téxel**, ruido a la frecuencia de la malla | double, y el producto de magnitud planetaria también |
| Grietas entre nodos (T-junction del cosido) | 8-21 px en 7 encuadres | **0 en los 7** |
| Escalón entre niveles | 2,747 m | **0,785 m** (mapa del ABUELO en el hueco) |
| Grieta del morph por nodo | 10,07 m | **0 exacto** (morph por VÉRTICE) |
| Corrugado del sombreado | marco tangente inventado | ejes reales del téxel |
| Suelo de colisión reesculpiéndose al andar | ancla saltaba 3,785 m = **0,430 de celda** | **0,000 de celda** |

**Reglas que salieron de los intentos fallidos, y conviene no volver a romperlas:**

1. **El morph tiene que ser función SOLO de (nivel, posición del vértice).** En cuanto mira a los
   vecinos, dos nodos del mismo nivel con vecindarios distintos evalúan distinto el punto que
   comparten. Cinco parches murieron por esto (rampas, estrechamiento, apagado junto a vecino fino,
   clamp de arista, mapa por stride sin ajustar el morph).
2. **Un gemelo en double no puede medir un fallo de precisión float.** Es estructural: el instrumento
   y el fallo viven en aritméticas distintas.
3. **Un cambio que deja los números EXACTAMENTE iguales es un no-op.** Pasó dos veces en la sesión.
4. **¿Sobre cuántos puntos se ha medido?** El test de grietas daba 0 con UN encuadre (1030 m) y 21 con
   siete — la altitud que se medía era la más limpia de todas.

**Borrado por medida:** el morph por ARISTA entero (2,747 m con y sin él; 8/11 px en GPU antes y
después). Eran dos capas de parche que no cerraban nada.

**Herramientas nuevas:** vista de depuración **5** (nivel/cara/stride exactos por píxel, para leer con
`readPixels` y atribuir un agujero) y **6** (la normal como color). `HARUKA_CHAR_STEPUP` /
`HARUKA_CHAR_STEPDOWN` para A/B-ear el controlador sin recompilar. El autotest del alambre de colisión
ahora dice **dónde** está el peor vértice, no solo cuánto.

---

## 🐛 Bugs abiertos


### Motor

| # | Bug | Dónde |
|---|---|---|
| 1 | **SSAO es un HUECO**: el ajuste viaja al shader pero no hay pase que genere la oclusión. Implementar o quitar el ajuste — hoy promete algo que no ocurre. | `settings` → `final.frag` |
| 2 | **Render targets estáticos**: `bloom` / `ssao` / `hdr` se crean con tamaño FIJO y no siguen el resize de ventana. | `application_render.cpp` |
| 3 | **Objetos de escena sin LOD ni cull**: full detalle a cualquier distancia (solo hay un corte por distancia). Con el instancing ya migrado, toca darles cull + LOD. | `application_render.cpp` |
| 4 | **Memory leaks / dangling refs en RHI**: sin auditar. Handles que se crean y nunca se destruyen, y punteros a recursos que sobreviven al `Device`. | `src/rhi/` |
| ~~5~~ | ✅ **ARREGLADO (2026-08-24)**: `preview.vert` borrado. Nadie lo referenciaba — `preview.frag` sí se usa, pero pareado con `simple.vert`. | — |
| 5-viejo | **`preview.vert` es código muerto** y además declara un `PerObjectData` DESACTUALIZADO (sin los campos de material). Nadie lo compila hoy; el día que alguien lo use, falla al LINKAR sin decir por qué. Borrarlo o actualizarlo. | `assets/shaders/preview.vert` |
| 7 | **Nadie fija la raíz de assets salvo el editor**: `Shader::setBaseDir` no lo llama ni el motor ni Survival, así que ambos dependen de que el cwd sea el del ejecutable. El editor ya la deriva de `SDL_GetBasePath`; el resto sigue a merced de desde dónde se lance. | `renderer/shader.h` |
| ~~9~~ | ✅ **NO EXISTIA (2026-08-24).** "En OpenGL un compute no puede muestrear texturas" fue un hecho durante una sesión entera: el generador devolvía 0,00 en los 16 641 téxeles y el terreno salía sin continentes. **Falso.** Todos los síntomas venían de una **fence que faltaba DESPUÉS de `copyBuffer`**: se leía el destino a medio llenar y se interpretaba como que el shader había calculado mal. En Vulkan no se notaba, así que parecía un fallo del backend de GL. Era del banco. Arreglado con `copyThenWait` en los tres tests que lo tenían. | `tests/rhi_test_main.cpp` |
| ~~9b~~ | ✅ **TAMPOCO EXISTIA**: los 399,59 m de divergencia GL↔Vulkan en `testTerrainNodeRender` eran la MISMA fence. Con ella, GL da 0,0204 m. | — |
| 14 | ⚠️ **El contrato de `copyBuffer` + `mappedData` no está documentado en el RHI**, y no esperar no da error: da datos a medio llenar. Debería exigir la fence en la firma, o hacerla dentro. Tres tests cayeron en lo mismo. | `src/rhi/rhi_device.h` |
| 10 | **Un render target offscreen deja el backend movido.** `testTerrainNodeShadeCost` es el único test que dibuja a un target propio, y tras él falla el siguiente que lee píxeles del framebuffer por defecto. NO lo explican ni el viewport (`beginRenderPass` lo restaura), ni destruir el target, ni un frame de restauración explícito. Mitigado poniéndolo EL ÚLTIMO del banco — mientras siga así, nadie puede añadir un test detrás sin comprobar que no hereda basura. | `src/rhi/` |
| 11 | **`readPixels` solo sabe leer del framebuffer por defecto.** No hay forma de leer un render target offscreen, así que cualquier test que dibuje a 1080p tiene que hacer su contraprueba en la ventana. | `rhi_device.h` |
| ~~12~~ | ✅ **ARREGLADO (2026-08-24)**: el comentario cita ahora los nueve shaders que de verdad leen el binding 16. | `src/game/planet.cpp` |
| 13 | ✅ **ARREGLADO, y encontrado de camino**: `HARUKA_GL_SPIRV` **solo sabía encender**. Con `=0` caía al camino automático, que enciende SPIR-V en cualquier GL 4.6 — así que un experimento que creía comparar "con SPIR-V / sin SPIR-V" comparaba SPIR-V consigo mismo, y me dio por descartada una hipótesis correcta. | `src/rhi/opengl/gl_device.cpp` |
| ~~8~~ | ✅ **ARREGLADO (2026-08-24)**: el clear usa un gris azulado DECLARADO cuando no hay planeta activo, en vez del ~0,005 que devuelve `getSkyColor` para el espacio. Con planeta no cambia nada. | `application_render.cpp` |
| 8-viejo | **El clear del frame usa el color de cielo aunque no haya cielo**: sin planeta activo `getSkyColor` devuelve 0.005 → un viewport casi negro que parece roto. Para el editor conviene un fondo neutro declarado, no el del espacio. | `application_render.cpp` |

⚠️ **Sobre el bug 5 y cualquier cambio al UBO per-object**: un bloque `uniform` debe declararse
**IDÉNTICO en todas las etapas del mismo programa**. Declararlo con menos campos en una de ellas da
`definitions of uniform block do not match` **al LINKAR**, no al compilar, y el pase se queda mudo
sin error visible. `simple.vert` linka con `final.frag` **y** con `preview.frag`: los tres van a la vez.

### 🔴 ABIERTO — la colisión no coincide con el render (2026-08-25)

Andoni lo confirmó mirando el alambre (`HARUKA_COLLISION_WIRE=1`): *"no son iguales"*. El autotest del
propio motor lo mide y dice explícitamente que debería ser ~0:

    AUTOTEST del alambre: peor 0,0418 m · media 0,0014 m · radio tangente 6..274 m
    distribucion: 4 de 141 por encima de 2 cm · 0 por encima de 5 cm  ->  OUTLIERS
    el PEOR: radio tangente 76,7 m · malla 991,652 m vs funcion 991,694 m (delta -0,0418 m)

**Es la forma de "me engancho en sitios concretos"**, no de "el suelo entero está mal": 4 vértices de
141 pasan de 2 cm, la media es 1,4 mm. Rango observado del pico entre ejecuciones: 0,04-0,10 m.

**Ya descartado, con medida:**
- **No es el corte de octavas.** El render corta en el téxel (0,5965 m) y la referencia en
  `TERRAIN_TRIM_FLOOR` (0,5 m), y la diferencia de altura es **0,0000 m**: las dos cotas caen bajo la
  guarda de la octava fina, que entra con peso 1 en ambas (`terrain_render_vs_reference_cut`).
- **No es el LOD.** Las columnas `triM0` y `triMr` del autotest dan lo mismo.
- **No es la forma del terreno.** 13,8° de pendiente máxima entre celdas de colisión y 0,123 m de
  desnivel, contra un límite de 50° y un escalón de 0,40 m (`terrain_collision_walkability`).
- **No es el ancla** (arreglado hoy: 0,000 de celda de desfase).
- **No es la dirección de muestreo en float** (arreglada hoy en `ringSample`; el twist del propio
  motor bajó 0,035 → 0,029 m).

**Encontrado un contribuyente real, medido, que explica la MEDIA pero no el pico**
(`terrain_ring_sample_lateral_shift`): `ringSample` muestrea la altura en `pc + dir·R` —a **nivel del
mar**— pero el vértice representa el punto a `R+h`, que cae en la tangente `x·(R+h)/R`, no en `x`.
Jolt lo coloca en `x`. Con el jugador a 991 m:

| radio | desplazamiento lateral | error de altura |
|---|---|---|
| 77 m | 0,0121 m | 0,0025 m |
| 256 m | **0,0408 m** | 0,0074 m |

✅ **ARREGLADO (2026-08-25)**: `ringSample` corrige la dirección con la altura y vuelve a muestrear
donde el vértice va a estar de verdad (una sola iteración; la corrección es de 1e-4 relativo).

| autotest del alambre | antes | ahora |
|---|---|---|
| media | 0,0014 m | **0,0004 m** |
| vértices por encima de 2 cm | 4 de 146 | **1 de 146** |
| pico | 0,0418 m | 0,0418 m (el mismo vértice) |

⚠️ **El factor va al revés de lo que parece, y se probó mal primero** (media 0,0014 → 0,0034 y de 4 a
10 outliers): el vértice que Jolt coloca en `(x, altura, z)` está a `R + altura` del centro, así que
la razón tangencial de SU dirección es `r/(R+h)` — **menor** que la de `dir`. Hay que CERRAR el rayo
(`k = R/(R+h)`), no abrirlo.

**Queda UN outlier**, siempre el mismo: radio 78,4 m, delta −0,0418 m, anillo 2 (celda 2 m). No lo
mueve la corrección, así que es de otra causa y está sin identificar.

⚠️ Medir esto con solo el detalle (sin la altitud base) da 0,0001 m y parece despreciable: el efecto
es proporcional a `h/R` y **la base es 991 m de los 991**. Un test sin bake mide otro problema.

⚠️ Y un efecto colateral medido que nadie evaluó: la celda del anillo fino bajó de **4 m a 0,5 m** por
paridad, y la cápsula pasó de tocar 1-2 triángulos a **7 estando quieta** (`CharContacts`, con
`HARUKA_DIAG=1`). Con `mWalkStairsStepUp = 0,40 m` sobre baches de 0,12 m, el camino de «subir
escalera» de Jolt se dispara en terreno normal. `HARUKA_CHAR_STEPUP` / `HARUKA_CHAR_STEPDOWN` lo dejan
probar sin recompilar.

### ✅ CERRADO — "me atasco, me deslizo, va pesado y me muevo estando quieto" (2026-08-26)

**No era la forma del suelo.** El personaje PARADO acumulaba la caída sin tope. La sonda `CharContacts`,
quieto sobre terreno llano (`HARUKA_DIAG=1`):

    onGround=1 · v radial -11.4 m/s · v TANGENCIAL 0.005 m/s
    onGround=1 · v radial -21.4 m/s · v TANGENCIAL 0.005 m/s
    ...
    onGround=1 · v radial -83.4 m/s · v TANGENCIAL 0.005 m/s      ← ≈ -10 m/s por segundo

`CharacterVirtual` **no devuelve la velocidad cancelada por el contacto** (no es un rígido), y
`physics_engine.cpp` integraba `gvec * dt` en TODOS los pasos. A 83 m/s el personaje pide meterse
**1,3 m dentro del terreno cada frame** y la colisión lo expulsa: caro (pesado), inestable (el sentido
de la expulsión depende de qué triángulos toque → "me redirige", "pisa un montículo") y errático con el
paso de escalera. Los cuatro síntomas, una causa. La tangencial era 0,005 m/s: **no resbalaba**.

**Arreglo** (patrón de los ejemplos de Jolt): la gravedad se integra **sólo en el aire**; apoyado se
anula la componente que empuja contra el suelo. A `ExtendedUpdate` se le sigue pasando `inGravity`,
que la usa para el escalón y el pegado al suelo.

| quieto sobre el suelo | antes | ahora |
|---|---|---|
| v radial tras 8 s | −83,4 m/s | **±0,000 m/s** |
| v tangencial | 0,005 m/s | 0,000 m/s |

Test de regresión `physics_character_resting_velocity`, con **contraprueba del propio test**: con el
`if` desactivado a mano da 78,5 m/s y FALLA; con el arreglo, 0,163 m/s. Y dos contrapruebas dentro
(sin ellas lo aprobaría un motor sin gravedad, que es peor que el bug): **(B)** en el aire la gravedad
sigue viva (−9,802 m/s tras 1 s) y **(C)** apoyado, la velocidad tangencial que se le impone sobrevive
(anda a 4,00 m/s). ⚠️ `Y=0,500` en los dos casos: **no se hundía**, sólo se clavaba — por eso el bug
era invisible mirando la posición.

### ✅ CERRADO — "al girar la cámara aparece un segundo terreno encima" (2026-08-26)

**El fallback por ancestro dibuja el nodo ancestro ENTERO**, y un ancestro cubre a sus cuatro hijos, no
solo al que falta. Si una hoja no está residente pero sus hermanas sí, se emiten las dos cosas: el
padre (que tapa las cuatro cuartas partes) y las hermanas finas. Dos superficies en el mismo sitio. Y
como el ancestro puede estar muchos niveles por encima, **una sola hoja que falta en el borde del
frustum pinta una sábana sobre toda la vista**.

Medido con la cámara girando y el pool **caliente** (`terrain_node_overlap_on_turn`, 60 frames de
calentamiento antes de medir — hacerlo en frío mide la carga inicial, que es otra cosa):

| girando | emitidos | tapados por un ancestro |
|---|---|---|
| como estaba | 499 | **457 (91,6 %)** |
| re-resolviendo | 252 | **0** |
| quieto (contraprueba) | 252 | 0 |

Las dos superficies se separan hasta **20,6 m** (el peor tapado, 7 niveles bajo su ancestro).

**No era falta de recursos**, y esto descartó la explicación fácil: 0 desalojos, 364 residentes de
2048, y 8-20 fallos por frame contra un presupuesto de 170. Lo que faltaba era **usar lo que ya se
había generado antes de dibujar**: la resolución se hacía ANTES de generar, así que la hoja nueva se
resolvía contra un pool que todavía no la tenía. Traza por frame del giro:

    f1 tapados  59 (caida 4 niv)    f4 tapados 274 (caida 7 niv)
    f2 tapados  18 (caida 2 niv)    f5 tapados  35 (caida 2 niv)

Un destello de un frame, cada dos frames, mientras giras.

**Dos arreglos, los dos medidos:**
1. **Re-resolver tras generar**, en el mismo frame (`terrain_node_renderer.h`). 457 tapados → 0, y de
   regalo el conjunto dibujado baja de 499 a 252 instancias: los ancestros gordos ya no se emiten.
2. **`nodeCoveredMask`** deja el conjunto como una PARTICIÓN (solo los maximales) para cuando el
   presupuesto no dé de sí. No abre agujeros —lo que se quita estaba tapado por algo que sigue— y **no
   empeora el LOD: el peor error en pantalla es el mismo, 31,9 px**, porque el ancestro ya se dibujaba.
   Solo corre si hubo alguna caída por ancestro; en régimen no corre.

⚠️ **El log casi lo esconde.** `TerrenoV5` imprime 1 de cada 120 frames y el valor instantáneo daba
`TAPADOS 0` siempre. Con el **pico de la ventana**, el mismo log en el juego da `TAPADOS 1238 a 14
niveles` — peor que en el banco headless. Para un artefacto de un frame hay que loguear el máximo del
intervalo. (Y el vsync no revela un destello, lo **alarga**: con los fps capados dura más en pantalla.)

### 🟡 ABIERTO — la caída por ancestro llega a 14 niveles, y SOLO mientras entra terreno (2026-08-26)

Con la doble superficie ya quitada, queda el otro artefacto: la caída por ancestro se va a **14
niveles** (una hoja de nivel 17 resolviendo a un nivel 3) y con la partición eso dibuja **el nodo
grueso en vez de** los finos que tapa. Un frame, pero se ve.

**Lo que la medida SÍ dice** (sonda `TerrenoV5`, picos por ventana de 120 frames):

    res 1624/2048  desal    0 (rompen    0)  VIVOS 2451/2048  TAPADOS 1246 a 14 niv · SIN GENERAR 0
    res 1624/2048  desal    0 (rompen    0)  VIVOS  188/2048  TAPADOS    0 a  0 niv · SIN GENERAR 0
    res 2048/2048  desal  373 (rompen    0)  VIVOS 1555/2048  TAPADOS  204 a  5 niv · SIN GENERAR 0
    res 2048/2048  desal 5894 (rompen  872)  VIVOS 1685/2048  TAPADOS 1124 a 14 niv · SIN GENERAR 0
    res 2048/2048  desal 8227 (rompen 1335)  VIVOS 1709/2048  TAPADOS 1209 a 14 niv · SIN GENERAR 0

- **`SIN GENERAR 0` en TODAS las ventanas.** No hay ni un solo evento en un frame que no estuviera
  generando nodos: **no es un fallo de estado, es el transitorio del streaming**. Esa columna es la que
  decide y por eso está separada — un único número no distingue las dos cosas.
- **`rompen cadena` sigue a los desalojos**: 0, 0, 0, 872, 1335. El LRU sí rompe eslabones, pero solo
  cuando ya está desalojando.
- **La capacidad ayuda y no cura**: a 4096 quedan 197 roturas y 12 niveles (contra 872-1335 y 14). Y
  `residentes == capacidad` NO es señal de nada: cualquier LRU acaba lleno, también a 4096.

**Lo que se probó y NO sirve, con la medida, para que nadie lo repita:**
- **Proteger en el LRU a los nodos con hijos residentes.** Llegaba a saltar 146-205 huecos en el
  barrido, pero ninguno era el que el LRU iba a elegir: resultado idéntico al último dígito en los tres
  regímenes. Revertido. `terrain_node_pool_chain_eviction` guarda la medida.
- ⚠️ **Y ese test headless daba `rompen cadena = 0` mientras el juego daba 358-1335.** El banco no
  reproducía la condición (regenera todo cada frame y el orden nunca la destapa). **La conclusión buena
  salió del juego, no del test** — anotado porque el error fue creer al banco.

**Lo que sí se arregló de camino:** `publish` ponía `lastUsed = m_frame` en el nodo nuevo **sin tocar a
sus ancestros**, y `touchAncestors` corta en cuanto ve un eslabón ya marcado con este frame. La
siguiente hoja que sube se paraba ahí y el padre se quedaba con la marca vieja. Es una violación real
de la suposición del corte. ⚠️ **Arreglado, pero NO se le puede atribuir mejora**: la varianza entre
ejecuciones (358, 565, 872, 1303, 1335) se come el efecto.

**A ojo, para distinguir los dos artefactos** (son distintos y solo mirándolos se separan):
`HARUKA_TERRAIN_V5_NOPART=1` apaga la partición — con ella se ve un parche GRUESO, sin ella se ven las
DOS superficies peleando por el z-buffer.

### ✅ CERRADO — la huella del fallback y los 390 MB del pool eran el mismo problema (2026-08-26)

Andoni: *"creo que es de arquitectura y cómo está creado más que de presupuesto"*. Lo era.

**El fallo.** La identidad GEOMÉTRICA de un nodo estaba atada a su slot de datos. Si la hoja pedida no
estaba residente, el pool devolvía un ancestro y se dibujaba **la huella del ancestro**, que cubre 4^k
hojas. Medido con la cámara girando y el pool caliente (`terrain_node_fallback_footprint`):

    2 hojas sin residencia, caida de 5 niveles
      area rota con la huella del ANCESTRO : 101,66 % de lo visible
      area rota con la huella de la HOJA   :   0,12 %
      -> el fallback estropea 820x su propia area

**DOS hojas de ~250 rompían la pantalla entera.** Por eso ningún ajuste del pool movía el número, y
por eso las tres cosas que probé antes (proteger la cadena en el LRU, presupuesto por segundo, más
capacidad) no sirvieron: atacaban la frecuencia del fallo, no su amplificación.

**Y no había regla de "dibujar o no" que saliera del dilema** — las dos se probaron EN EL JUEGO:

| regla | veredicto | qué pasa |
|---|---|---|
| basta un descendiente dibujado | *"ha mejorado, por lo menos el cercano, el lejano sigue mal"* | quita la sábana pero deja sin dibujar la huella de la hoja |
| tapado ENTERO | *"con este cambio es peor en general"* | no abre agujero, pero lejos no se cumple y la sábana se queda |

**EL ARREGLO — la hoja se dibuja SIEMPRE en su propia huella.** Si los datos vienen de `k` niveles más
arriba, lee el sub-rectángulo que le corresponde en la rejilla del ancestro: el téxel `t` cae en
`(cells·sub + t) / 2^k` con `sub = índice mod 2^k`, con bilineal entre téxeles
(`harukaNodeSample` en `terrain_node.vert`). Ni sábana ni agujero: un trozo menos detallado, del
tamaño que le toca, que se afina al frame siguiente.

`terrain_node_subrect_mapping` prueba que la coordenada apunta **al mismo punto de la cara del cubo**
que el téxel de la hoja: **0,000000 m** en 374 casos (niveles 6/11/17 × k=1..8, con índices NO
alineados a propósito). Contrapruebas: un téxel de más desplaza **1 221 m**; olvidar el sub-índice,
**3 283 km**.

**Y de ahí sale el otro problema, el de la memoria.** Cada nodo guardaba TRES mapas —el suyo, el del
padre y el del abuelo— sólo para poder fundirse hacia arriba, porque el shader únicamente sabía leer
su propio hueco. Sabiendo leer el sub-rectángulo del hueco del padre, las copias sobran:

| | antes | ahora |
|---|---|---|
| por nodo | 195 KB (3 mapas) | **65 KB (1 mapa)** |
| huecos | 2 048 | **4 096** |
| VRAM del pool | 390 MB | **260 MB** |
| pico de nodos VIVOS a la vez | 2 445 — **no cabían** | 1 747-2 385, caben |

⚠️ **Es además MÁS fiel**: el vecino grueso dibuja TRIÁNGULOS entre sus muestras, no la función
evaluada en mi punto. Leer su rejilla interpolada es exactamente la superficie contra la que hay que
cerrar la costura; la copia apuntaba a otra cosa parecida.

⚠️ **Contrapartida declarada**: el morph ahora depende de que el PADRE esté residente (antes la copia
estaba siempre). Si no está, `parSlot = -1` y no se morfea, en vez de leer el hueco 0 —que sería un
trozo de planeta cualquiera—. Lo mismo para el abuelo y para el salto de mapa por stride.

En el juego: `sel N -> dibujados N` con **SIN HUECO 0** siempre, y el pico de `por ancestro` baja de
520-1 200 a **0-216** pasada la carga.

⚠️ **LO QUE NO SE PUEDE AFIRMAR, Y YO LO AFIRMÉ**: que el RSS total del proceso bajara. Seis tomas del
mismo binario dan **823, 1 063, 1 322 MB con 2 048 huecos y 1 334, 1 038, 1 244 MB con 4 096**: los
rangos se solapan enteros. La dispersión entre ejecuciones (±500 MB) se come el efecto. Por el mismo
motivo, el *"la máscara costaba 530 MB"* que escribí antes (1 358 contra 827) **no está sostenido** —
cae dentro de esta misma dispersión. Lo único determinista es lo que el motor reserva y loguea.
**Para hablar de RSS hace falta un método que controle esa varianza.**

### 🔴 ABIERTO — el coste real del frame: NO es generar, es DIBUJAR (2026-08-26)

Andoni: *"se ve como el terreno se reconstruye"* y *"hay como 30 ms en juego sin mostrar en panel"*.
Las dos cosas tienen la misma respuesta y sale del profiler.

**Los 30 ms que faltaban en el panel.** El panel mide scopes de **CPU**, y la CPU no hace nada:

    game.onUpdate                      0.15 ms
    renderFrameContent                 2.33 ms   <- TODO el trabajo de CPU
      frame.compute.prepare              1.03 ms
      scene.prop.instanced               0.61 ms
      planet.v5.nodes                    0.12 ms
    present.swap(espera GPU/vsync)    31.24 ms   <- AQUI, y con HARUKA_NO_VSYNC=1

O sea: el frame es **espera de GPU**, y el profiler no tiene timers de GPU, asi que todo el coste
aparece agregado en `present.swap`. **Falta instrumentar la GPU por pase.**

**Coste de GENERAR un nodo**, medido con fence (`v5 F1: coste de generar nodos en GPU`), nivel 18,
16 641 texeles por nodo, tanda amortizada de 256:

| nodos por tanda | ms/nodo total | sin evaluar relieve | relieve |
|---|---|---|---|
| 1 | 0,1175 | — | (una sola tanda: ruido) |
| 16 | 0,0592 | 0,0183 | 69 % |
| 256 | **0,0555** | 0,0151 | **73 %** |

Reparto: **relieve 0,0404 ms/nodo · proyeccion+escritura 0,0151**. Y el A/B del cambio de esta sesion
(tres mapas por nodo -> uno): **0,1393 -> 0,0555 ms/nodo, 2,5x mas barato** (backend lento: 0,2016 ->
0,1228, 1,64x).

⚠️ **No hay mas que rascar por nodo**: el 73 % es evaluar el relieve — **56 hashes por texel** (8
esquinas x 7 octavas) — y eso no se abarata sin cambiar los valores, lo que romperia la paridad con la
fisica. El ruido ya hace el `floor` en double y el hash y la interpolacion en float.

**PERO GENERAR NO ES EL PROBLEMA.** A/B sobre el mismo binario (Vulkan, sin vsync):

| config | `present.swap` (GPU) | frame | triangulos |
|---|---|---|---|
| base (VERTPX=4) | **29,8-32,8 ms** | 35,1 ms | 6,9 M |
| VERTPX=8 | **12,2-12,6 ms** | 16,9 ms | 2,0 M |
| sin pase v5 (`HARUKA_TERRAIN_V5=0`) | **7,9-8,1 ms** | 17,0 ms | — |

**El pase de terreno cuesta ~22 ms de GPU**, y baja a 4,3 ms con 3,5x menos triangulos. Generar 85
nodos son 4,7 ms; **dibujarlos, 22**. Confirmado ademas por el A/B del presupuesto: bajarlo de 170 a
85 nodos/frame **no movio el frame** (OpenGL 70-73 ms, Vulkan 33,4-33,9).

✅ **DECIDIDO POR ANDONI MIRANDO: `VERTPX = 8` pasa a ser el valor por defecto.** Su veredicto con 8:
*"se sigue viendo bien y sin 20 ms de GPU"*. Son **18 ms de GPU** por una diferencia visual que el
autor da por buena. El coste del terreno nunca fue generarlo, sino dibujarlo.

**Reparto de la GPU con VERTPX = 8** (Vulkan, `HARUKA_NO_VSYNC=1`, por diferencia entre configs):

| A/B | GPU (`present.swap`) | lo que atribuye |
|---|---|---|
| base | 12,65-12,89 ms | — |
| `HARUKA_NOBLOOM=1` | 11,17-11,52 ms | **bloom ≈ 1,4 ms** |
| `HARUKA_TERRAIN_V5=0` | 8,05-8,13 ms | **terreno ≈ 4,6 ms** |
| las dos | 5,28-5,90 ms | **fondo ≈ 5,6 ms** (cielo, mar, props, nubes, composite, UI) |

Descartados con medida: props ≈ 0-0,4 ms · sombra ≈ 0 · agua ≈ 0,4 ms · `RenderScale = 1,00` (no hay
supersampling) · apagar las nubes sale **mas caro** (13,4 contra 12,0: el camino alternativo de
`sky.frag` cuesta mas que el volumetrico).

⚠️ **UNA MEDIDA MIA QUE NO VALIA, Y POR QUE.** El primer interruptor de post saltaba el pase ENTERO con
un `return` y daba "1,0 ms", que leido a la ligera dice "el post cuesta 11,5 ms". Es falso: **ese pase
es quien vuelca la escena al swapchain**, asi que sin el no hay nada que presentar y `present.swap` no
espera a nada. El interruptor bueno apaga SOLO el bloom (`HARUKA_NOBLOOM`) y deja el composite. Regla:
un interruptor de A/B que ademas quita el trabajo de presentar no mide un pase, mide otra cosa.

⚠️ **Y FALTA LO DE FONDO: EL PROFILER NO MIDE GPU.** Todo esto se ha sacado apagando pases y restando,
porque el RHI no tiene *timestamp queries* y las fences señalan al ENVIAR el frame, asi que no sirven
para cronometrar dentro. Mientras no las haya, el panel seguira enseñando 2,5 ms de CPU y escondiendo
el resto en `present.swap`. **Instrumentar la GPU por pase es la tarea que desbloquea las demas.**

⚠️ Y de camino se separo el presupuesto de generacion de la capacidad del pool: era `capacity/24`, asi
que subir el pool a 4 096 por COBERTURA doblo tambien el ritmo de generacion a 170 nodos/frame sin que
nadie lo pidiera. Ahora es 85 fijo, documentado con la medida.

### ⏸️ PAUSA en terreno/rendimiento — siguiente frente: VULKAN POR DEFECTO (2026-08-26)

Decision de Andoni: *"apuntar todo, hacer una pausa con esto y arreglar por completo el vulkan para
que se vea bien y sea el default definitivo"*.

**Estado en el que se deja el terreno** (todo medido, nada pendiente de verificar salvo lo que se dice):

| pieza | estado |
|---|---|
| gravedad del personaje acumulandose apoyado | ✅ cerrado, con test y contraprueba |
| "segundo terreno al girar" (resolver antes de generar) | ✅ cerrado, 457 tapados -> 0 |
| huella del fallback (sub-rectangulo) | ✅ cerrado, 820x -> 1x, mapeo exacto a 0,000000 m |
| 3 mapas por nodo -> 1 | ✅ 195 -> 65 KB/nodo · pool 2 048 -> 4 096 huecos · 390 -> 260 MB |
| presupuesto atado a la capacidad | ✅ separado, 85 nodos/frame fijos |
| `VERTPX` 4 -> 8 | ✅ por defecto, validado a ojo por Andoni: −18 ms de GPU |
| reparto de la GPU | ✅ terreno 4,6 ms · bloom 1,4 ms · fondo 5,6 ms |

**Lo que queda apuntado y SIN hacer**, por orden de lo que desbloquea:
1. **Timestamps de GPU por pase en el RHI.** Es la tarea que desbloquea las demas: hoy el panel enseña
   2,5 ms de CPU y esconde el resto en `present.swap`, y todo el reparto de arriba ha salido apagando
   pases y restando. Las fences no sirven (señalan al ENVIAR el frame).
2. **Tope de stride para los nodos lejanos.** Llegaba a 4 porque solo habia tres mapas; con el
   sub-rectangulo ese limite ya no existe. Quita triangulos **donde no se notan**, al reves que
   `VERTPX`, que los quita en todas partes.
3. El outlier del alambre de colision (radio 78,4 m, −0,0418 m, anillo 2).
4. El escalon de 0,785 m entre niveles (techo estructural del morph: un nodo dibujado siempre tiene
   `morph < 1`).
5. Esquinas en orbita · props sin geomorph.

### 🔴 EN CURSO — Vulkan bien visto y por defecto (2026-08-26)

**Primera pista, y encaja con `vulkan-is-opengl-assumed`**: `camera.cpp` compensa el eje Y de Vulkan
con `p[1][1] = -f` (invertir el VIEWPORT se probo y rompe el post: espeja los quads a pantalla
completa y con el bloom iterando cambia la PARIDAD de espejados). Pero invertir la PROYECCION **espeja
la geometria 3D**, y con ella el bobinado efectivo que ve el rasterizador — mientras
`vk_pipeline.cpp` declara `VK_FRONT_FACE_COUNTER_CLOCKWISE` **siempre**.

Hipotesis: para geometria 3D, Vulkan deberia tratar como frontal el bobinado CONTRARIO al de GL. Eso
explicaria exactamente lo unico que esta medido de este fallo:

    con CullMode::Back    OpenGL cobertura 97,4 %   ·   Vulkan 3,4 %  (descarta la cara BUENA)

⚠️ Y lo que hay que explicar ANTES de tocarlo: otros pipelines con `cull = Back` (props, malla base,
`nearground.vert`) *parecen* verse bien en Vulkan. O su malla esta bobinada al reves y los dos
espejados se cancelan, o estan igual de mal y nadie lo ha mirado. **Hasta saber cual de las dos, no se
toca `frontFace`**: es un cambio global.

❌ **HIPOTESIS DEL BOBINADO: REFUTADA.** `testCullWindingWithProjection` ya lo prueba y pasa en los
dos backends: un triangulo antihorario con la proyeccion REAL del motor y `cull = Back` **se ve** en
GL y en Vulkan. El convenio de cara frontal no esta invertido.

### ✅ CERRADO — Vulkan renderizaba TODO a 720p y lo estiraba a 1080p (2026-08-26)

Andoni: *"imgui no es nitido pero creo que es por el escalado en pantalla completa"*. La intuicion era
correcta y el alcance mucho mayor: **no era la UI, era el juego entero**.

Sonda `HARUKA_UI_SCALE_LOG=1`, que imprime los TRES tamanos que tienen que casar:

    OpenGL   SDL en pixeles 1920x1080 · DisplaySize 1920x1080 · escala 1.000 -> la UI pinta 1920x1080
    Vulkan   SDL en pixeles 1920x1080 · framebuffer 1280x720  · escala 0.667 -> la UI pinta 1280x720
                                                                 FontGlobalScale 1.500

**Causa raiz** (sonda en `VKSwapchain::chooseExtent`):

    swapchain extent: surface dice 0xFFFFFFFF (indefinido: mando yo) · SDL en pixeles 1280x720

La ventana **arranca a 1280×720** y crece despues. En Wayland el surface responde "decide tu"
(`currentExtent = 0xFFFFFFFF`) y **no marca el swapchain como obsoleto al redimensionar**: esperar a
`OUT_OF_DATE` es asumir una semantica que Wayland no da. OpenGL no lo sufre porque su framebuffer por
defecto sigue a la ventana solo. Es otra vez el patron de `vulkan-is-opengl-assumed`.

**Arreglo**: `VKDevice::beginFrame` compara el tamano REAL en pixeles con el del swapchain y lo recrea
si no casan. Dos enteros por frame, sin depender de que el driver avise.

    la ventana es 1920x1080 y el swapchain 1280x720: recreando
    ventana 1920x1080 · framebuffer 1920x1080 · escala 1.000 · FontGlobalScale 1.000

La UI sale nitida porque su atlas deja de rasterizarse a 8,7 px para agrandarse x1,5.

⚠️ **Y ESTO INVALIDA TODAS LAS COMPARATIVAS GL↔VULKAN DE RENDIMIENTO DE ESTA SESION.** "Vulkan 35 ms
contra OpenGL 70 ms" comparaba **el 44 % de los pixeles** contra el 100 %. Los repartos de GPU por pase
(terreno 4,6 ms, bloom 1,4 ms, fondo 5,6 ms) se midieron TODOS en Vulkan a 720p: **las proporciones
entre pases siguen valiendo, las cifras absolutas no**. Hay que rehacerlas.

### ✅ CERRADO — "props y terreno se ven quemados" en Vulkan: era el BLOOM (2026-08-26)

**El bug**: el pase de bloom hace 1 + 2xN draws con parametros distintos y los escribia todos en el
MISMO UBO. En OpenGL cada draw se ejecuta al vuelo y eso funciona; **en Vulkan los comandos se GRABAN
y todos leen el ULTIMO valor**. Resultado: el bright-pass se quedaba con `threshold = 0` —la escena
ENTERA entraba al bloom— y el desenfoque horizontal se volvia vertical.

⚠️ **El propio codigo lo avisaba y nadie lo cerro**: *"OJO (Vulkan): ... habra que usar offsets
dinamicos o un UBO por draw. Anotado para cuando entre el VKContext."* Es el MISMO fallo que ya se
arreglo en el pase de terreno (un SSBO por grupo). **Van dos; hace falta un test del banco que lo cace
a la tercera**: dos draws con valores distintos en el mismo UBO y blend aditivo, que con el bug daria
`B+B` en vez de `A+B`. Sin hacer.

**Arreglo**: `m_bloomUBOs`, uno por draw, creados bajo demanda y reutilizados entre frames (tope real
1 + 2x8 = 17).

| hora congelada | OpenGL | Vulkan | ratio |
|---|---|---|---|
| antes, a=1.1 | 7,6 15,1 13,3 | 11,3 23,3 19,4 | 1,50 |
| antes, a=4.2 | 113,6 181,5 96,7 | 187,7 235,0 158,6 | 1,48 (el verde ya saturando a 235) |
| **ahora, a=1.1** | 7,6 15,1 13,3 | **7,6 15,1 13,3** | **1,00** |
| **ahora, a=4.2** | 113,6 181,5 96,7 | **113,6 181,5 96,6** | **1,00** |

**Y NADA DE ESTO SE PODIA MEDIR AL PRINCIPIO.** Hicieron falta tres herramientas, cada una porque la
medida anterior mentia:

1. **`HARUKA_DAY_ANGLE=<radianes>` congela la hora del dia.** El angulo del Sol se acumula con el `dt`,
   asi que dependia del tiempo de carga: el MISMO OpenGL dio `29,5 32,8 38,3` y `50,1 96,3 60,2` en dos
   tomas. Con esa varianza llegue a atribuir a Vulkan un frame "1,64x mas claro" — **retirado**.
2. **La captura del juego va DESPUES del present.** `readPixels` copia la ultima imagen PRESENTADA;
   llamarlo a mitad de frame devolvia en Vulkan algo que no era la escena: daba los MISMOS valores con
   el Sol en cualquier sitio y hasta con el terreno apagado (`cambia 0,0` contra `355,8` en GL). Es la
   regla que el banco de RHI ya documentaba y que `application.cpp` no seguia.
3. **El volcado invertia las filas siempre** (convenio de GL), asi que la captura de Vulkan salia boca
   abajo. Ahora el orden depende del backend.

**Bisecar fue lo que lo cerro**, no mirar: con `HARUKA_NOBLOOM=1` los dos backends daban la MISMA
imagen (`112,8 182,2 96,2` contra `112,8 182,3 96,2`), y con el bloom encendido 1,48x. Eso señalaba el
pase sin ambiguedad.

### ✅ CERRADO — "las lineas de triangulos son blanco-azuladas": el ANILLO CERCANO sobraba (2026-08-26)

**No era el cielo colandose por las costuras**, y no era Vulkan. Test nuevo del banco (`escena: el
fondo NO se cuela por las costuras`): fondo MAGENTA, terreno encima con la vista 2, y se cuenta el
magenta superviviente. Al nadir, donde el terreno cubre el cuadro entero:

    sin dibujar :  65 536 de 65 536 magenta (100,0 %)   <- CONTRAPRUEBA del detector
    dibujando   :       0 de 65 536 magenta ( 0,0000 %)

Cero agujeros, en los dos backends.

**Era el anillo cercano peleando por el z-buffer.** Energia de borde en la captura del juego (|dif| con
el pixel de al lado; las lineas finas la disparan), hora congelada, con el post ENCENDIDO:

| | anillo ON | anillo OFF |
|---|---|---|
| OpenGL | 5,33 | **1,42** |
| Vulkan | 5,34 | **1,43** |

**Identico en los dos backends: no era un problema de backend.** El anillo dibuja dentro de ±192 m los
MISMOS triangulos que se pisan, y se diseño contra el CLIPMAP, **que le abria un hueco de 192 m**. El
pase v5 nunca abrio ese hueco: dos superficies coplanares sobre el mismo relieve. Literalmente lo que
la cabecera del v5 dice que viene a quitar.

**Arreglo**: no se dibuja cuando el pase v5 esta activo. `HARUKA_NEAR_RING=1` lo fuerza para comparar.
Lo que se pierde es la paridad EXACTA triangulo-a-triangulo cerca del jugador, y ya no hace falta: el
autotest del alambre mide **0,0004 m de media** con un unico outlier de 0,0418 m — 4 cm en el peor
vertice. Verificado por defecto: **borde 1,42 en OpenGL y 1,43 en Vulkan**.

⚠️ **DOS MEDIDAS MIAS QUE NO VALIAN, Y POR QUE:**
- **`HARUKA_NOBLOOM=1` NO apaga solo el bloom.** `m_postActive = (rscale != 1 || wantFXAA ||
  wantBloom)`, asi que con escala 1 y sin FXAA apaga **todo el post** y la escena deja de pasar por el
  target offscreen. La primera bisecion ("el bloom cuesta 1,48x") comparaba dos CAMINOS DE RENDER
  distintos. ⚠️ El arreglo del bloom (un UBO por draw) sigue en pie por otra via: la verificacion final
  fue con bloom ENCENDIDO en los dos backends y dio ratio 1,00.
- **Y el camino directo al backbuffer NO es la causa de nada aqui**: medido, la energia de borde es la
  misma con post (5,41) y sin post (5,43) en los dos backends. La hipotesis de la profundidad en punto
  fijo era plausible —OpenGL no pide `SDL_GL_DEPTH_SIZE` y Vulkan si crea su backbuffer en D32_SFLOAT—
  pero **la medida la descarta**. Queda anotada por si aparece otro sintoma por ahi.

### ✅ AÑADIDO — el banco ya dibuja UNA ESCENA: terreno + prop con luz (2026-08-26)

Pedido por Andoni: *"quiero que un test de motor dibuje una prueba de terreno y objeto como un prop con
iluminacion"*. El banco tenia `testTerrainLighting` y `testPropLighting` por separado, y el sintoma que
se persigue —*"props y terreno se ven quemados o sin luz"*— es justamente el que no se ve asi.

`escena: terreno + prop en el mismo pase, los dos con luz` dibuja los dos en el MISMO render pass con
la MISMA direccion de sol, dos veces (sol de frente y al otro lado), y mide la luminancia de cada uno
por separado dentro de la misma imagen — el prop en el centro, el terreno en una esquina:

    terreno (esquina)  137,8 -> 42,0   (cae 69,5 %)
    prop    (centro)   204,6 -> 58,6   (cae 71,3 %)

**Los dos responden al sol, y casi en la misma proporcion.** Asi que el camino de sombreado del prop NO
esta roto: lo blanco que se ve en el juego viene de otro sitio (material real sin albedo, tinte de
instancia o LOD), no del shader. Es un negativo util: acota donde NO buscar.

Para dibujar los dos en un pase hubo que partir `propLitPixel`: ahora acepta un `Context*` y devuelve
sus recursos en `PropRes`. ⚠️ No se destruyen dentro — en Vulkan los comandos aun no se han ejecutado
al volver y seria un uso-despues-de-liberar; los destruye el llamador tras `endFrame`.

Las dos capturas entran ademas en el careo GL↔Vulkan, y ahi el resultado es el que se queria ver:

    escena.terreno+prop sol A   0,00 % de pixeles distintos · |delta| medio 0,02 · peor canal 1
    color medio  OpenGL 203,5 205,0 206,4  ·  Vulkan 203,5 205,0 206,4

**Identicos.** Con el bloom y el swapchain arreglados, los dos backends dibujan la misma escena.

### 🟡 ABIERTO — "toda la cara con la misma luz, y es de Vulkan": CINCO hipotesis refutadas (2026-08-26)

Reportado por Andoni. **No he podido reproducirlo**: todas las medidas dicen que los dos backends
dibujan lo mismo. Lo que se ha descartado, con su dato, para que nadie lo repita:

| hipotesis | medida | veredicto |
|---|---|---|
| Calificador `flat` desajustado entre vertice y fragmento | 80 emparejamientos plausibles sacados del C++ | **0 desajustes** |
| Color por vertice leido como por INSTANCIA | captura .rdc: Vulkan 24 bindings `RATE_VERTEX` + 4 `INSTANCE`; GL, divisores 0 salvo dos bindings a 1 | correcto en los dos |
| Provoking vertex / polygon mode | 29 pipelines, todos `POLYGON_MODE_FILL`, sin la extension | descartado |
| Los backends dibujan distinto | diff pixel a pixel del juego, hora congelada | **solo difiere el texto del profiler** (esquina sup. izq.); \|delta\| medio 0,05 |
| Mi cambio de `VERTPX` 4 -> 8 (la mitad de vertices, normales mas bastas) | mismo suelo, A/B | **identico**: 17 colores y 99,5 % de vecinos iguales en los dos |

Y en el banco, la escena de terreno+prop carea **0,00 % de pixeles distintos** entre backends.

⚠️ **Una metrica mia que NO sirve para esto**: contar colores distintos / vecinos identicos no
distingue sombreado plano de degradado suave a 8 bits. El CIELO —degradado por construccion— da 40
colores y 99,6 % de vecinos iguales, igual que el suelo. Solo vale para A/B como el de `VERTPX`.

Las dos capturas (`testVColor.rdc` de Vulkan y `testGLColor.rdc` de OpenGL) envian geometria
comparable: 36 draws / 1 335 463 indices / 8 355 instancias contra 28 / 1 331 278 / 8 169. Son de
momentos distintos, asi que no se pueden carear draw a draw.

✅ **Y EL HUECO DEL BANCO QUE SÍ ERA REAL, cerrado.** Andoni: *"es un error que ya existia desde la
creacion del RHI"*. El hueco lo estaba: `testVertexColor` lo dice en su propio comentario —*"los tres
vertices del MISMO color: asi el centro es ese color exacto y no hay que razonar sobre la
interpolacion"*—. Comprueba que el atributo LLEGA, pero **con los tres iguales un fallo de
interpolacion lo pasa sin despeinarse**: un triangulo plano daria exactamente el mismo pixel central.

Nuevo `testVertexInterpolation`: UN triangulo con TRES colores (R/G/B) y cuatro puntos leidos.

    esquina A (231, 12, 12) · esquina B (127,116, 12) · esquina C (127, 12,116)
    centro    (127, 64, 64)
    separacion maxima entre esquinas: 104   (plano daria ~0)

**El RHI SI interpola**, y con cifras identicas en los dos backends. Asi que la hipotesis tampoco es
esa — pero el test que faltaba desde el principio ya esta, y a partir de ahora un fallo de
interpolacion no puede colarse.

**Lo que hace falta para seguir**: el EID del draw concreto que se ve mal en RenderDoc. Con eso se
puede mirar SUS entradas en el XML convertido en vez de razonar sobre el frame entero.

### ✅ CERRADO — el arranque estaba dominado por DESCOMPRIMIR PNG (2026-08-26)

`perf record` de 40 s del juego (Vulkan, RTX 3050). El perfil **no habla del frame, habla de la carga**:

    15,71 %  stbi__create_png_image_raw       5,34 %  stbi__fill_bits
    12,83 %  stbi__parse_zlib                 3,81 %  memmove
     1,71 %  stbi__create_png_alpha_expand8   2,40 %  bakeHeightMap

**~35 % de TODOS los ciclos del proceso descomprimiendo PNG**, y ni un punto caliente de render en
CPU — coherente con que el frame sea GPU-bound (`present.swap` se lleva el 83 %). ⚠️ Por eso un
profiler de CPU en este motor engaña por omision: mide el arranque, no el frame.

Las 8 capas de terreno de 4096x4096 se decodificaban **en serie**, una detras de otra:

    antes    4 240 ms + 5 654 ms = 9,9 s
    ahora    1 211 ms + 1 642 ms = 2,9 s     <- 3,5x, siete segundos menos de arranque

Son independientes (cada una escribe su propio hueco), asi que el bucle es paralelo por construccion;
`minLong` se calcula al unir, que es lo unico compartido.

⚠️ **Y se demuestra que carga LO MISMO, no se supone.** `HARUKA_TEX_SERIAL=1` vuelve a un hilo, y
comparando la captura del juego con la hora congelada: **0,0000 % de pixeles distintos, peor canal 0**.
Byte a byte igual.

**Lo que queda por ahi**: el bake sigue costando (`bakeHeightMap` 2,4 %) y las texturas se guardan en
PNG. Un formato ya descomprimido o comprimido para GPU (BCn/KTX2) quitaria el 35 % entero en vez de
repartirlo entre hilos. Sin hacer.

### ⚠️ MÉTODO — congelar la hora NO basta: hay que congelar la CÁMARA (2026-08-26)

`HARUKA_DAY_ANGLE` hace comparables dos ejecuciones en cuanto al Sol, pero **no en cuanto a adónde
mira la cámara**. Y sin eso, una región fija de la pantalla no significa nada. Me costó **tres
conclusiones falsas el mismo día**, y las tres dieron números creíbles:

1. Una medida de "suelo" cayó sobre la **barra de objetos** (colores planos de UI).
2. Otra sobre el **fondo entre los árboles**, no sobre los árboles.
3. La peor: sobre el **CIELO entero**. Concluí que *"la textura del terreno no llega a la pantalla"*
   con detalle local 0,12 — y era cielo. **Mirando al suelo de verdad da 8,30.** ❌ Retirada.

Añadido `HARUKA_CAM_PITCH=<grados>`, **respecto al horizonte LOCAL** (sobre una esfera un pitch
absoluto no quiere decir nada): conserva el rumbo y fija la inclinación. 0 = horizonte · −60 = el suelo
llena el cuadro · +90 = cenit.

⚠️ **Y una trampa que se llevó por delante otra medida**: `HARUKA_TERRAIN_V5_DEBUG=2` saca BLANCO del
shader del terreno, pero lo que llega al framebuffer ha pasado por atmósfera, nubes y post — así que
llega oscuro. Contar píxeles blancos daba "el pase v5 cubre el 0,3 % del cuadro". Contando los que
**CAMBIAN** al activar la vista, sin umbral de color: **89,3 %**. Para medir la huella de un pase, la
diferencia entre dos vistas es fiable; un umbral absoluto sobre el resultado final, no.

**Lo que sobrevive de las medidas de hoy** (cuadro completo o pares con la misma cámara en los dos
lados, así que no dependen de dónde se mire): el swapchain, el bloom, el anillo cercano, la carga de
texturas y el careo GL↔Vulkan. **Lo que no**: cualquier cifra por región anterior a este cambio.

### 🔴 ABIERTO — la luz de los props es un ESCALON, no una curva (2026-08-26)

**Lo encontro una idea de Andoni**: *"por que no pruebas a hacer un prop con luz en RHI completo?"*.
`testPropLighting` solo miraba DOS puntos —sol de frente y sol detras—, y con los extremos bien pasan
igual una respuesta correcta, una plana y una invertida. `testPropLightSweep` barre 12 angulos:

    204.7 204.6 204.3  82.2  58.7  58.7  58.7  58.7  58.7  82.2 204.3 204.6
      ###   ###   ###     #     #     #     #     #     #     #   ###   ###

**No es una curva, es un escalon.** La luz gira 60 grados y el prop no cambia ni una decima; hay UN
solo valor de transicion; medio circulo pegado a 58,7.

Y el numero lo identifica: **204,7 ≈ 0,80 × 255**, o sea el suelo constante de
`litCol = baseColor * (0.80 + 0.25 * sunLightColor)` en `prop_inst.frag`. Dentro de la banda iluminada
el prop **no responde al angulo de la luz en absoluto**.

Eso es, en un grafico, las dos quejas del autor: *"quemado"* (80 % fijo del albedo) y *"toda la cara
con la misma iluminacion"* (respuesta binaria). Y el terreno al lado usa `max(dot(n,L), 0)`, continuo
— por eso conviven mal: facetas planas y siluetas claras sobre un suelo suave.

⚠️ **Yo habia descartado esta hipotesis con una medida mal planteada.** El barrido de
`testSceneTerrainAndProp` movia la ELEVACION del sol, que apaga `sunLightColor` y hace bajar a los dos
por igual (peor desajuste 1,11x). Moviendo la DIRECCION a intensidad constante, el escalon salta a la
vista. **Barrer el parametro equivocado da un "responden igual" perfectamente creible y falso.**

Los dos backends dan la MISMA curva (204,7/204,6/204,3 contra 204,7/204,6/204,4): no es de Vulkan.

**Lo que queda es una decision de estilo, no un bug que medir**: que `litCol` siga a `N·L` en vez de
saltar a un suelo fijo, o al menos que la banda de transicion sea ancha. El comentario del propio
shader dice que la intencion era que props y terreno *"responden igual a la hora del dia"* — el suelo
de 0,80 lo rompe. Sin tocar: hay que verlo, no medirlo.

### ✅ CERRADO — VULKAN NO GENERABA LOS MIPMAPS: "los arboles lejanos salen blancos" (2026-08-26)

**El bug que Andoni llevaba toda la sesion señalando, y que yo negue tres veces.**

    OpenGL   glGenerateTextureMipmap(t.id)        <- genera los mips
    Vulkan   ii.mipLevels = t.mipLevels           <- los RESERVA
             si.maxLod    = (float)t.mipLevels    <- y permite muestrearlos
             (ni un vkCmdBlitImage ni un baseMipLevel > 0 en TODO el backend)

Los niveles 1..N eran **memoria indefinida**, y el nivel lo elige la DISTANCIA: de cerca se veia bien
(nivel 0, el unico subido) y de lejos salia basura, blanca en este driver. Es la misma familia que el
motor ya tenia escrita: *"en Vulkan un descriptor sin escribir es INDEFINIDO, no ceros"*.

Afecta a todo lo que pide `mipmaps`: props, **las cuatro capas de terreno**, IBL y texturas procedurales.

**Confirmado antes de tocar nada**, capando el muestreo al nivel 0 (`HARUKA_VK_MIP0=1`), color medio
de los arboles lejanos:

    Vulkan normal          123,1 121,8 129,2   <- el verde es el canal MAS BAJO
    Vulkan capado a mip 0  128,4 134,1 122,1
    OpenGL (referencia)    125,8 130,5 118,2

**Arreglo**: cadena de `vkCmdBlitImage` nivel a nivel en `createTexture`, con `TRANSFER_SRC` añadido al
uso de la imagen y `layerCount` cubriendo los arrays de una vez. Si el formato no admite blit lineal se
crea con UN nivel y se avisa — antes que anunciar niveles que nadie rellena.

**Verificado**: cero errores de validacion, y los dos backends coinciden hasta la decima:

    Vulkan   136,1 128,7 136,4
    OpenGL   136,1 128,6 136,4
    GL contra Vulkan (escena): 2,42 %   (ruido del banco ~3 %)

⚠️ **Y POR QUE TARDE TANTO EN VERLO.** Le dije tres veces que los backends dibujaban igual. Salia de un
banco cuyo ruido tapaba justo esto: **20 % en Vulkan** por el balanceo de los props, **93 %** por
capturar antes de que el terreno cargue, y regiones que median cielo creyendo medir suelo. Cada vez que
Andoni insistio tenia razon. **Un careo cuyo ruido no se ha medido no puede sostener un "son
iguales"** — y yo lo sostuve.

### 🔴 ABIERTO — el banco no comparaba los backends, y al hacerlo salen dos cosas (2026-08-26)

**Hueco del banco, y era el de fondo**: cada test dibuja, lee pixeles y comprueba una propiedad
*consigo mismo*. Los dos backends corren en el MISMO proceso, uno detras de otro, y **nadie comparaba
las dos imagenes**. Por eso una diferencia asi podia estar a la vista y pasar por buena:

    OpenGL   99,8 % de los pixeles cambian al encender el sombreado
    Vulkan   48,8 %                     <- misma escena, mismo test, la MITAD

Añadido `compareBackends()` en `tests/rhi_test_main.cpp`: guarda la captura de cada backend bajo un
nombre y al terminar los dos las carea — % de pixeles distintos, |delta| medio, peor canal, **mapa
ASCII de lo que dibuja CADA UNO** y del sitio donde difieren. La forma identifica la causa, que es la
regla que ya cerro las "capas que tapan el terreno".

⚠️ **Trae dos auto-comprobaciones, y las dos hicieron falta**:
- **Espejado en Y**: `readPixels` devuelve GL de ABAJO ARRIBA y Vulkan de ARRIBA ABAJO. Comparar a
  ciegas da ~50 % de diferencia en cualquier escena no simetrica, que se leeria como "Vulkan dibuja la
  mitad" siendo solo el origen de lectura. Se mide de las dos formas y se dice cual casa.
- **Imagen plana**: una captura sin rango casa con cualquier otra igual de plana. El primer criterio
  ("pixeles distintos del primero") descartaba por plana justo la captura de cobertura y **TAPO el
  fallo**; ahora se mide por RANGO.

**HALLAZGO 1 — el banco asumia OpenGL, como el motor en su dia.** Cinco tests construyen su proyeccion
con `glm::perspective`, que es de GL: **no invierte la Y de Vulkan y da profundidad en [-1,1]** cuando
Vulkan espera [0,1] y recorta lo que se salga. Ningun careo a traves de ellos era valido. Arreglado en
`testTerrainNodeShadeCost` (usa `Camera::getProjectionMatrix`); **quedan cuatro**.

**HALLAZGO 2 — al horizonte los dos backends dibujan cosas distintas, y el sospechoso es GL.**
Con la proyeccion del motor y la vista 2 (blanco plano = solo cobertura), camara a 800 m:

    OpenGL   ########  el terreno cubre el 100 % del cuadro
    Vulkan   --------  cielo arriba
             ########  terreno abajo

Parecia que Vulkan perdia medio cuadro. **No lo pierde**: apuntando al NADIR —donde no puede haber
cielo— los dos cubren el 100 % y el |delta| medio cae de **131 a 5,6**. O sea que Vulkan dibuja cielo
encima del horizonte y **OpenGL pinta terreno ahi**. Con `cull = None` el pase rasteriza tambien la
cara de ATRAS del planeta; si el depth la rechaza, queda cielo. **La hipotesis a comprobar es que el
descarte por profundidad no esta haciendo lo mismo en los dos backends, y que el que falla es GL.**

Cifras del careo (256x256, con la proyeccion del motor):

| captura | pixeles distintos | \|delta\| medio | peor canal |
|---|---|---|---|
| vista2 cobertura (horizonte) | 51,56 % | 131,48 | 255 |
| vista2 NADIR (control) | 65,81 % | **5,61** | 33 |
| sin sombreado | 70,05 % | 22,55 | 65 |
| sombreado | 98,76 % | 50,95 | 151 |

⚠️ El careo **mide y no sentencia**: solo el caso NADIR tiene `CHECK`, porque hasta saber quien acierta
en el JUEGO marcar fallo acusaria al backend equivocado. La suite sigue en 0 fallos a proposito.

### 🟡 HECHO A FALTA DE MIRARLO — previews de iconos del inventario en Vulkan (2026-08-29)

Eran DOS problemas, y el segundo era el grande. El primero, el que se veia venir: el HUD pasaba a ImGui
un id de textura ENTERO (`unsigned int itemPreviewTexture`), que solo funciona con `ImGui_ImplOpenGL3`
—donde `ImTextureID` ES el nombre de textura de GL— mientras que `ImGui_ImplVulkan` quiere un
`VkDescriptorSet`. El segundo, al abrir el fichero: **`item_preview.cpp` no usaba el RHI**. Eran 345
lineas con **114 llamadas de OpenGL crudo** (shaders, FBO, renderbuffers, VAO y ~40 lineas de salvar y
restaurar estado para poder dibujar a mitad del frame de otro). Bajo Vulkan no hay contexto GL, asi que
no habia nada que puentear: habia que portar un renderer entero.

**Lo hecho:**

  · Motor: `Device::imguiTextureId(TextureHandle) -> uint64_t`. En GL devuelve el nombre de textura; en
    Vulkan un `VkDescriptorSet` de `ImGui_ImplVulkan_AddTexture` cacheado por textura. La clave del
    cache es `TextureHandle::id`, que se RECICLA, asi que `destroy(TextureHandle)` borra la entrada
    (con `RemoveTexture`) o la siguiente textura heredaria el descriptor de la anterior.
  · Shaders `assets/shaders/item_preview.vert/.frag`: el GLSL en linea del RHI es solo-GL
    (`PipelineDesc`, opcion C), asi que para Vulkan tienen que ser ficheros que el build pase a .spv.
    Los uniforms sueltos pasan a UBO en `binding = 0` (en Vulkan no hay uniforms fuera de bloque).
  · Juego: `itemPreviewTexture` devuelve `uint64_t` y renderiza con el RHI (render target + pipeline +
    UBO). Las ~40 lineas de salvar/restaurar estado de GL desaparecen: el pipeline trae el suyo.
  · **UN UBO POR ITEM**, no uno compartido. Al abrir el inventario se generan muchos iconos en el mismo
    frame, y en Vulkan los comandos se GRABAN: con un UBO compartido los N dibujos leerian el ULTIMO
    valor y todos saldrian con las matrices del ultimo item. Es el mismo fallo ya medido en el bloom.
  · `CONFIGURE_DEPENDS` en el glob de `SHADER_SOURCES` (CMakeLists). Sin el, un shader NUEVO no se
    compila hasta relanzar cmake a mano, y el fallo es mudo: falta el .spv, el pipeline no se crea y lo
    que fuera a dibujar no aparece. Se perdio un rato con exactamente eso.

**Lo medido** (test nuevo del banco RHI, "icono de inventario", caja alta y descentrada a proposito
porque una simetrica saldria igual del derecho que del reves):

    OpenGL  11688 px con tinta (17.83% del cuadro)  filas 0..127  1.9% · filas 128..255 98.1%
    VULKAN  11688 px con tinta (17.83% del cuadro)  filas 0..127  1.9% · filas 128..255 98.1%
    careo: 98.1% vs 98.1% (diferencia 0.0 pts)

**Y la trampa que solo aparecio al medir:** la primera version SI invertia la Y en la proyeccion para
Vulkan, copiando lo que hace `camera.cpp`. Parece lo correcto y no lo es — aquello corrige la imagen que
se PRESENTA, y un icono es una textura que se MUESTREA: el tejel (0,0) ya es el de NDC y=-1 en GL y el
de y=+1 en Vulkan, asi que el convenio ya esta en el destino e invertir ademas la proyeccion lo aplica
DOS VECES. Con el flip la tinta caia en las filas 128..255 en GL y en las 0..127 en Vulkan (espejado, 0
errores, se dibujaba perfecto); sin el, identicos. El test lo deja comprobado con un CHECK, no impreso.

**FALTA MIRARLO.** Nada de esto demuestra que se vea un icono en pantalla: la partida corre en Vulkan 30 s
con 0 errores de validacion, pero el inventario no se abrio, asi que el camino del preview no llego a
ejecutarse en la partida real. Hay que abrir el inventario con objetos dentro y mirar. Aparte, si en GL
los iconos ya salian del reves (ImGui dibuja el tejel (0,0) ARRIBA, y en una textura de FBO de GL ese
tejel es el de ABAJO), ahora Vulkan sale igual de al reves: se igualo a GL a proposito, que es la
referencia mirada. Si al mirarlo estan volteados los dos, el arreglo es pasar `uv0=(0,1), uv1=(1,0)` en
los `AddImage` de `survival_hud.cpp` — no tocar la proyeccion.

### 🟢 ARREGLADO — "id duplicado": era de ImGui, y NO estaba en la pantalla de carga (2026-08-29)

Andoni sospechaba que era de ImGui y no de Vulkan, y acertaba. Es el detector del propio ImGui,
encendido por defecto (`imgui.h`, `ConfigDebugHighlightIdConflicts = true`).

**Pero no podia venir del loading**, y eso descarto el sitio donde se estaba mirando: durante la carga
`init.cpp` solo dibuja `g_loading.render()` y vuelve, y ahi dentro no hay UN SOLO widget con id —
`TextColored` y `ProgressBar` se registran con id 0, que el detector ignora. Ademas el aviso solo salta
**al pasar el raton por encima** de un item cuyo id comparten varios visibles (`imgui.cpp`: se compara
contra `HoveredIdPreviousFrame`), asi que lo que se vio venia de otra pantalla — el menu, que es lo
inmediatamente anterior.

**Ningun widget del motor ni del juego repite una etiqueta LITERAL** (auditado; los dos `##log` estan en
ventanas padre distintas, y el id de un hijo se acota al padre). Los conflictos estaban todos en
etiquetas DINAMICAS, que es donde ImGui saca el id de un texto que resulta no ser unico:

    menu.cpp:54          lista de partidas: el id salia de la etiqueta entera (nombre + mapa + fecha +
                         horas). Dos partidas creadas seguidas con el nombre por defecto -> mismo mapa,
                         misma fecha al minuto, 0h00m -> MISMO ID. Es el candidato de lo que se vio.
                         Cada guardado ya tenia un `s.id` unico sin usar.   <-- LA CAUSA PROBABLE
    settings_panel:170   lista de GPUs: dos placas del mismo modelo -> mismo nombre -> mismo id.
    settings_panel:274   dispositivos de audio de entrada (SDL): los nombres SE REPITEN a menudo.
    settings_panel:299   dispositivos de salida (OpenAL): igual.
    settings_panel:414   chips de teclas: acotados por indice pero NO por accion. Las tablas de ImGui
                         no meten la FILA en la pila de ids (solo la columna, y solo en las cabeceras
                         — verificado en imgui_tables.cpp), asi que dos acciones con la misma tecla en
                         la misma posicion compartian id y pulsar un chip reasignaba el de la otra fila.

Los cinco arreglados con `PushID` por elemento (el id real donde lo hay, el indice donde no). No es
cosmetico: dos widgets con el mismo id se pisan el estado, y en la lista de partidas eso significaba
que una de las dos no se podia seleccionar.

**Sin mirar:** el arreglo no esta visto en pantalla. La partida arranca y sale limpia (0 asserts de
`Mismatching PushID/PopID`, que ImGui comprueba, y 0 errores de validacion), pero para confirmar que el
aviso desaparecio hay que tener dos partidas con el mismo nombre y pasar el raton por encima. El caso de
prueba es exactamente ese: crear dos mundos seguidos sin cambiar el nombre.

### Reportado por el autor, sin reproducir

| # | Qué | Estado |
|---|---|---|
| K | **Las texturas no se ven / no se cargan en el inspector del IDE.** ⚠️ El camino del MOTOR está verificado en aislamiento: `getMaterialTextureGL` devuelve id GL válido para rutas del proyecto (y 0 para una inexistente, que dibuja el recuadro rojo), y `renderMaterialPreview` pinta la esfera con la textura aplicada. Lo que se vio en la captura era un planeta con **0/5 slots**, o sea nada que enseñar — pero eso no descarta que falle al ASIGNAR una. **Falta el repro**: asignar una textura a un slot de un objeto normal (un cubo, no un planeta) y decir qué pasa — ¿sigue gris, sale rojo, o se ve? Cada respuesta apunta a una pieza distinta. | Sin reproducir |

### Datos ya dañados por un bug ya arreglado

| # | Qué | Estado |
|---|---|---|
| 0 | **`Survival/scenes/main.scene` perdió el bloque `surface` de sus planetas** (Earth, Moon, Jupiter) al guardarla el IDE, cuando `SceneManager::save` aún no lo escribía. Arreglar el guardado no devuelve lo borrado. El backup `scenes/backups/main_20260731_131233.scene` **sí lo tiene**: de ahí se puede recuperar copiando solo la clave `surface` de cada objeto, sin tocar el resto de ediciones. | Pendiente de decisión del autor |

### Terreno del SimplePlanet (medidos con sonda offscreen, 2026-08-01)

| # | Bug | Evidencia |
|---|---|---|
| A | ✅ **ARREGLADO** — **Las normales del vértice eran RADIALES, no del terreno**: `v.n = normalize(dir)`, o sea la normal de una esfera perfecta. Consecuencias: `slope = 1 - dot(n, up)` vale **0 en todo el planeta**, así que el material de roca (pendiente > 0.55) **no se puede seleccionar jamás**; y el relieve no existe en la iluminación — una cordillera se sombrea igual que una llanura. | Render de material: nunca aparece la capa `rock` salvo en el limbo |
| B | **El terreno SUBMARINO se clasifica como tierra**: la humedad del vértice usa `ocean = elevKm < 0` → sobre océano vale **1.0**, y con temperatura cálida gana el material húmedo. El fondo oceánico entero sale de "pradera". Normalmente lo tapa la esfera de agua, pero el material no debería existir ahí. | ~80 % del disco visible clasificado como `green` |
| 0 | ✅ **ARREGLADO** — **el planeta era 97 % océano.** Medido sobre el disco visible: solo el **3 %** está por encima del nivel del mar (la Tierra real: 29 %). Todo lo demás que se ve raro **se deriva de esto**: el fondo oceánico se clasifica con humedad 1.0 y temperatura media 4.2 °C → gana `taiga` en el 77 % de la superficie, y el planeta se lee como una bola azul-verdosa uniforme. No es un fallo de materiales ni de biomas: es la distribución de ELEVACIÓN o el nivel del mar. | `heightFn` / nivel del mar |
| C' | ✅ **ARREGLADO** — **dos niveles del mar que no coincidían**: la clasificación usa `ocean = elevKm < 0` (nivel = `radius`) y la malla de agua se dibuja en `radius - 500`. 500 m de desacuerdo entre "aquí hay mar" y "aquí se dibuja mar". | `planetary_system.cpp` |
| C | **`ClimateOutput::humidity` devuelve 1.0 sobre océano** y eso entra en el mapa de biomas y en la selección de material. Es correcto como "el mar es la fuente", pero no como "aquí crece hierba": el fondo oceánico sigue clasificándose con un material de tierra, aunque ahora lo tape el agua. | mismo render |
| ~~H~~ | ✅ **ERA UNA ENTRADA OBSOLETA (2026-08-24)**: `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` **sí se llama**, en `gl_device.cpp:375`. El grep que la abrió no lo encontró porque la llamada va tras `if (glad_glClipControl)` y el símbolo lleva el prefijo de glad. Lo que sí era un riesgo real: ese `if` fallaba **en silencio** si el driver no lo expone — modo degradado indistinguible de un bug. Ahora lo dice con un error explícito. |
| I | **`tiling` se usaba INVERTIDO** (`wp = vFragPos * tiling` con un scale de 0.5 encima): con `tiling=100` daba un tile cada **2 cm**, muy por debajo del píxel, así que la textura se promediaba a gris plano. Es la razón de que el terreno se viera sin grano ni relieve por muchos PNG de 4096 que hubiera. ✅ ARREGLADO: `tiling` es metros por tile. | lectura del shader inline |
| J | **La malla del SimplePlanet tiene un vértice cada ~39 km** (`faceRes=256` sobre 6371 km). A 2,5 km de altura estás sobre UN triángulo: no hay geometría que sombrear, así que el primer plano es plano por construcción. El detalle cercano es del terreno V3 por chunks (quadtree LOD), que existe y no está cableado para esta escena. | (π/2)·R/256 = 39 090 m |
| G | **Manchas claras en el océano**, alargadas y de borde suave, en posición fija sobre el planeta. NO son el agua (siguen con `HARUKA_NOWATER=1`) y NO son la zona (el volcado del color leído del mapa sale limpio: solo los 5 colores de paleta, ice al 0.0 %). Están por tanto en el pipeline de color/iluminación posterior a la selección de material — el sospechoso es la modulación por macro-variación (`col *= 0.85 + 0.30 * macro.r`). | render 4096 con y sin agua |
| D | **Costas ANGULARES**: las fronteras de continente son las celdas Voronoi de las placas, sin ruido que las rompa. Se ve como polígonos rectos desde órbita. | render a 120° |
| E | **Moteado sobre el mar**: puntos oscuros dispersos = terreno que asoma justo en la cota del agua. Bajar el nivel del mar un 0.5 % del rango no lo quitó. | render a 0° y 120° |
| F | **El umbral de roca (`slope > 0.55`) es inalcanzable en malla planetaria**: con ~39 km entre vértices la pendiente medida no pasa de 0.026, así que el material de roca nunca gana. El valor lo escribí pensando en terreno cercano. | medido: slope 0.0004–0.026 |

⚠️ **Trampa de la sonda, y cuesta caro**: `renderSimplePlanet` hay que llamarlo con la proyección
**reversed-Z** del motor (`camera.cpp`, `near→1`, `∞→0`), no con `glm::perspective`. El pipeline
compara con GREATER y limpia depth a 0, así que una proyección normal **invierte el test** y se
queda con lo más LEJANO. Varias medidas de esta sesión se hicieron así y hubo que repetirlas; las
manchas claras (G) sobreviven a la corrección, o sea que son reales.

⚠️ Herramientas que dejó el diagnóstico, y su lección: las tres se encontraron **midiendo**, no razonando —
una sonda que enlaza el motor, llama a `renderSimplePlanet` contra un `RenderTarget` y vuelca un PNG, más
pintar el material elegido como color plano. Tres hipótesis previas ("es el agua", "es el hielo",
"es la temperatura") salieron falsas seguidas. Existe `HARUKA_NOWATER=1` para dibujar el planeta sin
la esfera de agua, que es lo que descartó la primera.

### Juego

| # | Bug | Estado |
|---|---|---|
| 7 | **¿El personaje se desliza, o son los edificios?** Los edificios son estáticos, así que un deslizamiento relativo apunta al PERSONAJE o al origin-shift del render a gran distancia. **Cómo separarlo**: pararse quieto mirando un edificio y ver si DERIVA (→ render/precisión) o si solo pasa al andar (→ controlador). | Sin diagnosticar |
| 8 | **Cintas de mar**: si el recorte del shader (`kSeaMinDepthKm=0.006`) no basta, la raíz es la generación (mid-relief hundiendo tierra bajo el mar) → riesgo de paridad. Tocarlo **con el test delante**. | Mitigado, no resuelto |
| 10 | **Los props no casan con el suelo que se dibuja** (reportado 2026-08-25). Se anclan con `sampleHeight`, que NO aplica el **geomorph por distancia** que sí aplica el render: `nodeParentMorph` mueve la superficie hacia la altura del padre según te acercas o alejas (hasta ~0,039 m a un nivel), y el prop se queda donde se plantó. Síntoma esperado: se hunden y salen **al moverte**, no estando quieto. **Descartado por medida**: el corte de octavas NO es la causa — en el nivel 17 `terrainTriM(0)`=0,5 m y el téxel 0,596 m dan la MISMA altura (0,0000 m), y donde difieren (0,333 m en nivel 15) cae a 0,14 px porque error y distancia escalan juntos. Ver `terrain_prop_anchor_mismatch`. **Cómo separarlo**: acercarse y alejarse de un árbol; si se hunde/sale → geomorph; si está mal puesto y quieto → otra cosa. | Causa acotada, sin arreglar |
| ~~11~~ | ✅ **CERRADOS (2026-08-25)**: no era la caída a ancestro (el juego reporta `por ancestro 0`). Era la dirección del vértice en FLOAT — 0,2148 m, el **36 % del téxel**, ruido a la frecuencia de la malla. Ver la sección de la sesión. La cadena de ancestros del pool se implementó igualmente (11 → 5 niveles al girar), pero el A/B demostró que las caídas profundas eran **transitorias**, no un estado estable. | `terrain_node.vert` |
| 12 | **Desde órbita el terreno se recorta / no se muestra bien en las esquinas** (reportado 2026-08-25). ⚠️ **NO es el recorte de nodos**: medido con rayos por el cuadro a 500/1000/2000/20000 km, **0 píxeles de planeta sin nodo que los cubra**, con presupuesto infinito y con el real del juego (3072). Y desde órbita el selector solo pide 638-1031 nodos, así que tampoco satura. El cono circunscribe las esquinas por construcción (`atan(t·√(1+aspect²))`). Ver `terrain_node_orbit_coverage`. **Quedan por descartar**: (a) el POOL — nodos elegidos sin hueco al llegar a órbita, 638 nuevos a 170/frame = ~4 frames; el log lo dice en `SIN HUECO n`; (b) la PROYECCIÓN — plano lejano / reversed-Z a distancia orbital, que recortaría geometría ya dibujada. | Selector descartado; causa sin encontrar |
| 9 | **Perf de la colisión**: parche frío de frontera ~43 ms. Mitigado por el refresco async (no bloquea el frame). Optimizar solo si molesta. | Aceptable |

---

## 🔍 Verificación pendiente (el autor ejecuta; yo no)

Todo esto tiene los tests en verde y **nadie lo ha visto funcionando**. No es lo mismo.

### ⚠️ TERRENO v5 (2026-08-24) — cinco cosas arregladas y NINGUNA vista en pantalla

Todo lo de abajo está demostrado por tests y **nada por el ojo**. El síntoma que lo arrancó
("no se ve el terreno al alejarte") no se ha vuelto a comprobar en el juego.

| Qué | Cómo está demostrado | Qué falta ver |
|---|---|---|
| El pase dibuja donde debe | cobertura en pantalla contra el horizonte analítico, 5 altitudes, 2 backends, error < 1,2 pts | que el terreno lejano aparezca de verdad |
| Costura entre caras del cubo | simetría 1488/1488 cruces · arista compartida a 0,0000 m · el cosido ve el nivel del vecino | que la grieta desaparezca |
| El nodo tiene continentes | paridad GPU↔CPU 0,0007 m con el bake · contraprueba: sin bake cambia hasta 2177 m | continentes y costa en el pase v5 (⚠️ **en OpenGL NO**, ver bug 9) |
| Sombreado real | +0,86 ms/frame en Vulkan (×1,08), caso peor · el 100 % de los píxeles cambian | que el nodo y el clipmap se vean IGUAL (si divergen, costura) |
| Recorte de frustum por esquinas | el cono cubre la esquina (49,66° vs 49,7°) · 0 nodos en pantalla descartados | ⚠️ el síntoma reportado ("chunks cortados") **NO se reprodujo**. Siguiente sospechoso: `nodeBelowHorizon`, que este test no toca |

⚠️ **Sin resolver desde el 2026-08-24**: el autor dijo *"veo con los 3 igual"* sobre las vistas de
depuración. No se aclaró si las tres se ven idénticas **entre sí** —lo que significaría que
`HARUKA_TERRAIN_V5_DEBUG` nunca llegó al shader y esas observaciones no midieron nada— o si el
terreno lejano faltaba en las tres. Ahora el nivel viaja por un `flat out` en vez del UBO, así que
esa vista puede comportarse distinto.

### ⚠️ Sombreado toon unificado (2026-08-18) — cambia el aspecto de TODO lo sombreado

`lib/surface_shade.glsl` unifica el terminador (**0,30**, término medio de los cuatro que había) y el
color de sombra (**ambiente real**, no la constante fría) en `final.frag`, `prop_inst.frag`,
`construction_inst.frag`, `planet.frag` y `Survival/prop.frag`.

**Nadie lo ha visto.** Hay que mirarlo **a distintas horas**: al amanecer y al atardecer es donde se
nota, porque antes ahí la sombra no cambiaba. `Survival/prop.frag` además perdió sus dos constantes
por hemisferio; el gradiente arriba/abajo se conserva como factor de brillo (×1,0 → ×1,25).

### ⚠️ Vulkan: el uso-después-de-liberar arreglado, sin ver en el juego (2026-08-18)

`VKContext::forgetBuffer` no limpiaba `m_vbs`/`m_ib` → `Invalid VkBuffer Object` + SIGSEGV al destruir
geometría. Reproducido y arreglado en el banco de tests. **Falta jugarlo en Vulkan** con el streaming
moviéndose, que es cuando se disparaba.

### ⚠️ Peñones enterrados de la roca (2026-08-14) — el ahorro es MUCHO menor de lo que dije

`bakeRockMesh` ya no genera los peñones que caen enteros dentro del blob principal. El test
`rock_interior` lo comprueba por **rayos** (629 200 rayos desde 26 direcciones: 0 ven la diferencia)
y trae contraprueba (recortando lo visible, sí se nota).

⚠️ **Corrección de una cifra mía**: dije "62 % de los crags son interiores". **Es falso.** Medido de
verdad sobre 200 semillas: **1,4 % de triángulos** (29 820 → 29 400). La diferencia está en que el
test correcto no es contra el elipsoide sino contra el **poliedro inscrito** (lat 4 × lon 7 → radio
seguro 0,83·R): con el criterio ingenuo se recortarían un 11 % de triángulos, pero 135 rayos ven la
diferencia — o sea que asomarían por las facetas planas. El cambio es correcto y gratis, pero **no
es una optimización que se vaya a notar**; no merece más tiempo.

- [ ] Nada que mirar en pantalla: la afirmación es justo que no se ve. Si se viera, es un bug.

### ✅ RESUELTO: "con RenderDoc y el backend en Vulkan carga OpenGL" (2026-08-12)

No elegía OpenGL: **fallaba la creación de la ventana** y el motor abortaba. El mensaje era mudo
(`Failed to initialize Window system`) porque `Window::init` hacía `if (!m_window) return false;`
sin registrar `SDL_GetError()`. Con eso puesto, la causa sale en una línea:

    [SDL] ventana VULKAN no creada: Installed Vulkan doesn't implement
                                    the VK_KHR_wayland_surface extension

**RenderDoc no soporta superficies Wayland.** Al inyectar su capa, la instancia deja de anunciar
`VK_KHR_wayland_surface` y SDL no puede crear una ventana Vulkan. No es un fallo del motor.

**Solución, verificada:** lanzar con `SDL_VIDEODRIVER=x11` (XWayland, que RenderDoc sí soporta).
Comprobado con `SDL_VIDEODRIVER=x11 renderdoccmd capture -d out ./survival` → *"Backend activo:
Vulkan (solicitado, sin fallback)"*. En la GUI de RenderDoc se pone en las variables de entorno de
la configuración de lanzamiento, junto con `HARUKA_BACKEND=vulkan` para no depender del ajuste.

⚠️ Descartado por medida, para no repetirlo: **NO es el directorio de trabajo**. Se probó lanzando
desde `/tmp` con el ajuste en Vulkan y arranca Vulkan igual — el `imgui.ini` se encuentra de todas
formas, pese a que la ruta sea relativa.

### ✅ VULKAN RENDERIZA (2026-08-12). Nueve bugs, y ocho eran OpenGL asumido

Punto de partida: bajo RenderDoc no arrancaba, y arrancando por su cuenta la UI se dibujaba pero el
MUNDO 3D salía negro. Al final del día: cielo con degradado y sol, terreno TESELADO con relieve
(`quad a los pies = 4.0 m`, idéntico a OpenGL), props con material, HUD, ~5 ms de
`renderFrameContent`, 0 errores de validación.

⚠️ **EL PATRÓN, que es lo reutilizable**: ninguno era "Vulkan roto" ni el backend mal escrito. En
todos, el motor daba por buena una semántica de OpenGL que Vulkan no comparte. Cuando aparezca el
próximo síntoma raro en Vulkan, ESA es la primera pregunta: *¿qué está asumiendo de GL?*

| Síntoma | Causa | Dónde |
|---|---|---|
| Con RenderDoc "carga OpenGL" | RenderDoc no soporta superficies Wayland → la ventana Vulkan no se crea y el motor abortaba. Reintento automático en x11 (XWayland) | `core/window.cpp` |
| Captura en negro absoluto | 4 fallos: `image(0)` cableada · `oldLayout=UNDEFINED` (autoriza a DESCARTAR) · leía el frame EN CURSO · copia con tamaño de ventana → `DEVICE_LOST` | `vk_device.cpp` |
| Colores intercambiados | swapchain `B8G8R8A8`, el llamador pide RGBA | `vk_device.cpp` |
| Manchas blancas | bindings de GL son PEGAJOSOS; un descriptor set es una TABLA. Sombra de estado que se vuelca en cada set nuevo | `vk_context.cpp` |
| **Cerraba el programa** | `vkCmdDispatch` DENTRO de un render pass es ilegal (en GL es normal). Compute movido a `TerrestrialPlanet::prepare`, antes de abrir el pase | `game/planet.cpp` |
| **MUNDO NEGRO** | el `loadOp` va HORNEADO en la render pass: `clearColor=false` limpiaba a negro igual, y el motor reabre el pase varias veces por frame → cada `begin` borraba lo anterior. 4 variantes por `(clearColor, clearDepth)` | `vk_device/vk_context` |
| Frame fantasma | la variante LOAD declara `initialLayout=COLOR_ATTACHMENT`, pero esas texturas venían de ser MUESTREADAS. Transición antes de abrir el pase | `vk_context.cpp` |
| Imagen repetida ×3 | el resize tomaba el tamaño LÓGICO; el swapchain usa PÍXELES | `application.cpp`, `window.cpp` |
| **TERRENO EN EL CIELO** | origen del dominio de teselación: GL `lower-left`, Vulkan `upper-left` → `gl_TessCoord.y` invertido. Solo afecta a lo teselado | `vk_pipeline.cpp` |
| Props grises | un sampler sin atar en GL lee negro; en Vulkan el descriptor es INDEFINIDO. Textura blanca 1x1 de relleno en todos los slots | `application_render.cpp` |

**Eje Y — se probó DOS veces y solo la segunda forma es la correcta.** La compensación estándar
(altura de viewport NEGATIVA) **no sirve aquí**: afecta a TODOS los pases, y un pase de post-proceso
dibuja un quad fullscreen muestreando una textura, así que lo espeja otra vez. Con el bloom iterando
un número configurable de veces, la PARIDAD de espejados cambiaba y el frame salía derecho o del
revés ALTERNANDO. Va en la PROYECCIÓN (`camera.cpp`: `p[1][1] = -f`), que solo toca lo que se
proyecta y deja intactos los quads en NDC.

⚠️ **Y la trampa de método que costó una hora**: la inversión de Y se descartó al principio porque
"no cambiaba nada" — no cambiaba nada porque el mundo estaba NEGRO por el `loadOp`. **No se puede
refutar una hipótesis sobre una imagen en la que no se ve nada.**

**Herramientas nuevas** (sin ellas nada de esto era medible):
- `HARUKA_BACKEND=vulkan|opengl` — fuerza el backend sin tocar `imgui.ini`. ⚠️ Se aplica ANTES de
  crear la ventana: aplicarlo solo al device daba ventana de un backend y device de otro → SIGSEGV.
- `HARUKA_SHOT_AFTER=<segundos>[,ruta.png]` — espera, captura un frame limpio y sale.
- Banco `haruka_tests_rhi`: **+2 tests** que cazan las dos trampas de portar GL→Vulkan (herencia de
  bindings entre pipelines, y dispatch dentro de un pase). 68 OK en los dos backends.

**NO hecho / sin verificar:**
- [ ] **Paridad GL↔Vulkan píxel a píxel.** NUNCA se ha hecho. Lo único comparado es una sonda
  numérica (el quad a los pies: 4,0 m en ambos), que no dice nada del relieve, el LOD ni las normales.
- [ ] **El swapchain es 1280x720 con la ventana a 1920x1080** (escalado del compositor). Funciona,
  pero se renderiza a menos resolución de la que se presenta. Decidir si se quiere nativa.
- [ ] Lanzar `haruka_tests_rhi` desde `build/` o `build/bin/` da igual (localiza los assets solo),
  pero el binario vive en `build/` y los assets en `build/bin/` — conviene unificarlo.

### ⚠️ Clima 3D (2026-08-11) — la nube ya es un volumen; falta que el ojo lo note

`cloudCover`/`precip` eran campos de SUPERFICIE: respondían "¿hay nube sobre este punto?", que basta
a ras de suelo y deja de bastar en cuanto despegas. Ahora `WeatherSample` lleva `cloudTopM` además
de `cloudBaseM`, y `WeatherSystem::cloudDensityAt(w, altM)` responde "¿estoy DENTRO?".

**Medido** (banco `weather_3d`, +17 checks): grosor medio **433 m sin lluvia · 3853 m descargando
(8,9×)**, techo de tormenta hasta **6336 m**; densidad 0 exacta bajo la base y sobre el techo en
3600/3600 muestras, con contraprueba (el modelo 2D da nube a cualquier altitud, incluida la órbita);
43200 combinaciones sin base ≥ techo.

Ya conectado: `sky.frag` tenía la cima del cúmulo **cableada a `base + 1700 m`** — el mismo
desarrollo vertical con buen tiempo que con tormenta. Ahora la trae el clima por `u_planet.w`.

**El cúmulo ya NO se pinta en el cielo.** Estaba en `sky.frag`, que es un pase de FONDO (sin
profundidad, antes que la escena): un telón que cualquier objeto tapaba y, sobre todo, **sin
interior** — atravesar una nube era imposible por construcción, y la única alternativa habría sido
fingirlo con un efecto de pantalla. Ahora lo dibuja `cloud_vol.frag` DESPUÉS de la escena, con la
profundidad a mano, marchando el rayo por la losa: estar dentro deja de ser un caso especial. El
cirro (8 km) y el altocúmulo (4 km) siguen de fondo — nunca se cruzan.

### ✅ "NO SON VOLUMÉTRICAS, SON UNA LÁMINA Y NO HAY CASI NUBES" (2026-08-14) — tres causas, medidas

Reportado en pantalla por el autor. **No era un bug de código**: el pase se ejecutaba y hacía lo que
decía. Eran tres cifras, cada una razonable por su cuenta, que se contradecían entre sí. Las tres se
midieron con sondas ANTES de tocar nada.

| # | Causa | Medida |
|---|---|---|
| 1 | **El campo era 2D extruido.** `harukaCloudField(uv)` no dependía de la altura, así que toda nube era un PRISMA: la misma silueta de la base al techo. Ningún raymarch arregla eso. | rasgo horizontal 2857 m contra 440-980 m de espesor = **6,5:1 a 2,9:1** |
| 2 | **La iluminación era CONSTANTE.** Sombreaba con `dot(normalize(p), sol)`, pero `p` va referido al CENTRO DEL PLANETA: sobre una nube de 3 km ese vector gira 4,7e-4 rad. Un volumen con un único valor de luz se ve igual que una calcomanía. | 3000/6,37e6 = **4,7e-4 rad de variación en toda la nube** |
| 3 | **La densidad era un número diminuto.** `max(campo − umbral, 0)` vale 0,054-0,21 sobre el campo real, y el umbral la encogía más cuanto menos cubierto el cielo. | cobertura **mediana del planeta 0,111**; opacidad resultante en el cénit **0,016**. El 60 % del planeta por debajo de 0,20 |

**Arreglo**, en tres piezas que se corresponden una a una:

1. `harukaCloudDensity` remapea la altura al **techo LOCAL** de cada nube (fuerza baja → jirón pegado
   a la base; fuerza alta → llena la losa) y erosiona el borde con **ruido 3D** de 2 octavas. Las
   panzas quedan todas a la misma altura y las cimas no — que es como se ve un cielo de cúmulos.
   Además la escala del campo pasa de 0,00035 a **0,0007** (1430 m de ancho) y el cuerpo de la losa
   de `260+900·cover` a `500+1400·cover`: relación ancho/alto **2,31:1 en el peor caso** (era 8,51:1).
2. La luz sale del **camino óptico analítico hacia el Sol** (distancia al techo local / elevación
   solar), que no cuesta ni una muestra extra del campo y **varía en horizontal** porque el techo
   local varía. Topado al ancho de la nube: con el Sol rasante la luz sale por el costado, y sin el
   tope el amanecer y el atardecer apagaban el cielo entero.
3. `harukaCloudStrength` **normaliza la fuerza a [0,1]**, así que el núcleo vale 1 y `kCloudExtinction`
   vuelve a ser un coeficiente por metro (0,014 → **0,008**). Medido con la fórmula nueva y la
   cobertura mediana: opacidad **0,974** contra 0,173 de la cota superior de la vieja.

Banco nuevo `cloud_shape`, con contraprueba en las tres: relación ancho/alto, visibilidad con la
cobertura real del planeta, y que el shader siga teniendo campo 3D + remapeo + normalización (esto
último leyendo el fichero, que es lo único que un test de CPU puede auditar del lado GLSL).
**25849 OK · 0 fallos.** GLSL validado con `glslangValidator` en Vulkan y en GL.

### ⚠️ SEGUÍA PLANA: era el MUESTREO, no el campo (2026-08-14, mismo día)

Con lo de arriba puesto, el autor reportó *"aún teniendo la nube pequeña no tiene altura"*. Al medir
la nube **que se dibuja** (columnas verticales sobre una rejilla de 6×6 km, densidad útil > 0,08) el
campo salió **0,82:1 — más alta que ancha**. O sea que la geometría estaba bien y el aplanamiento
venía de otro sitio.

⚠️ **El apartado 1 del test medía la LOSA, no la nube.** Pasó en verde con el muestreo roto. Es el
mismo error de método que con los peñones de la roca: **el test tiene que ir contra lo que se DIBUJA,
no contra la superficie ideal.**

**La causa: 24 pasos repartidos POR IGUAL.** Funciona mirando hacia arriba y se desmorona mirando al
horizonte — que es justo donde se ven las nubes de perfil y donde está casi toda el área de cielo.
Medido sobre la losa real (1303-1887 m) contra una nube de 332 m:

    cenit      0,6 km de recorrido ->  24 m de paso -> 13,6 muestras por nube
    70 grados  1,7 km              ->  71 m        ->  4,7
    85 grados  6,5 km              -> 271 m        ->  1,2
    88 grados 14,1 km              -> 587 m        ->  0,6   <- MENOS DE UNA

Con menos de una muestra por nube no queda relieve que promediar: sale un manchón uniforme. Y mi
cambio de escala lo **empeoró**, porque hizo las nubes la mitad de grandes.

**Arreglo: paso que CRECE con la distancia** (50 m al entrar, ×1,32 por paso). Fino donde la nube
ocupa muchos píxeles, grueso donde ya es subpíxel, con los MISMOS 24 pasos y por tanto **el mismo
coste**. 28,6 muestras por nube contra 0,38 del paso uniforme. El alcance de los 24 pasos (122 km)
tiene que superar el tramo rasante extremo (91 km) o las nubes del último trecho hacia el horizonte
se quedan sin marchar — con ×1,28 no llegaba, y el test lo cazó.

**25854 OK · 0 fallos.** El test nuevo incluye la contraprueba de que el paso uniforme se saltaba
nubes enteras.

### ⚠️ Y AUN ASÍ NO HABÍA NUBES: el planeta estaba DESPEJADO (2026-08-14)

Hipótesis del autor: *"seguramente sean sistemas diferentes"*. **La arquitectura sí son dos sistemas**
—`sky.frag` pinta cirro (8 km) y altocúmulo (4 km) como fondo, planos a propósito, con su propio
`cloudField`; `cloud_vol.frag` pinta solo el cúmulo— pero **no era eso lo que escondía las nubes**.

Medido: con la cobertura que tenía el mundo, **NINGUNO de los dos dibujaba nada**. El umbral del
cúmulo volumétrico quedaba en 0,54 y el del cirro de fondo en **0,683**, contra un campo cuyo
**máximo absoluto es 0,836** y cuyo p95 es 0,595. Cielo vacío por aritmética, en los dos sistemas.

**La raíz estaba en `cloudCoverAt`.** El fondo de nube era `0.12·H` y los frentes cubren poca esfera,
así que fuera de ellos todo caía a ese suelo: **0,078**. Mediana del planeta **0,111**, con el 60 %
por debajo de 0,20. La Tierra real ronda 0,67 de media. Cualquier ajuste del render se estaba
probando sobre un planeta sin nubes.

Fondo → `0.06 + 0.30·H` y frentes reforzados a `×(0.70 + 1.00·H)`. El fondo se deja BAJO a propósito:
es constante para una humedad dada, o sea un suelo plano, y subirlo daba cielo permanentemente
cubierto. La variación tiene que venir de los frentes, que sí se mueven.

| a humedad 0,65 | antes | ahora |
|---|---|---|
| mediana de cobertura | 0,111 | **0,302** |
| espesor mediano de nube | 359 m | **922 m** |
| cubierto (>0,85) | 4,6 % | 18,9 % |
| **lloviendo** | 11,0 % | **21,3 %** |

⚠️ **La lluvia casi se dobla y es INEVITABLE**: `precip` sale de la cobertura y no tiene otra fuente,
así que un cielo más nublado llueve más. Se compensó lo que se pudo subiendo `kPrecipCover` de 0,62
a 0,78 (sin eso habría sido 27,9 %); más arriba, la lluvia pasaría de nada a todo en una franja
estrechísima. **Si el 21 % te molesta, el mando es el término `0.30 + 0.70·H` de `sampleAt`, que
gradúa la INTENSIDAD, no este umbral.** Es una decisión de balance, no técnica.

Efecto secundario medido: con más cobertura la losa engorda, así que la relación ancho/alto de la
nube mejora sola a **1,63:1 en el peor caso** (era 2,31:1) y la opacidad del núcleo a 0,994.

**Capturado**: con el arreglo, la cobertura en el punto de aparición pasa de 0,06 a 0,21 y en la
captura **se ven nubes donde antes no había nada**.

⚠️ **LA FORMA SIGUE SIN JUZGARSE.** Las dos capturas salieron **de noche**, y de noche el término de
luz está en su suelo ambiental (0,18), que es justo lo que da el relieve. Hace falta una captura
**de día**: `weather 20` para acelerar los frentes, y mirar al HORIZONTE (de perfil), no al cénit —
desde abajo se ve la panza, que es plana por física.

⚠️ El coste no está medido — ver la lista de abajo.

⚠️ **NADA DE ESTE PASE SE HA EJECUTADO.** Compila y la suite está verde, pero la suite no dibuja un
píxel. Un raymarch recién escrito puede salir negro, invisible o costar el frame entero.

- [ ] **QUE SE VEA ALGO.** Primero de todo: que haya nubes. Si el cielo sale sin cúmulos, el pase no
  está pintando (mirar el log: `[Clouds] pase volumetrico: ok|FALLO`). Apagable con
  `m_volumetricClouds = false` → vuelve al cúmulo plano de antes, que es el estado conocido bueno.
- [ ] **QUE TENGAN FORMA DE NUBE**, que es lo que motivó el cambio: cimas abombadas y a alturas
  distintas, panzas todas a la misma cota, borde con grumos. Si siguen leyéndose como una sábana
  plana, el sospechoso ya no es la geometría (medida en 2,31:1) sino el ruido 3D: subir
  `HARUKA_CLOUD_ERODE` en `lib/cloud_volume.glsl`.
- [ ] **CUÁNTAS.** Con cobertura mediana (0,111) el cálculo da ~16 % del cielo con nube. Si sale
  mucho más cerrado o mucho más vacío que eso, el que está mal es el umbral `lo`, no la densidad.
- [ ] **⚠️ EL COSTE, que ha SUBIDO y no está medido.** El ruido 3D añade 16 hashes por paso donde hay
  nube (antes: 80 del campo 2D), o sea hasta **+20 % en el peor caso**, sobre un pase que nunca se
  midió. `scene.clouds.volumetric` en `HARUKA_PROFILE_DUMP`. `kCloudSteps` (24) es el primer número
  a bajar; el jitter nuevo hace que bajarlo cueste menos calidad que antes.
- [ ] **EL AMANECER Y EL ATARDECER**, que es donde el término de luz nuevo puede fallar: el borde de
  arriba tiene que encenderse y la panza quedarse oscura. Si sale todo gris plano, el tope al ancho
  de la nube se está quedando corto.
- [ ] **CUÁNTO CUESTA.** `scene.clouds.volumetric` en el profiler. Es un raymarch a pantalla
  completa con `kCloudSteps = 24`: es EL número a bajar, y si no basta, el pase va a media
  resolución. Sin medirlo no sé si es 0,5 ms o 15.
- [ ] **La oclusión contra la escena**: que una montaña delante tape la nube de detrás, y que la
  nube no se pinte encima de los props cercanos. Es lo que el pase de fondo no podía hacer.
- [ ] **ATRAVESARLA**, que es el requisito que pidió el autor: entrar en un cúmulo y perder
  visibilidad de forma continua, sin salto al cruzar la base.
- [ ] ⚠️ **MIRAR DESDE ARRIBA.** La parte más floja del shader: el tramo de marcha con la cámara
  POR ENCIMA de la capa invierte el orden de entrada/salida, y ese caso está derivado a mano y sin
  comprobar. Si desde un avión las nubes desaparecen o se ven del revés, es ahí.
- [ ] **La lluvia debería ocupar el volumen base→suelo**, no caer desde una cota fija. El pase de
  gotas usa `cloudBaseM` como techo; con la losa real, dentro de la nube no debería haber gotas
  distinguibles y por debajo sí.
- [ ] El look cambia: el cúmulo plano estaba estilizado a propósito (*"borde duro = el look de
  anime"*). Un volumen con autosombra no es lo mismo. Si no gusta, se decide a propósito.

✅ **Ya atado**: el perfil vertical estaba duplicado a mano entre C++ y GLSL. Ahora `kProfileRise`/
`kProfileFall` viven en `weather_system.h` y el test `weather_3d` **lee
`lib/cloud_volume.glsl` y compara los números** (con contraprueba de que el lector distingue un
valor distinto). Si divergen, el test cae.

### ⚠️ Props: colisión y talado por partes (2026-08-11) — nada de esto se ha visto en pantalla

Los árboles del scatter global **no colisionaban ni se podían talar** desde que `gameOnInit` apagó
el `ResourceSystem` del juego (`setGenerationEnabled(false)`): ese sistema era el único que llenaba
`m_propsQuery`, y con él se fueron `registerPropColliders()` y `harvestAt()`. Ahora el collider sale
del **esqueleto** del árbol (`treeSkeleton` en `tree_mesh.h` → `prop_collider.h`): tronco y cada rama
por separado, con `partId` propio.

**Lo que SÍ está medido** (no hace falta volver a mirarlo):

- El refactor del esqueleto no movió un vértice: 12/12 huellas idénticas entre binarios de las dos
  versiones del fuente, con contraprueba (mover un radio 0,04 % cambia las 12).
- `prop_collider`: `64/64 puntos libres bajo la copa por partes · 64/64 bloqueados por la caja
  envolvente` (contraprueba) y `634 triángulos · 0 con vértices de dos partes`.

**Lo que hay que ver ejecutando:**

- [ ] **Chocas con el tronco y pasas bajo las ramas.** Es la propiedad que motivó hacerlo por partes.
- [ ] **El número de la sonda `PropCollider`** al arrancar: `N props en 96 m -> M cajas (X ms)`.
  ⚠️ Importa de verdad: cada `addPropOBB` sube `m_staticsVersion` y Jolt **destruye y recrea TODOS**
  los cuerpos estáticos. Si M se dispara, bajar `kColliderRadiusM`. El radio no puede bajar de la
  deriva entre refrescos del scatter (30 m, `refreshM`) o llegas a un árbol antes que su collider.
- [ ] **Talar el tronco** tumba el árbol y da madera; **golpear una rama** la arranca dejando el
  árbol en pie, y una rama ≥ 1,2 m da palos (`stick`).
- [ ] **La rama arrancada deja de colisionar Y deja de proyectar sombra** (el pase de profundidad
  lleva la misma regla; si faltara, la sombra delataría la rama que ya no está).
- [ ] **El árbol talado sigue talado al volver** tras alejarse > 1 km (es lo que prueba que el estado
  vive en `m_propState` y no en el registro de instancias, que el scatter regenera).
- [ ] **Guardar y cargar** conserva talados y ramas (`scatterProps` en el save).

**Huecos CONOCIDOS, decididos, no olvidados** (si molestan en pantalla, aquí está el porqué):

- [ ] **No hay rebrote**: lo talado se queda talado para siempre. Nada decrementa `regrow`. El
  sistema viejo tenía respawn 90 s / grow 60 s y ese camino no se ha reconectado.
- [ ] **El árbol no cae: desaparece.** `state=Destroyed` hace que el render lo salte — sin animación
  de caída, sin tocón, sin tronco en el suelo.
- [ ] **La copa cuelga del TRONCO** (`partId` 0), no de las ramas: arrancar una rama no se lleva
  follaje. Es lo simple y honesto; repartir blobs por rama es trabajo aparte.
- [ ] **Softbody de ramas**: fuera de alcance, pero el esqueleto YA es el rig que necesitaría (cadena
  de segmentos con radio en cada extremo). No hay que rehacer nada para añadirlo.
- [ ] **Roca y casa colisionan con UNA caja**, no por partes: su bake no tiene esqueleto.

### IDE

- [ ] **Viewport tras el fix del render target**: la escena (cielo + primitivas/objetos) debe pintarse
  DENTRO del `ImGui::Image` del viewport, no en el backbuffer.
- [ ] **Materiales por objeto**: asignar una textura a un slot del inspector (o hornear un grafo con
  Bake Textures) y ver que el objeto la lleva puesta en el viewport. Sobre una primitiva las UV son
  por proyección de caja → hay costura en las aristas; eso es esperado, no un bug.
- [ ] **Panel Objects + Inspector con una escena real** (capas, tarjetas, menú contextual).
- [ ] **`libHarukaEngine.so` + editor compilan** en la máquina del autor; crear proyecto nuevo →
  compile → Play Mode round-trip; smoke test (escena, primitivas, gizmo, materiales, shutdown).

### Juego — clima (lo más nuevo, lo que más pide ojos)

Mandos: `weather [escala]` acelera los frentes · `rain <0-1|auto>` fuerza la lluvia.

- [ ] **Que la lluvia caiga de las nubes y SOLO ahí**: con `weather 20` pasa una tormenta en minutos.
  Mirar que el cielo esté cubierto CUANDO llueve, y que bajo un tejado/árbol no caiga nada.
- [ ] **Densidad y tamaño de gota a tu resolución**: el ancho mínimo es ~1.4 px por cálculo. Si se ve
  como rayas gordas o como niebla de puntos, tocar `kMaxDrops`/`kBoxM`.
- [ ] **Suelo mojado**: silueta SECA bajo lo que tenga collider, charcos en las vaguadas, salpicaduras
  solo mientras cae, y que al escampar tarde ~4 min en secarse sin quedarse pegajoso.
- [ ] **Nieve**: que cuaje donde ve el cielo, que las huellas se lean por su SOMBRA (no como manchas)
  y que el frenazo al andar (62 %) sea interesante y no molesto. Ídem arena en el desierto.

### Juego — casting, construcción y mundo

- [ ] **Que el sufijo cambie lo que VES**: muro que se lea como muro, meteorito que caiga donde
  apuntas, tornado que arrastre, parpadeo que no te meta en el suelo. `spell <incantacion>` da los números.
- [ ] **Escribir la incantación**: que la glosa en vivo se entienda y que Intro → apuntar → clic no
  estorbe al moverse.
- [ ] **Que los archivos manden a la vista**: `building karsk 1 temple` y `city bram 7 0 capital` —
  templo monumental, almacén SIN ventanas, capital con avenidas más anchas. Tocar un `.json` de
  `empires/` y ver el cambio SIN recompilar.
- [ ] **Panel de mundo** (F3 o `world`): que el árbol servidor → zona → grupos se lea y que un
  edificio salga como padre con su (nº de piezas).
- [ ] **Instancing de piezas de construcción**: implementado, NO verificable headless. Riesgos
  concretos: offset de posición (origin-shift), luz distinta, o el fantasma de preview instanciándose.
- [ ] **El suelo**: que ya no se caiga ni flote al girar, props asentados, `mem` sano, sin cintas de
  mar, terreno cálido (no gris lavado).

---

## 💭 Ideas y decisiones abiertas

Sin versión asignada porque **falta decidir**, no porque falte tiempo.

- **CAPAS DE DEPURACIÓN SUPERPUESTAS sobre el mundo.** Poder ver, por separado y encima del
  terreno, la **temperatura**, la **humedad** y cada capa que decide algo — zonas, biomas, y las que
  vengan (zona de un tipo de árbol, densidad de recursos, rutas). Y **superponerlas** para ver cómo
  se distribuyen y cómo se afectan entre sí.
  *Por qué merece la pena, y no es un capricho de debug*: buena parte de los fallos de esta sesión
  —el clima de juguete que hacía inalcanzable el bosque, el 97 % de océano, el material equivocado
  en el fondo marino— se encontraron **pintando el dato como color plano** con una sonda offscreen y
  mirándolo. Con esa vista dentro del IDE, cada uno de ellos habría sido evidente en segundos en vez
  de en horas. Es la herramienta que convierte "no se ve bien" en "aquí está el problema".
  Nota de implementación: el motor ya sabe pintar el dato (lo hice tres veces a mano); lo que falta
  es un modo de render seleccionable y un selector de capa en el viewport, no un sistema nuevo.

- **CLIMA DINÁMICO "realista" por distancia al Sol.** Que la temperatura salga de la irradiancia
  recibida —distancia al astro, inclinación del eje, hora local— en vez del perfil por latitud
  cableado que hay hoy (`27 − 0.006·lat²`). Con eso, un planeta más lejano es frío **porque está
  lejos**, no porque alguien lo escriba, y las estaciones y el día/noche salen del mismo cálculo.
  ⚠️ Ojo con dónde vive: el clima ya es una **función pura de (semilla, tiempo, dirección)** para que
  cliente y servidor vean la misma tormenta compartiendo solo el reloj. Meter la órbita dentro
  mantiene esa propiedad (la órbita también es analítica), pero cualquier estado acumulado la rompe.

- **¿World UI editor, o basta el editor de objetos?** Si el editor de objetos exporta y edita
  cualquier `SceneObject`, puede que una UI de mundo sea el mismo editor con otro filtro. Decidir
  antes de escribir panel nuevo.
- **¿Se enciende el MESO?** Ya no está bloqueado: no mueve el suelo (`giro` en frío 0.000 m) y no
  rompe el 1 cm. La decisión es solo si su detalle compensa su coste. Criterio escrito en
  `mesoEnabled()`: `HARUKA_MESO=1 HARUKA_DISKCACHE=0 ./haruka_tests giro` ≤ 0.01 m.
- **¿`minLOD` a 3?** Hoy 2 (96 chunks). Subir a 3 son 384 chunks: solo si desde ÓRBITA se ve basto.
- **Outliers de paridad** (max 37.5 m, 3/4096): están en las RAMAS posteriores a la suma (recorte de
  costa `landMask>0.5` y lagos), no en los términos. Es lo más visible que queda del terreno.
- **Cuevas** e **islas flotantes**: aparcadas hasta que la superficie sea creíble — prerrequisito ya
  cumplido, así que la pausa es ahora una decisión, no un bloqueo.
- **Namespaces**: clases a sub-namespaces por carpeta, file-by-file. Cosmético.
- **Cinematics** (editor ImGui): material de vídeo para portfolio.
- **`bolsillo`** (utilidad de almacenaje) necesita inventario espacial; hoy avisa en vez de fingir.
- **Forma ESCRITA del idioma**: runas y grimorios como objetos del mundo.

---

## 🔴 ANOTADO, SIN HACER — la voluta: la elipse de Gerstner (2026-09-01)

La cresta **no puede plegarse**, y ya no es por la longitud de onda: la dispersión de profundidad
finita está puesta (`oceanWaveNumber` / `harukaWaveNumber`, residuo 1,47e-07) y el tren de 61 m pasa
a 32,2 m en 3 m de fondo. El jacobiano apenas se movió: 1,2 m de fondo, +0,883 → **+0,864**.

La causa es otra: `Q = min(0.75/(k·A·N), 1)` acota `Σ Q·A·k ≤ 0,75` **por construcción**, así que el
jacobiano no puede bajar de 0,25 valga lo que valga `k`. El pliegue pide el desplazamiento horizontal
real de la ola trocoidal en profundidad finita — **`A/tanh(k·d)`** en la superficie — en vez del `Q`
ad-hoc.

**Ahora es viable, y antes no lo era.** El intento que se revirtió abría `Q` con `k` de aguas
profundas: como `Q·A = presupuesto/(k·N)` no dependía de la amplitud, en un lago de 30 cm salían los
mismos ~3 m de desplazamiento que en mar abierto y la lámina se plegaba sobre sí misma. `A/tanh(kd)`
sí depende de la amplitud y del fondo: con `A ≤ 0,55d` y `k ≈ √(k₀/d)`, en 30 cm da ~0,94 m.

⚠️ Al tocarlo, correr `shallow_water_*`: es lo que rompió la vez anterior.

## 🤔 Sin decidir (2026-08-24) — el mar, medido y esperando criterio

| Qué | El número | La pregunta |
|---|---|---|
| **Escala de marea ×15** | `ocean_wave.h:167` multiplica el término P₂ por 15 → **5,6 m** de carrera de marea | La marea real de la Tierra en mar abierto son ~0,5 m; 5,6 m es la de un estuario. ¿Se queda por jugabilidad o baja a lo físico? |
| **`buoyRatio = 1.1` a fuego** | `physics_engine.cpp:1422` — razón densidad agua/objeto, la MISMA para todo | Un tronco y una piedra flotan igual. ¿Va por material, por cuerpo, o se queda? |

## 🤔 Sin decidir (2026-08-18) — medido, esperando criterio

**El maestro de biomas se hornea a 25× lo que se sube.** `upload 18750x9375 -> 3750x1875`: se evalúan
25 píxeles por cada uno que llega a la GPU. Son **14,6 s de los 21 s** del arranque en frío.
Hornearlo a la resolución de subida lo dejaría en <1 s (arranque ~6 s), a cambio de perder el maestro
que el streaming futuro querría y de re-hornear al subir la calidad de terreno.

**393 líneas de solver a mano** (`integrateForces`, `broadPhaseAABB`, `detectCollisions`,
`resolveCollisions`, `resolveStaticCollisions`) = el **19 % de `physics_engine.cpp`**, alcanzables
solo con `HARUKA_JOLT=0`. No es código muerto por accidente, pero obliga a hacer cada cambio dos veces
o se pudre en silencio — y Jolt cuesta 0,02 ms/frame.

**Escalones en las costuras de la colisión lejana**, medidos y publicados en cada rebuild:
`0.5 km:0.51 m · 2.0 km:2.38 m · 8.2 km:9.07 m · 131.1 km:113.63 m`. El arreglo acordado (sin
implementar) es **costura bloqueada**: sustituir la altura de los nodos del borde exterior del anillo
fino por la interpolación lineal de sus dos vecinos gruesos — el valor que `terrainRingSeamStep` ya
calcula para MEDIR el error. Toca solo el perímetro y la sonda existente lo verifica (pasaría a
`0.00 m`). ⚠️ Baja la fidelidad para ganar continuidad. Alternativa de fondo: los anillos lejanos
sobran el día que haya raycast contra la función de altura.

**Un test que vigile los `.spv`.** Un fallo de compilación a SPIR-V es invisible: el motor cae al GLSL
del driver, se ve igual y los tests pasan. Comparar `assets/shaders/**` contra los `.spv` del build
sería barato y taparía un agujero real.

**Tres duplicados exactos de shader sin fusionar** (cosmético, 6 ficheros → 3):
`planet/clipmap.vert` = `planet/ocean.vert` · `equirect_to_cubemap.vert` =
`irradiance_convolution.vert` = `prefilter_env.vert` · `screenquad.vert` = `brdf_lut.vert`.

