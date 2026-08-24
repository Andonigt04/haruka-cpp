# TODO — bugs, verificaciones e ideas

**Qué hay aquí**: lo que está **roto**, lo que está **sin mirar** y lo que está **sin decidir**.
Nada de plan por versión.

| Este fichero | Otro fichero |
|---|---|
| Bugs abiertos · verificaciones pendientes · ideas sin decidir. | [ROADMAP.md](ROADMAP.md) — el plan por versión de los tres proyectos. |
| | [docs/HISTORIAL.md](docs/HISTORIAL.md) — lo cerrado, con las trampas que costaron sesiones. |

**Suite**: `./haruka_tests` → **26043 OK · 1 FALLO** (2026-08-24; el fallo es
`terrain_quality_mapping: default quality is Low`, preexistente). RHI: `./haruka_tests_rhi` →
**219 OK · 0 FALLOS** en los dos backends.
⚠️ **Para medir tiempos, `HARUKA_NO_VSYNC=1`**: con FIFO cualquier ms/frame se clava en 16,67 y mide
la presentación, no la GPU. El test de coste del sombreado lo detecta y avisa, los demás no. Juego:
`Survival/build/bin/survival_tests assets/data/magic/language/` → **53 OK**.
⚠️ **`build.sh` NO construye los binarios de test**: `cmake --build build --target haruka_tests haruka_tests_rhi -j16`.
*⚠️ El README sigue diciendo 19 460 y este fichero decía 20 135: los dos estaban obsoletos. El
recuento de arriba es el de la última ejecución real, no una estimación.*

---

## 🐛 Bugs abiertos

### Motor

| # | Bug | Dónde |
|---|---|---|
| 1 | **SSAO es un HUECO**: el ajuste viaja al shader pero no hay pase que genere la oclusión. Implementar o quitar el ajuste — hoy promete algo que no ocurre. | `settings` → `final.frag` |
| 2 | **Render targets estáticos**: `bloom` / `ssao` / `hdr` se crean con tamaño FIJO y no siguen el resize de ventana. | `application_render.cpp` |
| 3 | **Objetos de escena sin LOD ni cull**: full detalle a cualquier distancia (solo hay un corte por distancia). Con el instancing ya migrado, toca darles cull + LOD. | `application_render.cpp` |
| 4 | **Memory leaks / dangling refs en RHI**: sin auditar. Handles que se crean y nunca se destruyen, y punteros a recursos que sobreviven al `Device`. | `src/rhi/` |
| 5 | **`preview.vert` es código muerto** y además declara un `PerObjectData` DESACTUALIZADO (sin los campos de material). Nadie lo compila hoy; el día que alguien lo use, falla al LINKAR sin decir por qué. Borrarlo o actualizarlo. | `assets/shaders/preview.vert` |
| 7 | **Nadie fija la raíz de assets salvo el editor**: `Shader::setBaseDir` no lo llama ni el motor ni Survival, así que ambos dependen de que el cwd sea el del ejecutable. El editor ya la deriva de `SDL_GetBasePath`; el resto sigue a merced de desde dónde se lance. | `renderer/shader.h` |
| 9 | **OpenGL: un compute NO puede muestrear texturas.** `terrain_node.comp` lee el bake por `sampler2DArray` (binding 15): en Vulkan casa a 0,0007 m contra la CPU, en GL devuelve **0 en los 16 641 téxeles**. Consecuencia real: **con OpenGL el terreno del pase v5 sale sin continentes**. Descartado: el `.spv` está al día, la subida de texturas en capas es correcta, y el MISMO binding funciona en GL desde el TESE del clipmap. Nunca se había ejercido: ningún otro `.comp` del proyecto muestrea texturas. | `src/rhi/opengl/`, `terrain_node.comp` |
| 10 | **Un render target offscreen deja el backend movido.** `testTerrainNodeShadeCost` es el único test que dibuja a un target propio, y tras él falla el siguiente que lee píxeles del framebuffer por defecto. NO lo explican ni el viewport (`beginRenderPass` lo restaura), ni destruir el target, ni un frame de restauración explícito. Mitigado poniéndolo EL ÚLTIMO del banco — mientras siga así, nadie puede añadir un test detrás sin comprobar que no hereda basura. | `src/rhi/` |
| 11 | **`readPixels` solo sabe leer del framebuffer por defecto.** No hay forma de leer un render target offscreen, así que cualquier test que dibuje a 1080p tiene que hacer su contraprueba en la ventana. | `rhi_device.h` |
| 12 | **Comentario obsoleto**: `planet.cpp:3358` cita `water.frag`, que ya no existe. | `src/game/planet.cpp` |
| 8 | **El clear del frame usa el color de cielo aunque no haya cielo**: sin planeta activo `getSkyColor` devuelve 0.005 → un viewport casi negro que parece roto. Para el editor conviene un fondo neutro declarado, no el del espacio. | `application_render.cpp` |

