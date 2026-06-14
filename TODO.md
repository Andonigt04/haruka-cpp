Incoherencia en el objectType y renderKind

---

# Cinematics system (showcase — alta prioridad para el portfolio)
Lo que más vende un motor en vídeo/trailer. Plan:

- **Timeline / sequencer** data-driven (`.cine` JSON): pistas con keyframes + easing
  (Catmull-Rom / Bézier) — interpolación suave en double precision (camera-relativo ya resuelto).
- **Tipos de pista**: cámara (pos/orientación/FOV), transform de objeto, luz (color/intensidad),
  post-proceso (exposición/bloom/DoF), hora del día/sol, y **triggers de evento** (spawn, sonido,
  partículas, llamada a GameInterface).
- **Reproducción**: play/pausa/scrub, **blend in/out** desde la cámara de juego (no cortes secos),
  barras letterbox y **profundidad de campo** para el look cine.
- **Editor**: integrar en el editor `haruka` (ImGui) — colocar keyframes de cámara desde la vista actual,
  curva editable, preview en viewport. (Reusa `setEditorTarget`/`renderFrameContent`.)
- **Export**: captura de frames / secuencia PNG → vídeo (para trailers). Modo "demo reel" en bucle.
- Reusa: `Camera`, UBO per-frame, post stack (`_postScene`/bloom/FXAA), `AudioManager`, scene.

---

# Known issues (cinematics / loading / streaming) — TODO
Surfaced while polishing the intro showcase. Root cause is shared: **streaming can't keep
up with a fast interplanetary camera**, and the cinematic camera isn't clamped to bodies.

1. **Loading screen still "jumps" (finishes early).** Even after switching to the real backlog
   (`queued + in-flight` via `getQueuedChunks`), it can complete before the world is full.
   Likely: the LOD requests chunks in **successive waves as the player settles**, so the desired
   queue hits 0 between waves and the settle window passes. Fix ideas: also wait for `cached`
   (resident) to stop growing; require a longer/again-empty settle; or expose a definitive
   "target chunk set size" from the LOD so completion is `loaded == target`.

2. **Cinematic camera clips INTO the sun / planet (zoom in→out, ends inside).** The `.cine`
   anchors put the camera too close (sun) and the Catmull-Rom descent (orbit→ground, looking at
   the planet centre) **overshoots below the surface**. Fix ideas: push `near_sun` much farther
   out; clamp the cinematic camera radius to ≥ `planetRadius + margin`; don't look at the planet
   centre on descent (look at the horizon); make descent keyframes monotonic in radius.

3. **Approaching the planet: terrain not visible but water is (full-res).** The ocean is a single
   Gerstner SHELL (always present, no streaming), while terrain is CHUNKED and **streams with the
   camera** — a fast fly-in outruns chunk generation, so you see the water sphere with no land.
   Fix ideas: pre-warm/await chunks at the orbit keyframe before descending; slow the descent;
   or redesign the shot to START at orbit (terrain can stream) instead of flying from the sun.

> Tension: the artistic "fly from the sun to the surface" fights the engine's per-camera terrain
> streaming. A faithful version needs either pre-generation along the path or a much slower fly-in.

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
1. **Water cuts/clips objects.** The ocean shell intersects props/objects at the waterline (depth/
   transparency/order). Objects partially submerged get sliced by the water surface.
2. **Procedural props (trees/rocks) render inconsistently** — sometimes visible, sometimes not / look
   wrong. Likely frustum culling or the altitude-LOD cap / scatter visibility vs camera.
3. **Normal (placed) scene objects have NO LOD** — they draw at full detail at any distance (no
   distance cull / impostor), unlike terrain. Need a distance-based cull/LOD for scene Models.
