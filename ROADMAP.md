# Roadmap — Haruka

**Qué hay aquí**: el plan por **versión** de los tres proyectos. Nada de bugs ni de ideas sin decidir.

| Este fichero | Otro fichero |
|---|---|
| Plan por versión, por proyecto. Qué entra en la siguiente y qué no. | [TODO.md](TODO.md) — **bugs abiertos**, verificaciones pendientes e **ideas** sin versión asignada. |
| | [docs/HISTORIAL.md](docs/HISTORIAL.md) — lo YA cerrado, con las trampas que costaron sesiones. |

---

## Los tres proyectos

Son **repositorios separados** y versionan por separado. La regla que los ordena: **lo que sabe de
planetas es del motor**; el IDE trata cualquier objeto como un `SceneObject` genérico y el juego
consume el motor como una `.so`.

| Proyecto | Repo | Versión | Qué es | Qué NO es |
|---|---|---|---|---|
| **HarukaEngine** | `haruka-cpp` | **1.0.0** | El motor: `libHarukaEngine.so`. Terreno planetario a escala real, RHI (OpenGL; Vulkan por hacer), clima, físicas (Jolt), red, ProcGraph. | No es un editor ni un juego. No conoce reglas de jugabilidad. |
| **HarukaEditor** | `haruka` | **0.1.0** | El IDE: escena, inspector, panel de objetos por capas, editor de materiales por node graph, Play Mode. | **No sabe qué es un planeta.** `surfaceConfig`/`terrainSettings` los preserva como datos genéricos; los interpreta `PlanetarySystem::buildFromScene()`. |
| **Survival** | `Survival` | **0.2** | El juego: construcción, spellcasting + idioma antiguo, supervivencia, multijugador con DGS. | No es la referencia del motor: lo que resulte general sube al motor, no al revés. |

### Ramas del motor

`main` (estable) · `develop` · **`opengl`** (donde se trabaja hoy) · `vulkan` (backend RHI, sin empezar).

---

# Motor — HarukaEngine

## v1.0.0 — ENTREGADO

Lo que define la versión actual. Detalle y trampas en [docs/HISTORIAL.md](docs/HISTORIAL.md).

- **Escala planetaria real** con origen en doble precisión, reversed-Z + far infinito.
- **Terreno V3 por proceso** (tectónica → erosión → hidrología → clima → biomas → props) generado en
  GPU y transmitido grueso→fino, con **un solo suelo** (`reference_surface`, GL-free y compartible
  con el servidor). Paridad render↔colisión **0.3 mm** dentro del radio de juego.
- **RHI + PSO completo**: ningún renderer usa GL crudo. Backend OpenGL.
- **Clima como estado del mundo**: frentes deterministas, precipitación derivada de la cobertura,
  máscara cenital (no llueve bajo techo) y capa granular con huellas.
- **ProcGraph** + texturas procedurales por node graph, biomas configurables desde el JSON de escena.
- **Módulos por bitmask**, logging estructurado, modo `--headless`, sistema unificado de entidades
  (`ObjectType::CUSTOM` + registry), API pública congelada y documentada.

## v1.1 — que el material del objeto llegue a la pantalla, y que el viewport del IDE pinte

> **Nota de la tanda**: lo que empezó como "cablear materiales" destapó una cadena de cuatro fallos
> que se tapaban entre sí, todos con el mismo síntoma —el viewport negro— y ninguno con un error que
> lo dijera. Están todos abajo. La lección, anotada porque volverá: **cuando el síntoma es "no se ve
> nada", medir antes que razonar.** Tres hipótesis bien argumentadas salieron neutras; lo que resolvió
> fue una sonda que enlaza el motor, pinta a un `RenderTarget` y **cuenta los píxeles**, más una
> captura del backbuffer del editor a PNG. Las dos herramientas están en el historial de esta rama.


El hueco que quedaba entre el IDE y el motor: el editor de node graph horneaba texturas y el motor
no las miraba. Sin esto, todo el editor de materiales es autoría que no se ve.

- [x] **Binding de `MaterialComponent` por objeto** en el pase de escena: los 5 slots
  (albedo/normal/metallic/roughness/ao) a las unidades 0-4, caché de texturas por ruta (los fallos
  también se cachean), escalares PBR + emisión en el UBO per-object.
- [x] **UV de las primitivas**: `PrimitiveShapes` no generaba ninguna y las dejaba a (0,0) → cualquier
  textura sobre un cubo se resolvía a un téxel. Ahora proyección de caja en `setupSimpleMesh`.
- [x] **Fix del render al target del editor** (los pases de cielo y de escena iban al backbuffer aunque
  hubiera `_editorTarget`) — es lo que impedía que el viewport del IDE mostrase la escena.
- [x] **Restaurar la pantalla al salir del frame**: en GL el fin de pase no desata nada, así que el
  editor pintaba su ImGui DENTRO del target del viewport y el backbuffer se quedaba sin escribir →
  la ventana alternaba entre dos buffers viejos (parpadeo).
