# Diagramas de Flujo y Arquitectura

## 1. Relación de Propiedad entre Sistemas

```
┌──────────────────────────────────────────────────────────────────┐
│                         APPLICATION / EDITOR                     │
└────────────────────┬─────────────────────────────────────────────┘
                     │
                     │ Crea y mantiene
                     ▼
          ┌─────────────────────┐
          │   WorldSystem       │  ◄── Centro de orquestación
          │                     │
          │ ┌─────────────────┐ │
          │ │ Floating Origin │ │  Precisión astronómica
          │ │  (glm::dvec3)   │ │
          │ └─────────────────┘ │
          │                     │
          │ ┌─────────────────────────────┐
          │ │  m_planetarySystem (unique) │
          │ │                             │
          │ │   PlanetarySystem           │
          │ │   ┌───────────────────────┐ │
          │ │   │ m_cache               │ │
          │ │   │ m_generator           │ │
          │ │   │ m_renderer            │ │
          │ │   │ m_streaming           │ │
          │ │   │ m_lod                 │ │
          │ │   └───────────────────────┘ │
          │ └─────────────────────────────┘
          │
          │ ┌─────────────────────┐
          │ │ m_bodies[] (vector) │
          │ │ CelestialBody...    │
          │ └─────────────────────┘
          └─────────────────────────────────────────────────────────
                           │
                           │ Referencia (no propiedad)
                           ▼
                    ┌──────────────────────┐
                    │  SceneManager        │
                    │                      │
                    │ • m_objects[]        │
                    │ • m_registry[name]   │
                    │ • m_idRegistry[id]   │
                    └──────────────────────┘
                             │
                             │ Almacena
                             ▼
                    ┌──────────────────────┐
                    │  SceneObject         │
                    │                      │
                    │ • name               │
                    │ • type (string)      │
                    │ • position (double)  │
                    │ • ObjectType (enum)  │
                    │ • JSON config        │
                    └──────────────────────┘

```

---

## 2. Flujo de Carga de Escena

```
┌─────────────────────┐
│ project.hrk         │  ◄── Archivo JSON
│ scenes/scene.json   │
└──────────┬──────────┘
           │
           │ Parser JSON
           ▼
    ┌──────────────────┐
    │ SceneLoader      │  ◄── Valida estructura
    └──────┬───────────┘
           │
           │ Deserializa cada objeto
           │ ObjectType type = stringToObjectType(json["type"])
           │
           ▼
    ┌──────────────────────────────────┐
    │ SceneObject[] (temporal)         │
    │ ├─ name: "Earth"                 │
    │ ├─ type: "planet"                │
    │ ├─ position: [x, y, z] (double)  │
    │ ├─ terrainSettings: {...}        │
    │ └─ lodSettings: {...}            │
    └──────┬───────────────────────────┘
           │
           │ addLoadedObject(obj)
           ▼
    ┌──────────────────────────────────┐
    │ SceneManager::addLoadedObject()  │
    │                                  │
    │ • m_objects.push_back(obj)       │
    │ • m_registry[obj.name] = obj     │  ◄── O(1) lookup
    │ • m_idRegistry[hash] = obj       │  ◄── O(1) lookup
    └──────┬───────────────────────────┘
           │
           ▼ getAllObjects()
    ┌──────────────────────────────────┐
    │ Viewport / Renderer / Streaming  │
    │ accede a los objetos cargados    │
    └──────────────────────────────────┘

```

---

## 3. Flujo de Renderizado de un Planeta

