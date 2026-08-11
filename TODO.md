# TODO — bugs, verificaciones e ideas

**Qué hay aquí**: lo que está **roto**, lo que está **sin mirar** y lo que está **sin decidir**.
Nada de plan por versión.

| Este fichero | Otro fichero |
|---|---|
| Bugs abiertos · verificaciones pendientes · ideas sin decidir. | [ROADMAP.md](ROADMAP.md) — el plan por versión de los tres proyectos. |
| | [docs/HISTORIAL.md](docs/HISTORIAL.md) — lo cerrado, con las trampas que costaron sesiones. |

**Suite**: `./haruka_tests` → **25806 OK · 0 FALLOS** (ejecutado 2026-08-11). Juego:
`Survival/build/bin/survival_tests assets/data/magic/language/` → **53 OK**.
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

### 🔴 VULKAN: la UI se dibuja, el MUNDO 3D no (medido 2026-08-12)

Ya no es "no se ve bien": está acotado. `HARUKA_BACKEND=vulkan HARUKA_SHOT_AFTER=45,x.png` da una
captura donde **el HUD de ImGui sale perfecto** (hotbar, paneles, barras de estado) y **la escena 3D
es negro con ruido de sal**. O sea: instancia, dispositivo, swapchain, descriptores, SPIR-V y el
render de ImGui funcionan; lo que no llega al backbuffer es la escena.

Lo que el log DESCARTA (no hace falta volver a mirarlo):
- Vulkan arranca sin fallback, elige la RTX 3050, y **todos** los pipelines dan `ok` — incluidos
  clipmap, teselado, culling por compute, sombras de props y el pase volumétrico de nubes.
- Ni un error de SPIR-V, ni de validación. `glslangValidator` 11:16.2.0 presente.
- El motor funciona: paridad de terreno, física, props y anillo cercano dan valores normales.

- [ ] **Encontrar por qué la escena no llega al backbuffer.** El ruido de sal apunta a un color
  attachment que nadie limpia ni escribe. Primer sitio a mirar: si los pases de escena dibujan a un
  render target offscreen (`_postScene`) que en Vulkan nunca se compone al swapchain — ImGui sí
  dibuja directo al backbuffer, que es justo lo único que se ve.
- [ ] **Vulkan sale solo a los 60 s EXACTOS** (dos ejecuciones), sin error y por el camino de apagado
  limpio; OpenGL pasa de 75 s sin inmutarse. Número redondo = temporizador, no fallo. Sin localizar.

✅ **Arreglado de paso: la captura de pantalla en Vulkan.** Daba negro absoluto (0,0,0,0) y eso
ocultaba el bug de verdad — parecía que no se renderizaba nada. Eran TRES fallos apilados en
`VKDevice::readPixels`, ninguno relacionado con el render:
1. `m_swapchain->image(0)` — imagen 0 **cableada**, no la que se estaba usando.
2. `oldLayout = VK_IMAGE_LAYOUT_UNDEFINED` — transicionar desde `UNDEFINED` **autoriza al driver a
   descartar el contenido**; la barrera tiraba el frame justo antes de copiarlo.
3. Leía la imagen del frame EN CURSO, cuyo command buffer sigue abierto y sin enviar cuando se llama
   a `readPixels` (la captura ocurre a mitad de frame). Ahora se copia la última PRESENTADA.
4. Y la copia usaba el tamaño de VENTANA en vez del del swapchain: con `e` obtenido y sin usar,
   pedía una región fuera de la imagen → **`VK_ERROR_DEVICE_LOST`**, no un error de validación.

⚠️ Efecto secundario a saber: la captura de Vulkan va **un frame por detrás** y por tanto **incluye
el HUD**, mientras que la de OpenGL es limpia (se toma a mitad de frame, antes de ImGui). Para
comparar backends píxel a píxel habría que igualar eso.

**Herramientas nuevas** (las dos hacían falta para poder medir esto):
- `HARUKA_BACKEND=vulkan|opengl` — fuerza el backend sin tocar `imgui.ini` ni reiniciar ajustes.
- `HARUKA_SHOT_AFTER=<segundos>[,ruta.png]` — espera, captura un frame y sale. Con lo anterior, da
  dos PNG comparables del mismo escenario.

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

⚠️ **NADA DE ESTE PASE SE HA EJECUTADO.** Compila y la suite está verde, pero la suite no dibuja un
píxel. Un raymarch recién escrito puede salir negro, invisible o costar el frame entero.

- [ ] **QUE SE VEA ALGO.** Primero de todo: que haya nubes. Si el cielo sale sin cúmulos, el pase no
  está pintando (mirar el log: `[Clouds] pase volumetrico: ok|FALLO`). Apagable con
  `m_volumetricClouds = false` → vuelve al cúmulo plano de antes, que es el estado conocido bueno.
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
