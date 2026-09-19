# PLAN PINCEL DEL MUNDO — deformar, pintar mapas, y colocar cuevas/islas desde el editor

> Sustituye a `PLAN_CUEVAS_ISLAS.md` (2026-07-05), que da por existentes cosas borradas el 2 de
> agosto. Estado real a 13-09-2026: existe UN campo volumétrico por chunks (`VoxWorld`, dos
> canales: quitado `d` / añadido `a`) donde viven cuevas, islas y lo que pica el jugador; malla,
> colisión, recorte del terreno, agua y props lo leen del mismo sitio. Cuevas e islas se definen
> por mapas PNG en `bakes/` y se COLOCAN por fichero de partida (`vox/caves.txt`, `vox/islands.txt`).
> No hay herramienta: ni pincel, ni colocar con el ratón, ni pintar mapas, ni deshacer.

Principio que no cambia (decisión de Andoni): **una sola forma de tocar el mundo**. Editor y juego
usan la misma API (`VoxWorld::stroke / placeCave / placeIsland`); el panel vive en el MOTOR
(`src/tools/`, como `prefab_edit_panel`) y el editor y el juego lo hospedan.

---

## 0. Qué es "deformar" y qué no

| Capa | Qué es | Quién la toca | Estado |
|---|---|---|---|
| **A. Campo volumétrico** (`VoxWorld`) | lo que le ha PASADO al relieve: cuevas, islas, picar, construir | jugador (pico/mano) y editor (pincel) | hecho; sin herramienta |
| **B. Relieve base** (heightfield horneado, `PlanetFields`) | cómo NACIÓ el planeta: cordilleras, valles, costa | sólo el editor de mundos | inerte (`editTerrain` ya no va aquí) |

El pincel de este plan es la capa **A**. Deformar la capa **B** (subir una cordillera) no cabe en
el campo: a 4 m de vóxel y 100 m de fondo la Tierra son ~800 TB. Si se quiere, es **otra
herramienta** (§4) con su propio coste: un delta de altura disperso que shader y física lean encima
del bake, o rehornear (2 s por bake, cacheado por hash). Se decide después de tener A.

---

## 1. Pincel sobre el campo (deform)

**Gesto**: clic/arrastre en la vista → rayo → primer punto donde `density(p) ≥ 0` (contra el CAMPO,
no contra la malla: es lo que ya hace `sandbox.cpp` con `Application::worldFieldAt`) → `stroke`.

**Parámetros del panel**
- Modo: **Quitar** (min, abre aire) / **Añadir** (max, pone roca). Un solo control, signo cambiado.
- Forma: esfera (hoy) · cilindro radial (pozo/columna: eje = `up` local, alto h) · caja alineada al
  tangente (pared/losa). Todas son distancias con signo → mismo `applyLocal` con un funtor de forma.
- Radio 1–60 m (un trazo no puede ser mayor que un chunk: 27 claves por trazo, ver `stroke`).
- Paso de arrastre: un trazo cada `radio·0,5` m recorridos (no por frame: el resultado no puede
  depender de los FPS).
- Vista previa: la forma en alambre en el punto de impacto, verde (quitar) / naranja (añadir), con
  la normal del campo (`gradient`) como eje.

**Persistencia**: trazos por chunk en un fichero de texto, como hoy. Los del diseñador van a la
carpeta `voxEdits` que declara el planeta en la escena (`assets/vox/<planeta>/`, §2); los del
jugador a `<partida>/vox/` y se aplican ENCIMA al cargar el chunk (dos listas por chunk, en ese
orden). Hoy sólo existe la segunda.

**Deshacer/rehacer**: los trazos son una lista ordenada por chunk → `VoxWorld::undo(n)` quita los
últimos n trazos (globales: se lleva un contador de secuencia por trazo), rehornea el chunk y
reaplica los que quedan. Test: campo bit a bit igual al de antes del trazo (contraprueba: sin
deshacer, distinto). Coste: rehornear un chunk con cueva ~2 ms.