```
Frame N: Render Loop
│
├─ 1. DETERMINE VISIBLE CHUNKS
│  │
│  └─ WorldSystem::update(dt, camPos)
│     │
│     ├─ toLocal(camPos) → obtiene posición relativa al origen
│     │
│     └─ PlanetarySystem::update(dt, camPos)
│        │
│        ├─ Para cada Planet en m_planets
│        │  │
│        │  ├─ PlanetChunkKey = calculateVisibleChunks(camPos)
│        │  │  ├─ Itera 6 caras (FRONT, BACK, TOP, BOTTOM, RIGHT, LEFT)
│        │  │  └─ Para cada cara:
│        │  │     ├─ Distancia a cara → LOD (0=cerca, 3=lejano)
│        │  │     └─ Si LOD 0: subdivide en 2x2 chunks (4 nietos)
│        │  │        Si LOD 1: subdivide en 2x2 chunks
│        │  │        ...
│        │  │
│        │  └─ Resultado: Lista de PlanetChunkKey visibles
│        │     Ej: [(FRONT, LOD=0, x=0, y=0), (FRONT, LOD=0, x=1, y=0), ...]
│        │
│        └─ TerrainStreamingSystem::updateQueue(visibleChunks)
│
├─ 2. CARGAR/GENERAR CHUNKS
│  │
│  └─ TerrainStreamingSystem::updateQueue()
│     │
│     ├─ newChunks = visibleChunks - cachedChunks
│     │
│     ├─ Para cada chunk en newChunks:
│     │  │
│     │  ├─ ¿Existe en disco (cached)?
│     │  │  │
│     │  │  ├─ YES
│     │  │  │  └─ m_cache->retrieve(chunkKey) → ChunkData
│     │  │  │
│     │  │  └─ NO
│     │  │     └─ m_generator->generateChunk(chunkKey, terrainSettings)
│     │  │        │
│     │  │        ├─ Perlin Noise en LayerSettings
│     │  │        │  ├─ layer[0] (freq=0.1): macroterrain
│     │  │        │  ├─ layer[1] (freq=1.0): medium details
│     │  │        │  └─ layer[2] (freq=4.0): fine details
│     │  │        │
│     │  │        ├─ Combine layers con persistence/lacunarity
│     │  │        │
│     │  │        ├─ Normal mapping (altura → normal)
│     │  │        │
│     │  │        └─ Retorna ChunkData
│     │  │           ├─ vertices[]
│     │  │           ├─ normals[]
│     │  │           ├─ indices[]
│     │  │           └─ key
│     │  │
│     │  └─ m_cache->store(chunkKey, chunkData)  ◄── Caché para frame siguiente
│     │
│     └─ evictChunks = cachedChunks - visibleChunks
│        │
│        └─ Para cada chunk en evictChunks
│           ├─ Si memoria < umbral
│           │  └─ m_cache->evict(chunkKey)  ◄── Libera RAM
│           └─ else
│              └─ Mantén en caché (probablemente visible pronto)
│
├─ 3. PROCESAR LODS
│  │
│  └─ LODSystem::calculateLOD()
│     │
│     └─ Para cada chunk visible
│        │
│        ├─ dist = distance(chunk.center, camPos)
│        │
│        ├─ if (dist < 100m)   → LOD = 0  (máx vértices)
│        ├─ if (dist < 500m)   → LOD = 1
│        ├─ if (dist < 2000m)  → LOD = 2
│        └─ if (dist < 10000m) → LOD = 3  (mín vértices)
│
└─ 4. RENDER
   │
   └─ Renderer::render()
      │
      ├─ glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
      │
      ├─ Para cada SceneObject en SceneManager
      │  │
      │  ├─ ObjectType type = obj.type
      │  │
      │  ├─ if (isPlanetary(type))
      │  │  │
      │  │  └─ PlanetarySystem::renderTerrain()
      │  │     │
      │  │     ├─ Shader = terrainShader
      │  │     │
      │  │     ├─ Para cada ChunkData en m_cache (visible)
      │  │     │  │
      │  │     │  ├─ glBindVertexArray(chunk.VAO)
      │  │     │  ├─ glBindBuffer(GL_ARRAY_BUFFER, chunk.VBO)
      │  │     │  │
      │  │     │  ├─ Floating Origin Correction
      │  │     │  │  ├─ worldPos = chunk.position + planetPos
      │  │     │  │  ├─ localPos = toLocal(worldPos)  ◄── Resta origin
      │  │     │  │  └─ model matrix incorpora localPos
      │  │     │  │
      │  │     │  ├─ shader.setUniform("model", modelMatrix)
      │  │     │  ├─ shader.setUniform("view", viewMatrix)
      │  │     │  ├─ shader.setUniform("projection", projMatrix)
      │  │     │  │
      │  │     │  └─ glDrawElements(GL_TRIANGLES, chunk.indiceCount, ...)
      │  │     │
      │  │     └─ [Repeat para todos los chunks visibles]
      │  │
      │  ├─ else if (isRenderable(type))
      │  │  └─ renderMesh(obj)  ◄── Malla estándar
      │  │
      │  └─ else if (type == LIGHT)
      │     └─ accumulateLight(obj)  ◄── Iluminación
      │
      ├─ Post-process (Bloom, Tone mapping)
      │
      └─ swapBuffers()

Frame N+1
```

---

## 4. Ciclo Completo: Desde Escena a Pantalla