- [x] **Clear del frame incondicional y en un solo sitio**: lo hacía el pase de cielo, que vive bajo
  tres condiciones; una escena sin planeta no limpiaba ni color ni profundidad, y con reversed-Z
  (test GREATER) el z-buffer del frame anterior rechazaba todo desde el segundo frame.
- [x] **Guardado de escena SIN pérdida**: `SceneManager::save` no escribía `surface` ni `modelPath`
  aunque el loader los lee → cada guardado borraba la config de superficie de los planetas (y un
  planeta sin `surface` es invisible: ni comando de dibujo ni entrada en el terreno) y la malla de
  los objetos con modelo.
- [x] **Sistema planetario en el camino del editor**: `initPlanetarySystem()` solo se llamaba desde
  `loadScene()`, así que por `init(scene)` —la vía del IDE— quedaba nulo y con él se caían el cielo,
  el terreno y los SimplePlanet.
- [x] **Preview de material y de texturas** (`Application::renderMaterialPreview` +
  `getMaterialTextureGL`): el motor pinta una esfera con `final.frag`, el mismo shader del viewport,
  y sirve las miniaturas desde la MISMA caché que el render.
- [x] **Persistencia de `MaterialComponent` en escena**: el loader ignoraba `material` y `save()` no lo
  escribía → las texturas horneadas por el node graph se perdían al recargar. Ahora `fromJSON` en
  carga (scene_loader.cpp) y `toJSON` en guardado. Earth de `Survival/scenes/main.scene` reparado
  (recuperada su `surface` desde el backup de 13:12 — sin ella el planeta es invisible —, conexión
  perlin→output del materialGraph arreglada y material con albedo/normal
  `assets/textures/earth_*.png`).
- [x] **Segunda raíz de assets para el proyecto abierto** (`AssetPaths::setProjectRoot`): el IDE fija
  la raíz del MOTOR (sus assets/shaders) al arrancar, así que toda textura del proyecto —materiales,
  horneados del node graph, biomas por defecto de SimplePlanet— fallaba bajo el editor (8 WARN por
  planeta y fallback procedural). Ahora los resolutores de textura prueban también la raíz del
  proyecto (`<proyecto>/assets/textures/…`), fijada por el IDE en `loadFile`, `enterPlayMode` y
  `createNewProject`.
- [ ] **SIGABRT al abrir `main.scene` en el IDE** (crash limpio, sin mensaje de assert): la consola
  muere tras el doble clic en la escena, dentro de `loadFile → bindPanelsToScene → Application::init
  → buildFromScene → addSimplePlanet`. El backtrace previo (`glm::min` en `func_common.inl:185`) está
  DESCARTADO: GLM solo tiene `GLM_STATIC_ASSERT` en min/max/clamp (línea 185 no existe en ninguna
  versión) y la aritmética de `generateBiomeMap` es demostrablemente finita (replica en Python: 0
  no-finitos en 32768 muestras). **Diagnóstico instrumentado** en `addSimplePlanet` (log por paso +
  try/catch) y guarda `isfinite` por píxel en `generateBiomeMap`: el próximo arranque dice en qué
  paso muere. Falta validar tras recompilar.
- [ ] **Los props no se mojan ni se cubren de nieve** (solo el terreno): misma `u_skyMask`, falta
  cablearla en `prop.frag`. Es lo que más canta tras una lluvia.
- [ ] **Evaluar el `materialGraph` en carga**, sin pasar por PNG horneado (`evaluateMaterial(graph)`
  → los 5 slots de una; hoy `proc_texture.h` solo evalúa un slot cada vez).
- [ ] **LOD y cull de objetos de escena** (hoy full detalle a cualquier distancia).

### Biomas en 2D (humedad × temperatura) — HECHO

`BiomeConfig::evaluate` tomaba **un solo float**, la humedad. La temperatura se calculaba
(`ClimateOutput::temperature`) y viajaba al shader, pero solo teñía la luz y la bruma: a igual
humedad salía el mismo bioma a −30 °C que a +35 °C. Tundra, taiga y sabana **no podían existir**, y
por eso el planeta se leía como bandas de humedad por latitud.

Ahora son tres filas por temperatura, cada una con su escala de humedad, mezcladas con smoothstep:

| Fila | Banda | Biomas |
|---|---|---|
| Hielo | < −12 °C | hielo/nieve permanente |
| Fría | −12 → 11 °C | tundra (seco) → taiga (húmedo) |
| Templada | 11 → 19 °C | desierto → estepa → pradera → bosque → selva (la de siempre) |
| Cálida | > 27 °C | desierto → sabana → selva |

Bandas calibradas sobre la Tierra. `evaluate(H)` sigue existiendo y equivale a `evaluate(H, 15 °C)`,
así que una escena que no declare nada nuevo se ve **exactamente** igual que antes.