⚠️ **Sobre el bug 5 y cualquier cambio al UBO per-object**: un bloque `uniform` debe declararse
**IDÉNTICO en todas las etapas del mismo programa**. Declararlo con menos campos en una de ellas da
`definitions of uniform block do not match` **al LINKAR**, no al compilar, y el pase se queda mudo
sin error visible. `simple.vert` linka con `final.frag` **y** con `preview.frag`: los tres van a la vez.

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
| H | 🔴 **`glClipControl(ZERO_TO_ONE)` NO SE LLAMA NUNCA.** El comentario de `camera.cpp:58` dice "ya puestos", pero un grep por todo el árbol (motor + IDE + juego) solo encuentra esa mención. Sin él, la proyección reversed-Z (near→1, ∞→0) se mapea al rango GL por defecto [-1,1] → la profundidad usable queda en (0.5, 1]: **se tira la mitad de la precisión** que reversed-Z venía a ganar. El orden es correcto, así que no se ve como un fallo — se ve como z-fighting donde no debería haberlo. | grep en los tres repos |
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

### ⚠️ Normal per-píxel dentro del clipmap (2026-08-18) — sin medir el coste

`biome.frag` calculaba la normal per-píxel solo FUERA del clipmap; dentro heredaba `vNorm`, del
gradiente en vértices teselados cada 4 m. Iluminación per-píxel sobre una normal basta = el suelo
cercano se veía por triángulos. Añadida la rama que faltaba.

**Sin verificar:** (a) que se vea suave, (b) **cuánto cuesta**. Las dos tomas de `HARUKA_FRAMELOG=1`
no eran comparables (el juego arranca en estados distintos) y los rangos se solapan: sin cambio
23,5-23,9 ms de media, con él 21-29. Hay que ponerse **quieto en el mismo sitio** y lanzar las dos.
Revertir = quitar la rama `else` de `biome.frag`, aislada y comentada.

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

### ⚠️ Clipmap por ANILLOS ANIDADOS (2026-08-14) — cambia lo que se ve al subir

El clipmap era **una rejilla estirada** por `clipScale = 2^k` según la altura: cubría más, pero
gruesa POR TODAS PARTES, así que a 640 m el suelo bajo los pies pasaba a quads de 128 m. Ahora se
dibujan **varios anillos concéntricos** (nivel r: quad `4·2^r` m, alcance `±1,9·2^r` km), cada uno
con el centro hueco donde vive el de dentro. Un draw por anillo, el mismo buffer de vértices, **un
ClipParams por anillo** (buffers distintos: con uno solo, en Vulkan todos los anillos leerían el
último — el fallo del UBO de material y del buffer de instancias, por tercera vez).

Verificado en test (`terrain_lod_invariants`, con contraprueba): el quad coincide **exacto** a los
dos lados de las 5 fronteras, y sin el redondeo a potencia de dos no coincidiría. Medido: a 640 m de
altura el quad mejora **32×** a 100 m, **8×** a 3 km, **2×** a 10 km y **queda igual** a 30-63 km.

- [ ] **QUE NO HAYA RENDIJAS.** Es el riesgo #1 y el test solo cubre la frontera *exacta*. Subir
  despacio de 20 m a 2,5 km mirando al horizonte: si aparece un círculo de puntos de cielo alrededor,
  son T-junctions. `HARUKA_NEAR_RING=0` descarta que sea el anillo de colisión.