```
┌────────────────────────────────────────────────────────────────┐
│                         INITIALIZACIÓN                         │
└────────────────┬───────────────────────────────────────────────┘
                 │
    ┌────────────▼──────────────┐
    │ SceneLoader::load()       │
    │ (Lee project.hrk / .json) │
    └────────────┬──────────────┘
                 │
        ┌────────▼────────────────────┐
        │ Deserialize SceneObjects[]  │
        │ Valida ObjectType           │
        └────────┬────────────────────┘
                 │
        ┌────────▼─────────────────┐
        │ SceneManager::addLoaded  │
        │ (Indexa m_registry)      │
        └────────┬─────────────────┘
                 │
        ┌────────▼─────────────────────────┐
        │ WorldSystem::init()               │
        │ • PlanetarySystem::init()         │
        │ • Create LOD, Cache, Streaming   │
        └────────┬─────────────────────────┘
                 │
         ========▼========  ◄── Escena cargada, lista para jugar
                 │
    
┌────────────────▼───────────────────────────────────────────┐
│                      GAME LOOP                              │
└───────────────┬──────────────────────────────────────────┬──┘
                │                                          │
         ┌──────▼─────────┐                      ┌─────────▼───┐
         │ Input System   │                      │ Physics     │
         │ • Movimiento   │                      │ • Gravity   │
         │ • Rotación cam │                      │ • Velocity  │
         └──────┬─────────┘                      └─────────┬───┘
                │                                          │
                └──────────────────┬──────────────────────┘
                                   │
                           ┌───────▼────────┐
                           │ Camera Update  │
                           │ camera.pos     │
                           └───────┬────────┘
                                   │
                           ┌───────▼─────────────────────┐
                           │ WorldSystem::update()       │
                           │ • Floating Origin shift     │
                           │ • Subsystem coordination    │
                           └───────┬─────────────────────┘
                                   │
                    ┌──────────────▼──────────────┐
                    │ PlanetarySystem::update()   │
                    │ • updateOrbits()            │
                    │ • LOD calculation           │
                    │ • Streaming queue update    │
                    └──────────────┬──────────────┘
                                   │
         ┌─────────────────────────┴──────────────────────┐
         │                                                │
    ┌────▼──────────┐                         ┌──────────▼────┐
    │ Chunk Loading │                         │ Chunk Evict   │
    │ (Disk/Proc)   │                         │ (Memory mgmt) │
    └────┬──────────┘                         └──────┬────────┘
         │                                           │
         └─────────────────┬──────────────────────────┘
                           │
                    ┌──────▼────────┐
                    │ Render Phase  │
                    │ Viewport      │
                    └──────┬────────┘
                           │
              ┌────────────▼─────────────┐
              │ Renderer::render()       │
              │ • Clear buffers          │
              │ • For each SceneObject   │
              │   • Check ObjectType     │
              │   • Render appropriately │
              │ • Post-process           │
              │ • SwapBuffers            │
              └────────────┬─────────────┘
                           │
                    Frame displayed
                           │
              ┌────────────▼──────────────┐
              │ Editor UI Sync (if editor)│
              │ • Update inspector        │
              │ • Refresh scene hierarchy │
              │ • Debug overlay           │
              └────────────┬──────────────┘
                           │
                    ========▼========
                           │
                      NEXT FRAME
```

---

## 5. Memoria y Optimizaciones

### ChunkCache Strategy

```
ChunkCache (Memory-Bound Streaming)
│
├─ RAM Budget: ej. 256 MB
│
├─ LRU Eviction
│  ├─ Si memoria > budget
│  │  └─ Evict least recently used
│  │
│  └─ Prioridad
│     ├─ Chunks visibles: Nunca evicta
│     ├─ Chunks cercanos (LOD 0-1): Difícilmente evicta
│     └─ Chunks lejanos (LOD 3): Primero evicta
│
├─ Storage por Chunk
│  ├─ Vértices: vec3 × 1024 = 12 KB (típico)
│  ├─ Normales: vec3 × 1024 = 12 KB
│  ├─ Colores:  vec3 × 1024 = 12 KB
│  ├─ Índices:  uint × 6000  = 24 KB
│  └─ Total: ~60 KB/chunk
│
└─ Con 256 MB buffer
   └─ ~4000 chunks máximo

LOD Hierarchy
│
├─ LOD 0 (Máx detalle)
│  ├─ Vértices: 1024×1024 = 1M (malla densa)
│  ├─ Distancia: <100m
│  └─ Uso: Caminar en superficie
│
├─ LOD 1
│  ├─ Vértices: 512×512 = 256K
│  ├─ Distancia: 100-500m
│  └─ Uso: Vista cercana
│
├─ LOD 2
│  ├─ Vértices: 256×256 = 65K
│  ├─ Distancia: 500-2000m
│  └─ Uso: Vista media
│
└─ LOD 3 (Mín detalle)
   ├─ Vértices: 128×128 = 16K
   ├─ Distancia: >2000m
   └─ Uso: Vista lejana

Bandwidth Savings
│
├─ Sin LOD
│  └─ Descarga: 4000 chunks × 60 KB = 240 MB CONTINUO
│
├─ Con LOD (LOD 0 + 1 visible)
│  └─ Descarga: 50 chunks LOD 0 + 200 chunks LOD 1 ≈ 15 MB
│
└─ Ahorro: 94% menos bandwidth
```

