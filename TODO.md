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
1. **Water cuts/clips objects.** The ocean shell intersects props/objects at the waterline (depth/
   transparency/order). Objects partially submerged get sliced by the water surface.
2. **Procedural props (trees/rocks) render inconsistently** — sometimes visible, sometimes not / look
   wrong. Likely frustum culling or the altitude-LOD cap / scatter visibility vs camera.
3. **Normal (placed) scene objects have NO LOD** — they draw at full detail at any distance (no
   distance cull / impostor), unlike terrain. Need a distance-based cull/LOD for scene Models.
