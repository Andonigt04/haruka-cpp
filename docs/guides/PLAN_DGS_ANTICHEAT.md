# PLAN — DGS dinámico + módulo de reglas por proyecto (física + casting), anti-cheat, persistencia, predicción, baneos

> Estado: DISEÑO v2 (revisado con las correcciones del usuario). DGS real en `~/Documents/GitHub/dgs/`
> (microservicios epoll + k8s + MongoDB). Cliente/juego en `Survival/scripts/` + engine `haruka-cpp/`.

## 0. Modelo REAL del DGS hoy (medido en el código)

- Microservicios: `head_server`(TCP 42424, routing+k8s) · `zone_node`(TCP+UDP 42420, **relay+interés**, NO
  simula física) · `cache_node` · `anticheat_node`(UDP+TCP 42427/8, **valida plausibilidad de posición**) ·
  `persistance_node`(TCP 42429, MongoDB) · `client_node`(HTTP+TCP+UDP).
- Modelo de autoridad actual = **cliente-autoritativo + validación de plausibilidad en server**. El cliente
  manda `EntityTransfer` (pos/chunk/stats/EntityState/data[4096]); el `zone_node` lo reenvía y difunde
  `GhostDelta` a los vecinos; el `anticheat_node` comprueba `dist ≤ maxSpeed·dt + margen`. **La validación
  está HARDCODEADA** (`static bool validate()` en anticheat_node.cpp).
- `EntityTransfer` ya trae lo necesario para reglas de juego: `EntityState` bitmask rwx (MOVING/PHYSICS_OWNED/
  INTERACTABLE/DESTRUCTIBLE/EMITTING/CONSUMABLE), `Stats`(health/speed[3]/baseDMG/healing), `data[4096]` opaco.

Esto **valida el modelo que pediste**: el player es una **entidad nativa del DGS** (no un scene object del
juego); el DGS lo valida y predice. Los scene objects (props) y el casting son del juego.

## 1. Principio: DGS genérico + MÓDULO DE REGLAS por proyecto (dlopen)

El DGS no debe tocarse por juego. Cada proyecto entrega **un módulo de reglas** (su física + casting + qué
props se mueven + cómo se edita el mundo) que:
- se **linka en el CLIENTE** (para predicción), y
- se compila como **`.so` que el DGS carga con `dlopen`** (para validación/predicción server-side).

Mismo código fuente → mismas reglas en cliente y server. El DGS define un **ABI C estable y versionado**; el
juego lo implementa. Añadir un juego nuevo = nuevo `.so` + variable de entorno. **DGS intacto.**

```
   CLIENTE (Survival+engine)                     DGS (genérico, sin reglas de juego)
   ├─ predice con  libgameA_rules (linkado)      ├─ zone_node ─┐
   └─ manda INTENTS + WORLD-DELTAS ──────────────┤             ├─ dlopen(libgameA_rules.so) ──► valida+predice
                                                 ├─ anticheat ─┘        (MISMO código que el cliente)
                                                 └─ persistence ► MongoDB (blob opaco del módulo)
```

## 2. Qué libs hay que hacer

