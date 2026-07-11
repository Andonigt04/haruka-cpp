# Updates
- **DGS implementacion completa** con networkLib y gestion de objetos de juego con fisicas lo integre directamente el juego y el motor lo admita y que tenga cierto layout para el network de **DGS** como *lib*. **Fisicas** a nivel de servidor como lib para que el anticheat lo valide y asi esten en tanto local(cliente), como server. Plantear como se implementara a nivel de server el sistema de guilds titles, shop/traiding, chat etc..., de forma coherente y mas o menos unificada sin agrandar codigo de **DGS** sin que otro proyecto lo tenga si, no lo necesita.
  - **PLAN CERRADO (v2):** ver `docs/guides/PLAN_DGS_ANTICHEAT.md`. Arquitectura = DGS genérico + **módulo de reglas POR PROYECTO** (física+casting) cargado por `dlopen` (ABI C `dgs_game_module_v1`), compilado estático en cliente (predicción) y `.so` para el DGS (validación). DGS real en `~/Documents/GitHub/dgs/`. Anti-cheat: movimiento + objetos/props (via `EntityState`) + magia (sin aprendizaje) + world-edit como "cómo+dónde" (`PKT_WORLD_DELTA`). Cambios+optimizaciones del DGS listados. Empezar por **F0** (ABI+host dlopen+esqueleto libsurvival_rules). *Decisión pendiente: hacer ahora vs esperar.*