**Lo que toca del motor** (todo con test):
- `VoxWorld::stroke(pos, Shape, remove)` con `Shape{esfera|cilindro|caja, radio, alto, ejes}`;
  el fichero de trazos gana columnas (forma, alto, ejes) — versión 2, la 1 se sigue leyendo.
- `VoxWorld::undo(n)` / `redo(n)` + `strokeCount()`.
- `VoxWorld::raycast(o, d, maxM)` por marcha sobre el campo (paso = mitad del vóxel, refinado por
  bisección): hoy está en `sandbox.cpp` del juego y el editor lo necesita igual.

---

## 2. Cuevas e islas SON OBJETOS DE LA ESCENA, sin semilla (decisión de Andoni, 13-09)

Hoy una cueva colocada es "la celda que contiene lat/lon" y su forma sale de (seed, celda): el
fichero de la partida sólo guarda el punto. Eso se va. **Una cueva o isla colocada es una entrada
del `.scene`** (JSON), como el planeta o un prefab colocado, y sus mapas son ASSETS referenciados
por ruta, como `surface.elevationMap` del planeta:

```json
{ "type": "Cave", "name": "cueva_mina", "position": [x, y, z],          // centro de la caja, mundo
  "properties": { "radiusM": 320, "depthM": 140, "mouth": [0.4, 0.6, 0.05],
                  "planta": "assets/vox/cueva_mina_planta.png",
                  "perfil": "assets/vox/cueva_mina_perfil.png" } }
{ "type": "Island", "name": "isla_norte", "position": [x, y, z],        // centro de la BASE
  "properties": { "radiusM": 260, "topM": 30, "rootM": 110,
                  "planta": "...", "cima": "...", "raiz": "..." } }
```

y el planeta gana `"surface": { ..., "voxEdits": "assets/vox/earth/" }`: la carpeta de trazos del
diseñador (pincel §1), un fichero por chunk como hoy.

- La identidad es el nombre del objeto de escena. Dos cuevas pueden solaparse (el campo es un
  `min`: se funden). El editor las trata como cualquier objeto: seleccionar, mover con el gizmo
  (mover = otra `position`; la caja se rehornea en los chunks), duplicar, borrar.
- La **semilla sólo sirve para el borrador**: "Nueva cueva aquí" hornea unos mapas con la semilla
  que sea (`caveBakeMaps`) y los GUARDA como PNG en `assets/vox/`; la entrada de escena apunta a
  ellos. A partir de ahí el generador no vuelve a tocarlos; "Regenerar" pide otro borrador y los
  sobrescribe. Pintar es editar esos PNG (§3).
- Lo DERIVADO (`_vol.png`, `_boca.png`) sigue en `bakes/`, con clave = hash de las propiedades +
  hash de los PNG: cambiar un texel rehace el volumen; un PNG idéntico no (contraprueba).
- La siembra automática (`caveAutoProbability`, hoy a 0) queda como productora de borradores, no
  como definición del mundo. `CaveBox::seed/cell` salen de la definición; `caveBoxForCell` pasa a
  ser "propón una caja aquí".
- **La partida**: no copia nada. El juego carga la escena (ya lo hace) → `VoxWorld` recibe las
  entradas Cave/Island y la carpeta `voxEdits`; lo que pique el jugador va a `<partida>/vox/`
  encima, nunca a los assets. Los `caves.txt`/`islands.txt` de partida actuales desaparecen (eran
  el andamio de estos días; se migran a entradas de escena una vez).

Lo que cambia en el motor (con test cada uno):
- `CaveSystem`/`IslandSystem` se cargan de (propiedades + rutas de PNG), no de (seed, celda):
  `caveLoad(const CaveDef&)`, `islandLoad(const IslandDef&)`.
- `SceneObject` tipo `Cave`/`Island` (serialización en `SceneManager`), y en `PlanetarySystem`,
  al montar el planeta desde la escena, `vox().setCaves(defs)` / `setIslands(defs)` /
  `setDesignerEdits(dir)`.