Claves nuevas en `surface.biomePalette`, todas opcionales: colores `ice`/`tundra`/`taiga`/`savanna`
y umbrales `taiga`/`savanna`/`hotJungle` (humedad) e `iceTempC`/`coldTempC`/`warmTempC` (en grados,
no normalizados: son los mismos números que da el clima).

**Y el clima que lo alimenta, corregido.** El eje 2D no servía de nada mientras las entradas no
llegaran: `ClimateOutput` —el clima que HORNEA el mapa de biomas— era una rampa lineal con 15 °C de
máximo en el ecuador y humedad **constante 0.3** sobre tierra, que tras modular quedaba en 0.15–0.24.
Con `forestEdge0 = 0.46`, bosque y selva eran **inalcanzables sobre tierra**: toda la superficie
emergida salía desierto o estepa, y por eso el planeta se veía liso. Ahora usa los mismos perfiles
que `PlanetFields::computeClimate` (el clima que ya lee el shader por píxel): ~27 °C en el ecuador
y −21 °C en el polo, y humedad por **celdas de circulación** (ITCZ húmeda, cinturón subtropical
seco, franja templada húmeda, polo seco).

Medido de polo a ecuador: selva (0°) → sabana (10°) → **desierto (20-30°)** → estepa (40°) →
bosque templado (50°) → tundra (70-80°) → hielo (90°). Los nueve biomas alcanzables.

⚠️ **Había DOS climas describiendo el mismo planeta** y solo uno era bueno. El shader lee `tempC`/
`humedad` del SSBO (binding 10, el campo de terreno V3 con sombra orográfica) para elegir textura y
teñir, mientras el COLOR venía del mapa horneado con el modelo de juguete. Que discreparan es lo
que hacía que el color del terreno contradijera a la humedad con la que se eligen textura y props.

### Materiales de terreno DINÁMICOS — HECHO (ruta SimplePlanet)

Un material del terreno ya no es una rama de GLSL: es una **regla sobre el clima y la forma** que
vive en `surface.materials` del JSON de la escena y viaja a la GPU en un UBO
(`core/planet/terrain_material.h`). El shader **recorre la tabla** en vez de tener `if` fijos.

```json
"materials": [
  { "name": "taiga",  "humidity": [0.42, 1.0], "tempC": [-12, 11],
    "tile": "grass", "tint": [0.96, 1.0, 0.98], "grain": 0.9, "detail": 1.0, "priority": 1.1 },
  { "name": "rock",   "slope": [0.55, 1.0], "tile": "rock", "grain": 1.3, "priority": 3.0 }
]
```

Cada límite es una **banda suave** (`feather`), no un corte: con corte duro dos materiales
contiguos dejan una línea recta visible a kilómetros. Solapes: el aspecto se **mezcla** por peso,
el tile lo gana la **prioridad** (mezclar dos texturas por píxel costaría el doble de muestreos
para algo que solo aporta grano). Ausente en el JSON = tabla por defecto; presente = **sustituye**
la tabla entera, para que borrar un material del JSON lo borre de la vista.

Tres defectos cerrados de paso:

- **La selección ya usa los dos ejes.** Era `H > 0.42 ? hierba : tierra`. La temperatura llegaba
  por vértice (`vClimate.y`) y **se ignoraba**: una taiga usaba grano de hierba y el hielo, grano
  de tierra.
- **Los normal maps se muestrean por fin.** `uSandNormal`/`uGrassNormal`/`uLandNormal`/
  `uRockNormal` estaban **declarados y sin usar** — media VRAM de terreno cargada para nada.
  Ahora dan relieve por triplanar en *whiteout blend* (mezclar normales de mundo las promedia
  hacia la geométrica y aplana justo las caras diagonales, que son media esfera).
- **El PNG aporta grano, no color**, y ahora es explícito: `grainAmt` por material decide cuánto
  se nota (el hielo casi nada, la roca más que nadie).

⚠️ **Hay DOS shaders de terreno y solo uno se ha migrado.** El planeta que se ve en el editor es un
`SimplePlanet` y usa un GLSL **inline** dentro de `planetary_system.cpp` (`kBiomeFragSrc`);
`planet.frag` es la ruta de terreno V3 por chunks. Son copias del mismo look, y esa duplicación es
la misma clase de fallo que "había TRES suelos" y "había DOS climas". `planet.frag` tiene hoy la
versión ESTÁTICA de esta tabla (materiales cableados) y le falta la dinámica.

- [x] **Una sola definición de materiales para los dos shaders.** GLSL no tiene `#include`, así que
  el backend lo resuelve por texto al cargar (`resolveIncludes`, con tope de profundidad para que un
  ciclo falle en vez de colgar el arranque) y la raíz la fija `Shader::setBaseDir` junto a las otras
  dos. La tabla vive en `assets/shaders/lib/terrain_material.glsl` y la incluyen `planet.frag` **y**
  el shader inline del SimplePlanet. El fichero lleva `#extension GL_GOOGLE_include_directive` para
  que glslc lo valide en el build; el RHI **retira esa línea** antes de pasarla al driver, que no la
  reconoce. Verificado con GL real: un shader compilado DESDE CADENA que incluye la librería linka.