- [ ] **Que ya no aparezca "una capa nueva muy cerca" al subir**, que es el síntoma que originó esto.
- [ ] **Qué cuesta.** `planet.clipmap.draw` en el profiler, a ras de suelo y a 640 m. A ras de suelo
  tiene que ser **idéntico** al de antes (1 solo anillo, k=0); si sube, el cambio toca donde no debía.
  A 640 m son 6 draws: cota alta 4 641 parches contra 961. **Sin medir todavía.**
- [ ] **El solape de 64·2^r m** entre anillos (el descarte es por parche entero, así que el de fuera
  empieza un poco antes). Deberían ser dos superficies idénticas superpuestas → invisible. Si se ve
  un anillo más oscuro o parpadeando, es z-fighting y hace falta sesgo por nivel.
- [ ] **Que el agua y la malla base sigan casando** en el borde exterior: el recorte de la base ahora
  lee la cobertura del anillo EXTERIOR, no la de una rejilla estirada.

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

### ⚠️ Terreno v4 (clipmap) — qué se lleva por delante el cambio del 2026-08-06

El v4 sustituye el streaming por chunks por un **clipmap con teselación hardware**
(`planet/clipmap.vert/.tesc/.tese`). El motivo es bueno y va al README: un planeta a escala real
con chunks horneados y cacheados **no cabe en disco** (orden de TB); generando en la `tese` el
coste de almacenamiento es **cero**. Es un experimento, pero estable.

Lo que hay que confirmar antes de tocar el README, porque el documento sigue describiendo el v3:

- [ ] **¿Cuántos suelos hay ahora?** `reference_surface.cpp:43` devuelve `h = 0.0` (esfera lisa) y
  `application.h:260` la declara *fallback*. El suelo real parece ser
  `PlanetarySystem::groundHeightKmAtDir` leyendo `m_heightCPU`. **La afirmación estrella del README
  ("un solo suelo" + 0,3 mm medidos) es del v3 y hoy no está respaldada.** O se re-mide con el
  clipmap, o se reformula. Sospecha del autor: sigue habiendo uno solo — *falta comprobarlo*.
- [ ] **¿La paridad render↔colisión se mantiene con teselación?** Ahora el desplazamiento ocurre en
  la `tese`, en GPU, y la CPU no tiene la malla. Si el suelo se evalúa en dos sitios distintos con
  fórmulas distintas, vuelve el bug de los dos terrenos por otra puerta. Es **la** verificación
  crítica del v4.
- [ ] **`terrain_gen.comp`**: sin ningún consumidor hoy. ¿Reconectar (el v4 sigue necesitando el
  campo erosionado) o retirar? Sospecha: sigue haciendo falta, viene del v3.
- [ ] **Fences + mapeo persistente**: solo aparecen ya en el RHI, sin consumidor. Si el clipmap no
  hace readback, dejan de ser una feature del terreno y pasan a ser **capacidad del RHI**. Eso está
  bien, pero el README no puede seguir vendiéndolo como pipeline de terreno.
- [ ] **Caché LRU + caché en disco (`core/cache/`)**: **cero consumidores**. Si el v4 genera en la
  `tese`, no hay malla que cachear — y entonces la caché no es deuda: es la **consecuencia lógica**
  del cambio y hay que contarla como simplificación, no dejarla muerta y callada.
- [ ] **MESO**: solo sobrevive en un comentario de `reference_surface.h`. Retirar del README y de
  los switches de entorno si ya no existe.
- [ ] **`drawIndexedIndirect` / `gl_DrawID`**: ¿lo conserva el camino del clipmap o murió con los
  chunks? Afecta a lo que se puede afirmar sobre draws agrupados.

> Regla mientras esto esté abierto: **el README no promete cifras del v3 como si fueran del v4.**
> Un aviso de "en reescritura, cifras pendientes de re-medir" suma credibilidad; una cifra que el
> código contradice la destruye.

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

