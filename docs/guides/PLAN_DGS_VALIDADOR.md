# PLAN — Validador + globalización de reglas + request de zona + métricas del orquestador

> Estado: DISEÑO v3 (renombramiento `anticheat`→`validador`). DGS real en `~/Documents/GitHub/dgs/`
> (microservicios epoll + k8s + MongoDB). Este repo contiene SOLO el SDK cliente (headers +
> `external/dgs/sdk/libdgs_client.a`), así que aquí se entrega el PLAN y el PROTOCOLO; los `#HECHO`
> requieren aplicar los diffs marcados en el repositorio DGS real.

## 0. Contexto

`docs/guides/PLAN_DGS_ANTICHEAT.md` describe el diseño v2. Esta revisión (v3) aborda 5 frentes pedidos:

1. **Sistema DGS robusto + tests robustos de mediciones.**
2. **Plan de globalización/generalización en código** sobre *objetos de escena* y *validación de físicas*.
3. **Renombrar `anticheat` → `validador`.**
4. **Implementar un request en `zone_node`** para el anticheat/validador (que NO dependa SOLO del validador),
   enviado al **server master** (`head_server`).
5. **Nueva medición para el orquestador**: ancho de banda de red usado + nº de transferencias fallidas
   (además de `ramUsage`/`performance` actuales).
6. **Herramienta de mapa del universo (viewer)** — ver/dónde está cada zona y cada objeto, gestionar
   zonas (crear/partir/fundir/destruir) y visualizar el planeta en 3D/2.5D/2D (§10).
7. **Selección de recursos: múltiples CPUs/GPUs** por nodo/zona — inventario + solicitud + fit en el
   orquestador, backends y viewer (§3.10).

El SDK local ya da la base genérica:
- `include/dgs/game_module.h` — ABI estable v4, POR ZONA (`ZoneHandle`, `createZone/destroyZone`,
  `validateMove/validateAction`, `serializeRegion/mergeRegion/dropRegion`, `setPieceCatalog`). Nombrado YA
  como "módulo de reglas" (no "anticheat"), listo para la globalización.
- `include/dgs/types.h` — `ServerMetrics` (con `ZoneInfo node` + `ramUsage`/`performance`),
  `EntityTransfer`, `PacketType` (`PKT_METRICS`, `PKT_ENTITY_TRANSFER`, `PKT_GHOST_DELTA`, ...).
- `include/dgs/orchestrator.h` — `Orchestrator` con `evaluateServer()` que escala solo por
  `ramUsage > .80f && performance < .36f`.

> ⚠️ RESTRICCIÓN DEL WIRE FORMAT: `Packet::pack(ServerMetrics)` / `unpackServerMetrics()` están definidos en
> el .cpp del DGS real (compilados en `libdgs_client.a`), NO en este árbol. Añadir campos a `ServerMetrics`
> (bandwidth/failedTransfers) DESINCROniza el layout binario con la lib precompilada a menos que el .cpp
> `pack/unpack` se recompile a la vez (diffs en el repo DGS real, §4e).

---

## 1. Renombrado `anticheat` → `validador` (§3 del pedido)

Semántica: el nodo no "caza trampas"; **valida plausibilidad** y delega las reglas al módulo por proyecto.
Renombre el identificador para reflejar responsabilidad real y para que el sistema quede generalizable.

### 1.1 Mapa de reemplazo (aplicar en `~/Documents/GitHub/dgs/`)

| Hoy (anticheat) | Nuevo (validador) |
|---|---|
| microservicio `anticheat_node` | `validador_node` |
| binario / Deployment k8s `anticheat-node` | `validador-node` |
| `static bool validate()` | `static bool validateMove()` + host del módulo (`GameModuleHost`) |
| `lastKnown` en anticheat | `lastKnown` en validador (mismo struct) |
| puertos UDP+TCP 42427/42428 | mantiene los mismos en esta revisión (si cambian, actualizar firewall/k8s) |
| `PKT_*` de paquete si existiera `PKT_ANTICHEAT` | `PKT_VALIDATE` / `PKT_VALIDATE_REQ` (ver §3) |
| comentarios/`cout "CHEAT detectado"` | `"VIOLATION detectada (validador)"` + `Violation{type,peso}` |
| variable de entorno `GAME_MODULE_SO` (ya existe) | se mantiene (carga el módulo, no el anticheat) |

Reglas de ejecución:
- **Identificadores de Código** (clases, funciones, variables, `#ifndef`) y **servicios k8s** → renombrar
  (buscar `anticheat`, `anticheat_node`, `anticheat-node`, `ANTICHEAT_`).
- **Nombres de paquete en el wire** que ya estén en producción → NO renombrar el byte del `PacketType`, salvo
  coordinar versión de protocolo (de lo contrario clientes/servidores viejos leen mal). Poner el alias de
  tipo (enum) pero mantener el valor numérico.
- **Docs/scripts/deploy** → renombrar junto con el código en el mismo commit (atómico).

---

## 2. Request de `zone_node` → validador (que NO dependa SOLO del validador) + master (§4 del pedido)

Hoy la cadena es: cliente → `zone_node` → reenvía `EntityTransfer` → `anticheat_node` revalida. Sigue
habiendo **una sola fuente de decisión** (el validador). Si el validador cae o tarda, `zone_node` queda ciego.
La robustez pedida = **autoridad redundante y comprobable**: el `zone_node` debe poder
(self-check + consultar al master) sin depender en exclusiva del validador.

Diseño: **request–ack activo del zona→validador + heartbeat al master**.

### 2.1 Flujo

> **DECISIÓN (2026-08-05): la física es ZONE-AUTHORITATIVE y distribuida.** Cada `zone_node` es dueño de la
> simulación de sus entidades (ejecuta `module->step` en tick fijo, C4 del plan v2). El validador NO simula:
> **valida** la afirmación del cliente contra el estado predicho por la zona dueña. Esta sección mantiene el
> request/ACK (sigue siendo necesario para el gate de acciones destructivas y el chequeo de plausibilidad),
> pero el validador ya no es la fuente de la física.

```
cliente ──inputs/intents──► zone_node  (simula SUS entidades con module->step, tick fijo)   [autoridad física]
                            │  ──predicción autoritativa + GhostDelta──► clientes + vecinos (solape de frontera)
                            ├── PKT_VALIDATE_REQ ──► validador  (afirmación del cliente vs estado predicho por la zona) [S2]
                            │         (id, zone, entidad/op, estado predicho)
                            │   ◄── PKT_VALIDATE_ACK  (id, veredicto 1/0 + peso)                                     [S3]
                            ├── pre-chequeo local GENÉRICO S1 (si el validador no responde → fail-open/fail-closed)
                            ├── PKT_VALIDATOR_STATUS (heartbeat + métricas) ──► head_server                           [S4]
                            └── PKT_REASSIGN / PKT_DROP_REGION (traspaso de AUTORIDAD en frontera / si el master lo ordena)
```

- **S1 (defensa en profundidad):** el `zone_node` aplica la **misma** plausibilidad genérica que antes
  (`dist ≤ maxSpeed·dt + margen`). Si el validador no responde a `PKT_VALIDATE_REQ` dentro del timeout,
  `zone_node` decide con su pre-checke (degradación controlada), **no** ciego.
- **S2/S3:** nuevo par de packets `PKT_VALIDATE_REQ`/`PKT_VALIDATE_ACK` con un `uint32_t requestId` para
  correlación y timeout (antiguos acks de otra request no cuentan). El REQ incluye el **estado predicho por
  la zona**; el validador lo comprueba con `module->validateMove` (plausibilidad + tolerancia por RTT), NO
  re-simula: el estado autoritativo lo produce la zona.
- **S4:** el `zone_node` reporta periódicamente a `head_server` (master) un `PKT_VALIDATOR_STATUS` con:
  estado del validador (UP/DEGRADED/DOWN), cuántas validaciones pidió, cuántas fallaron por timeout, y
  contadores de ancho de banda (§4). El master usa esto para **decidir escalar/reasignar** y para **elegir
  validador** cuando haya varios. Así ya no es solo "depender del validador": es **supervisión desde el master**.

### 2.2 Especificación de packets (aplicar al `PacketType` + `pack/unpack` del DGS real)

| PacketType | dir | payload | sentido |
|---|---|---|---|
| `PKT_VALIDATE_REQ` | zone_node → validador | `uint32_t requestId, uint64_t entityUuid, uint32_t ownerZone, bytes opacos(estado predicho por la zona + afirmación del cliente)` | pedir veredicto contra el estado predicho |
| `PKT_VALIDATE_ACK` | validador → zone_node | `uint32_t requestId, int8 verdict(1/0), uint16 weight` | veredicto correlacionado |
| `PKT_VALIDATOR_STATUS` | zone_node → head_server | `int8 state, uint32 reqSent, uint32 reqTimeout, uint64 bytesRecv, uint32 failedTransfers` | telemetría al master |

```cpp
// Añadir a Packet (packet.h/.cpp) — adaptar al estilo existente
struct ValidateRequest {
    uint32_t requestId;     // seq por remitente (anti-replay, §2.3)
    uint64_t entityUuid;    // entidad/op a validar
    uint32_t ownerZone;     // zona dueña de la simulación (la que predice)
    uint8_t  moduleId;      // módulo de reglas esperado (env GAME_MODULE_SO)
    uint8_t  kind;          // 0=move, 1=action (verbos críticos → fail-closed)
};
// payload opaco: tras el header, el estado PREDICHO por la zona + la afirmación del cliente
// (el validador los compara con validateMove/validateAction y tolerancia por RTT; NO re-simula)
struct ValidateAck     { uint32_t requestId; int8_t  verdict; uint16_t weight; };
struct ValidatorStatus { int8_t state; uint32_t reqSent; uint32_t reqTimeout;
                         uint64_t bytesRecv; uint32_t failedTransfers; };
```

### 2.3 Robustez añadida

- **Timeout + retry** en `zone_node` por `requestId` (no bloquear el tick: asíncrono, como ya es el modelo).
  Retry con **backoff** (`50ms · 2^n`, máx 3) y **dedupe por requestId** (un ACK tardío de un retry anterior
  no cuenta doble; el buffer de pending se vacía al vencer el TTL).
- **Circuit breaker del validador** (estados CLOSED→OPEN→HALF_OPEN): si la tasa de timeouts supera un umbral
  en una ventana, `zone_node` deja de mandar `PKT_VALIDATE_REQ` y decide SOLO con S1 (fail-open) durante el
  cooldown; reintenta con `PKT_VALIDATOR_STATUS` de sonda. Así el fallo del validador degrada, no bloquea.
- **Política fail-open / fail-closed por verbo**: movimientos/ediciones NO destructivas → **fail-open** (S1
  decide); acciones destructivas (destroy/place) y de economía (transfer) → **fail-closed** (rechazar) si no
  hay veredicto. Un validador caído no debe convertir el mundo en un sandbox para volar por el mapa.
- **Idempotencia + anti-replay**: `requestId` se deriva de un **sequence number por remitente** y el ACK lleva
  el mismo; el validador mantiene una ventana deslizante de requestIds ya vistos (mapa circular) para
  descartar ACKs duplicados y paquetes reinyectados. Un actor que reinyecta el MISMO `EntityTransfer` con el
  mismo seq → descartado (además de la sospecha de replay).
- **Colas acotadas + backpressure**: la cola de validaciones pendientes tiene tamaño máx (p.ej. 4096). Si se
  llena, se descartan los nuevos con violación de "flood" (no se crece sin límite ni se bloquea el tick).
- **Tamaño máx de mensaje**: cap en `receive` (p.ej. 64 KB) y validar `dataSize ≤ 4096` ANTES de usar
  `data[]` (un cliente hostil puede mentir en `dataSize`). Nunca copiar `EntityTransfer.data[4096]` completo
  si solo se manda `dataSize` (O1).
- **Auth entre nodos**: token compartido (env `NODE_TOKEN`) en un header de los PKT_VALIDATE_*/STATUS; un
  cliente no puede fabricar `PKT_VALIDATE_ACK` ni espoofear `PKT_VALIDATOR_STATUS`. (Añadir 4 bytes HMAC/CRC
  si no hay TLS entre pods.)