- [x] **Un PNG por material**: `TextureDesc::layers` → `GL_TEXTURE_2D_ARRAY` en el RHI, y el terreno
  sube dos arrays (albedo y normal) con una capa por material. El `tile` de la tabla pasa a ser un
  **índice de capa**, así que elegir textura deja de ser una cadena de `if` — GLSL no indexa
  samplers dinámicamente, pero sí capas. En el JSON, `"tile"` acepta nombre (`sand`/`grass`/`land`/
  `rock`/`none`) o número, para las capas que añada el proyecto a partir de la 4.
  Verificado con GL real: array de 3 capas, se muestrea la 2 y devuelve su color.
- [x] **El motor ya no decide qué materiales existen.** La lista `sand/grass/land/rock` estaba
  escrita en `loadBiomeTextures`: el motor daba por hecho que "hierba" es un material que siempre
  existe, y gateaba el camino de bioma con `RHI::valid(grassAlbedo)`. Ahora cada material declara
  su **propia textura** en el JSON y el motor recorre la tabla del proyecto para construir el array
  (`rebuildTerrainArrays`). La capa de cada material es su **posición en la tabla**, asignada por el
  motor: un índice escrito a mano en el JSON podría apuntar a la capa de otro material, y el síntoma
  sería "la roca tiene textura de hierba" sin nada que lo explique.
  `TerrainMaterialTable::defaults()` ya **no nombra ningún PNG**: una escena que no declare
  materiales sale con color de bioma y sin grano, que es honesto — no hay texturas porque nadie
  ha dicho cuáles — en vez de que el motor invente rutas que el proyecto quizá no tiene.

```json
"materials": [
  { "name": "sand",  "albedo": "sand_albedo.png",  "normal": "sand_normal.png",
    "humidity": [0.0, 0.2], "slope": [0.0, 0.5], "priority": 2.0 },
  { "name": "grass", "albedo": "grass_albedo.png", "normal": "grass_normal.png",
    "humidity": [0.42, 1.0], "tempC": [11, 1000] },
  { "name": "taiga", "albedo": "grass_albedo.png", "normal": "grass_normal.png",
    "humidity": [0.42, 1.0], "tempC": [-12, 11], "tint": [0.96, 1.0, 0.98], "grain": 0.9 },
  { "name": "ice",   "tempC": [-1000, -12], "tint": [1.06, 1.08, 1.12],
    "grain": 0.15, "priority": 1.2 },
  { "name": "rock",  "albedo": "rock_albedo.png",  "normal": "rock_normal.png",
    "slope": [0.55, 1.0], "grain": 1.3, "detail": 1.4, "priority": 3.0 }
]
```

- [x] **Nivel del mar DERIVADO de la tierra emergida.** `surface.landFraction` (0.29 = la Tierra) y
  el motor calcula el nivel del mar como el **cuantil** de la distribución de alturas que deja esa
  fracción arriba. Así "cuánta tierra tiene el planeta" es un número que se declara y se cumple, en
  vez de lo que salga del reparto de placas de esa semilla. Medido antes: **3 %** de tierra emergida.
  El campo se desplaza para que el nivel del mar sea 0, con lo que desaparece el desacuerdo de 500 m
  que había entre la clasificación (`elevKm < 0`) y la esfera de agua (`radius - 500`).
- [x] **La geología ya es determinista.** `generateGeology` usaba `std::random_device`: **cada
  arranque generaba un planeta distinto con la misma escena**. Rompía tres cosas a la vez — no se
  podía reproducir un bug, ni comparar dos renders, ni que cliente y servidor generaran el mismo
  mundo. Ahora `GeologyConfig::seed` viene de la semilla del planeta.
- [x] **Normales del terreno** por diferencias centrales sobre la retícula de la cara, en vez de la
  radial (que es la normal de una esfera perfecta: ni relieve en la luz, ni pendiente para los
  materiales).
### Mapa de ZONAS pintado a mano — HECHO

`surface.zoneMap` es un PNG equirectangular de **colores planos** donde el autor pinta qué hay en
cada sitio. Cada material declara con qué color está pintado (`"zone": [42,106,186]`) y si esa zona
está **bajo el agua** (`"submerged": true`).

**La zona manda sobre el clima.** Donde el mapa dice agua, es agua, aunque la humedad y la
temperatura de ese punto dijeran otra cosa — si el clima pudiera contradecirlo, pintar una isla no
garantizaría que hubiera isla. Los materiales que NO declaran zona (la roca, que depende de la
pendiente) siguen decidiéndose por reglas: el mapa no puede pintar una pendiente.

