# PLAN — Puertos autorizados (montaje + interacción) y RAÍLES (mecanismos, no animaciones)

> Estado: DISEÑO v1 (2026-08-15). Decisiones del usuario CERRADAS (§0). El **puerto genérico** y el
> **raíl** viven en el motor (`haruka-cpp`) y en el editor (`haruka`); el **vocabulario de requisitos**
> (estanco, alimentado, afinidad, manual) es del proyecto — misma separación que
> [[PLAN_CONSTRUCCION.md]] y §3-ter de `Survival/docs/DIRECCION.md`.
> Consumidor de referencia: `Survival/docs/VEHICULOS.md` §9 (emplazamiento válido) y §6 (paneles).

## §0 — Decisiones cerradas

1. Los puntos de acople se **AUTORIZAN en el asset**, no se derivan de la caja envolvente.
2. Un puerto sirve para **montar** y para **interactuar**: es el mismo dato con distinta clase.
3. **Puertas, rampas, escotillas, torretas y suspensiones son MECANISMOS con raíl**, no animaciones
   estáticas. Un raíl es un puerto con **grados de libertad**.
4. No es una herramienta nueva: es un **panel + gizmo** en el editor que ya existe.

---

## §1 — ESTADO ACTUAL: el hueco

Hoy los sockets se **derivan del AABB**: los centros de cara de la caja envolvente
(`Survival/scripts/build_system.cpp:240`, y así está apuntado en [[PLAN_CONSTRUCCION.md]] —
*"Sockets/SNAP derivados del AABB"*).

Eso vale para muros y bloques. **No puede valer** para nada de lo que ya está diseñado:

| Pieza | Por qué la caja no basta |
|---|---|
| Cámara de reacción | La toma de aire va en **una cara concreta**, no en las seis |
| Bancada de torreta | Anillo mirando **arriba**, con eje de giro |
| Asiento de piloto | Tiene **orientación**: se mira hacia algún sitio |
| Manómetro | Va en un **mamparo**, a la altura de los ojos, y pertenece a una cubierta |
| Puerta / rampa | Necesita **eje y recorrido**, no un punto |

⚠️ El trabajo no es "hacer una herramienta de modelado". Es **que los puertos sean un dato del asset**.

---

## §2 — Referencia: el *Item Port* de Star Citizen

Star Engine es un fork de CryEngine (vía Lumberyard) y su DCC principal es Maya con exportadores
propios, herederos de la línea CryExport. **Eso es fontanería de formato y no es lo que hay que
copiar.**

Lo que sí hay que copiar es su **Item Port**: todo lo acoplable (planta de energía, escudo,
refrigerador, arma, tobera) monta en un puerto autorizado en el asset, con **clase** y **talla**. Una
pieza de talla 2 no entra en un puerto de talla 1. No existe una tabla de "qué chasis admite qué":
existen puertos, y las piezas encajan o no.

Es lo mismo que ya se derivó por otro camino en `VEHICULOS.md` §9 (`mount` + `mountStrengthMPa`),
con un eje más. También coinciden su sistema de habitaciones (atmósfera/presión → §10 de VEHICULOS)
y su framework de UI en el mundo (→ §5 de DIRECCION).

---

## §3 — EL DATO

```
Port {
  id        : nombre estable ("intake_fwd", "seat_pilot", "door_starboard")
  transform : posición + orientación LOCALES al prop (no al mundo)
  cls       : clase — "mount" | "interact" | "rail" | "socket"
  kind      : vocabulario del PROYECTO ("floor","bulkhead","hull_bottom","chamber"…)
  size      : talla (entero). Una pieza de talla N pide puerto ≥ N
  partIndex : a QUÉ PARTE del prop pertenece (-1 = al prop entero)
  motion    : null | Rail{...}   ← §4
}
```

`partIndex` es la pieza clave y sale gratis: los props **ya tienen collider por parte** (esqueleto
del árbol → cápsulas). Ligar el puerto a una parte hace que **montaje, interacción y daño hablen del
mismo trozo**. Un manómetro atornillado a la cubierta 2 se rompe cuando se rompe la cubierta 2, sin
código nuevo.

---

## §4 — RAÍLES: puertas, rampas y suspensiones NO son animaciones