- **Autoridad redundante**: decisión = veredicto del validador **si llega**; si no, pre-checke local S1.
  La sospecha se reporta por `PKT_VALIDATOR_STATUS` para que el master decida ban/reasignación.
- **Supervisión del master**: el `head_server` alimenta `evaluateServer` con `ValidatorStatus` (no solo
  ram/performance) → el orquestador puede escalar también por sobrecarga del validador o por
  `failedTransfers` (§4).
- **Sin "solo el validador"**: la fuente última de verdad de planes pasa por el master (que conoce topología
  y métricas), mientras el validador queda como **validador** de la física (plausibilidad + gate de verbos
  críticos) y la **simulación corre en la zona dueña** (§3.6).
- **Heartbeat con lease**: la entrada del validador en el master caduca si no llega `PKT_VALIDATOR_STATUS`
  (p.ej. 2× intervalo). El master lo marca DOWN y reasigna su zona vía `PKT_REASSIGN`. Sin lease, una zona
  huérfana queda sin servir para siempre.

---

## 3. Globalización / generalización en código (objetos de escena + validación de físicas) (§2 del pedido)

El objetivo: **todo lo específico del juego fuera del DGS**. El DGS opera sobre **objetos de escena
genéricos** y **verbos de física genéricos**; el módulo por proyecto aporta la semántica. Ya existe el ABI v4
en `game_module.h` (por zona, con traspaso). Esta sección concreta qué se "globaliza".

### 3.1 Objetos de escena genéricos

Hoy el player es una entidad nativa (`EntityTransfer`). Los props/scene objects son del juego. Globalizar:

- **Contrato genérico de objeto de escena** (ya cubierto por `EntityState` bitmask + `data[4096]` opaco +
  `EntityTransfer`). Definir en el DGS, por contrato, que un scene object ES un `EntityTransfer` con
  `EntityState`+`data[]`, sin conocer su contenido. (Sigue igual; se documenta como binding permanente.)
- **`updateNodeTopology`** no conoce "zona" de juego; trabaja sobre `ZoneInfo` con `chunk*` bounds → **ya es
  genérico** (chunks del mundo, no tipos). Mantener.
- **Serialización de escena** = `serializeRegion/mergeRegion/dropRegion` por `ZoneHandle` (v4) → mover un
  trozo de escena entre nodos sin conocer sus contenidos: **la globalización ya está resuelta** por la zona.
  Nuevo juego = nueva semántica del módulo, DGS intacto.

### 3.2 Física y validación genéricas (dos trabajos, dos nodos)

> **Simular ≠ Validar.** La física se SIMULA en la zona dueña; se VALIDA en el validador. Esto es la decisión
> de autoridad de §3.6.

- **Simulación (en `zone_node`)**: la zona ejecuta `module->step` en tick fijo sobre **sus** entidades
  (C4 del plan v2). El estado resultante es el **autoritativo**; se difunde como `GhostDelta` y se predice.
- **Validación (en el validador)** — `validateMove(ZoneHandle, MoveSample, WorldQuery)`: NO reimplementa
  física; **comprueba** la afirmación del cliente contra el estado predicho por la zona, con tolerancia por
  RTT. `WorldQuery` presta `planetCenter/planetRadius/seed/reliefStrength/profile` para que el módulo
  reconstruya el terreno analítico (mismo sampler de CPU en cliente y server) y valide colisión/suelo/vuelo.
  → La *validación de físicas* queda **parametrizada por el mundo**, no por reglas hardcodeadas.
- **`validateAction`** con `ActionHeader`+`PlaceAction`**: mover/interactuar/colocar es un verbo genérico;
  el geometría de `ACT_PLACE` se valida en el engine (código compartido con la predicción del cliente).
- **Catálogo de piezas** `setPieceCatalog` viaja desde el host al módulo → el validador conoce el tamaño
  real de cada pieza sin tener reglas de juego.
- **Un solo binario de reglas** compilado estático (cliente) y `.so` (zona y validador) → mismas reglas: la
  "generalización" es estructural (un módulo, varias cargas).

### 3.3 Dónde queda el fallback genérico

Si no hay módulo (`GAME_MODULE_SO` vacío o `abiVersion` inválido), el validador usa el pre-checke S1 de
plausibilidad (posición/velocidad) — el "validador genérico". Ese fallback es **código del DGS**, pequeño y
sin semántica de juego, y es lo que permite que juegos sin módulo sigan funcionando.

### 3.4 Resumen de generalización (lo que NO depende del juego)

| Pieza | Genérico en el DGS | Semántica por proyecto (módulo) |
|---|---|---|
| objeto de escena | `EntityTransfer` + `EntityState` + `data[4096]` | decodificación interna |
| física (SIM en zona) | `module->step` en la zona dueña, `WorldQuery` (terreno analítico) | integrador + colisión |
| física (VALIDACIÓN) | `validateMove` contra estado predicho por la zona | plausibilidad + tolerancia RTT |
| mover/interactuar | `ActionHeader`+verbos | payload + reglas del juego |
| colocar | `PlaceAction` + `setPieceCatalog` (geometría engine) | tipos/catálogo |
| traspaso de escena | `serializeRegion`/`mergeRegion`/`dropRegion` + ownership con lease | formato de blob |
| feedback | `Violation`/`SuspicionScore` (peso+decaimiento) | pesos por violación |

### 3.5 Robustez de la globalización (reglas críticas para que el modelo no diverja)

- **Determinismo en los dos lados**: el módulo se compila estático (cliente) y `.so` (validador); el MUNDO
  (seed + `WorldGenParams` derivados del `seed`, reloj de sim) debe producir el **mismo terreno analítico**
  en ambos. Regla de oro: el módulo **no usa `rand()` ni reloj de pared ni floats no-deterministas** — toda
  aleatoriedad va por el seed y por el reloj de mundo autoritativo (§4bis del plan v2). Tests de determinismo
  (mismo seed → mismos outputs, §4.3) para que una optimización de un lado no rompa la validación del otro.
- **Alcance del módulo = zona**: todo estado colgando de `ZoneHandle`; NUNCA globals del `.so` (si un nodo
  sirviera dos zonas las mezclaría, y el estado moriría en el `dlclose`). El `dlopen` debe hacer `dlclose`
  del `.so` viejo al recargar reglas en caliente (hot-reload), y el `ZoneHandle` se serializa/mergea (v4)
  para no perder estado.
- **Contención de crashes del módulo**: un segfault dentro del `.so` mataría el nodo entero. Mitigaciones (en
  orden de esfuerzo): (a) correr la validación en un hilo del worker pool con **signal guard** (SIGSEGV en un
  hilo alterno → descartar y marcar el módulo como sospechoso); (b) opcional/avanzado: validador en un
  **subproceso** (`fork`) cuyo crash solo reinicia la zona, no el nodo.
- **GC del estado de validación**: el `lastKnown` por uuid debe tener **TTL/eviction** (entidad inactiva
  > N segundos → se purga). Sin TTL, un mapa que acumula uuids durante una sesión larga es una fuga de
  memoria silenciosa y el `dist` contra un punto podrido puede dar falsos positivos.
- **Sanidad de entrada en el módulo**: el host debe validar que el `.so` exporta `dgs_game_module_v1`,
  `abiVersion == GAME_MODULE_ABI`, y que ningún puntero de la vtable está nulo donde el host lo necesita
  (o usar el fallback S1 para ese slot). Un `.so` corrupto → no cargar, no crashear.

### 3.6 Autoridad de la física: POR ZONA (no centralizar en el validador)

**Decisión cerrada (2026-08-05):** la simulación física es **distribuida por zona**; el validador NO es el
motor de física. Razones: la física paraleliza por entidades (que se reparten por zona) → un solo validador
sería cuello de botella serial + SPOF + ancho de banda (habría que mandarle el estado de cada entidad cada
tick, el problema O1). El ABI ya es por zona (`ZoneHandle`/`createZone`/`serializeRegion`), y el estado del
módulo vive en la zona → simular localmente es barato y con localidad de datos.

**Regla de autoridad (el punto difícil):**
- **Una entidad la simula EXACTAMENTE un `zone_node` a la vez.** El nodo dueño es el que cubre su chunk
  (según `ZoneInfo`/`findTargetNode`); es quien ejecuta `module->step`, difunde `GhostDelta` y responde
  `validateMove` a los clientes.
- **Frontera de zona (handoff):** cuando una entidad cruza a una zona vecina, la propiedad se traspasa con
  `serializeRegion/mergeRegion` + `PKT_REASSIGN` (los `GhostDelta` cubren el solape para que no haya hueco
  visual). Durante el solape los dos nodos la conocen, pero **solo el dueño la simula** (marcador de
  ownership con lease) → sin divergencia por doble autoridad.
  > **Implementado en el repo real (2026-08-05):** la zona destino **promueve** el `GhostDelta` a
  > `EntityTransfer` real al recibir `PKT_ENTITY_TRANSFER` (borra su ghost → no duplica en `broadcast`), ignora
  > ghosts de uuids que ya posee como reales, y purga ghosts por TTL (`GHOST_TTL_MS`, default 3000) para no
  > proyectar entidades sin dueño. **P4: ownership con lease completo** — `zone_node` marca las entidades que
  > cubre (`ENTITY_LEASE_MS`, default 3000), cede la autoridad explícitamente con `PKT_REASSIGN` al cruzar la
  > frontera (el head la enruta por chunk a la nueva dueña, que renueva el lease), y hace **GC de entidades/
  > `lastPosition` inactivas** (el lease expirado → purga de `entities`/`lastSnapshot`/ownership). El ABI
  > `game_module.h` del repo real se igualó a **v4** del SDK (`ZoneHandle` + `createZone`/`serializeRegion`/
  > `mergeRegion`/`dropRegion`) y el validador crea su zona y pasa `g_zone` a `validateMove`.
- **Interacciones cross-zona** (p.ej. proyectil disparado en A que impacta en B): las resuelve **el nodo
  dueño del proyectil**; el objetivo solo recibe el delta de daño (`PKT_GHOST_DELTA`/`validateAction`). Nunca
  dos nodos simulan el mismo cambio de estado.
- **El validador interviene** en: (a) plausibilidad de lo que afirma el cliente vs el estado predicho por la
  zona dueña (S2/S3); (b) gate fail-closed de verbos destructivos; (c) fallback S1 si la zona dueña no
  responde. NO es el sim.

**Resumen:** física por zona (escala con las zonas), validador como peer de validación barato, y el trabajo
de diseño se concentra en la frontera/handoff de autoridad (ownership con lease), no en centralizar la física.

### 3.7 Plano social/cuenta: guilds, parties, chat y gestión (fuera de las entidades)

**Qué YA estaba planteado** (PLAN_DGS_ANTICHEAT.md, no inventar): (a) `PKT_CHAT` existe (C3: hoy el DGS solo
tiene "transform/chat/transfer"); (b) **el ban es a nivel cuenta vía HeadServer/persistence (MongoDB)**
(§4 + C5: `SuspicionScore` → reporte a head/persistence → ban de cuenta). No había diseño para guilds,
parties, amigos, moderación ni economía. Esto completa y ordena eso.

**Regla (igual que §3.6, aplicada al dato): la autoridad sigue al TIPO de dato, no a la ubicación.**

| Plano | Datos | Autoridad | Dónde | Frecuencia |
|---|---|---|---|---|
| **Espacial** | posición, física, props, chunk | zona dueña (§3.6) | `zone_node` | tick |
| **Cuenta/social** | guilds, parties, amigos, banes, settings, economía de gremio | **nodo social/account** (NO espacial) | `head_server` + `persistance_node` (MongoDB) + `cache_node` (read-through) | baja, consistente |
| **Canal** | chat (local/guild/trade/global), notificaciones | **servicio de chat** (fan-out + rate-limit) | `head_server` o dedicado; el canal `local` sí pasa por `zone_node` (interés) | alta, efímera |

**Gestión por plano:**