**Y manda sobre la ELEVACIÓN, no solo sobre el color.** Una zona `submerged` recorta el campo por
debajo del nivel del mar y una emergida por encima. Sin eso saldría terreno asomando por encima del
océano pintado: el mapa diría una cosa y la geometría otra. Es un recorte, no una sustitución — el
relieve procedural sigue mandando *dentro* de cada zona.

Detalles que importan:
- **Vecino más cercano, no bilineal**, al leer el mapa: es una PALETA, y promediar dos colores de
  paleta da un color que no es ninguno de los dos (una costa interpolada entre azul y verde daría
  un turquesa que no casa con ningún material).
- **Sin umbral de distancia** al emparejar color→material: un píxel que no case exacto (antialias
  del pincel, recompresión del PNG) cae en el material más parecido en vez de quedarse sin material
  y salir como un agujero.
- El color de zona se empaqueta en **un solo float** (RGB888 = entero exacto hasta 2²⁴) para no
  crecer el UBO por tres floats.
- Con mapa, **`landFraction` se ignora** y el log lo dice: el mar lo dibuja el autor, no una
  estadística.

- [x] **La zona también decide el COLOR.** Cada material puede declarar `"color"` (y un
  `colorWeight` opcional, 1 por defecto) que **sustituye** al tono del mapa de biomas donde manda
  ese material. Sin esto, una isla pintada salía con forma y textura de isla y color de desierto:
  la zona mandaba en el material y en la elevación pero no en el tono, que es difícil de explicar.
- [x] **Generador de mapas de zona** (`Survival/tools/gen_planet_zones.py`): continentes por fBm con
  warp de dominio, nivel del mar por **cuantil** (pides 30 % de tierra y sale 30 %), franja de arena
  en la costa, bosque por segundo ruido tierra adentro y casquetes con borde roto. El ruido se
  evalúa sobre la **dirección en la esfera**, no sobre el píxel: sin costura en el antimeridiano,
  sin aplastamiento en los polos, y la MISMA semilla da el MISMO planeta a cualquier resolución.
  Mapas en 512², 2048² y 4096², los tres con exactamente 5 colores de paleta y el mismo reparto de
  área (agua 66 % · tierra 15 % · bosque 8 % · hielo 7 % · arena 3 %).
  El recuento se pondera por **cos(latitud)**: contar píxeles a pelo daba 24 % de hielo para un
  casquete que ocupa el 7 % de la esfera — en equirectangular una fila polar cubre muchísima menos
  superficie que una ecuatorial.
- [x] **Escenas de prueba de carga**: `main_2048.scene` y `main_4096.scene`. `zoneMap` y `texRes`
  son costes SEPARADOS y conviene no confundirlos — el mapa de zonas es un PNG que se sube tal cual
  (VRAM, casi nada de CPU) mientras `texRes` gobierna los mapas de bioma y macro, que se evalúan
  **píxel a píxel en CPU** al arrancar. Medido: 512² ≈ 2 s por mapa y planeta; 2048² ≈ 32 s; 4096²
  serían ~13 min por escena, que no es una prueba de render sino de paciencia. Por eso las variantes
  suben el `zoneMap` y dejan `texRes` en 1024.
- [ ] **`planet.frag` (terreno V3 por chunks) no bindea el mapa de zonas** — solo la ruta
  SimplePlanet. Ahí sigue decidiendo el clima.

- [x] **Aplicado a `Survival/scenes/main.scene`**: `surface` restaurado del backup + `zoneMap`
  apuntando a `earth_albedo.png` (que resultó ser un mapa de zonas, no una textura de material) y
  5 materiales — water/land/forest/ice por zona, rock por pendiente.
- [ ] **Quitar las 8 texturas sueltas de `loadBiomeTextures`**: el shader ya no las lee (usa los
  arrays), pero siguen cargándose porque de ellas cuelga el **fallback procedural** que genera
  texturas cuando el proyecto no trae ninguna. Ese fallback debería producir capas del array.
- [ ] **`ClimateOutput` sigue sin sombra orográfica de verdad** (trazar contra el viento hasta el
  mar necesita la retícula de elevación, que vive en `PlanetFields`). Lo correcto a medio plazo es
  que el mapa de biomas se hornee del MISMO campo que lee el shader, no de un modelo paralelo.

### El detalle cercano: qué pieza resuelve qué (análisis, 2026-08-01)

La aritmética que ordena todo lo que queda de terreno. Una textura ÚNICA para el planeta entero no
existe: a 1 cm por téxel son **5,1 × 10¹⁸ téxeles** (5 exabytes); ni a 1 m por téxel cabe (510 TB).
Y el mapa de zonas actual, a 4096², da **9,8 km por téxel**. Entre lo que hay y lo que se quiere hay
seis órdenes de magnitud, así que **ninguna resolución de archivo llega** — no es cuestión de subir.