---

## 6. ObjectType Usage in Different Contexts

```
ObjectType enum
│
├─ EDITOR CONTEXT
│  ├─ SceneHierarchy Panel
│  │  └─ Muestra icono diferente según tipo
│  ├─ Inspector Panel
│  │  └─ Muestra properties específicas
│  └─ Asset Browser
│     └─ Filtra por tipo
│
├─ RENDERER CONTEXT
│  ├─ if (isPlanetary(type))
│  │  └─ Use TerrainRenderer pipeline
│  ├─ if (isRenderable(type))
│  │  └─ Use Standard mesh pipeline
│  └─ if (type == LIGHT)
│     └─ Accumulate in light buffer
│
├─ PHYSICS CONTEXT
│  ├─ if (type == PLANET)
│  │  └─ Apply planetary gravity
│  ├─ if (type == CHARACTER)
│  │  └─ Apply standard physics + gravity
│  └─ if (type == COLLIDER)
│     └─ Register collision volume
│
├─ STREAMING CONTEXT
│  ├─ if (type == PLANET)
│  │  └─ Schedule chunk loading
│  ├─ if (type == MODEL)
│  │  └─ Stream mesh+animation data
│  └─ if (type == AUDIO_SOURCE)
│     └─ Schedule audio buffer load
│
└─ SCRIPTING CONTEXT
   ├─ Script can check type
   └─ Trigger behavior based on type
```

---

## 7. Error Handling Flow

```
Load Scene
│
├─ Validation
│  ├─ Check JSON format
│  ├─ Check ObjectType validity
│  │  └─ if stringToObjectType() == UNKNOWN
│  │     └─ Emit warning, use EMPTY as fallback
│  ├─ Check transform values
│  │  └─ if NaN or Inf detected
│  │     └─ Clamp to reasonable range
│  └─ Check component dependencies
│     └─ if MeshRenderer without Mesh
│        └─ Emit warning
│
├─ Recovery Strategy
│  ├─ Invalid ObjectType
│  │  └─ Convert to EMPTY (safe default)
│  ├─ Invalid transform
│  │  └─ Reset to origin (0,0,0)
│  ├─ Missing mesh data
│  │  └─ Use placeholder cube
│  └─ Memory pressure
│     └─ Reduce LOD distances, increase cache eviction
│
└─ Logging
   ├─ If ANY warning
   │  └─ Write to debug.log
   └─ If CRITICAL error
      └─ Throw exception, rollback load
```

---

## 📊 Performance Metrics to Monitor

```
Frame Profiling
│
├─ Update Phase
│  ├─ WorldSystem::update()              target: <2ms
│  ├─ PlanetarySystem::update()          target: <5ms
│  │  ├─ updateOrbits()                  target: <1ms
│  │  ├─ LOD calculation                 target: <2ms
│  │  └─ Streaming queue update          target: <2ms
│  └─ Physics                            target: <3ms
│
├─ Render Phase
│  ├─ Visibility culling                 target: <1ms
│  ├─ Shader compilation (one-time)      OK: <100ms
│  ├─ Draw calls (TerrainRenderer)       target: <10ms
│  │  └─ Ideally: <200 draw calls/frame
│  ├─ GPU time                           target: <8ms (60 FPS)
│  └─ Post-processing                    target: <3ms
│
└─ Memory
   ├─ Cache size                         target: <256 MB
   ├─ Active chunks                      target: <1000
   ├─ Streaming backlog                  target: <100 chunks
   └─ Peak memory (scene load)           target: <2 GB
```