- **Cuenta/social** (guilds, parties): keyed por `playerUuid`, NUNCA por chunk — un gremio abarca zonas, la zona
  solo sabe quién está, no la membresía. Estado pequeño + **deltas por evento** (join/leave/rango/disband) a
  los miembros ONLINE (la UI se suscribe, no consulta). `persistance_node` = fuente de verdad (write-through
  en economía de gremio, async en lo cosmético); `cache_node` = cache de lectura. Reutiliza el camino de ban
  de cuenta YA planteado (head + persistence).
- **Economía (banco de gremio, loot de party, trade) → fail-closed**: entra por el MISMO `validateAction`/
  `ACT_TRANSFER` (game_module.h) ya definido; el validador rechaza sin veredicto. La economía jamás es
  cliente-autoritativa (mismo principio que destroy/place en §2.3).
- **Canal (chat)**: routing por canal y suscripción por uuid, no por proximidad. Rate-limit + moderación en el
  **servicio de chat**, antes del fan-out (es anti-spam/anti-abuse, NO física → no pasa por el validador).
  Orden por canal con seq (fan-out tipo `GhostDelta` pero de canal, no de chunk).
- **Gestiones (banes, permisos, friend-list, ajustes)**: cuenta-level, persistidos y cacheados; los consulta
  `head_server` (login/routing) y el nodo social. **La zona nunca los decide**: aplica lo que le dicen (p.ej.
  "uuid baneado" → bloquea entrada). El ban escalado de la sospecha se materializa aquí (validador → head →
  nodo social → todas las zonas lo ven).

**Packets nuevos (al DGS real)**: `PKT_SOCIAL_DELTA` (guild/party: miembro, rango, disband, id de zona por
miembro), `PKT_CHAT` ya existe (ampliar payload con canal/seq/rate-limit), `PKT_ACCOUNT` (ban/permisos).
Economía sigue yendo por `PKT_ACTION`+`ACT_TRANSFER` (no payload social).

**Reglas transversales (anti-repetición de los errores del plano espacial):**
1. **Un solo dueño por tipo de dato** (igual que "una entidad = un dueño" §3.6): el gremio lo escribe UN nodo
   social; las zonas solo leen referencias (ids).
2. **El cliente no calcula nada social**; todo se valida/replica en server.
3. **La UI es una vista suscrita**: se liga a eventos del nodo social por ids, no a objetos; al cambiar de
   zona no se reconstruye por `GhostDelta`.
4. **No mezclar planos en `EntityTransfer`/`GhostDelta`**: la carga social va por `PKT_SOCIAL_DELTA`, no en
   `data[4096]` de una entidad (inflaría el ancho de banda, O1).

### 3.8 Despliegue: instalador + modos de ejecución + Terraform opcional (cloud / multi-servicio)

Hoy el DGS solo corre en k8s (`orchestrator.h` `spawnZoneNode` llama directo a la API k8s, líneas 180-258). Se
añade un **instalador/deployer parte del programa** para: correr en el **mismo nodo** o **libre sin instalar**
(portable), **o instalar** (systemd), **o subir a nube multi-servicio** con **Terraform opcional**.

**Método clave: abstraer el backend de spawn del Orquestador** (hoy el spawn k8s está hardcodeado en
`orchestrator.h`): `enum class SpawnBackend { LOCAL, K8S, TERRAFORM }`. El resto de la lógica (`evaluateServer`,
topología, métricas) no cambia según el modo.

| Modo | Qué es | Instalación | Backend de spawn | Uso |
|---|---|---|---|---|
| `standalone` (portable / libre) | un solo binario `dgs run` levanta head+zone+validador+cache+persistence en UN nodo (MongoDB embebido o externo por config) | **NO** — corre desde su carpeta/tarball, no toca el sistema | `LOCAL` (procesos/hilos) | dev, demo, servidor de 1 nodo |
| `instalado` | binarios a `/opt/dgs` + unidades systemd + config/env | `dgs install [--systemd]` / `dgs uninstall` | `LOCAL` (o `K8S` si el nodo es worker) | producción single-node |
| `cluster` (nube) | k8s multi-zona + **multi-servicio** (todos los microservicios + MongoDB + red) | `dgs up [--terraform]` (o `kubectl apply`) | `K8S` + (`TERRAFORM` para provisionar infra) | producción multi-nodo |

**Instalador (`dgs deploy`)** — es OPCIONAL y el mismo binario decide:
- `dgs run` — portable: arranca standalone desde donde esté, **sin instalar** (la opción "libre").
- `dgs install [--systemd]` / `dgs uninstall` — la opción "instalada"; pregunta/flag `--portable|--install`.
- `dgs up [--terraform]` / `dgs status` / `dgs logs` — cluster; con `--terraform` provisiona la infra antes de
  aplicar k8s. `status`/`logs` funcionan igual en los 3 modos.
- Regla: **el comportamiento del DGS no depende del modo** (mismas reglas, mismo validador, misma topología);
  el modo solo cambia el backend de spawn y la infra. Así un bug solo visible en standalone se detecta igual
  que en cluster.

**Terraform como MÓDULO OPCIONAL** (`terraform/`, solo necesario en `cluster`):
- Módulos HCL: `network` (VPC/subredes/security-groups), `k8s` (EKS/GKE/AKS o k3s en VM), `mongodb` (managed o
  StatefulSet in-cluster), `zone_node` (deployment + Service NodePort UDP/TCP), `validador`, `social`.
- **Multi-servicio**: un solo `apply` provisiona TODO el DGS (head, zone, validador, cache, persistence,
  social) + dependencias (MongoDB, cache/social store); los **outputs** alimentan al orquestador k8s
  (`zone-node` image, `MY_NODE_IP` para el NodePort UDP, direcciones de servicios).
- Multi-cloud: providers AWS/Azure/GCP con variables; credenciales por env (`TF_VAR_*`), igual que `NODE_TOKEN`.
- Opcional de verdad: `standalone`/`instalado` no lo tocan; `dgs up --terraform` solo si se pide.
- Tests de paridad entre backends (spawn `LOCAL` vs `K8S` dev) para que el modo no cambie el comportamiento.

### 3.9 Ciclo de vida de zonas/nodos: creación, división, fusión y destrucción

**Estado actual en el código:** solo está la **creación** y la **división** — `evaluateServer` (orchestrator.h)
divide una zona sobrecargada por la mitad en X, llama a `spawnZoneNode` + `sendResizeCommand` (§4.2). **NO hay
fusión (escalar down) ni destrucción ordenada**: `currentReplicas` solo crece (orchestrator.h:245) y la evicción
por lease (bug 1 §4.6) solo borra la anotación de `activeZones`, no el pod real → **fuga de réplicas/pods zombie**.

**Estados de vida** (que el orquestador debe conocer): `PROVISIONING → READY → DRAINING → DESTROYED`, más `DEAD`
(lease vencido, no planificado).

| Operación | Desencadenante | Qué hace | Paquetes/reutiliza |
|---|---|---|---|
| **Creación** | arranque o split (división) | `spawnZoneNode` para un rango de chunks; al llegar su primer `ServerMetrics` → `READY` y registro en `activeZones` | CMD_CREAT_SERVER, `updateNodeTopology` |
| **División** (escalado UP) | umbral alto (load/bandwidth/failedTransfers, §4.2) | parte la zona por su eje más ancho en la mediana; spawnea la mitad alta; la vieja conserva la mitad baja; **handoff de la columna de corte** con `serializeRegion/mergeRegion` para que ningún chunk quede en dos zonas | `spawnZoneNode`, `sendResizeCommand`, `serializeRegion/mergeRegion` |
| **Fusión** (escalado DOWN) ⚠️ NUEVA | dos zonas vecinas consistentemente bajo umbral (ventana + histéresis) | drena la más pequeña, serializa su región, la mergea en la vecina, borra su pod, `currentReplicas--`, actualiza topología | `PKT_DRAIN`+ack, `mergeRegion`, `PKT_DELETE_ZONE` |
| **Destrucción ordenada** (scale-down / shutdown) | escala down, fin de servicio, `dgs down` | `DRAINING`: deja de aceptar, `serializeRegion` hacia el receptor, espera ack del merge, **borra el Deployment/Service**, → `DESTROYED` | `PKT_DRAIN`, `PKT_DELETE_ZONE` |
| **Muerte no planificada** (crash) | lease vencido (F3) | master reasigna la región a un vecino **Y** borra el deployment zombie para no fugar réplicas; quita la entrada de `activeZones` | lease (§2.3), `PKT_REASSIGN`, `PKT_DELETE_ZONE` |
| **Evicción** (bug 1) | lease vencido | SOLO la anotación en `activeZones` (complementa a "muerte no planificada", que sí borra el pod) | — |

**Reglas anti-flappy (no alternar split/merge sin parar):**
- **Histéresis**: umbral alto de split ≠ umbral bajo de merge (espacio entre ambos); un nodo solo mergea tras
  estar **consistentemente** bajo durante una ventana (p.ej. 2 min), nunca tras un solo `ServerMetrics`.
- **Timer de asentamiento** tras un split antes de permitir un merge (y viceversa).
- En el **mismo tick** nunca se divide y se fusiona la misma zona; las operaciones van a una **cola de vida**
  con prioridad (crash > merge > split) y se procesan una a la vez por zona.
- **Drain con timeout**: si al drenar no llega el ack del merge, abortar la destrucción y volver a `READY`
  (fail-safe), no dejar la región vacía.

Todo esto extiende el backend de spawn (§3.8): cada `SpawnBackend` implementa `create/destroy/resize` con la
misma semántica de ciclo de vida, de modo que standalone y cluster se comporten igual (F14).

### 3.10 Selección de recursos: múltiples CPUs/GPUs por nodo/zona (NUEVO)

> **Añadido (2026-08-05):** opción de elegir **cuántos recursos** usa cada pieza del sistema. Hoy cada nodo es
> un proceso de un solo hilo de red (loop epoll) y el orquestador escala por Nº de réplicas, no por "forma" de
> recursos. Se añade el **inventario de recursos por nodo** (CPUs, GPUs, RAM) y la **solicitud por zona**, para
> que el orquestador (y el viewer) pueda **seleccionar dónde colocar** cada carga y **limitar** lo que usa.

- **Inventario (nodo → orquestador).** Cada nodo reporta en `ServerMetrics` (§4) su presupuesto y su uso:
  `cpuCores`, `cpuUtil` (0..1), `gpuCount`, `gpuUtil` (0..1), `gpuMemMB`, `ramMB`. El orquestador construye con
  eso la **tabla de capacidad** de la flota (qué nodo tiene qué libre) — igual que hoy hace con `ramUsage`.
- **Solicitud (orquestador → nodo).** Al spawnear (crear/partir/fundir, §3.9), el master incluye en el
  `Command` inicial la **solicitud de la zona**: `ResourceSpec{cpuCores, gpuCount, gpuMemMB, ramMB, threads}`.
  El nodo la aplica: enlaza sus hilos de simulación (§3.6) al nº de CPUs pedido y reserva la GPU declarada.
  Si la solicitud excede lo disponible → el nodo responde `REJECT` y la zona queda `PROVISIONING`/en cola, no
  "corre a medias" (oversubscription controlada, F17).
- **Política de selección (orquestador).** Al asignar o reubicar una zona, el planificador elige con
  **fit por recursos libres** (best-fit; tie-break por carga/bandwidth, §4.2) sobre la tabla de capacidad.
  `evaluateServer` puede usar `cpuUtil`/`gpuUtil` además de `ramUsage`/`performance` para pedir split.
- **Backends.** Cada `SpawnBackend` (§3.8) traduce la solicitud a su mundo:
  `LOCAL` → `cpuset`/`taskset` + nº de hilos del proceso; `K8S` → `resources.requests/limits` + device plugin
  de GPU (nodo que las tenga); `TERRAFORM` → tipo de instancia / `TF_VAR_cpu_count`/`TF_VAR_gpu`.
- **En el validador (§1/§2).** Si el módulo por proyecto necesita GPU (p.ej. física/IA), el orquestador coloca
  el validador en un nodo con GPU libre (lo declara el ABI del módulo: `GameModule::needsGpu`/`needsCores`,
  consultable antes de spawn).