⚠️ **Decisión.** Una puerta no es un clip que se reproduce. Es un cuerpo **restringido a un grado de
libertad**, con límites y opcionalmente un motor. Un raíl es, literalmente, un puerto con `motion`.

```
Rail {
  dof        : "hinge" | "slider"          // giro alrededor de un eje | deslizamiento por un eje
  axis       : eje local
  limits     : [min,max]                   // grados o metros
  drive      : "motor" | "manual" | "spring"
  motorForce : N·m o N                     // solo drive=motor
  compliance : α (m²/N)                    // 0 = rígido; >0 = amortiguado (suspensión)
  breakForce : N                           // supera esto y la unión CEDE (VEHICULOS §5)
}
```

| Mecanismo | dof | drive | Nota |
|---|---|---|---|
| Puerta batiente | hinge | manual/motor | límites 0-95° |
| Rampa de carga | hinge | motor | pide `powered`: la mueve la presión de la cámara |
| Escotilla corredera | slider | motor | |
| Torreta | hinge (yaw) + hinge (pitch) | manual/motor | dos raíles encadenados |
| **Suspensión** | slider | spring | `compliance > 0` — ya está en `xpbd/constraints.h` |
| Compuerta estanca | slider | motor | cerrarla **particiona el aire** (VEHICULOS §10) |

### Por qué raíl y no animación — cinco razones, todas prácticas

1. **Colisión correcta.** Una animación atraviesa o teletransporta lo que tenga delante. Un raíl
   **empuja** la caja que estorba, o **se bloquea** contra ella. Una puerta que no cierra porque hay
   un cadáver en medio no hay que programarla.
2. **Sobrevive al daño.** Si la unión supera `breakForce` (VEHICULOS §5), **la puerta se cae**. Una
   animación no puede caerse: o se reproduce o no.
3. **Marco de referencia gratis.** Una puerta en un crawler en marcha no hay que reautorizarla: la
   restricción vive en el espacio local del vehículo (VEHICULOS §3).
4. **Carga real.** Una rampa con peso encima **cede**; el motor tiene que vencer ese par o no sube.
   Con animación, una rampa levanta un acorazado igual que una pluma.
5. **Se autoriza en el mismo panel que los puertos.** Un gizmo, un dato, dos usos.

### Respaldo del motor: ya está en Jolt

`HingeConstraint`, `SliderConstraint`, `FixedConstraint`, `DistanceConstraint`, `MotorSettings` y
`GearConstraint` están en el árbol (`third_party/JoltPhysics/Jolt/Physics/Constraints/`). El mapeo es
directo: `hinge`→Hinge (+Motor), `slider`→Slider, suspensión→Slider con muelle o el
`DistanceConstraint` con `compliance` del XPBD propio.

⚠️ **No hay que escribir solvers.** Hay que escribir el **dato**, el **gizmo** y el **puente** a Jolt.

---

## §5 — Reparto motor / proyecto (§3-ter de DIRECCION)

| | Qué define | Dónde |
|---|---|---|
| **Motor** | `Port`, `Rail`, serialización, puente a Jolt, gizmo, raycast a puerto | `haruka-cpp` + `haruka` (editor) |
| **Proyecto** | El **vocabulario** de `kind` y los **predicados** (`sealed`, `penetration`, `freeVolume`, `powered`, afinidad, manual) | `Survival` |

El motor nunca sabe qué es "estanco". Sabe que un puerto tiene una clase y una talla, y que alguien
le pregunta si algo encaja. Los predicados de `VEHICULOS.md` §9 se evalúan **en el proyecto**, sobre
los puertos que le da el motor.

---

## §6 — API mínima

```cpp
// MOTOR — consulta
const PortSet* ports(ObjectId);              // los puertos de un prop/malla
glm::dmat4     portWorld(ObjectId, PortId);  // transform en MUNDO (aplica el marco local)
struct PortHit { ObjectId obj; PortId port; int partIndex; };
bool           raycastPort(ray, PortHit&);   // mira → puerto (interacción y montaje)

// MOTOR — mecanismo
RailState      railState(ObjectId, PortId);  // ángulo/desplazamiento actual, bloqueado, roto
void           railDrive(ObjectId, PortId, float target);  // pide posición al motor

// PROYECTO — reglas
bool fits(const Port&, const PartDef&);      // clase + talla + predicados del juego
void onInteract(PortClass, Handler);         // registrar manejador por clase
```