1. **`dgs_game_module.h` — el CONTRATO (vive en DGS, genérico).** Cabecera C con una *vtable* de punteros a
   función + el número de versión de ABI. Es lo ÚNICO que ambos lados comparten. No contiene reglas.
   ```c
   // ABI C estable. El DGS hace dlopen + dlsym("dgs_game_module_v1").
   typedef struct DgsWorldQuery {   // el DGS presta al módulo acceso de solo-lectura al estado autoritativo
       int  (*terrainHeight)(void* ctx, double x, double z, float* outY);
       int  (*getEntity)(void* ctx, uint32_t uuid, DGS_EntityTransfer* out);
       void* ctx;
   } DgsWorldQuery;

   typedef struct DgsGameModule {
       uint32_t abiVersion;                 // == DGS_GAME_MODULE_ABI (v1)
       // física/predicción (mismo paso en cliente y server)
       void (*step)(DGS_EntityTransfer* e, float dt, const DgsWorldQuery* w);
       int  (*validateMove)(const DGS_EntityTransfer* now, const DGS_EntityTransfer* last,
                            float dt, const DgsWorldQuery* w);          // 1=ok, 0=cheat
       // acciones de juego decodificando data[]/payload (casting, mover prop, editar mundo)
       DgsVerdict (*validateAction)(uint32_t actor, const uint8_t* blob, uint16_t n,
                                    const DgsWorldQuery* w);
       // persistencia opaca (el DGS solo mueve bytes)
       uint16_t (*serializeEntity)(const DGS_EntityTransfer* e, uint8_t* out, uint16_t cap);
       void     (*deserializeEntity)(DGS_EntityTransfer* e, const uint8_t* in, uint16_t n);
   } DgsGameModule;
   const DgsGameModule* dgs_game_module_v1(void);  // símbolo exportado por CADA juego
   ```
2. **`lib<proyecto>_rules` — el MÓDULO POR PROYECTO (física + casting).** Para Survival: `libsurvival_rules`.
   Implementa la vtable. Contiene: integrador de movimiento + colisión contra el heightfield del engine,
   resolución de casting (maná/cooldown/rango/LoS — **sin “aprender”**, §4c), reglas de qué `EntityState`
   permite mover/interactuar, y decodificación de WORLD-DELTAS (§4d). Se compila DOS veces:
   - **estática** dentro del cliente (predicción, 60+fps), y
   - **`.so`** con `-fPIC` que exporta `dgs_game_module_v1` para el DGS.
3. **`haruka_simbase` (opcional, compartida del engine).** Helpers reusables que el módulo por proyecto usa
   para no reescribir: query de altura de terreno, math, colisión cápsula-heightfield, scaffolding del tick
   fijo. Vive en `haruka-cpp/`. Mantiene pequeños los módulos por-proyecto. (Si prefieres, la física va
   entera en el módulo por-proyecto y esta lib no existe — es una decisión de reparto, no de arquitectura.)

## 3. Cómo se hace DINÁMICO por proyecto

- El DGS (`anticheat_node`/`zone_node`) al arrancar: `dlopen(getenv("GAME_MODULE_SO"))` →
  `dlsym("dgs_game_module_v1")` → comprueba `abiVersion` → guarda el `const DgsGameModule*`. Si no hay módulo
  → usa el `validate()` genérico de posición actual (fallback, otros juegos sin reglas).
- **Todo lo específico del juego pasa por la vtable.** El DGS nunca conoce “maná” ni “árbol”: opera sobre
  `EntityState`/`Stats`/`data[]` y delega la semántica al módulo.
- Versionado: `dgs_game_module_v1`, `_v2`… El DGS soporta N versiones; romper el ABI = símbolo nuevo, no
  editar el core.
- Nuevo proyecto = escribir su `lib<proj>_rules` contra `dgs_game_module.h` + apuntar `GAME_MODULE_SO`. Cero
  cambios en el DGS.

## 4. Anti-cheat (server, vía el módulo) — con tus reglas

Todo lo que el cliente afirma se revalida contra el estado autoritativo del DGS.

- **4a. Movimiento** (`validateMove` + `step`): el módulo re-simula/plausibiliza el movimiento (colisión vs
  terreno) → speed/teleport/fly. Sustituye al `validate()` hardcodeado; el DGS mantiene el genérico como
  fallback. El player es entidad DGS → esto vive 100% en la capa de validación del DGS + módulo.
- **4b. Objetos/props** (`validateAction`): el server es dueño del `EntityState`. Mover exige `MOVING|
  PHYSICS_OWNED` + ownership; interact/destroy/consume por su bit. Mover prop estático o entidad ajena →
  rechazo + violación.