- **En el viewer (§10).** La herramienta de mapa tiene la **misma opción**: selector de dispositivo
  GPU/back-end de render y nº de hilos CPU para el modo 3D/2.5D (persistido en `settings/map_viewer.json`).
  Se reutiliza `ResourceSpec`; en la máquina del operador se aplica localmente (no afecta al DGS).
- **Modelo de datos (types.h, wire).** `struct ResourceSpec { uint16_t cpuCores; uint16_t gpuCount;
  uint16_t gpuMemMB; uint16_t ramMB; uint8_t threads; }` — entra en `Command` (solicitud) y los campos de uso
  van en `ServerMetrics` (§4, al final, D-D: recompilar pack/unpack a la vez).

**Decisiones abiertas (cerrar al implementar, en P8):**
- (a) ¿la GPU es **opcional del módulo** (solo si `.so` declara `needsGpu`) o un campo siempre presente?
- (b) ¿`threads` por zona configurables o fijos por nodo (1 hilo de red + N de simulación)?
- (c) ¿hot-plug de GPU (asignar/liberar en runtime) o solo asignación en spawn (§3.9)?

---

## 4. Métricas del orquestador: ancho de banda + transferencias fallidas + más (§5 del pedido)

### 4.1 Qué medir hoy y añadir

Hay que distinguir **qué se mide en el nodo** y **qué se recopila en el orquestador**.

> Layout REAL en `types.h`: `ServerMetrics { ZoneInfo node; float ramUsage; float performance; }`. No existe un
> `ServerNodeMetrics`; las métricas por nodo son `ramUsage`/`performance` a nivel `ServerMetrics` y `node` es
> un `ZoneInfo` (chunks/addr/port). Por eso los campos nuevos se añaden a nivel `ServerMetrics` (junto a
> `ramUsage`/`performance`) — ahí es donde el orquestador los lee y donde encajaría el `pack/unpack`.

| Métrica | dónde se mide | estado actual |
|---|---|---|
| `ramUsage` | nodo (nivel `ServerMetrics`) | ✅ ya y alambrada a `evaluateServer` |
| `performance` | nodo (nivel `ServerMetrics`) | ✅ ya |
| **ancho de banda usado (bytesRx/bytesTx)** | nodo (local `UDPSocket`/`TCPSocket` + `broadcastToClient` `GhostDelta`) | ❌ añadir |
| **transferencias fallidas (`failedTransfers`)** | nodo (timeouts `PKT_VALIDATE_REQ`, resend/traspaso fallidos O1-O5) | ❌ añadir |
| latencia/RTT | nodo (README menciona `latency_compensation`) | ❌ añadir (recomendado) |

Propuesta concreta (extiende `ServerMetrics` en `types.h`):

```cpp
struct ServerMetrics {       // extender (ver ⚠️ wire format §4e)
    ZoneInfo node;           // campos actuales: chunk ranges, addr, port
    float    ramUsage;       // actual
    float    performance;    // actual
    uint64_t bytesRx;        // bytes recibidos desde el arranque
    uint64_t bytesTx;        // bytes enviados desde el arranque
    uint32_t failedTransfers;   // nº de transferencias/validaciones fallidas (timeout o error)
    uint32_t activeEntities;    // entidades servidas (más medición útil para escalado)
};
```

- `bytesRx/Tx`: incrementar en el punto de envío/recepción de cada nodo (envolver `UDPSocket::send/receive`
  y `TCPSocket::send/receive` en un contador o, más simple, sumar `p.getSize()` en `broadcastToClient` y en
  cada `receive`).
- `failedTransfers`: incrementar en `zone_node` cada vez que un `PKT_VALIDATE_REQ` hace timeout o un
  `serializeRegion/mergeRegion`/traspaso falla (O1/O5). Es LA señal de "el validador va mal" → la consume el
  master (§2.3).

**Medición robusta (cómo contar sin mentir):**
- **Contadores monotónicos + deltas**: el nodo reporta **acumulados** desde arranque (no "por segundo"); el
  orquestador calcula la tasa como `Δ/Δt` entre dos `ServerMetrics`. Los acumulados son libres de
  redondeo por ventana y resisten reinicios (comparar con `uptime`/`startTime`).
- **Desbordamiento de `uint64`**: a 10 Gbit/s, `bytesTx` da la vuelta en ~58 años — seguro, pero comparar
  siempre con **diferencia sin signo** (substracción modular), nunca `a > b` directo entre muestras.
- **EWMA para tasas** (ancho de banda, RTT): media exponencial en vez del cociente crudo `bytesTx/bytesRx`
  para no reaccionar a un pico de 1 tick. El cociente asimétrico del ejemplo es solo ilustrativo.
- **Coste de muestreo**: incrementar contadores por paquete es barato (2 `+=`); NO medir con
  `recvmsg`/temporizadores por datagrama bajo carga (O4). Samplear la tasa en el hilo de métricas, no en el
  hot path de red.

### 4.2 Cómo las usa `Orchestrator::evaluateServer`

Hoy: solo `m.ramUsage > .80f && m.performance < .36f`. Añadir señales de red/robustez (con cooldown 30s ya
existente):

```cpp
void evaluateServer(const ServerMetrics& m, int nodeFD)
{
    bool load      = m.ramUsage > .80f && m.performance < .36f;
    bool netSaturated = m.bytesTx > 0 &&
                        (double)m.bytesTx / (double)(m.bytesRx + 1) > 4.0;      // asimetría: manda mucho
    bool failureProne = m.failedTransfers > 40;                                  // validador/traspaso mal

    if (load || netSaturated || failureProne)
    {
        // ...mismo cuerpo de escalado (valida cooldown, parte zona por medio X, spawnZoneNode, resize)...
    }
}
```

Regla de diseño: cada nueva señal escala el **mismo mecanismo** (dividir por X + spawn). No añadir rutas de
escalado paralelas → el "sistema robusto" es un umbral claro y un solo camino de acción. Dejar constantes
(`4.0`, `40`, `.80`, `.36`) en el top del archivo como configurables.

### 4.3 Tests robustos de mediciones

Los contadores deben ser puros y comprobables sin red. En `tests/test_dgs.cpp` del DGS real (o del reposit
del juego si se acopla el SDK):

- **Round-trip de métricas**: `pack(ServerMetrics)` → `unpackServerMetrics()` y comprobar que `bytesRx/Tx`,
  `failedTransfers`, `activeEntities` sobreviven (requiere recompilar `pack/unpack` con los campos nuevos,
  ⚠️ §4e).
- **Aritmética del ancho de banda**: simular N envíos de `Packet` de tamaño `len_i` y assert
  `bytesTx == Σ len_i` (probando el envoltorio contador, no UDP real).
- **Decisión de escalado**: inyectar `ServerMetrics` con `failedTransfers` alto / `bytesTx>>bytesRx` y
  assert de que `evaluateServer` escala (mock o instanciando `Orchestrator` con un `TCPSocket` falso que no
  abra socket real; el cuerpo con k8s se puede saltar con un flag de "dry-run" para unit test).
- **Timeout del validador**: en `zone_node`, simular validador mudo → assert S1 (decisión local) y de que
  `failedTransfers` se incrementa; y que llega `PKT_VALIDATOR_STATUS` con `state=DEGRADED`.

**Tests de robustez adicionales:**
- **Golden test del wire format**: fijar el layout exacto (ojo a `alignas(16)` de `GhostDelta`, tipos) y
  comparar byte-a-byte `pack()` contra un buffer conocido. Es EL guarda contra la desincronización de
  `libdgs_client.a` (§4e): si alguien reordena/añade campos o cambia el `alignas`, el test falla de
  inmediato. Útil también para los campos nuevos de `ServerMetrics`.
- **Fuzz del round-trip** `pack→unpack→pack`: para `EntityTransfer`, `ServerMetrics`, `GhostDelta`,
  `ValidateRequest/Ack`, generar buffers aleatorios (y truncados) y assert de que: o lanza `runtime_error`
  limpio (overflow del `Packet::read`), o devuelve datos sin romper. Hay que fuzzear el `readString`/`readRaw`
  con tamaños mentirosos.
- **Propiedades del pack**: pack/unpack es una **biyección** en el dominio válido (p.ej.
  `unpack(pack(x)) == x` sin pérdida de float); assert en cientos de inputs, incl. NaN/Inf (regla: NaN
  debe rechazarse en `validateMove`, no serializarse como válido).
- **Chaos / corte del validador a mitad de flujo**: en un test de integración con `UDPSocket` real (loopback),
  meter `PKT_VALIDATE_REQ`, matar la respuesta N veces (simular pérdida), y assert: (a) circuit breaker se
  abre, (b) fail-open decide por S1 para movimientos y **fail-closed** rechaza `ACT_DESTROY`, (c) llega
  `PKT_VALIDATOR_STATUS` con `state=OPEN/DEGRADED`. Probar también **ACK duplicado/tardío** (idempotencia) y
  **replay del mismo seq** (descartado).
- **Eviction de zonas**: alimentar `updateNodeTopology` con 2 fds, avanzar tiempo simulado > lease sin nuevos
  `ServerMetrics`, y assert de que `activeZones` queda vacía y `findTargetNode` devuelve -1 (regresión del
  bug 1 de §4.6).
- **Determinismo del módulo**: mismo `seed`+`WorldQuery` y secuencia de `MoveSample` → mismos veredictos y
  mismos `serializeRegion` bytes en 2 cargas del `.so` (assert byte-igual).
- **Concurrencia de contadores**: correr N hilos incrementando `bytesTx`/`failedTransfers` con TSAN/ASAN y
  assert del total exacto (los `+=` sobre `uint64_t` compartido deben ser atómicos o en el hilo único de
  métricas).

**Bench/método de límite de transferencia (transferencia entre servidores).** El usuario quiere saber
"cuánto puede transferir el DGS entre servidores con latencias 10/20/50/80/150 ms y volúmenes
1/2.5/5 GB hasta que algo falle". Se implementa en `tools/bench_dgs_bandwidth.cpp` (header-only, sin `.a`
ni `httplib.h` → compila en CI; `--fast` para verificación rápida). Dos pases:

- **Modelo BDP** (determinista): `throughput = min(enlace, ventana/RTT)`, `BDP = throughput·RTT`, y se marca
  **FAIL** cuando `BDP > buf_recv` (OVERFLOW: el receptor no puede retener en vuelo → el nodo suelta y suma
  `failedTransfers`) o `t = volumen/throughput > deadline` (TIMEOUT). Matriz de escenarios cli/srv
  1G/1G, 1G/2.5G, 1G/5G, 2.5G/2.5G, 5G/5G por cada RTT. Claves por env `BENCH_LINK_MBPS` (1250 = 10 Gbit/s),
  `BENCH_WINDOW_MIB` (64), `BENCH_BUF_MIB` (256), `BENCH_DEADLINE_S` (60).
- **Estrés del transporte real**: transmite el volumen completo a través de `Packet::toFramed()` →
  `PacketFramer::feed` (fragmentado en recvs de ~7 KB, el bug 6), comprueba integridad byte-a-byte del
  payload y mide MiB/s. Así el límite no es solo teoría: el framer aguanta ese tráfico sin perder un byte.

El punto de rotura en la práctica aparece cuando el **BDP supera el buffer del receptor**: a RTT alto la
ventana fija (`64 MiB`) limita el throughput a `ventana/RTT` y el BDP queda clavado en `ventana`; si `buf <
ventana` ⇒ OVERFLOW, independiente del volumen. Con `BENCH_BUF_MIB` ≥ ventana, el límite lo pone el
deadline: `capacidad_max = min(enlace, ventana/RTT)·deadline`.

### 4.4 Correcciones previas del DGS que apoyan lo robusto (del diseño v2, §6)

- **O1/Métrica**: `broadcastToClient` enviaba `sizeof(EntityTransfer)` (~4KB con `data[4096]`) por entidad y
  cliente cada tick. **Mandar solo `offsetof(data)+dataSize` bytes o el `GhostDelta`.** Esto reduce
  `bytesTx` y hace la métrica de ancho de banda significativa (medir lo que de verdad se manda).