- **RHI Basico**, con intencion de hacer que funcione y un test de **Vulkan** basico de creacion basica y *resize* de la ventana.
  - **SPIKE HECHO (2026-07-10):** `tests/vk_resize/` (standalone, no toca el motor) — SDL3+Vulkan+ImGui con recreación robusta de swapchain en Wayland (gotchas: currentExtent=0xFFFFFFFF, recrear por evento SDL no solo OUT_OF_DATE). **Probado: resize funciona limpio** (~37 recreaciones sin crash/errores). Conclusión: Vulkan es viable → NO hace falta RHI-solo-GL primero. *La migración RHI completa (1399 llamadas GL) queda diferida: NO ayuda a perf (GPU ociosa, CPU-bound) y no hay driver concreto aún.* Falta: instalar `vulkan-validation-layers` para cerrar el veredicto.
  - **RHI EN CURSO (2026-07-11):** interfaz + backend GL (`src/rhi/` + `src/rhi/opengl/`) hechos y verificados. Migrados por **leaf-swap** (API pública intacta + escotillas `native*`). VERIFICADOS ejecutando (arnés aislado): Texture, RenderTarget, Shader, hdr, bloom, ssao, shadow, point_shadow, cascaded_shadow, gbuffer, mesh, model, gpu_instancing, ComputeShader, ComputePostProcess, GpuHeightfield, skeletal, virtual_texturing, shallow_water, softbody, fluid. RHI ganó: MRT, depth-textura/cubemap, sampler de sombra, formatos (R11G11B10F/R8/D24/D32F/RG32UI/RGBA16UI/RGB8), buffers Stream (`uploadBuffer`) + Dynamic + `copyBuffer`, pipelines desde ruta/SPIR-V/**source inline**.
    - **⚠️ terrain_renderer + water_renderer: MIGRADOS PERO COMPILE-ONLY — PENDIENTE VALIDAR EN EL JUEGO.** Son el core GPU-driven perf-crítico (pools de capacidad fija, growPool con `copyBuffer`, indirect multidraw). NO se pudieron verificar headless (necesitan planeta+cámara+LOD). El draw indirecto (`cmdBuf`/`drawSSBO`, stream por-frame con orphan `STREAM_DRAW`) se dejó en GL a propósito. **Antes de confiar: arrancar el juego con un planeta y confirmar que terreno y agua se ven bien (chunks, LOD, morph, mar).** Si algo falla, el fallback GL directo sigue ahí (sin device).
    - **Orquestación** (`application_render`: quad, bloom mip-chain, UBOs per-frame/object; `ui_world_panel`: UBO + malla Stream): migrados ✅. `buffer_objects.h` (muerto) BORRADO ✅.
    - **ibl**: migrado (cubemaps env/irradiance/prefilter + BRDF LUT + geometría; el render-a-cara vía captureFBO sigue GL). **COMPILE-ONLY** como terrain/water — validar en engine (necesita HDR+render de convolución). RHI ganó: cube color en `createTexture` + formatos RGB16F/RG16F.
    - **ESTADO: renderer migrado al 100% en creación de recursos.** 0 `glGen*`/`glBufferData`/`glTexImage`/`glCreate*` de recursos fuera de `src/rhi/opengl/` (salvo fallbacks sin-device y draw-command streams indirectos, dejados a propósito). Lo que sigue en GL = bind/draw/dispatch/VAO/attrib-pointers → es el **refactor PSO real** (diferido, ver abajo). VALIDAR EN JUEGO: terrain, water, ibl (compile-only).
  - **TODO — TAMAÑOS DE RT ESTÁTICOS:** `bloom`, `ssao`, `hdr` se crean con `width,height` FIJOS y NO siguen el resize de la ventana (quedan a la resolución inicial → escalado/aliasing tras redimensionar). Hay que recrearlos (o reasignar su render target del RHI) al evento de resize. `point_shadow` (y los shadow maps) SON estáticos a propósito: su tamaño es la resolución del shadow map, no depende de la ventana → dejar como está. Relevante para el backend Vulkan (el spike vk_resize ya trata la recreación de swapchain).
  - **TODO (diferido) — REFACTOR PSO REAL:** el leaf-swap NO crea Pipeline State Objects de verdad; `Shader` sigue siendo solo-programa y el estado (`glEnable`/`glBlend`) + vertex layout siguen dispersos. Para mapear 1:1 a Vulkan y optimizar (cache de PSOs, menos cambios de estado) hay que reestructurar shader+vertexlayout+estado en objetos Pipeline inmutables y reescribir los ~12 consumidores de `Shader`. Hacer DESPUÉS de que el leaf-swap deje 0 GL fuera de `src/rhi/opengl/`. Ojo: el loader prefiere **GLSL source** sobre SPIR-V por fiabilidad en Mesa/AMD (`glSpecializeShader` incompleto) → el PSO debe conservar ese fallback.
- Terreno con seleccion de texturas en bioma *segun inclinacion* (para hacer que la inclinacion de la montaña sea roca); hacer que como tal tenga diferentes tipos de texturas como tierra o hierva si es que tiene, o nieve sobre la tierra....
- Sistema de fisicas unificado para vehiculos, monstruos o jugadores
- Creacion de caminos por construccion de mallas trozo de tierra lo aplanas en el suelo se "sobrepone" al terreno en la posicion, *como poner grava sobre tierra*.
- Implementacion de Guilds/Parties/Allience/Groups/....


---

Incoherencia en el objectType y renderKind

---

# Cinematics system (showcase — alta prioridad para el portfolio)
Lo que más vende un motor en vídeo/trailer. Plan:

- **Tipos de pista**: cámara (pos/orientación/FOV), transform de objeto, luz (color/intensidad),
  post-proceso (exposición/bloom/DoF), hora del día/sol, y **triggers de evento** (spawn, sonido,
  partículas, llamada a GameInterface).
- **Editor**: integrar en el editor `haruka` (ImGui) — colocar keyframes de cámara desde la vista actual,
  curva editable, preview en viewport. (Reusa `setEditorTarget`/`renderFrameContent`.)

---

# Namespace migration — pending (cosmetic, no collision risk)
Globals → DONE (0 classes in the global namespace; Vosk-style collisions impossible).
Remaining = classes ALREADY in `Haruka::` to move into per-folder sub-namespaces. Pure
organization; cascades (nested namespaces like fluid::, std specializations, forward-decl webs)
make scripting risky — do file-by-file. ~103 left:

- **renderer/ → Haruka::Renderer** (25): Animation, Animator, Bone, BoneInfo, FloatingIslandRenderer,
  FluidRenderer, KeyPosition/Rotation/Scale, NodeData, ParticleSystem, RenderMesh, ShallowWaterRenderer,
  SkinnedMesh/Model/Vertex, SoftBodyRenderer, Terrain, TerrainPatch, TerrainRenderer, WaterRenderer (+ DrawStats/Entry/P internals).
- **core/ → Haruka::Core** (46): AssetPaths, ChunkCache, Component, DeformationField, GameInterface,
  Locale, LODSystem, NoiseGenerator, Project, Propagation, SceneLoader, SceneManager, SceneObject,
  SceneValidator, TerrainGenerator, TerrainStreamingSystem, WorldSystem, *Component, terrain/* structs…
- **game/ → Haruka::Game** (13): Character, CharacterInputProvider, Inventory, ItemDef, ItemRegistry,
  ItemStack, Planet, PlanetarySystem, RespawnData, SpawnPoint, SpawnSystem…
- **tools/ → Haruka::Tools** (10): Event, EventManager, LogEvent, ObjectEvent, PlanetChunkKey, Profiler,
  ScopedTimer, Section, ChunkData, LayerSettings.
- **audio/ → Haruka::Audio** (4): AudioLoader, AudioManager, AudioSystem (+ World).
- **physics/ → Haruka::Physics** (2): WindField, WindZone.
- **cinematics/ → Haruka::Cinematics** (2): CinematicSequencer, Key.
- **settings/ → Haruka::Settings** (1): SettingsManager.

Method that works: per file, wrap `namespace Haruka { … }` → `namespace Haruka { namespace Sub { … } }`,
add `namespace Haruka { using Sub::X; … }` back-compat, fix forward-decls (`class X;` inside Haruka →
`namespace Sub { class X; } using Sub::X;`). Build after EACH file.

---

# Render issues (reported) — TODO
1. **Normal (placed) scene objects have NO LOD** — they draw at full detail at any distance (no
   distance cull / impostor), unlike terrain. Need a distance-based cull/LOD for scene Models.

---

# Agua — PENDIENTE (no OK, aparcado 2026-07-10)
La espuma de cresta per-vértice del cascarón de océano (rejilla 192²) aliasa/interpola sobre triángulos
grandes → manchas/"triángulos" blancos random sin relación con el oleaje. Fade por distancia (400-2000m,
water.frag) mejoró pero NO lo resuelve. Real fix pendiente: foam en fragment-space (ruido) o bajar la
dependencia per-vértice del oleaje en el cascarón. Perf: `water.draw` ~3ms fragment-bound con pantalla llena
de mar (sky-reflect+SSS+foam+spec+oceanElevKm per-píxel). Ver memoria perf_cpu_bound_2026-07 (saga agua).

---

# Perf (ms) — pendientes (2026-07-10, ver memoria perf_cpu_bound_2026-07)
Frame CPU-bound, GPU ociosa. Mucho mejor tras la saga (frames de 46ms → 12-30ms), pero quedan cuellos:

- **LOD en frames de RECOMPUTE (~22ms, el peor caso, en la COSTA mirando al mar).** Desglose medido:
  `lod.recompute` 8.6ms (= `tree` 4.8 recursión + `balance` 2.4 balanceo 2:1) · `lod.stream` 5.1 · `lod.desired`
  4.7 (rebuild de clausura). Ya no hay un único dominante → micro-opts seguros dan poco. **Palanca real = el
  CONTEO DE CHUNKS** (res 8-9k en la costa) que infla recompute+stream+desired+drawset a la vez → subir targetPx
  / reducir reach de costa (VISUAL, requiere OK). Alternativa grande: recompute en hilo worker (GPU ociosa 42ms).
- **`water.draw` ~3.1ms — AHORA el cuello #1 en frames sin recompute** (mirando al mar, pantalla llena de agua →
  fragment-bound: el shader de océano hace sky-reflect + SSS + foam + spec sol/luna + `oceanElevKm` per-píxel).
  Pendiente: expandir los sub-scopes `water.build/upload/drawcall` en el HUD para saber si es build (CPU, al
  moverse) o drawcall (GPU/fill). Fixes listos: bajar densidad de la rejilla del océano con distancia, o gatear
  efectos del shader lejos.
- **`lod.catchup` ~5ms en frames de throttle** (subida de backlog de agua/terreno quieto). Mi memoria decía ~0;
  reasomó. Menor prioridad (frame barato) pero recorte fácil.
- **Micro-opt A1 hecha** (dedup de residencia en recursiveProcess, 8→4 lookups): output-idéntico, bajó ~3ms el
  frame pero NO por donde se predijo (lod.recompute casi igual; el ahorro salió en stream/varianza).