⚠️ `railDrive` **pide**, no impone: si hay algo bloqueando, el motor empuja hasta su límite de par y
se queda a medias. Ese es el punto.

---

## §7 — Editor: panel + gizmo (no ejecutable nuevo)

El editor ya tiene `inspector`, `viewport`, `objects_panel`, `node_graph_editor` y `ui_builder`.
Esto es:

- **Panel "Puertos"**: lista los puertos del prop seleccionado; añadir/borrar; editar `cls`, `kind`,
  `size`, `partIndex`; si es raíl, `dof`/`axis`/`limits`/`drive`.
- **Gizmo en el viewport**: colocar y orientar el puerto sobre la malla. Para raíles, dibujar el
  **eje** y **barrer el recorrido** entre límites (ver la puerta abrirse en el editor, no en el juego).
- **Validación en sitio**: avisar si un raíl barre a través de geometría propia.

---

## §8 — UI en el mundo anclada a puerto

Un puerto `cls="interact"` es donde vive un panel (asiento de piloto, manómetro, válvula). Reglas:

- El panel **se ancla al puerto**, así que se mueve con el vehículo sin trabajo extra.
- ⚠️ **Su estado es estado del MUNDO, no del cliente** (§5 de DIRECCION): dos jugadores ven la misma
  aguja, y por eso es arquitectura de red, no de interfaz.
- Si la parte (`partIndex`) se destruye, el panel **desaparece o miente** — la instrumentación
  dañada es una decisión de diseño ya tomada en `VEHICULOS.md` §6.

---

## §9 — Orden de trabajo

**HECHO (2026-08-15):**

1. ✅ **`Port` + `Rail` + serialización** — `src/game/ports/port.{h,cpp}`. Registro por assetId,
   `portWorld`/`railAxisWorld`, `portAccepts`, `raycastPorts`, `railClamp`/`railBreaks`/`railStep`,
   ida y vuelta por JSON y carga de directorio con **orden estable** (`directory_iterator` no lo
   garantiza y el registro acabaría dependiendo del sistema de ficheros).
4. ✅ **Evaluador de predicados en el proyecto** — `Survival/scripts/vehicles/placement_rules.{h,cpp}`.
   Los ocho predicados de VEHICULOS §9, leyendo los `requires` del JSON real.
   ⚠️ El orden de comprobación es deliberado: primero lo que no se arregla sin rehacer el casco
   (volumen, anclaje, estanqueidad), luego lo que se arregla añadiendo una pieza. Así el motivo que
   se le enseña al jugador es el **de fondo**, no el primero del struct.

**Tests** (`tests/test_ports.cpp` en el motor, `Survival/tests/test_placement.cpp` en el proyecto),
todos con **contraprueba** — un test que solo comprueba que algo "no peta" no distingue una
implementación buena de una que devuelve siempre lo mismo:

- El eje de un raíl **gira con el prop**, y se aserta que NO es invariante al giro (si los puertos
  fuesen caras de un AABB, eso no podría distinguirse).
- `portAccepts` rechaza talla mayor, otro `kind` y los puertos de interacción.
- El raíl **BLOQUEA** cuando la resistencia supera el motor y **ROMPE** cuando supera `breakForce`,
  y una vez roto no vuelve a obedecer. Es exactamente lo que una animación no sabe hacer.
- Contra Jolt (`test_physics.cpp`): el motor **llega** al objetivo (44,8° de 45°), el tope **acota**
  lo que se pide de más **y se llega a él** (89,7° al pedir 500° — sin esa segunda mitad, un raíl que
  no se moviera pasaría el test), sin motor **no se mueve sola**, con más carga que par **se queda a
  medias y lo reporta**, y por encima de `breakForce` **cede y ya no obedece**.
- Enganches (`Survival/tests/test_sockets.cpp`): **las dos mitades por separado** — sin `PortSet`
  salen los cinco centros de cara de siempre (si solo se probara el caso autorizado, un
  `pieceSockets` que ignorase el AABB pasaría igual, y eso sería la regresión); con `PortSet` mandan
  los puertos y el AABB **no se mezcla**; el socket inferior **mira hacia abajo**, que es justo lo
  que la caja no puede dar; y al girar la pieza gira **la normal**, no solo la posición.