- **O2**: `std::map`→`unordered_map` en `lastKnown`/`lastSnapshot` (O(1) por paquete).
- **O3**: usar RTT real en plausibilidad (`latency_compensation`), no fudge fijo `SCALE/1000`.

### 4.5 ⚠️ Wire format al añadir campos a `ServerMetrics`

`pack(ServerMetrics)/unpackServerMetrics` viven en el .cpp del DGS real (compilados en `libdgs_client.a`).
Por tanto, al añadir campos:
1. Añadir campos en `types.h` (este repo).
2. **Recompilar el DGS real con el mismo `types.h`** para que `pack/unpack` serialice los campos nuevos.
3. **`libdgs_client.a` debe regenerarse** (o el cliente linkar contra el `.so`/fuente del DGS) — sino hay
   **desincronización binaria** entre lo que el cliente escribe y lo que el servidor lee.
4. Compatibilidad: añadir campos NUEVOS al final del struct; los bytes viejos se rellenan por defecto
   (versión de protocolo). No reordenar.

### 4.6 Mejoras de robustez detectadas EN el código del SDK (arreglar junto a P2/P3)

Estos fallos están en los headers reales de este repo (`external/dgs/include/`) y deben corregirse al
tocar `orchestrator.h`/`network.h`:

1. **`Orchestrator::updateNodeTopology` NUNCA purga nodos muertos** (orchestrator.h:24-47): solo actualiza o
   inserta en `activeZones`; no hay ruta de borrado. Un nodo que muere deja su `ZoneInfo` para siempre →
   `findTargetNode`/`findNeighbors` pueden devolver un fd/zona zombi. Fix: **lease por zona** — cada `fd`
   se marca con `lastSeen` (estampa del `ServerMetrics`); una tarea periódica purga `activeZones` con
   `now - lastSeen > lease` y notifica al master (PKT_REASSIGN). Mismo lease de §2.3.
2. **`std::strncpy(zone.addr, ..., sizeof-1)` sin null-terminator forzado** (orchestrator.h:33,44): si la
   fuente ≥ 16 bytes, `addr` queda sin `\0` → lectura fuera de límites al usarlo como `char*`. Fix:
   `addr[sizeof-1] = '\0'` o `snprintf`.
3. **Cooldown de escalado por `nodeFD` reutilizable** (orchestrator.h:121-123): `lastScaleTime` es por fd;
   si un nodo muere y el fd se reusa, una estampa vieja puede bloquear (o permitir) un escalado erróneo.
   Fix: `lastScaleTime` keyed por `addr:port` (identidad de nodo), no por fd, y limpiarlo en la purga (bug 1).
4. **`socket.send(fd, ...)` sin comprobar el retorno** (orchestrator.h:269, `sendResizeCommand`): un envío
   fallido (fd muerto) se ignora. Fix: comprobar `<= 0` y contar en `failedTransfers` + reemitir.
5. **`evaluateServer` no reporta `startTime`/`uptime`** → no se puede distinguir "nodo recién arrancado con
   contadores a 0" de "nodo sano sin tráfico" (relevante para §"medición robusta"). Fix: añadir
   `uint64_t startTime` a `ServerMetrics`.
6. **Buffer de `receive` sin cap / truncación UDP**: un datagrama mayor que el buffer se trunca en silencio
   (el parseo manda bytes basura al `Packet::read` → `runtime_error` que puede tumbar un hilo). Fix: cap de
   tamaño + comprobar `received < max` y descartar con conteo en `failedTransfers`.

---

## 5. Fases

- **P0 — Recompilación de wire format**: extender `ServerMetrics` (§4.1) y tocar `pack/unpack` en el DGS real
  (§4e). Regenera `libdgs_client.a`.
- **P1 — Renombrado `anticheat`→`validador`** (§1) en un commit atómico (código + k8s + docs). El `validate()`
  pasa a `GameModuleHost` (F0 ya demostró el dlopen).
- **P2 — Request activa** §2: `PKT_VALIDATE_REQ/ACK` + timeout + retry/backoff + circuit breaker + colas
  acotadas + auth/token en `zone_node`, pre-checke S1, y `PKT_VALIDATOR_STATUS` → `head_server` (con lease).
  ⚠️ Estado: RQ/ACK, timeout/backoff, breaker, colas, S1 y `PKT_VALIDATOR_STATUS` están en `zone_node` real;
  **auth/token → decisión D-K** (mTLS/Secret k8s, sin tocar wire-format).
- **P3 — Métricas y orquestador** (§4): contadores en nodo (monotónicos), nueva lógica de `evaluateServer`
  (EWMA/Δ, `startTime`, `failedTransfers`), constantes configurables.
- **P3b — Fixes de robustez del SDK** (§4.6, este repo): eviction/lease de `activeZones`, null-terminator de
  `addr`, cooldown por `addr:port`, comprobación de `socket.send`, cap de `receive`. Incluye `types.h`
  (`startTime`).
- **P4 — Globalización + autoridad por zona** (§3): terminar ABI v4 (traspaso por región) si no está;
  **`module->step` en la zona dueña (tick fijo)**, **ownership de entidad con lease**, **handoff de frontera**
  (`serializeRegion/mergeRegion` + `PKT_REASSIGN`), determinismo + GC del `lastKnown` + contención de crashes
  del módulo; módulo `lib<proyecto>_rules`.
- **P5 — Tests robustos** (§4.3): `tests/test_dgs.cpp` cubriendo round-trip de métricas, golden wire format,
  fuzz, aritmética de bandwidth, decisión de escalado, timeout/circuit-breaker del validador, eviction de
  zonas, determinismo del módulo y concurrencia de contadores. ⚠️ Estado (2026-08-05): en el repo DGS real
  `tests/wire_test.cpp` cubre el round-trip pack/unpack de todos los packets (P5 paso 3); **¡NUEVO!**
  `tests/robust_test.cpp` (target `robust_test`, enlazado contra `packet.cpp` + `dl`): (1) **determinismo del
  módulo** — carga `lib<proyecto>_rules.so` DOS veces (2 `dlopen` → 2 instancias con estado separado), crea
  una zona en cada una con el MISMO `seed`/`WorldQuery`, aplica la MISMA secuencia de `MoveSample` y exige
  veredictos idénticos en las 2 cargas Y bytes idénticos de `serializeRegion` (`memcmp`); si no hay `.so`
  (`GAME_MODULE_SO`, def `libharuka_rules.so`) → SKIP limpio, no fallo; (2) **concurrencia de contadores** —
  8 hilos × 100000 incrementos sobre `std::atomic` `bytesTx`/`failedTransfers` (patrón de `zone_node`) y
  assert del total EXACTO (con `-fsanitize=thread` un `+=` no atómico sobre esos contadores falla aquí mismo,
  §4.3).
- **P6 — Predicción y persistencia** (v2 C4/C6) y **traspaso por métrica fallida** (P2 + O5). ⚠️ Estado
  (2026-08-05): **C4 (predicción)** ya estaba — la zona dueña ejecuta `module->step` a tick fijo
  (`ZONE_STEP_MS`, def 100ms) con crash-guard y lease por entidad en `zone_node.cpp`. **C6 (persistencia del
  blob del módulo) CERRADO hoy:** `persistance_node::insertEntity` guarda ahora el blob OPACO del módulo
  (`e.data[0..dataSize)`) como `moduleBlob` binario BSON además de pos/stats (el `EntityTransfer` ya lo
  transporta vía `pack`/`writeRaw(data,dataSize)`, y el validador lo reenvía crudo a persistence) — al
  recargar una zona el módulo puede reconstruir su estado sin re-simular desde cero. **Traspaso por métrica
  fallida (P2+O5, F1) CERRADO hoy:** el orquestador ya no escala hacia arriba cuando un nodo falla; en
  `evaluateServer` la señal `failureProne` (`failedTransfers > EVAL_FAIL_THRESH`) encola un
  `LIFECYCLE_REASSIGN` (prioridad 2, tras crash y antes que merge/split) y `tryReassign(fd)` cede la región
  del nodo fallido a la vecina sana de mayor volumen (absorbRegion + DRAIN → ack → DELETE_ZONE, mismo camino
  que la fusión pero con el fd fallido como víctima). Sin vecino sano → se mantiene la zona (fail-open
  degradado).
- **P7 — Plano social/cuenta** (§3.7): `PKT_SOCIAL_DELTA` (guild/party), ampliar `PKT_CHAT` (canal/seq/
  rate-limit), `PKT_ACCOUNT` (ban/permisos); nodo social sobre head+cache+persistence (reusa el camino de ban
  ya planteado, PLAN_DGS_ANTICHEAT C5); economía de gremio por `ACT_TRANSFER` fail-closed. ⚠️ Estado
  (2026-08-05): **wire y tipos CERRADOS hoy** en el repo real — `types.h` añade `ChatMessage` ampliado
  (`channel`/`seq`/`timestampMs`), enums `ChatChannel`/`SocialKind`/`AccountActionKind`, y structs
  `SocialDelta`/`AccountAction`; `packet.h/.cpp` añaden pack/unpack para `PKT_SOCIAL_DELTA` y `PKT_ACCOUNT` y
  amplían `PKT_CHAT`; `tests/wire_test.cpp` cubre los 3 round-trips (chat con canal/seq/época, social, cuenta).
  **Nodo social NUEVO hoy:** `nodes/social_node.cpp` (target `social_node`, TCP:42430) — un solo dueño del
  plano NO espacial: estado de guilds/parties/amigos/bans en memoria, deltas por evento con `seq` de fan-out a
  los suscriptores ONLINE (regla 1 de §3.7), servicio de chat por canal con rate-limit por uuid (500ms) y seq
  de orden ANTES del fan-out, y acciones de cuenta (ban/permisos) aplicadas y reenviadas a todas las zonas;
  write-through best-effort a persistance_node (MongoDB). **Dos pendientes CERRADOS hoy:**
  (a) **economía `ACT_TRANSFER` fail-closed** — `validador_node.cpp` ahora distingue `kind==0` (movimiento →
  `validateMoveRequest`) de `kind!=0` (acción → **`validateActionRequest`**, NUEVO): si hay módulo sano con
  `validateAction` el veredicto lo decide el proyecto sobre el blob OPACO que viaja en `data[0..dataSize)`;
  si NO hay módulo, módulo sospechoso o `validateAction` null → **fail-closed: se rechaza** (la economía de
  gremio, loot y trade jamás es cliente-autoritativa, §2.3/§3.7); (b) **suscripción zona→nodo social** —
  `zone_node.cpp` conecta ahora a `SOCIAL_HOST`/`SOCIAL_TCP_PORT` (def `social`:42430, junto al canal del
  validador), recibe `PKT_ACCOUNT` del fan-out del social y aplica el ban en `bannedUntilMs` (0=permanente);
  un `EntityTransfer` de una cuenta baneada se descarta en la ZONA (bloqueo de ENTRADA, la zona no juzga, solo
  aplica lo que el social decide). Los clients quedan así: el chat `local` lo emite la zona, los demás canales
  y los bans los sirve el social con la zona como suscriptor.
