# Plan: Sandbox System (spawn de objetos/sistemas en runtime)

> Estado: PROPUESTA (sin implementar). Visión: modo sandbox PARA EL JUGADOR —
> colocar agua, cascadas, softbodies, emisores y objetos en el mundo mientras
> juega, con clic/UI. Vive en Survival (gameplay), apoyado en el engine.

## 0. Qué hay ya (auditoría)

**Reutilizable:**
- `World` (Survival) ya posee todos los sims (shallow-water, PBF, XPBD, hybrid) y
  sabe crearlos/destruirlos. Hoy los crea en `init()`; el sandbox los creará bajo demanda.
- `getTerrainHeightAt(worldPos)` (engine) → raycast/colocación sobre el terreno.
- `MotorInstance::getCamera()` → posición + frente para apuntar.
- Input: `init.cpp` ya registra acciones y captura SDL en `gameOnEvent` (teclas +
  mouse-motion). Falta capturar BOTONES de ratón.
- `WindField` (zonas) → modelo de datos ya listo para "pincelar" viento/tormentas.

**NO reutilizable directamente:**
- `game/spawn_system.h` (engine) es para spawn points de NPCs/respawn de gameplay
  clásico, NO para colocación libre sandbox. Lo dejamos como está.

## 1. Arquitectura

Una capa nueva en Survival: **`Sandbox`** (scripts/sandbox.{h,cpp}), poseída por
`World` o por `init.cpp`. Responsabilidades:
1. **Estado de herramienta**: qué va a colocar el jugador (enum `SandboxTool`).
2. **Targeting**: dónde colocar → ray desde cámara, intersección con el terreno
   (marcha de rayo sobre `getTerrainHeightAt`, barato) → punto de colocación + un
   marcador visual (preview).
3. **Spawn**: instancia el sistema/objeto elegido en ese punto.
4. **Gestión**: lista de cosas colocadas (para borrar/limpiar/persistir).

`World` expone métodos para que `Sandbox` añada cosas a los sims existentes sin
duplicar lógica (p.ej. `World::spawnWaterSource(worldPos)`,
`World::spawnJelly(worldPos)`, `World::spawnSoftbodyFlag(worldPos)`).

## 2. Herramientas del sandbox (incremental)

Empezar con 3-4 de alto impacto que reusan sistemas ya construidos:

| Tool | Qué coloca | Reusa |
|------|-----------|-------|
| WaterSource | un manantial que llena el heightfield | shallow-water `addSpringWorld` |
| Jelly | un cubo softbody | `makeBox` (XPBD) |
| Cloth | una bandera/tela | `makeCloth` (XPBD) |
| WindZone | una zona de viento/tormenta | `WindField::add` (ya es el modelo de tormenta) |
| (futuro) Cascade | emisor PBF directo | PBF `add` + coupler |
| (futuro) Light/Prop | luz o malla en la escena | SceneManager::addLoadedObject |

Cada tool = una entrada en el enum + un caso en `Sandbox::placeAt(worldPos)`.

## 3. Input / UX

- **Selección de tool**: tecla cicla (p.ej. `Tab`) o una **hotbar** ImGui (1-9).
  Registrar acciones nuevas en `registerActions()`.
- **Colocar**: clic izquierdo → `gameOnEvent` captura `SDL_EVENT_MOUSE_BUTTON_DOWN`
  (hoy solo captura motion → añadir). Raycast desde el centro de pantalla (o desde
  el cursor si el ratón está libre).
- **Borrar**: clic derecho sobre un objeto colocado (pick por proximidad).
- **Preview**: en cada frame, dibujar un marcador (esfera/contorno) en el punto
  donde caería el objeto, con el color de la tool activa.
- **HUD**: la hotbar + el nombre de la tool activa (reusa el HUD ImGui existente).

## 4. Targeting (raycast al terreno)

Sin física de raycast completa: marcha de rayo barata.
```
origin = camera.position; dir = camera.getFront();
for t in [1m .. 500m] step adaptativo:
    p = origin + dir*t
    surfaceR = planetRadius + getTerrainHeightAt(p)   // along planet up
    if |p - planetCenter| <= surfaceR: return p (hit)
```
Refinar con bisección en el último paso. Devuelve punto de colocación.
(Para props pequeños basta; para precisión fina, RaycastSimple del engine.)

## 5. Persistencia (encaja con el plan de saves)

Cada cosa colocada = un registro {tool, worldPos, params}. Esa lista ES el "estado
sandbox" del jugador → se serializa en el save (ver PLAN_PACKAGING_SAVES_CONFIG.md
§6: seed + diffs del mundo). Al cargar, se re-spawnan. Coherente con que el mundo
es procedural y el save son los cambios del jugador.

## 6. Fases de implementación

1. **Targeting + preview**: raycast al terreno + marcador visual. Sin spawn aún.
   Demostrable: un marcador que sigue donde apuntas.
2. **Una tool (WaterSource)**: clic coloca un manantial permanente. Reusa
   shallow-water. Demostrable: pintar agua en el mundo.
3. **Hotbar + más tools** (Jelly, Cloth, WindZone): selección + 3-4 tools.
4. **Borrado + gestión**: clic derecho borra; lista de colocados.
5. **Persistencia**: guardar/cargar la lista (depende del save system).

Fase 1-2 son el MVP jugable (apuntar + colocar agua).

## 7. Lo que hay que DES-hardcodear primero (dependencia)

Para que el sandbox sea de verdad moldeable, conviene parametrizar lo que hoy es
fijo en `World` (hoy todo son constantes en world.cpp):
- patch de agua (span/cells), caudal de manantial, umbral de cascada, re-anchor %.
Moverlos a una struct `SandboxConfig` o a campos editables. NO es bloqueante para
el MVP (puede colocar con los defaults actuales), pero es el paso natural después.

## 8. Decisiones abiertas (para más adelante)
- ¿El sandbox es un MODO aparte (menú) o siempre disponible con una tecla?
- ¿Límite de objetos (presupuesto de CPU) — cuántos manantiales/jellies a la vez?
- ¿Las tools del jugador son las mismas que las de dev, o un subconjunto?
