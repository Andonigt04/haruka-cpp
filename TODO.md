# TODO — HarukaEngine

Estado a 2026-07-15. Lo HECHO se resume en una línea; el detalle vive en las guías (`docs/guides/`).
La dirección del JUEGO (estilo, multijugador, construcción) está en `Survival/docs/DIRECCION.md`.

---

## ✅ Hecho (no re-abrir sin motivo)

- **RHI + PSO completo.** Interfaz + backend GL (`src/rhi/`). Renderers sin GL crudo: terrain, water,
  fluid, ibl, escena, partículas, bloom, sombras — todos por pipeline. Validado en juego.
  Queda de GL: los 3 compute al PSO (trivial) y los blits del fluido (falta `blit` en el RHI).
- **Reversed-Z + far infinito + D32F.** Depth uniforme de un guijarro a un planeta.
- **Terreno V3 (F0–F7 + F10).** Tectónica → erosión → hidrología → clima → biomas → props, todo
  dirigido por UN campo. Reversed-Z, agua sin re-derivar terreno, MESO (detalle erosionado que pisas,
  ON por defecto), y **F10: la física muestrea LA MALLA** (una sola fuente de verdad para la altura →
  se acabaron los props flotando / caer a través del suelo). Relieve realista: cordilleras a ~8.7 km.
  Detalle en `docs/guides/PLAN_TERRENO_V3.md`.
- **Props por prototipos + instancing.** Ya no se genera una malla por objeto: ~50 prototipos por
  seed, cada prop es índice + matriz + tinte. `GPUInstancing` migrado al PSO.
- **Perf.** Frame CPU-bound domado (readback mapeado persistente, direcciones de rejilla derivadas en
  GPU, pool de hilos, meso multi-worker, vigilante de memoria). `planetary.update` p99 ~18 ms.
- **Herramientas.** `SURVIVAL_SPAWN=lat,lon` (mundo reproducible) · `HARUKA_PROF_LOG=N` (profiler a
  stderr). Suite: **74/74**.

---

## 🎯 Lo que queda — por orden de valor

### 1. Estilo: CEL-SHADING (anime) — **antes de producir más arte**
Decisión del autor (`DIRECCION.md`): el juego es anime-like full. El look actual es realista
(triplanar PBR), así que hay que rehacer el look pass: rampa de luz (2-3 tonos), contorno, follaje y
agua estilizados. ⚠️ Cada textura/modelo realista que entre ANTES de esto es trabajo que se tira.
Decisiones abiertas: ¿contorno? · ¿2 o 3 bandas de luz? · ¿se quitan las texturas de bioma?

### 2. Mundo habitado: construcción modular + caminos
- **Kit modular con SOCKETS** (esquina, vano, remate) + tabla `región → kit` + snapping. NO modelar
  100 paredes por región: combinar ~15 piezas. Nace **autoritativo en servidor** (multijugador).
- **Roads** por splines, editable, que deforman el terreno (ya hay `flatten`) y siguen valles/collados
  (el campo ya los conoce).
- **Lugares hechos a mano** (ruinas, torres): un mundo 100% procedural se siente vacío.

### 3. Vida: cuerpos procedurales + audio
- **Monstruos con generación de cuerpo** (esqueleto → miembros → tamaño, animación por IK). El BIOMA
  decide qué aparece (reusa los campos que ya colocan los props). En testing.
- **Audio ambiental** por bioma (viento en árboles, ríos — el campo sabe dónde). La propagación (el
  sonido rodea paredes) ya existe: es material de trailer.

### 4. Jugabilidad
- **Interacción física, mínima UI** (`DIRECCION.md`): los grimorios son OBJETOS del mundo que se
  pasan en físico → UI en espacio-mundo, estado sincronizado. HUD del jugador: chat, grupo, inventario.
- **Progresión** — solo cuando existan materiales/recursos de verdad.

---

## 🐛 Deuda técnica concreta (bugs, no features)

- **Caché de disco** (`HARUKA_DISKCACHE=1`): con la caché poblada el streaming no se asienta → 3 tests
  en rojo. Sin diagnosticar. Sería justo lo que acelera el tiempo de carga.
- **SSAO es un HUECO**: el ajuste está en el menú y el flag viaja al shader, pero **no hay pase** que
  genere la oclusión. O se implementa o se quita el ajuste (hoy engaña).
- **RT estáticos**: `bloom`/`ssao`/`hdr` se crean con tamaño FIJO y no siguen el resize de la ventana.
- **Objetos de escena SIN LOD**: se dibujan a full detalle a cualquier distancia (a diferencia del
  terreno). Con el instancing ya migrado, es el momento de dárselo (cull + LOD + impostores).
- **Clamp de costa**: puede dejar un pequeño acantilado en el borde exacto tierra/mar (a vigilar tras
  el nuevo relieve).

---

## 🔮 Grandes, decisión del autor (no técnicas)

- **Vulkan backend** (`src/rhi/vulkan/` vacío). La costura RHI/PSO está lista → es un backend, no un
  rewrite. Spike de resize hecho (`tests/vk_resize/`). ⚠️ **NO sube los fps** (el frame es CPU-bound):
  tiene valor de motor/portfolio, no de optimización. Al implementarlo: cargar settings ANTES de crear
  el device, recompilar SPIR-V con `--target-env=vulkan`, añadir `blit` al RHI.
- **DGS / anticheat**: plan cerrado en `docs/guides/PLAN_DGS_ANTICHEAT.md`. Física + reglas por
  proyecto vía `dlopen`. *Decisión pendiente: ahora vs esperar.*
- **Cinematics** (editor ImGui): lo que vende un motor en vídeo. Alta prioridad para portfolio.

---

## 🧹 Cosmético (sin riesgo, sin prisa)

- **Namespaces**: ~103 clases ya en `Haruka::` a mover a sub-namespaces por carpeta. File-by-file.
- **Código muerto**: `MeshLOD`, `SoftbodyRenderer` sin call-site. `GPUInstancing` YA no es muerto
  (props). `SSAO` es el hueco de arriba, no basura.

---

## ⏸️ Aparcado a propósito

- **Cuevas** e **islas flotantes**: hasta que la superficie sea creíble (tienen la tectónica y la
  hidrología como prerrequisito, ya hechas — se pueden retomar).