Lo que sí llega son tres piezas distintas, y conviene no confundirlas:

| Pieza | Qué resuelve | Coste |
|---|---|---|
| **Tiles que se REPITEN** | grano y relieve de superficie: 2048² cada 2 m = **0,98 mm/téxel** | 17 MB FIJOS, todo el planeta |
| **Chunks de GEOMETRÍA** (quadtree LOD) | la FORMA: laderas, costas, montañas | streaming, ya existe en el motor |
| **Chunks de TEXTURA** (virtual texturing) | detalle ÚNICO donde el autor pinte | 64 chunks a 1024² = 268 MB |

Chunkear **no baja la resolución: la sube**. Es la MISMA textura de 4096² cubriendo menos superficie
— sobre el planeta entero da 9,8 km/téxel, sobre un chunk de 100 m da **2,4 cm**, sobre uno de 40 m
da **1 cm**. Lo que cambia es cuántos caben a la vez, y de ahí el streaming.

**El 1 cm de material ya está** (tiles), y era justo lo que anulaba el bug del `tiling`. Lo que falta
es la FORMA, y eso no tiene rodeo: sin triángulos no hay ladera por mucho normal map que se ponga.

- [ ] **1. Cablear el terreno por chunks a la escena.** Es lo único que arregla el relieve y las
  costas cuadradas. Todo lo demás son parches sobre un problema que no es de textura.
- [ ] **2. Virtual texturing** — `src/renderer/virtual_texturing.{h,cpp}` está en el árbol **sin un
  solo call-site**. Es la pieza que permite "1 cm donde miras" sin guardar exabytes.
- [ ] **3. Materiales por PROFUNDIDAD** (tierra vegetal → arcilla → roca → vetas). Como **función
  de la profundidad**, no como textura 3D: una textura volumétrica a escala planetaria tiene el
  problema de arriba elevado al cubo. Un perfil de estratos por material son cuatro números, se
  evalúa donde haga falta y sirve igual para pintar el corte de una excavación que para decidir qué
  suelta un pico. El sistema de materiales ya tiene la forma correcta — hoy un material es una regla
  sobre (humedad, temperatura, pendiente); la profundidad es un eje más, no un sistema nuevo.
- [ ] **4. El PINCEL / editor de objetos**, que escribe en lo que montan 1 y 2. Hacerlo antes es
  construir la herramienta de autoría encima de algo que no puede representar lo que pintes.
  Sobre el viewport: para OBJETOS el mismo viewport es lo correcto (una cámara, un gizmo, un
  target). Para pintar un planeta también —pintas donde miras—; lo que no puede ser el mismo es el
  DATO: no editas vértices de una malla, escribes en una estructura dispersa.

## Optimización del render del terreno — candidatos (roadmap, 2026-08-05)

> Un planeta a escala real se aligera en tres niveles: geometría (cuántos triángulos), culling (qué
> no se dibuja) y fragmento/streaming (shading y VRAM). Lo que el motor **ya tiene** —un solo suelo,
> clipmap, teselación GPU (`terrain.tesc/tese`), culling de limbo/horizonte (`patchHidden`), culling
> de frustum por esfera conservadora, backface y LOD de material por octavas (`triM`)— no está listado.
> Lo de abajo es lo que **falta**, por prioridad de impacto/esfuerzo.

- [ ] **HiZ occlusion culling + depth-prepass**: tras un prepass de profundidad barato, consultar la
  pirámide de profundidad para saltar los parches que el relieve ya tapa (más que el frustum solo).
  Barato de añadir sobre el pipeline actual.
- [ ] **Streaming de tiles de terreno desde disco**: no hornear/generar todo el planeta en RAM;
  cargar por región. El quadtree de chunks de geometría ya existe en el motor (ver § detalle cercano).
- [ ] **Virtual texturing** — `src/renderer/virtual_texturing.{h,cpp}` está en el árbol **sin un solo
  call-site**: es la pieza que permite "1 cm donde miras" sin guardar exabytes. Ya anotado en
  § "El detalle cercano".
- [ ] **GPU-driven / Meshlets (Nanite-style) + sub-píxel culling**: pre-generar meshlets (~64-126
  vértices) y recortarlos por clúster y por triángulo sub-píxel antes de rasterizar. El gran salto,
  pero es overhaul (requiere mesh/task shaders, otra pila Vulkan).
- [ ] **Draw-indirect / instancing GPU-driven para Props y vegetación**: `drawIndexedIndirect` ya
  existe en el RHI (`src/rhi/opengl/gl_context.cpp:233`) **sin llamadores**; falta el culling
  GPU-driven y el `MultiDrawElementsIndirect` para objetos a gran escala.

## v1.2 — Materiales de verdad

- [ ] **Texturas por material compartidas** entre edificios, piezas de construcción e items (hoy
  color plano): es lo que hará que una baldosa de piedra parezca piedra y no un cubo gris.