- El generador de O₂ no entra en un coche pequeño **y sí en un crawler sellado** — misma pieza,
  mismas reglas, sin que "coche pequeño" aparezca en ningún sitio.
- Cada predicado rechaza por sí solo, y una pieza sin requisitos cabe hasta en el peor sitio
  (contraprueba de que `canPlace` no rechaza por costumbre).

3. ✅ **Puente a Jolt** — `PhysicsEngine::addRail/railSetTarget/railGet/removeRail` con
   `RailSpec`/`RailInfo` (`src/physics/physics_engine.h`). ⚠️ `HarukaPhysics` no puede depender de
   `game/`, así que la especificación se repite en la frontera de la librería a propósito.

   **Cuatro cosas que solo aparecen al atarlo de verdad** (todas comentadas en el código):
   - Dos `BodyLockWrite` sueltos hacen **assertar a Jolt** por orden de bloqueo. Hay que usar
     `BodyLockMultiWrite`, que ordena los ids él mismo.
   - **El cero del ángulo es la postura de montaje.** Jolt pide una normal perpendicular al eje para
     fijar el 0; con una normal arbitraria la hoja *nace* a un ángulo cualquiera de su propio cero y
     los límites dejan de significar nada (medido: pedía 90° y leía 123,8°). Se toma la dirección
     ancla→hoja: cerrada = 0, límites relativos al reposo.
   - El muelle por defecto del motor de Jolt (**2 Hz**) es de suspensión, no de mecanismo: una rampa
     tardaba decenas de segundos y se quedaba corta (37° de 45°). Un actuador es rígido — lo que lo
     frena debe ser la carga y el tope de par, no la blandura del muelle. Se sube a 20 Hz.
   - **`blocked` hay que medirlo sobre una VENTANA**, no paso a paso: un mecanismo vencido no se queda
     quieto, tiembla milésimas, y el contador por paso se reiniciaba siempre (una rampa de 589 N·m con
     motor de 1 N·m reportaba `blocked=0` estando claramente vencida). Media vuelta de segundo sin
     recorrer medio grado = bloqueada.

   Y una **limitación observada**: los límites del *hinge* los sujeta el solver, así que un contacto
   fuerte puede empujar la hoja **más allá del tope** durante el choque (medido: 123,8° con tope de
   90° al forzarla contra el suelo). Con el motor mandando, el tope se respeta (89,73° al pedir 500°).

6. ✅ **Snap de construcción migrado a puertos** — `Survival/scripts/build_sockets.{h,cpp}`, consumido
   por `build_system.cpp`. Es una **PRECEDENCIA, no una sustitución**: si el asset trae `PortSet`
   mandan sus puertos; si no, se derivan los cinco centros de cara del AABB como siempre. Ninguna de
   las ~130 piezas actuales cambia de comportamiento mientras no se le autorice nada, así que la
   migración no puede provocar una regresión.
   - Solo cuentan los puertos de clase **`Socket`**: un `Mount` es dónde se atornilla un componente y
     un `Interact` dónde vive un panel; mezclarlos haría saltar el fantasma a sitios sin sentido.
   - ⚠️ Un `PortSet` **con puertos pero sin sockets** significa *"esta pieza no engancha"*, y **no**
     se cae al respaldo: caerse convertiría una decisión deliberada en un despiste.
   - **Convenio de la normal**: el **+Y local** del puerto. Es el mismo eje de referencia que ya usa
     la construcción (`glm::rotation({0,1,0}, up)`).
   - Los `PortSet` se cargan en `init.cpp` desde `assets/data/ports/`. La carpeta puede no existir.
   - Primer asset autorizado: `stone_column.json` — una columna quiere apilarse **por arriba y por
     abajo y no por los lados**, y la derivación del AABB da exactamente lo contrario (arriba y los
     cuatro laterales, ninguno abajo). Es el caso donde la diferencia se ve.