- **4c. Magia (SIN aprendizaje)** (`validateAction`): valida **maná ≥ coste + cooldown + cast-rate + rango +
  LoS + fuente/elemento legal**. NO hay gate de “aprendida/requisitos” (quitado de v1). Descuenta maná
  autoritativo y aplica efecto; rechazo imposible repetido → violación.
- **4d. Edición de objetos/terreno = “cómo + dónde”** (nuevo `PKT_WORLD_DELTA`): el cliente NO manda el
  estado del mundo; manda **la operación**: `{ op, punto/área, params }` (p.ej. `DIG radius=r at (x,z)`,
  `PLACE modelId at pose`, `DAMAGE uuid amount`). El módulo valida (¿puede el actor editar ahí? ¿tiene el
  ítem/maná? ¿área legal?) y, si pasa, el DGS **replica la MISMA delta** a los vecinos y la persiste. Barato
  en ancho de banda y determinista (todos aplican la misma op). Encaja con el `DeformationField` del engine
  (deltas sobre terreno procedural) — ver [[deformable_terrain]].

Cada rechazo → `Violation{type,peso}` → `SuspicionScore` con decaimiento → LOG→CORRECT→KICK→TEMPBAN→PERMABAN;
ban a nivel cuenta vía HeadServer/persistence (MongoDB) → persistente.

## 4bis. Estado de MUNDO/ENTORNO autoritativo (posiciones planetarias + viento/viento marino)

No todo es por-entidad: hay **estado de simulación global** que afecta a gameplay/física/anti-cheat y que
**todos los clientes deben ver idéntico**, o el mundo diverge (unos con marea alta, otros baja; unos con
viento, otros no) y la validación falla:

- **Posiciones de cuerpos celestes** (Sol/luna/planetas): dirigen el **día/noche**, la **iluminación** y,
  vía la luna, las **mareas** (`u_tideHeight`, nivel radial del mar) → y con ello el oleaje/espuma. Ver
  [[heliocentric_plan]] (mecánica orbital Kepler que ya existe).
- **Campos de física de entorno**: **viento** y **viento marino** (dirección + fuerza) → dirigen el oleaje
  Gerstner del agua (`u_windDir`/`u_windStrength`), el movimiento de vegetación/props, y pueden afectar
  proyectiles/vehículos. Ver [[fluid_softbody_system]] (WindField/WindZone en `physics/`).

**Diseño:** el DGS mantiene un **reloj de mundo autoritativo** (tiempo de simulación + semilla) del que se
DERIVAN deterministamente las posiciones celestes (Kepler) y los campos de viento por región. No se replica
un estado pesado: se replica **el reloj + parámetros** (época, semilla de viento por zona, override de
tormentas) en un `PKT_WORLD_STATE` de baja frecuencia; **cliente y server evalúan la MISMA función** (en el
Sim Core / módulo por-proyecto) → mismas mareas/viento/olas en todos lados, sin mandar el oleaje vértice a
vértice. El anti-cheat usa ese mismo entorno (p.ej. validar empuje de viento, o que la marea no "teletransporte"
un barco). Las **tormentas/eventos** = overrides puntuales que el server difunde (región + ventana temporal).
Encaja con "mandar solo cómo+dónde": el viento es un parámetro por zona, no un campo denso.

## 5. Cambios a hacer EN el DGS (`~/Documents/GitHub/dgs/`)

- **C1. Host de plugin**: `dlopen`/`dlsym` del módulo en `anticheat_node` (y `zone_node` para predicción).
  Fallback al `validate()` actual si no hay módulo. (~1 archivo nuevo `game_module_host.{h,cpp}` + init.)
- **C2. `validate()` → módulo**: reemplazar la función estática por `module->validateMove(...)`; mantener la
  genérica como default.
- **C3. Nuevos packets**: `PKT_ACTION` (casting/interacción/mover-prop) y `PKT_WORLD_DELTA` (cómo+dónde) +
  su `pack/unpack` en packet.cpp. Hoy solo hay transform/chat/transfer.