- **P8 — Instalador + despliegue + Terraform** (§3.8): backend de spawn abstracto (`LOCAL/K8S/TERRAFORM`) en
  `orchestrator.h`; CLI `dgs run/install/uninstall/up/status/logs`; `terraform/` opcional (network/k8s/
  mongodb/zone_node/validador/social) con outputs al orquestador; tests de paridad entre backends. ⚠️ Estado
  (2026-08-05): **CERRADO hoy** en el repo real —
  (a) **backend de spawn abstracto**: `orchestrator.h` añade `enum class SpawnBackend { SPAWN_LOCAL,
  SPAWN_K8S, SPAWN_TERRAFORM }` + `resolveSpawnBackend()` (env `DGS_SPAWN_BACKEND`; default: K8S si hay
  ServiceAccount in-cluster, si no LOCAL). `spawnZoneNode`/`deleteZoneNode` ahora DELEGAN:
  `spawnLocalNode` (fork/exec del binario `DGS_ZONE_BIN` con SIGTERM+waitpid en `deleteLocalNode`),
  `spawnK8sNode(...,viaKubectl)` (in-cluster API o `kubectl apply -f` del MISMO manifest para TERRAFORM,
  `k8sManifestPath()` en `DGS_MANIFEST_DIR`); la **paridad de env es por construcción**: `zoneSpawnEnv()`
  (estática, pública) genera las 13 variables de topología (CHUNK_*, CHUNK_SIZE_*, ZONE_UDP_PORT,
  HEAD_SERVER_*, MY_POD_IP) que usan LOCAL (putenv) y el manifest k8s por igual.
  (b) **CLI `dgs`**: `tools/dgs_cli.cpp` (target `dgs`) con `run` (standalone fork/exec de los 6 nodos en
  orden de conexión, logs a `DGS_LOG_DIR`, SIGTERM a todos), `install [--systemd]` (binarios a `/opt/dgs` +
  unidades systemd), `uninstall`, `up [--terraform]` (kubectl apply de `k8s/`, con terraform antes),
  `down`, `status` y `logs [nodo]`. Regla §3.8: el modo no cambia el comportamiento del DGS.
  (c) **Terraform opcional**: `terraform/` con `main.tf` (providers por `TF_VAR_cloud`/`TF_VAR_region`)
  y módulos `network` (VPC/subredes/security groups 42424-42430+27017), `mongodb`, `k8s`, `zone_node`
  (deployment + NodePort UDP 30425, env idéntico al manifest), `validador`, `social`; outputs al
  orquestador (`kubeconfig`, `mongodb_endpoint`, `zone_node_image`, `node_pool_ips`).
  (d) **test de paridad**: `tests/spawn_parity_test.cpp` (target `spawn_parity_test`, enlaza httplib) —
  verifica que `zoneSpawnEnv` es completa (13 vars, sin duplicados, determinista, con los CHUNK_*/puerto/
  head/IP correctos) y que `resolveSpawnBackend` respeta `DGS_SPAWN_BACKEND=local/k8s/terraform`; la paridad
  LOCAL/K8S/TERRAFORM queda garantizada por construcción (un solo generador de env).
- **P9 — Ciclo de vida completo de zonas** (§3.9): fusión/escalado down + destrucción ordenada (`PKT_DRAIN`/
  `PKT_DELETE_ZONE`), histéresis y anti-flappy, drenado con drain-timeout, y limpieza de pods zombie en muerte
  no planificada (F15). ⚠️ Estado (2026-08-05): implementado en `orchestrator.h` real (fusión `tryMergeDown` con
  histéresis/ventana/anti-flappy, drain fail-safe, zombie sweep) y en `zone_node` (DRAIN cede región+entidades,
  DELETE_ZONE destruye zona). **Gaps cerrados hoy:** (a) scale-down por Nº de jugadores/vecindad —
  `EVAL_MERGE_ENTITIES` (def 1): una zona con pocas entidades activas arranca la ventana de fusión aunque tenga
  RAM media; (b) el `zone-node` **base** se registra ahora en `portToName` (fix `ZONE_BASE_PORT`/42425 → "zone-node")
  para que su pod se borre al evictar/morir. **P9b (deuda menor, CERRADA hoy):** cola de vida con prioridad formal
  `crash>merge>split` — los disparadores de `evaluateServer`/`sweepStaleZones` ya NO mutan el clúster inline;
  encolan su operación (`enqueueLifecycle`, enum `LifecycleOp`: SPLIT/MERGE/EVICT) y `processLifecycleQueue()`
  ejecuta UNA operación por evaluación por prioridad global, saltando zonas en asentamiento anti-flappy por-zona
  (`EVAL_LIFECYCLE_SETTLE_S`, def 30s). El split vive ahora en `trySplitDown` (con su cooldown `EVAL_COOLDOWN_S`)
  y la evicción en `evictStaleZone` (mismo cuerpo que el antiguo inline del sweep).

---

## 6. Dependencias y archivos

| Frente | archivos DGS real | archivos este repo (SDK) |
|---|---|---|
| renombrado | `anticheat_node.cpp/.h` → `validador_node.*`, k8s `anticheat-node` | docs (`ROADMAP.md:395`) |
| request zona→validador | `zone_node.cpp`, `head_server.cpp`, `packet.cpp` | — (documentado §2) |
| métricas | `orchestrator.h/.cpp`, `network.cpp` (contadores), `packet.cpp` | `types.h` (campos), `orchestrator.h` (lógica) |
| globalización + autoridad por zona | `GameModuleHost.*`, `module->step` en `zone_node`, ownership/lease + handoff en `head_server`/`zone_node` | `game_module.h` (ya v4, `serializeRegion`/`mergeRegion`) |
| social/cuenta | nodo social sobre `head_server`+`cache_node`+`persistance_node`, `PKT_SOCIAL_DELTA`/`PKT_CHAT`/`PKT_ACCOUNT` en `packet.cpp` | — (documentado §3.7) |
| despliegue + Terraform | CLI `dgs deploy`, backend de spawn en `orchestrator.h`, `terraform/` (HCL) | `orchestrator.h` (abstract el spawn, §3.8), `network.h` |
| ciclo de vida de zonas | `PKT_DRAIN`/`PKT_DELETE_ZONE` en `packet.cpp`, merge-down/envío en `orchestrator.h`, `types.h` (estados de zona) | `orchestrator.h` (fusión/destrucción, §3.9), `types.h` |
| tests | `tests/test_dgs.cpp` | `tests/` existentes |

---

## 7. Decisiones abiertas

- **D-A. Versión de protocolo**: ¿renombrar el byte del `PacketType` `PKT_ANTICHEAT` (si existe) a
  `PKT_VALIDATE` o mantener el valor y solo renombrar el enum? (Rec.: mantener valor numérico → retrocompatible.)
- **D-B. Umbrales** de `evaluateServer` (constantes `4.0`, `40`, `.80`, `.36`): ¿configurables por env?
- **D-C. Mover los contadores a un helper** (`MetricsCounter`) compartido por todos los nodos, o contadores
  locales por nodo que solo se agregan en `ServerMetrics`.
- **D-D. ¿`libdgs_client.a` se regenera aquí** o el cliente pasa a linkar contra el DGS real (.so)? (influye
  en P0/P3).
- **D-E. Fail-open vs fail-closed por defecto** (§2.3): se propone abierto para movimientos/ediciones no
  destructivas y cerrado para destroy/place/trade. Confirmar el catálogo exacto de verbos "críticos".
- **D-F. VM/containertación del módulo** (§3.5): signal-guard en hilo (light) vs `fork` del validador (aislamiento
  total). El fork cuesta memoria por zona; decidir según cuán de fiable se considera el `.so` del proyecto.
- **D-G. Lease timeout** de zonas/validador (bug 1 §4.6 y heartbeat §2.3): valor (propuesta 2× intervalo de
  `PKT_VALIDATOR_STATUS`).
- **D-H. Ownership de entidad en frontera** (§3.6): ¿lease por entidad (periodo de traspaso ~1-2 s, el dueño
  viejo sigue simulando hasta confirmación del nuevo) o handoff atómico (serialize→merge→drop)? Propuesta:
  lease corto con doble-conocimiento (los `GhostDelta` cubren el hueco) y **solo un dueño simula**.
- **D-I. Terraform obligatorio o solo-cluster** (§3.8): ¿`dgs up` sin `--terraform` usa `kubectl`/k3s y Terraform
  queda como módulo aparte para infra gestionada, o el `cluster` SIEMPRE pasa por Terraform? (Rec.: Terraform
  OPCIONAL — vía `--terraform` — y `kubectl`/k3s como camino mínimo, para que el deploy no dependa de una
  herramienta externa en el caso simple.)
- **D-J. Escalado down automático vs manual** (§3.9): ¿fusionar/destruir zonas automáticamente con histéresis+
  anti-flappy, o dejar el scale-down en un comando explícito `dgs prune`/`dgs scale`? (Rec.: automático con
  histéresis y ventana, más `dgs prune` manual para forzar.)
- **D-K. Autenticación entre nodos (§2.3 "Auth entre nodos") — pendiente (decisión B)**: los nodos `zone_node`
  ↔ `validador`/`head` se hablan por TCP **interno k8s aislado**, por lo que se decide **NO añadir `NODE_TOKEN`
  en bandera de paquete** (evita romper el wire-format congelado de `ValidateRequest/ValidateAck/ValidatorStatus`
  y re-sincronizar otra vez `libdgs_client.a`). La auth se delega a infraestructura: **secret en la red k8s /
  mTLS entre pods**. Queda pendiente de materializar como `NetworkPolicy` + `Secret`/mTLS en `k8s/` (P8) si el
  despliegue sale de esa red aislada. NO tocar `packet.h/.cpp` mientras el wire-format siga congelado (P0).

---

## 8. Matriz de fallos y degradación

| # | Falla | Consecuencia | Mitigación (robustez) |
|---|---|---|---|
| F1 | Validador mudo / caído | `zone_node` sin veredicto | Circuit breaker → fail-open (movs) / fail-closed (destroy/place/trade); S1 local; `failedTransfers`++ ; `PKT_VALIDATOR_STATUS=DOWN` → master reasigna |
| F2 | Validador lento (latencia) | ACKs tardíos, cola llena | Backoff+dedupe por `requestId`; cola acotada con backpressure; EWMA/RTT para no saturar (O3) |
| F3 | `zone_node` muere | parte del mundo sin servir | Master lo detecta por lease de `ServerMetrics` (bug 1 §4.6) → `PKT_REASSIGN` de su región a un vecino vía `serializeRegion/mergeRegion` (v4) |
| F4 | Nodo zombi tras reinicio de fd | `findTargetNode` a zona muerta | Eviction/lease de `activeZones`; cooldown keyed por `addr:port` no por fd (bugs 1 y 3) |
| F5 | Replay/reinyección de packet | doble aplicación de una acción | Sequence number por remitente + ventana deslizante en el validador; dedupe de `requestId` |
| F6 | `dataSize` mentiroso / datagrama truncado | parseo basura; `runtime_error` puede tumbar un hilo | cap de `receive` (bug 6); validar `dataSize ≤ 4096` antes de usar `data[]`; `Packet::read` ya lanza `runtime_error` limpio |
| F7 | Añadir campo a `ServerMetrics` sin recompilar | desincronización binaria vs `libdgs_client.a` | Golden wire-format test (§4.3) + recompilar juntos (§4e) |
| F8 | Segfault del `.so` de reglas | muere el nodo | Signal-guard en worker hilo (§3.5) → descartar y marcar módulo sospechoso; opcional validador en `fork` |
| F9 | Overflow de contador | tasas negativas/ilógicas | contadores monotónicos + Δ modular (substracción sin signo) (§4.1) |
| F10 | Reglas divergen cliente/server | falsos positivos o cheats colados | determinismo (seed+reloj, sin `rand()`/reloj de pared) + test de determinismo (§3.5, §4.3) |
| F11 | Entidad cruzando frontera de zona | **doble autoridad → divergencia** de física | ownership con lease: solo el nodo dueño simula; handoff con `serializeRegion/mergeRegion` + `PKT_REASSIGN`; interacciones cross-zona las resuelve el dueño del proyectil (§3.6) |
| F12 | Doble escritura de guild/party (dos nodos) | membresía/rango divergentes | **un solo dueño por tipo de dato** (§3.7): el gremio lo escribe UN nodo social; las zonas solo leen ids |
| F13 | Chat spam/flood de canales | CPU de fan-out, abuso | rate-limit + moderación en el **servicio de chat** antes del fan-out; colas acotadas por canal (§3.7) |
| F14 | Divergencia standalone vs cluster | bug que solo aparece en un modo | backend de spawn abstracto (`LOCAL/K8S/TERRAFORM`) + tests de paridad entre backends (§3.8) |
| F15 | Escalado down / nodo muerto sin destruir el pod | **fuga de réplicas + pods zombie** (`currentReplicas` solo crece, §3.9) | ciclo de vida completo (drain→merge→`PKT_DELETE_ZONE`→`currentReplicas--`); la muerte no planificada también borra el deployment (§3.9) |
| F16 | Viewer muestra objeto/región desactualizado o "fantasma" (hueco entre snapshot y deltas) | decisor vé una zona vacía o un objeto en el sitio equivocado | **snapshot + suscripción**: el viewer pide un `PKT_ZONE_SNAPSHOT` bajo demanda y abre una suscripción; cada delta lleva `reqId`/seq para descartar los que llegan antes o fuera de su ventana (§10.3); re-snapshot al reeditar o rezonificar la zona |
| F17 | Nodo sin recursos suficientes / oversubscription de CPU/GPU | la zona corre con menos CPUs/GPU de las pedidas → tirones/latencia (validador y física lentos) | **solicitud vs inventario** (§3.10): el nodo REJECT si la solicitud excede su capacidad; la zona queda `PROVISIONING` en cola de placement; el planificador usa fit por recursos libres, nunca oversubscribir |