2. ✅ **Panel y gizmo en el editor** — `haruka/src/panels/ports_panel.{h,cpp}` + integración en
   `viewport.{h,cpp}` y `editor_app`. Menú *View → Puertos*.
   - **Alta/edición/borrado** de puertos, `Cargar`/`Guardar` a `<proyecto>/assets/data/ports/<asset>.json`
     (la misma carpeta de la que tira el juego), y un botón *← del objeto* que saca el `assetId` del
     `modelPath` del objeto seleccionado.
   - **Gizmo**: con un puerto activo, ImGuizmo manipula **el puerto**, no el objeto — se compone con
     el transform del prop y se descompone de vuelta a local. Al cerrar el panel el gizmo vuelve al
     objeto, para no dejar un puerto invisible capturando el ratón.
   - **Superposición**: el **+Y local** (la normal de un socket, y el error más fácil al autorizar),
     el **eje** del raíl y el **barrido** entre sus límites — arco para bisagra, segmento para
     deslizadera — con el **0 marcado** aparte. Dibujado con la draw list de ImGui proyectando a
     pantalla: no hace falta un renderer de líneas para responder a la única pregunta que importa
     aquí, que es si el eje y el cero están bien puestos.
   - **Avisos, no bloqueos** (se puede guardar a medias, pero se dice en voz alta): id vacío o
     repetido, socket sin `kind`, eje del raíl a cero, límites iguales, motor con fuerza 0, y
     **límites que no contienen el 0** — como el 0 es la postura de montaje, esa pieza nacería fuera
     de su propio recorrido. Es exactamente el bug que costó tiempo en el puente a Jolt.

**PENDIENTE:**

5. ✅ **Panel en el mundo anclado a puerto** (la mitad que LEE) — `Survival/scripts/vehicles/joint_health.{h,cpp}`
   (el modelo de `VEHICULOS.md` §5-§6) y `Survival/scripts/ui/world_panel.{h,cpp}` (anclaje + dibujo).
   Se hizo primero el que lee porque **leer es local y escribir es autoridad de servidor**.
   - **Carga y fatiga son dos medidas, no una barra**: `L = F/(F_rot·(1−D)·k_T)` sube y baja con lo
     que haces; `D` solo sube. Un crawler parado con carga 0 y fatiga 0,9 parece perfecto y se parte
     en el primer bache — momento que con una sola barra de vida no existe.
   - **Agregar por MÁXIMO y recuento, jamás por media** (§6). En el test, nueve uniones al 0,05 y una
     al 0,95 dan media 0,14: la media diría que todo va bien.
   - **El instrumento dañado miente hacia el lado benévolo**, no enseña ruido: una aguja que salta al
     azar se descarta de un vistazo y no engaña a nadie; la que hace daño es la que dice "vas bien"
     con la cubierta al 90 %.
   - El anclaje descarta lo que está detrás de la cámara, fuera del viewport o **demasiado lejos para
     leerse** — sin eso, una flota llenaría la pantalla de manómetros.

**PENDIENTE:**

5-bis. **La mitad que ESCRIBE** del panel (palancas, válvulas): cambia el mundo, así que va por el DGS.
*(El 6 original ya está hecho — ver arriba.)*

### Orden original

1. **`Port` + serialización en el motor.** Pequeño y desbloquea todo lo demás.
2. **Panel y gizmo en el editor** — colocar, orientar, clasificar.
3. **`Rail` + puente a Jolt** (hinge/slider/motor/límites) y barrido en el editor.
4. **Evaluador de predicados en el proyecto** — el §9 de VEHICULOS, que ya está en datos.
5. **Panel en el mundo anclado a puerto** — la instrumentación de §6.
6. Migrar el snap de construcción de AABB a puertos autorizados (compatibilidad: si un prop no trae
   `PortSet`, seguir derivando del AABB como hasta ahora).

## §10 — Abierto

- ¿La talla (`size`) es un entero, o basta con `kind` + los predicados del proyecto? SC usa entero;
  aquí puede sobrar, porque `mountStrengthMPa` ya cubre el eje "aguanta o no".
- Encadenar raíles (torreta = yaw + pitch): ¿jerarquía de puertos o dos raíles independientes?
- Autoría en DCC: exportar puertos desde Blender/Maya como *empties*/locators con convención de
  nombre, además de editarlos en el editor propio.