- [ ] **Malla procedural por semilla** (`shape` orgánico: log/branch/rock/ore). Hoy solo funciona la
  familia primitiva; las orgánicas se dibujarían como cajas.
- [ ] Nodos de materiales PBR combinados en un solo grafo (albedo + normal + roughness).
- [ ] Clima dinámico conectado: `WeatherSystem` → `SampleWeatherNode` → biome map animado.

## v1.3 — Sistema solar heliocéntrico (la Tierra orbita el Sol)

> **Estado: MILESTONE pendiente, decidido NO implementarlo de momento** (autor, 2026-08-01).

Hoy el ciclo día/noche se implementa haciendo girar el **Sol alrededor del planeta**
(`WorldSystem::updateDayNight`): es el atajo clásico de los juegos y, con el floating-origin
anclado a la Tierra, desde el suelo es **idéntico** a que la Tierra girara — el movimiento es
relativo. La Luna sí orbita de verdad (`updateMoon`). La escena ya es heliocéntrica en su
**disposición** (Earth está ~1 UA del Sol), solo el **movimiento** está invertido.

Para que el día/noche salga de la **rotación axial** de la Tierra y Earth se traslade por una
**órbita kepleriana** real alrededor del Sol hace falta desacoplar el mundo del suelo: hoy todo
(el terreno, la cámara, el jugador) vive anclado al planeta, así que "Earth se mueve" no es
representable en espacio-mundo; solo lo es el movimiento aparente del Sol. Implicaciones:

- La posición del planeta pasa a ser una órbita (a, e, T, fase) alrededor del Sol, no un punto fijo.
- El **día** sale de la rotación del suelo respecto a la cámara/star y el **año** del desplazamiento
  orbital; el `originShiftingTarget` tiene que seguir al planeta mientras el Sol queda fijo.
- El Sol deja de moverse por frame; lo que rota es el marco local del planeta.
- Riesgo: todo el render/colisión asume suelo estático — es un cambio de simulación, no de escena.

## v2.0 — Decisión del autor, no técnica

- [ ] **Backend Vulkan** (`src/rhi/vulkan/` está vacío). La costura RHI/PSO ya está: es un backend,
  no un rewrite. ⚠️ No sube fps (el frame es CPU-bound) — su valor es de motor/portfolio.
- [ ] **DGS / anticheat** — `docs/guides/PLAN_DGS_ANTICHEAT.md`. Reglas por proyecto vía `dlopen`.
- [ ] **Exportar a Windows / Linux / macOS.**

---

# IDE — HarukaEditor

## v0.1.0 — ENTREGADO

- **Port completo a la API actual del motor**: SceneManager, `shared_ptr`, GLFW → SDL3, viewport
  delegado a `Application::renderFrame()` sobre un `RenderTarget`, gizmos con ImGuizmo.
- **Separación IDE/motor**: fuera todo conocimiento planetario (`PlanetarySystem`, `createPlanet()`,
  panel de terreno). Un planeta es un `SceneObject` más.
- **Panel Objects** como rejilla de tarjetas agrupada por capas (Props, Monsters, Spawns, Characters,
  Lights, Default + capas custom), con creación rápida por capa y menú contextual.
- **Inspector** con capa, Material & Textures, Monster/Entity, Spawn Point y atributos dinámicos JSON.
- **Node Graph Editor** de texturas/materiales: canvas pan/zoom, catálogo completo de nodos,
  Material Output de 5 slots, preview en vivo y **Bake Textures** a PNG.
- **Flujo de proyecto nuevo**: template del motor + `build.sh`, `createNewProject` / `compileProject`.

## v0.2 — EN CURSO: editor de modelos por vértices

El motor ya mantiene una copia CPU editable de la malla; falta la interfaz.

- [x] **Encuadrar al seleccionar**: clicar un objeto en Objects o en la jerarquía lleva la cámara
  hasta él y lo deja **centrado ocupando la misma fracción de pantalla**, mida 40 cm o 6371 km. El
  radio lo da el motor (`Application::getObjectBoundingRadius`), que es quien sabe si detrás de un
  `SceneObject` hay un planeta. Va en **doble clic**: seleccionar para editar no mueve la cámara.
- [x] **Rueda del ratón = dolly** con paso proporcional a la distancia al objeto seleccionado.
- [x] **Preview visible de material y texturas** en Inspector y Node Graph Editor: esfera con el
  material montado + miniatura por slot (recuadro rojo si la ruta no carga, que es el diagnóstico
  que antes solo se veía como un objeto de color plano en el viewport).
- [x] **Raíz de assets del editor** (`Shader::setBaseDir` desde `SDL_GetBasePath`): no la fijaba
  NADIE —ni motor, ni editor, ni juego—, así que el motor pedía `shaders/*` relativo al cwd mientras
  los ficheros viven en `assets/shaders/`. No compilaba ni un pipeline y no había más pista que un
  `HARUKA_LOGW` por shader.