---

## 10. Herramienta de mapa del universo (viewer) — gestión de zonas + mapa del planeta

> **Añadido (2026-08-05):** una herramienta de VISUALIZACIÓN Y GESTIÓN del universo. Complementa a
> `head_server`/`orchestrator` y al ciclo de vida de §3.9: con ella el operador VE qué ocurre en el
> mundo (dónde está cada zona y cada objeto) y puede CREAR/REPARTIR/FUNDIR zonas de mapa con un clic,
> sin tocar config. Vive en `haruka-cpp` (un `tools/map_viewer`) y conecta por la red del DGS, igual
> que un nodo cliente autoritativo.

### 10.1 Objetivo y dos planos de la herramienta

La herramienta tiene **dos responsabilidades que se muestran juntas** en un panel:

1. **Universo / zonas** — la malla mundial vista como conjunto de zonas (límites `chunkXMin..ZMax`,
   `@:port` de cada nodo, estado `ZoneState`). Aquí el operador **gestiona** zonas: crea (nueva zona),
   divide (repartir carga), fusiona/drena (aplicar §3.9) y destruye, emitiendo los `PKT_COMMAND` /
   `PKT_DRAIN` / `PKT_DELETE_ZONE` al master (o a `evaluateServer`).
2. **Planeta / objetos** — dentro de una zona, el mapa del terreno y **dónde está cada objeto**
   (`EntityTransfer.uuid` → `pos`, tipo `ENT_PLAYER/ITEM/NPC`, estado) y cada pieza colocada
   (`PlaceAction`). Aquí NO se inventa nada: el viewer **lee/aprende la zona** pidiendo el estado
   autoritativo real (§10.3), nunca una copia querida.

**Intuición:** el viewer lleva el mismo punto de vista que haría un "cerebro" del universo: arriba la
capa de zonas (mapa 2D/2.5D/3D del mundo), abriendo cada zona ves sus objetos en su posición real.
Todas las capas se muestran/ocultan con botones y filtros (nada de sobrecarga de información).

### 10.2 Modos de render (3D / 2.5D / 2D)

- **2D (mapa mundial)**: proyección plana; cada zona es un rectángulo por sus bounds de chunk;
  color = estado (`PROVISIONING/READY/DRAINING/DEAD/DESTROYED`, §3.9) y calor = métricas de carga
  (`activeEntities`, `ramUsage`, `bytesTx` de `ServerMetrics`). Es la vista principal para **gestionar**.
- **2.5D (mapa isométrica/elevación)** — las zonas como bloques extrusionados por la carga, con la
  capa de terreno del planeta de fondo (reusa el sampler del motor, `haruka_simbase`): escala/densidad
  por zoom.
- **3D (mundo/órbitas)** — vista libre del planeta con `SimplePlanet`/mesh; los objetos de la zona como
  gizmos/íconos en su `pos` real. Para "ver el planeta" tal cual.

Las tres vistas comparten el MISMO observador de datos (una sola fuente: servidor → cache local), solo
cambia la proyección (gizmos planos vs volumen). Nada de la física se reproduce: es RENDER de un estado
leído, nunca simular la zona en el client.

### 10.3 Aprender/leer una zona (la herramienta "se aprende" el mapa)

La clave de "tener el mapa que se aprende/explora": el viewer **no confía en su cache para los
detalles**: para una zona pedida hace (por zona, lazy, LOD):

1. **Listar** `PKT_ZONE_LIST` (o `zone_query` al head) → bounds/estados de TODAS las zonas.
2. **Enumerar** la zona → entra en descubrimiento: `PKT_ZONE_SNAPSHOT` (nuevo) al `zone_node` que la
   sirve. El nodo devuelve la lista actual de objetos (`uuid`, `type`, `pos`, `state`) + piezas.
3. **Suscripción live**: abre una suscripción; el nodo envía `PKT_ENTITY_TRANSFER`/`PKT_GHOST_DELTA`
   (deltas de movimiento/estado) mientras dure. El viewer aplica deltas encima de la snapshot.
4. **Coherencia**: cada delta lleva `sequence` respecto al snapshot; los que llegan antes/descolgados
   del estampado se descartan (F16). Si la zona cambia de dueño (handoff §3.6) o se divide (§3.9), el
   viewer detecta el cambio de `@:port`/bounds y **re-snapshot** la zona.

Solo la zona VISIBLE (o las seleccionadas) se mantiene suscrita → el tráfico del viewer escala con lo
que el operador ve, no con el total del universo (igual que LOD de malla: cerca → detalle).

### 10.4 Capas y filtros (qué mostrar, de forma intuitiva)

- **Capas** (toggles por pestaña, presets "Modo mundo/Modo zona/Modo térmico"):
  - Zonas (bounds + estado), grilla de chunk, terreno/punto de referencia.
  - Entidades por tipo (`T_PLAYER`, `T_ITEM`, `T_NPC`) y por estado (`PHYSICS_OWNED`, `INTERACTABLE`…).
  - Piezas colocadas (`ACT_PLACE`), proyectiles/balas, efectos (desde `state`).
  - Calor de carga (heatmap): `ramUsage`, `bytesTx`, `activeEntities`, `failedTransfers`.
- **Filtros**: de búsqueda (`uuid`, tipo, `owningZone`), por zona, por jugador (uuid/name), y por estado.
- **Persistencia**: el conjunto visible se guarda en un JSON (`settings/map_viewer.json`) y se restaura;
  los filtros se aplican del lado del viewer (no over-red) → el server no recorta por defecto.

### 10.5 Gestión de zonas desde el viewer (panel operador)

Sobre la vista 2D/2.5D, el operador hace clic en una zona y abre acciones (alineadas con §3.9 y
`Orchestrator`):

- **Crear**: "nueva zona aquí" → `spawnZoneNode` (backend `LOCAL/K8S/TERRAFORM` según `SpawnBackend`).
- **Dividir** (up): encerrar la región + botón → `sendResizeCommand`/split (creación ya existe).
- **Fusionar / drenar** (down): la zona que se va a reducir emite `PKT_DRAIN`, se `serializeRegion` →
  `mergeRegion` en la vecina, `PKT_DELETE_ZONE` y `currentReplicas--` (al cierre §3.9).
- **Reasignar**: mover un trozo de región a otra zona (handoff §3.6);
- Feedback de seguridad: el viewer muestra `DRAINING` mientras se mueve y suscribe a un ack
  (`PKT_DELETE_ZONE` confirmado) antes de limpiar de la lista.

TODAS las acciones van por el master/orquestador (la herramienta es un **cliente**, no hace vida):
el viewer solo pinta el estado que devuelve el orquestador; nada de "cambios → mundo" se ejecuta
localmente.

### 10.6 Arquitectura y conexión con el DGS

- **`tools/map_viewer_console.cpp` (solo CPU, headless)**: la capa de datos view (listar, snapshot,
  suscripción, heatmaps) sin GL → es la pieza comprobable por CI/non-UI; alimenta una interfaz:
  - **`map_viewer_app` (GL)**: los 3 modos 3D/2.5D/2D renderizan lo que el console entregó.
  - Separar vista (console/headless) de la presentación (GL) permite **testear la lógica de
    suscripción/descubrimiento/render en CI** igual que `test_dgs.cpp` (P5), sin necesitar ventana.
- **Consumo de red**: mismo ABI del DGS (`external/dgs`): `Packet`, `orchestrator` (vista zona),
  `Client` para dialogar con head/zone_node. Nuevo packet para snapshot (§10.3): `PKT_ZONE_SNAPSHOT`
  (request) y `PKT_ZONE_SNAPSHOT_REPLY` (lista de `EntityTransfer` + piezas + rev/seq) — añadir a
  `PacketType` (no rompe valores anteriores, van al final) y a `pack/unpack` de `packet.h`.
- Estado del viewer = **principalmente cache**: mantiene una copia local por zona visible (bounds/estado
  de `ZoneInfo`/`ZoneState` + último snapshot + deltas) reconciliada por el `sequence` (F16).

### 10.7 Plan/cómo encaja con la matriz y fases

- Revisar capas en §6 (archivos): añadir `tools/map_viewer_console.cpp`, `tools/map_viewer_gl/`,
  `PKT_ZONE_SNAPSHOT(_REPLY)`, y el JSON de preferencias.
- En §8 matriz: previsión F16 (datos desactualizados) ya añadida.
- Fase propuesta (tras P9 del repo DGS real); en el repo haruka-cpp, en orden:
  1. **Console (datos)**: listar zonas, snapshot de una zona, seq/discard, capas/filtros, persistencia.
     Probable en CI (headless) como `test_map_viewer.cpp` (P5 de esta herramienta).
  2. **GL 2D**: mapa mundial + toggles + actualización por suscripción.
  3. **2.5D/3D**: planet mesh + objetos de la zona sobre el terreno, varios presets de cap.
  4. **Gestión (operador)**: acciones crear/split/merge/destroy + rebuild al `zone_state`.

### 10.8 Decisiones abiertas (para cerrar al implementar)

- (a) ¿`PKT_ZONE_SNAPSHOT` es a petición (pull) con suscripción (push) — o una suscripción
  persistente probe desde el servidor? MVP: pull para abrir, push de deltas.
- (b) El viewer ¿es solo **lectura + herramienta de operador** (no propaga cambios a clientes) o
  también servidor proxy para que jugadores vean el mapa? Para este añadido: **solo admin/visión**.
- (c) ¿La vista 3D exige terreno de ref (sampler) o solo gizmos de posición? MVP: gizmos + opcional
  terreno; en modo 3D ilustramos con `SimplePlanet` si está disponible.
- (d) LOD del mapa: ¿a demanda vs relativo al zoom del viewer? Ver §10.3 (LOD por zona visible).

---

## 9. Orden sugerido de ataque (corto plazo, en este repo)