- Test: escena con una cueva cuyo `planta.png` es un corredor recto pintado → el campo tiene aire
  exactamente por ese corredor y en ningún otro sitio; seed 0 y seed 1234 dan el MISMO campo
  (contraprueba de que no depende de la semilla).

## 3. Pintar los mapas

**Panel "Mapas"** (para la cueva/isla seleccionada): los 2 (cueva) o 3 (isla) PNG de su carpeta
como imágenes 256² editables: pincel de valor (0–1, radio, dureza), relleno, y para la planta de
cueva un modo "corredor" (línea de anchura w). Botón **Rehornear**: guarda los PNG, tira el sistema
cargado, descarga los chunks de la caja (`dropChunksNearNoLock`) → se ve al instante; muestra la
conectividad que ya calcula `caveBakeVolume` (componentes antes → después, vóxeles de aire): si al
pintar salen 5 bolsas, se ve el número. Vista previa 3D: `caveBuildMesh` en alambre sobre el
terreno, sin esperar a los chunks.

**Colocar ("spawn")**: modo Cueva/Isla → clic = "Nueva aquí" (borrador horneado y guardado como
autoría, caja en alambre); clic sobre una puesta = seleccionar; deslizadores de centro, radio,
fondo/altitud (el `.txt`), Supr = quitar la carpeta. La isla avisa en rojo si la raíz corta el
terreno (`islandColumnAt` vs `sampleHeight`).

## 4. Relieve base (más adelante, si se quiere)

Sólo para que quede escrito lo que costaría. Opción única realista: **capa de deltas de altura**
dispersa (parches de 256² a 1 texel del bake) que `uHeightTex` no conoce → el shader del nodo y
`terrainHeightAt` de la física suman el delta si el parche existe. Es la capa que
`warnDeformationInert` ya reclama. Coste: un parche en GPU por nodo afectado + la ruta de
muestreo en CPU. Alternativa (rehornear el bake entero): 2 s por cambio, inviable como pincel.
Si se hace, entra en la MISMA ventana de paridad que el terreno ([[terrain-parity-state]]).

---

## 5. Dónde vive cada cosa

- Motor, `src/tools/world_edit_panel.{h,cpp}` (ImGui): las tres pestañas (Pincel · Colocar ·
  Mapas), sobre `VoxWorld&` + un `IWorldHost` con tres inyecciones: `raycast`, `drawWire`
  (líneas de depuración del anfitrión), `worldDir` (carpeta de autoría). Sin gizmo propio.
- Editor (`haruka/`): anfitrión del panel + su vista (ya tiene líneas de depuración y picking).
- Juego: el mismo panel bajo una tecla de depuración (F-algo) para "desde el juego sin editar",
  como pidió Andoni con los prefabs.

## 6. Orden y qué se mide

1. Modelo: cuevas/islas como objetos de escena sin semilla (§2), `voxEdits` del planeta, y el
   juego cargándolos de la escena. Tests: mismo campo con dos semillas; corredor pintado a mano.
2. `VoxWorld`: formas, `undo/redo`, `raycast`. Tests: deshacer bit a bit; raycast contra la cima.
3. ✅ (14-09) Panel Pincel + Colocar: motor `src/tools/world_edit_panel`, anfitrión
   `haruka/src/panels/world_panel` + gesto e indicador en `viewport.cpp` (`handleWorldTool`).
   La escena manda (`syncFromScene` cada frame). Arranque sin manos: `HARUKA_EDITOR_PROJECT`,
   `HARUKA_EDITOR_OPEN=world`, `HARUKA_EDITOR_QUIT_AFTER`, `HARUKA_EDITOR_WORLD_TEST=1`.
4. ✅ (14-09) Panel Mapas (pestaña del mismo panel): pincel de valor, corredor a dos clics,
   Rehornear con conectividad; la planta de isla se pinta como máscara y se chanflea al guardar.
   Sin hacer: vista previa 3D de la malla entera; el panel en el JUEGO bajo una tecla.
5. (Opcional) §4.

Cada paso se cierra viéndolo: captura RHI del panel no tiene sentido; lo que se mide es el CAMPO
(tests) y una corrida del juego con `donde` sobre lo colocado.