- **C4. Predicción en zone_node**: hoy solo reenvía. Añadir tick fijo que llame `module->step` para
  dead-reckon de ghosts y revalidar contra estado predicho (tu “que el DGS haga predicciones”).
- **C5. Escalado de baneos**: hoy el anticheat solo hace `cout "CHEAT detectado"` y descarta el paquete.
  Añadir `SuspicionScore` + reporte a persistence/head para ban de cuenta.
- **C6. Persistencia por módulo**: `persistance_node` guarda el blob de `serializeEntity` (opaco) además de
  pos/stats.
- **C7. Reloj de mundo + `PKT_WORLD_STATE`** (§4bis): época/tiempo de sim + semillas de viento por zona +
  overrides de tormenta, difundido a baja frecuencia. Cliente y server derivan mareas/viento/olas de la MISMA
  función (Sim Core). Head/Orchestrator es el dueño del reloj.

## 6. Optimizaciones detectadas en el DGS (aparte del anti-cheat)

- **O1. Ancho de banda**: `broadcastToClient` envía `sizeof(EntityTransfer)` (~4 KB con `data[4096]`) por
  entidad y cliente cada tick, pese a que el README promete “delta compression con `dataSize`”. Enviar solo
  `offsetof(data)+dataSize` bytes (o el `GhostDelta`, que es lo suyo). Ganancia enorme con muchos jugadores.
- **O2. Estructuras**: `std::map<uint32_t,...>` en anticheat (`lastKnown`) y zone (`lastSnapshot`) →
  `unordered_map` (O(1) vs O(log n) por paquete).
- **O3. Modelo de plausibilidad**: `validate()` usa solo `stats.speed[0]` como maxSpeed y un fudge fijo
  `SCALE/1000`; el README menciona `latency_compensation` pero el código no usa la latencia real reportada.
  Mejorar: vector de velocidad real + RTT medido.
- **O4. Recepción UDP**: recibir de a un datagrama; usar `recvmmsg` para lotes bajo carga.
- **O5. TODO del propio DGS**: replicación de clientes/objetos entre servers, y quitar zone_nodes por conteo
  de jugadores (ya lo listan) → encaja con el tick/predicción de C4.

## 7. Fases

- **F0 — ABI + host** ✅ **HECHO (2026-07-10, verificado)**: `include/dgs/game_module.h` (ABI v1) + dlopen en
  anticheat_node (`loadGameModule`/`checkMove` con fallback al validate() integrado) + `examples/game_module_example.cpp`
  → `libgame_module_example.so` (validateMove replica el validate() histórico). Test dlopen: carga ok, mov legal→1
  teleport→0. Arreglados 2 bugs preexistentes del DGS (packet.cpp `GhostDelta.data`→`st`; IPv6 `sin_port` roto → build con
  `-DHARUKA_USE_IPV6=OFF`). **Plugin carga y valida igual que hoy sin romper nada.**
- **F1 — Movimiento**: `step`/`validateMove` reales (colisión terreno) + O2/O3.
- **F2 — Acciones/props**: `PKT_ACTION` + `validateAction` + EntityState authority (C3, 4b).
- **F3 — Magia**: casting server-side (4c), sin aprendizaje.
- **F4 — World-delta**: `PKT_WORLD_DELTA` (4d) + replicación + `DeformationField`.
- **F5 — Baneos + persistencia módulo**: C5 + C6 + O1.

## 8. Decisiones abiertas

- **D1. Reparto física**: ¿toda en `lib<proj>_rules`, o base común en `haruka_simbase`? (rec: base común mínima).
- **D2. Predicción del player en cliente**: ¿reconciliación por tolerancia (rec) o el cliente confía en su
  propia sim y el server solo corrige en violación? (el modelo actual del DGS es lo segundo, más simple).
- **D3. Formato del payload de acción** (`data[]`/PKT_ACTION): esquema binario propio vs reutilizar el JSON de
  abilities compilado a binario.