> **ESTADO (2026-08-05):** P0+P3b ya aplicados en el SDK local y cubiertos por un golden wire-format test
> nuevo (ver "Hecho en este repo" abajo). En el **repo DGS real** (`Documents/Github/dgs`): **P0 aplicado**
> (`types.h`: constantes + packets nuevos + `ZoneInfoPublic` antes de `ZoneInfo` + `ServerMetrics` ampliado +
> `ZoneState` + structs validación; `packet.h`; `src/packet.cpp` pack/unpack + `dgs_client` en CI; fix
> `std::atan2` en `client.cpp`) y **P1 aplicado** (renombrado `anticheat`→`validador`: `nodes/validador_node.cpp`,
> `Dockerfile.validador_node`, `k8s/validador/`, target/img/env `VALIDADOR_*`, logs `[Validador]`, textos
> "VIOLATION detectada"). **P2 aplicado** (§2): `ValidateRequest` con payload de movimiento (`entity` +
> `lastGX/Y/Z` + `maxSpeed` + `dtSeconds`) y su pack/unpack en `src/packet.cpp`; `validador_node.cpp` responde
> `PKT_VALIDATE_ACK` (vía `validateMoveRequest`, módulo o fallback genérico); `zone_node.cpp` conecta por TCP al
> validador, hace pre-chequeo local S1, manda REQ con throttle/reintentos/circuit breaker y emite
> `PKT_VALIDATOR_STATUS` periódico al head; `head_server_node.cpp` registra handler de `PKT_VALIDATOR_STATUS`.
> **P3 aplicado** (§4): contadores monotónicos `bytesRx/bytesTx` en `zone_node` (incrementados en los puntos
> reales de envío/recepción) + `Orchestrator::evaluateServer` reescrito (EWMA/Δ de tasa de banda, detección de
> reinicio por `startTimeS`, señales `load`/`netSaturated`/`failureProne`, constantes `EVAL_*` configurables).
> **Handoff ghost→entidad** (§3.6): la zona destino promueve el `GhostDelta` a `EntityTransfer` real (borra su
> ghost, no duplica), ignora ghosts de uuids ya reales, y purga ghosts por TTL (`GHOST_TTL_MS`).
> **P4 aplicado** (§3.5/§3.6): `game_module.h` del repo real igualado a v4 del SDK local (`ZoneHandle` +
> `createZone`/`destroyZone` + `serializeRegion`/`mergeRegion`/`dropRegion`); validador adaptado (crea la zona,
> pasa `g_zone` a `validateMove`); `PKT_REASSIGN` (types+packet+head handler de routing por chunk+zona que
> renueva lease); ownership con lease en `zone_node` (`ENTITY_LEASE_MS`) + GC de entidades/`lastPosition`
> inactivas.
> **Crash-containment (§3.5):** `validador_node` envuelve `validateMove` en un **crash-guard** por señales
> (`sigaltstack` + manejador de SIGSEGV/SIGBUS/SIGFPE/SIGILL que, solo si la señal cae dentro del módulo,
> hace `siglongjmp` de vuelta → el proceso sigue vivo y el módulo queda marcado **sospechoso** → fallback
> genérico; las señales fuera del módulo se re-lanzan como crash real).
> **`module->step` en la zona dueña (§3.6, C4):** `game_module.h` promueve el slot reservado `step` a miembro
> real de la vtable (append, sin mover offsets → sigue ABI 4, null = sin simular); `zone_node` carga el módulo
> (`GAME_MODULE_SO`, default `libharuka_rules.so`), crea su zona y ejecuta `module->step` a **tick fijo**
> (`ZONE_STEP_MS`, default 100) sobre las entidades **bajo su lease**, con **su propio crash-guard** (un crash
> en `step` suspende la simulación local sin matar el nodo); `zone_node` enlaza `${CMAKE_DL_LIBS}`.
> El `.a` se regenera en CI (`dgs_client`→`build/dgs_client.a`). Verificación de compilación: CI del repo real.
> **§3.9 ciclo de vida de zonas aplicado:** `ZoneLifecycle{requestId, ack}` (types+packet+`packDelete`) para
> `PKT_DRAIN`/`PKT_DELETE_ZONE`; `zone_node` maneja DRAIN (flag de drenado → deja de reclamar entidades, cede
> su lease al head, ack) y DELETE_ZONE (destruye la zona del módulo y sale); el orquestador añade estados de
> vida (`zoneStates`, `markZoneState`/`zoneState`/`isRoutable`), **fusión por histéresis** (`EVAL_MERGE_LOAD_RAM`
> + `EVAL_MERGE_WINDOW_S` + anti-flappy `lastLifecycleMs`, elige la vecina menor, `absorbRegion` en topología,
> `PKT_DRAIN` + deadline), **drain fail-safe** (timeout → READY, nunca región vacía), **destrucción ordenada**
> (`handleZoneLifecycle` → `deleteZoneNode` k8s + `PKT_DELETE_ZONE` + `currentReplicas--`) y **evicción de pods
> zombies** (`sweepStaleZones` por lease `EVAL_ZONE_LEASE_S`); el routing (`findTargetNode`/`findNeighbors`) y
> `updateNodeTopology` saltan zonas DRAINING/DEAD/DESTROYED.
> **Blob de región wired (§3.9):** `PKT_ZONE_REGION` + `ZoneRegion{ancla, srcZone, size, data[4096]}` con
> pack/unpack (cap `MAX_REGION_BYTES`); la zona que cede serializa su región (`serializeRegion`) y la manda
> al head, que la enruta por ancla (`findTargetNode`) a la superviviente, que la incorpora (`mergeRegion`).
> Pendiente real: el transporte TCP no tiene framing (recvs de tamaño fijo) — el blob funciona para regiones
> ≤ 4 KB; regiones mayores requieren framing de paquetes (§4.6 bug 6).
> **P5 — tests (§4.3) aplicados:** `tests/wire_test.cpp` en el repo real (round-trip pack/unpack de TODOS los
> packets: EntityTransfer, Command, ServerMetrics, ZoneQuery/Response/List, GhostDelta, Chat, Validate*,
> ValidatorStatus, EntityReassign, ZoneLifecycle, ZoneRegion) enlazado contra `packet.cpp` (COMMON_SRCS) y
> ejecutado en CI (falla el build si un pack/unpack se desincroniza). El golden `test_dgs.cpp` del SDK local
> añade invariantes de layout (static_assert/offsetof) para los structs nuevos de P4/§3.9.
> **§4.6 bug 6 (framing TCP) + batería de robustez en este repo:** `Packet::toFramed()` ([len:4]+payload) y
> `PacketFramer` (acumulador TCP que entrega paquetes completos ante recvs parciales/concatenados y
> re-sincroniza descartando cabeceras mentirosas con contador `discards()` → `failedTransfers`); `network.h`
> añade `receivedWasTruncated` (cap de receive/truncación UDP); `Orchestrator` ganó ctor sin socket +
> `dryRun` + `scaleEvents` para testeabilidad sin red/k8s. Nuevo `tests/test_dgs.cpp::test_dgs_robust`
> (framing concatenado/partido/corrupto, truncación, evicción por lease, decisión de escalado dry-run para
> `failedTransfers`/asimetría/carga/width<1/cooldown, y fuzz de `Packet::read`) registrado en
> `test_common.h` + `haruka_tests.cpp` (`--robust`).

**Hecho en este repo:**
- **`types.h`** — constantes `MAX_ADDR_LEN`/`MAX_ENTITY_DATA`/`MAX_PACKET_SIZE`/`DEFAULT_LEASE_MS` (mágicos 16/4096
  eliminados); `ZoneInfo : ZoneInfoPublic { int fd; }` (sin cambio de layout); `ServerMetrics` +`startTimeS`,
  `bytesRx`, `bytesTx`, `failedTransfers`, `activeEntities` (⚠️ layout de red cambiado → regenerar
  `libdgs_client.a`, D-D); `PacketType` +`PKT_VALIDATE_REQ/ACK`, `PKT_VALIDATOR_STATUS`, `PKT_SOCIAL_DELTA`,
  `PKT_ACCOUNT`, `PKT_DRAIN`, `PKT_DELETE_ZONE`; structs `ValidateRequest/ValidateAck/ValidatorStatus`;
  enum `ZoneState`.
- **`orchestrator.h`** — `copyAddr` con null-terminator forzado (bug 2, también en `findZoneResponse`);
  cooldown de escalado por `addr:port` (bug 3); `socket.send` comprobado (bug 4); `lastSeen`+`evictStaleZones`
  (lease, bug 1) con `ZoneState`; `evaluateServer` con `netSaturated`/`failureProne` + guard de `startTimeS`
  (bug 5); enum `SpawnBackend` (hook §3.8).
- **`network.h`** — `MAX_PACKET_SIZE` documentado como cap en `receive` (bug 6); helper
  `receivedWasTruncated()` (convención de truncación UDP centralizada).
- **`packet.h`** — API completada para validación: `pack/unpack` declarados para
  `ValidateRequest`/`ValidateAck`/`ValidatorStatus` (P2 §2.2). Definiciones → en el lib regenerado
  (repo real, D-D); `ZoneState` NO va al wire (§3.9) y por eso no tiene pack/unpack.
  **Nuevo (bug 6):** `Packet::toFramed()` (prefijo [len:4]) y `PacketFramer` (acumulador TCP O(n), con
  `feed/next/buffered/discards/clear` y re-sincronización ante prefijos corruptos).
- **`orchestrator.h`** — además de lo ya listado: ctor sin socket (test), `dryRun`, `scaleEvents`
  (contador de decisiones; habilita los tests de §4.3 sin clúster ni red).
- **`.github/workflows/ci.yml`** — la CI (Fedora, headless) ya compilaba y corría `haruka_tests`;
  ahora además construye `haruka_rules` (el `.so` que dlopen `test_dgs_rules_module`) y lo copia a la
  raíz del runner para que `${./libharuka_rules.so}` resuelva, y añade un paso `haruka_tests dgs`
  (golden wire-format + módulo de reglas). Así el golden test de P0 se **verifica en CI** (compilador
  + ejecución) sin toolchain local.
- **`tools/bench_dgs_bandwidth.cpp`** — **¡NUEVO!** test de estrés de transferencia entre servidores
  (ver "bench/método de límite" en §4.3): modelo BDP (throughput=min(enlace, ventana/RTT)) sobre la
  matriz 1/2.5/5 GiB × RTT 10/20/50/80/150 ms con fallo por OVERFLOW (BDP>buf) o TIMEOUT (t>deadline),
  y estres real del `PacketFramer` (bug 6) con el volumen completo midiendo integridad + MiB/s.
  Header-only (sin `.a`, sin `httplib`) → compila en CI. Target `bench_dgs_bandwidth` en `CMakeLists.txt`
  (junto a `mapc`); la CI (`ci.yml`) lo construye y corre con `--fast`.
- **`tests/test_dgs.cpp`** — **¡NUEVO!** `test_dgs_wire_format()` (P5, paso 3): golden wire-format/layout de
  `ZoneInfo`, `EntityTransfer`, `GhostDelta` y `ServerMetrics` (constantes MAX_*, offsetof de campos clave y
  que los campos nuevos de `ServerMetrics` van al final). Compile-time (`static_assert`) + runtime (`CHECK`).
  Registrado en `test_common.h` y en `haruka_tests.cpp` (`--dgs`). No enlaza el `.a` → seguro aunque siga
  desincronizado. **¡NUEVO!** `test_dgs_robust()`: framing (§4.6 bug 6), truncación, evicción, escalado
  dry-run y fuzz (ver "§4.6 bug 6" arriba); la parte que usa `Orchestrator` se compila solo donde exista
  `httplib.h` (`__has_include`), el resto (framing/fuzz) corre siempre.
  **Extensión robustez:** re-sincronización EN MITAD DE STREAM (frame corrupto entre dos buenos → se
  descartan exactamente sus bytes y los buenos se entregan sin `clear()`, con `discards==8` y contador de
  frames==2) y fuzz de `readRaw`/`readString` con tamaños mentirosos (§4.3): `readRaw(dest, n>)` no escribe
  en `dest` (chequeo previo) y `readString` con length mentiroso lanza `runtime_error` limpio.

Para cerrar lo verificable aquí, el orden práctico sería:

1. **P3b (§4.6)** en `orchestrator.h`/`network.h`/`types.h` de este repo: eviction/lease, null-terminator,
   cooldown por `addr:port`, `socket.send` comprobado, `startTime`. Cambios puros y comprobables. ✅ HECHO.
2. **Golden wire-format test** del layout actual → ✅ HECHO (arriba, `test_dgs_wire_format`).
3. **Tests de eviction/contadores/cap de receive** → en este repo se cubrieron en **headers puros** con
   `test_dgs_robust()` (framing `toFramed`/`PacketFramer`, truncación `receivedWasTruncated`, evicción por
   lease, decisión de escalado en `dryRun` con `scaleEvents`, fuzz de `Packet::read`) — no requieren el `.a`
   y corren siempre salvo los que usan `Orchestrator` (necesitan `httplib.h`, `__has_include`). El
   round-trip pack/unpack de métricas sigue en el repo real (`tests/wire_test.cpp`, enlazado contra
   `packet.cpp`).
4. Después, en el repo DGS real: P1 (rename), P2 (request), P3 (métricas), P4 (globalización), P5 (tests).