- [x] **Use-after-free al cargar una escena** (`SIGSEGV` en `getObjectByName`): cada punto que
  sustituía la escena repuntaba su propia lista de paneles —init() seis, loadFile() tres,
  createNewProject() cinco— y los que quedaban fuera conservaban el puntero a la `SceneManager`
  destruida. Ahora hay un único `bindPanelsToScene()`, llamado también cuando la carga FALLA (la
  escena vieja ya está destruida en ese punto).
- [x] **Arrastre de nodos del node graph**: el cuerpo y cada socket de un nodo son ahora
  `InvisibleButton` (registrados ANTES que los widgets de parámetros → ImGui les da prioridad a
  esos), así el arrastre ya no depende de `!ImGui::IsAnyItemActive()`: antes un DragFloat del propio
  nodo —o un `InputText` de otro panel— congelaba los nodos y parecía que estaban clavados.
- [ ] **Modelo de datos**: editar vértices/normales/índices vía `MeshRendererComponent::getSource*`
  + `setMesh`.
- [ ] **Objetos con malla editable**: `MeshComponent`/`MeshRendererComponent` como malla de verdad,
  no solo primitivas fijas.
- [ ] **Edit Mode en el MISMO viewport** (no ventana ni panel nuevo): toggle Object/Edit junto a los
  radio buttons de gizmo. Seleccionar, mover, alisar, extruir con el gizmo ya integrado.
  *Por qué en el viewport*: hay una cámara, un ImGuizmo y un `_editorTarget`; un segundo viewport son
  un RenderTarget y un `renderFrame()` más por frame.
- [ ] **Preview 3D del modelo en las tarjetas del panel Objects** (lo más caro: es un tercer render
  target offscreen). Va al final de la versión, no al principio.

## v0.3 — Editor de objetos y de mundo

- [ ] **Editor de objetos de escena** (custom, básicos y planetas) con **exportación en varios
  formatos**, incluido el propio del motor.
- [ ] **World UI editable** desde el IDE. *Pregunta abierta: ¿hace falta un editor de World UI aparte,
  o el editor de objetos cubre el caso?* — ver [TODO.md](TODO.md).
- [ ] Aprovechar la rama `docking` de ImGui para layouts acoplables también en la UI del motor.

---

# Juego — Survival

Prioridad por valor. El detalle de lo cerrado está en [docs/HISTORIAL.md](docs/HISTORIAL.md) y en
`Survival/docs/DIRECCION.md`.

## v0.2 — ENTREGADO, PENDIENTE DE VALIDAR EN JUEGO

Cuatro tandas cerradas con los tests en verde y **ninguna vista jugando**. La lista de qué mirar en
cada una está en [TODO.md](TODO.md) § Verificación.

- **Estilo cel-shading** (toon suave + rim frío) en escena, props, follaje, hierba y terreno; bruma
  cálida, paleta por bioma, nubes por fases en `sky.frag`.
- **Construcción**: piezas desde items, sockets/snap, grafo de fijaciones, rotura estructural en
  cascada, prefabs, instancing, herramienta de terreno y caminos.
- **Edificios / imperios / ciudades DESDE ARCHIVOS**: añadir un imperio ya no recompila.
- **Clima A→E** y **spellcasting + idioma antiguo** (gramática raíz+modificador+forma, manifestación
  por forma, y una LENGUA completa que comparte piezas con los conjuros).

## v0.3 — Lo siguiente

- [ ] **Bloom + color grade** (el "glow" anime) → **agua toon** → acorde de bioma en props →
  contorno solo en personajes.
- [ ] **Modelos base de casa por ROL** (los hace el autor; van en los huecos como fixtures).
- [ ] **F2 construcción**: prefabs con fijaciones internas, yaw a lo largo del tramo, curva por
  spline, escaleras. **Roller/apisonadora** y superficie visual de las vías.
- [ ] **Vida**: monstruos con cuerpo procedural (esqueleto → miembros, IK), el bioma decide qué
  aparece. Audio ambiental por bioma.
- [ ] **Jugabilidad**: interacción física, HUD (chat, grupo, inventario), grimorios como objetos.
- [ ] **El jugador no es un `Combatant`** → los hechizos no pueden excluirle ni curarle.
- [ ] Que el clima **se oiga** y afecte más allá de la capa (visibilidad, agarre, temperatura).

## v0.4 — Mundo compartido

- [ ] **Mapa 2D/2.5D/3D como herramienta del motor**: el jugador descarga un mapa (terreno,
  edificios, objetos) y lo va **mapeando** conforme lo explora; puede compartirlo. Limitable a una
  zona, una ciudad o un planeta entero.
- [ ] Frentes de clima en el HUD/minimapa y sincronía con el DGS (el estado ya es replicable:
  seed + reloj).
- [ ] El DGS valida un hechizo **como verbo**.
