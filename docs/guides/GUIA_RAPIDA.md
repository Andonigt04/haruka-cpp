# Guía Rápida y Referencia Cruzada

## 📑 Índice Rápido por Tema

### 🎯 Quiero...

#### ...entender cómo funciona el sistema
→ [ANALISIS_SISTEMAS.md](ANALISIS_SISTEMAS.md) (Conceptos fundamentales)

#### ...ver cómo interactúan los sistemas
→ [DIAGRAMAS_FLUJO.md](DIAGRAMAS_FLUJO.md) (Flujos y relaciones)

#### ...implementar una característica
→ [PATRONES_USO.md](PATRONES_USO.md) (Ejemplos de código)

#### ...debuggear un problema
→ [#Troubleshooting](#troubleshooting-rápido) (Soluciones comunes)

---

## 🔑 Conceptos Clave

### ObjectType
```
📍 Ubicación: src/tools/object_types.h
📌 Propósito: Clasificar objetos para diferentes pipelines
🔍 Busca: stringToObjectType(), isPlanetary(), isRenderable()

PLANET    → TerrainRenderer + LOD + Streaming
MODEL     → MeshRenderer estándar
LIGHT     → Accumulator de iluminación
CHARACTER → Physics + Animation
```

### SceneManager
```
📍 Ubicación: src/core/scene/scene_manager.h
📌 Propósito: Almacenar y buscar SceneObjects
🔍 Busca: getObjectByName(), getAllObjects(), addLoadedObject()

Características:
  ✅ O(1) lookups por nombre
  ✅ Thread-safe (mutex)
  ✅ JSON flexible por objeto
```

### WorldSystem
```
📍 Ubicación: src/core/world_system.h / .cpp
📌 Propósito: Orquestador central + Floating Origin
🔍 Busca: shiftOrigin(), toLocal(), getPlanetarySystem()

Responsabilidades:
  ✅ Mantener precisión astronómica
  ✅ Coordinar PlanetarySystem
  ✅ Gestionar CelestialBody[]
```

### PlanetarySystem
```
📍 Ubicación: src/game/planetary_system.h
📌 Propósito: Simular y renderizar planetas
🔍 Busca: update(), addPlanet(), renderTerrain()

Subsistemas internos:
  m_cache       → ChunkCache
  m_generator   → TerrainGenerator
  m_renderer    → TerrainRenderer
  m_streaming   → TerrainStreamingSystem
  m_lod         → LODSystem
```

### PlanetChunkKey
```
📍 Ubicación: src/tools/planetary_types.h
📌 Propósito: Identificador único de chunk
Estructura: face + lod + x + y

Ejemplo:
  face = FRONT
  lod = 0 (máx detalle)
  x = 5
  y = 3
  ↓
  Chunk específico en cara FRONT de nivel 0
```

### Floating Origin
```
📍 Documentado en: DIAGRAMAS_FLUJO.md sección 2
📌 Propósito: Precisión astronómica (millones de km)

Concepto:
  1. CPU: double precision (64 bits) → resolución mm
  2. GPU: float (32 bits) + offset → resolución mm a km
  3. Reseteado dinámicamente cada 5000 km

Implementación clave:
  m_worldOrigin + toLocal() = Corrección de precisión
```

---

## 🔗 Relaciones entre Archivos

```
ObjectType (enum)
    ↓
    └─→ stringToObjectType()
    └─→ isPlanetary()
    └─→ isRenderable()

SceneObject (struct)
    ↓
    └─→ SceneManager (almacena)
    └─→ Todas las propiedades JSON

CelestialBody (struct)
    ↓
    └─→ WorldSystem::m_bodies[]

PlanetChunkKey (struct)
    ↓
    └─→ ChunkData::key
    └─→ ChunkCache (lookups)
    └─→ TerrainRenderer (renderizado)

LayerSettings (struct)
    ↓
    └─→ SceneObject::terrainSettings
    └─→ TerrainGenerator::generateChunk()
```

---

## 📊 Tabla de Responsabilidades

| Sistema | ¿Quién lo posee? | ¿Cuándo se actualiza? | ¿Qué renderiza? |
|---------|------------------|----------------------|-----------------|
| **ObjectType** | - | Carga de escena | - |
| **SceneManager** | Aplicación | Carga/edición | - |
| **SceneObject** | SceneManager | Carga/edición | Según tipo |
| **WorldSystem** | Aplicación | Cada frame | - |
| **CelestialBody** | WorldSystem | Cada frame | Referencia |
| **PlanetarySystem** | WorldSystem | Cada frame | Terreno |
| **Planet** | PlanetarySystem | Carga | - |
| **PlanetChunkKey** | - | - | - |
| **ChunkData** | ChunkCache | Streaming | Terreno |
| **LayerSettings** | SceneObject | Edición | - |

---

## 🎬 Flujo Típico de Ejecución

```
startup
  ↓
SceneLoader::load(filepath)
  ├─ Parse JSON
  └─ SceneManager::addLoadedObject() x N
  
World initialization
  ├─ WorldSystem::init()
  │  └─ PlanetarySystem::init()
  │     ├─ ChunkCache::init()
  │     ├─ TerrainGenerator::init()
  │     ├─ LODSystem::init()
  │     └─ TerrainStreamingSystem::init()
  └─ Load planets from SceneManager
  
MAIN LOOP (each frame)
  ├─ Input & Physics
  ├─ Camera.update()
  │
  ├─ WorldSystem::update(dt, camPos)
  │  ├─ Check Floating Origin threshold
  │  ├─ shiftOrigin() if needed
  │  │  └─ for each CelestialBody: update position
  │  │
  │  └─ PlanetarySystem::update(dt, camPos)
  │     ├─ updateOrbits()
  │     ├─ LODSystem::calculateLOD()
  │     ├─ TerrainStreamingSystem::updateQueue()
  │     │  ├─ Load visible chunks
  │     │  │  └─ TerrainGenerator::generateChunk()
  │     │  │     └─ ChunkCache::store()
  │     │  └─ Evict invisible chunks
  │     │
  │     └─ [Chunks now ready]
  │
  ├─ Renderer::render()
  │  ├─ glClear()
  │  ├─ For each SceneObject:
  │  │  ├─ if (isPlanetary)
  │  │  │  └─ PlanetarySystem::renderTerrain()
  │  │  ├─ else if (isRenderable)
  │  │  │  └─ renderMesh()
  │  │  └─ else if (LIGHT)
  │  │     └─ accumulateLight()
  │  ├─ Post-processing
  │  └─ swapBuffers()
  │
  └─ [If Editor]
     ├─ InspectorPanel::update()
     ├─ SceneHierarchyPanel::update()
     └─ Debug overlay

NEXT FRAME
```

---

## 🔴 Debugging Rápido

### Verificar que un objeto está cargado
```cpp
auto& sceneManager = getSceneManager();
auto obj = sceneManager.getObjectByName("Earth");

if (!obj) {
    ERROR("Object not found!");
    return;
}

printf("Object type: %s\n", obj->type.c_str());
printf("Position: [%.2f, %.2f, %.2f]\n", 
    obj->position.x, obj->position.y, obj->position.z);
printf("Terrain settings keys: ");
for (const auto& [key, val] : obj->terrainSettings.items()) {
    printf("%s ", key.c_str());
}
```

### Verificar que WorldSystem está actualizado
```cpp
auto& world = getWorldSystem();
auto& planetary = world.getPlanetarySystem();

printf("World origin: [%.2f, %.2f, %.2f]\n", 
    world.m_worldOrigin.x, world.m_worldOrigin.y, world.m_worldOrigin.z);
printf("Camera local position: [%.2f, %.2f, %.2f]\n",
    world.toLocal(camPos).x, world.toLocal(camPos).y, world.toLocal(camPos).z);
printf("Planets: %zu\n", planetary.m_planets.size());
```

### Verificar que chunks están siendo generados
```cpp
auto& planetary = world.getPlanetarySystem();

// Forzar generación de un chunk específico
PlanetChunkKey key{
    .face = PlanetFace::FRONT,
    .lod = 0,
    .x = 0,
    .y = 0
};

auto data = planetary.m_generator->generateChunk(key, planet->terrainSettings);
printf("Generated chunk: %zu verts, %zu indices\n",
    data.vertices.size(), data.indices.size());

// Verificar que está en caché
data = planetary.m_cache->retrieve(key);
if (data) printf("✓ Chunk in cache\n");
else      printf("✗ Chunk NOT in cache\n");
```

### Verificar que LOD está funcionando
```cpp
float distance = 1500.0f;  // Distancia a chunk
int lod = world.getPlanetarySystem().m_lod->calculateLOD(distance);
printf("Distance %.2f → LOD %d\n", distance, lod);

// Debe dar:
// 100.0 → 0
// 500.0 → 1
// 2000.0 → 2
// 10000.0 → 3
```

---

## 🎨 Configuración Típica de JSON

### Planeta Detallado
```json
{
  "name": "Earth",
  "type": "planet",
  "position": [0.0, 0.0, 0.0],
  "hasChunks": true,
  "isPersistent": true,
  "terrainSettings": {
    "seed": 42,
    "minHeight": -200.0,
    "maxHeight": 1200.0,
    "layers": [
      {"freq": 0.1, "octaves": 4, "persistence": 0.6, "strength": 1.0},
      {"freq": 1.0, "octaves": 3, "persistence": 0.5, "strength": 0.5},
      {"freq": 4.0, "octaves": 2, "persistence": 0.4, "strength": 0.25}
    ]
  },
  "lodSettings": {
    "distances": [100.0, 500.0, 2000.0, 10000.0],
    "quadtreeMaxDepth": 12
  },
  "streamingSettings": {
    "mode": "procedural",
    "cacheSizeMB": 256,
    "preloadRadius": 5000.0
  },
  "properties": {
    "radius": 6371.0,
    "mass": 5.972e24,
    "gravity": 9.81,
    "color": [0.2, 0.4, 0.8]
  }
}
```

### Mesh Simple
```json
{
  "name": "SpaceStation",
  "type": "model",
  "position": [1000.0, 500.0, 200.0],
  "scale": [5.0, 5.0, 5.0],
  "hasChunks": false,
  "isPersistent": false,
  "components": {
    "mesh": {
      "modelPath": "assets/models/station.fbx"
    },
    "material": {
      "albedo": "assets/textures/station_albedo.png"
    }
  }
}
```

### Luz
```json
{
  "name": "SunLight",
  "type": "directional_light",
  "position": [-100000.0, 150000.0, 80000.0],
  "properties": {
    "intensity": 1.5,
    "color": [1.0, 0.95, 0.8],
    "castShadows": true,
    "shadowBias": 0.0005
  }
}
```

---

## 📚 Archivos Principales

| Archivo | Responsabilidad |
|---------|-----------------|
| `src/tools/object_types.h` | Clasificación de objetos |
| `src/tools/planetary_types.h` | Tipos para planetas |
| `src/tools/math_types.h` | WorldPos, LocalPos, double/float |
| `src/core/scene/scene_manager.h` | Almacén de SceneObjects |
| `src/core/world_system.h` / `.cpp` | Orquestador + Floating Origin |
| `src/game/planetary_system.h` | Motor de planetas |
| `src/core/lod_system.h` | Cálculo de Level of Detail |
| `src/core/terrain/terrain_generator.h` | Generación procedural |
| `src/core/terrain/chunk_cache.h` | Caché de chunks |
| `src/core/terrain/terrain_streaming_system.h` | Carga asincrónica |
| `src/renderer/terrain_renderer.h` | Renderizado de terreno |
| `haruka/src/panels/viewport.cpp` | Integración editor |

---

## ⚡ Performance Tips

### Para mejorar FPS

1. **Reducir resolución de chunks**
   ```cpp
   int resolution = 32;  // En lugar de 64
   // Ahora cada chunk tiene 4x menos vértices
   ```

2. **Aumentar LOD distances**
   ```cpp
   lodDistances[0] = 200.0f;   // Antes: 100.0f
   // Terreno bajo detalle más cerca
   ```

3. **Reducir caché de chunks**
   ```cpp
   m_maxMemoryBytes = 128 * 1024 * 1024;  // 128 MB en lugar de 256 MB
   // Menos chunks en RAM, pero más thrashing
   ```

4. **Usar menos capas de Perlin**
   ```cpp
   "octaves": 2  // En lugar de 5
   // Más rápido pero menos detalle
   ```

5. **Desactivar shadowing para terreno**
   ```glsl
   // En shader: deshabilitar shadow mapping para chunks lejanos
   if (lod > 1) {
       useShadows = false;
   }
   ```

### Para mejorar memoria

1. **Cachear agresivamente**
   - Pre-cargar chunks alrededor de la cámara
   - Mantener en RAM los chunks cercanos

2. **Streaming asincrónico**
   - Generar chunks en thread separado
   - No bloquear main thread

3. **Comprimir datos**
   - Quantizar vértices (uint16 en lugar de float)
   - RLE para altura

---

## 🔧 Comandos Útiles de Debug

### GDB Breakpoints
```bash
# Cuando se carga un objeto PLANET
gdb> break object_types.cpp if stringToObjectType == PLANET

# Cuando se genera un chunk
gdb> break terrain_generator.cpp:42

# Cuando se cambia el Floating Origin
gdb> break world_system.cpp if shiftOrigin
```

### Logging
```cpp
// Habilitar logs detallados en PlanetarySystem
#define PLANET_DEBUG 1

#if PLANET_DEBUG
    #define PLANET_LOG(...) printf("[PLANET] " __VA_ARGS__)
#else
    #define PLANET_LOG(...) ((void)0)
#endif

PLANET_LOG("Generating chunk (face=%d, lod=%d, x=%d, y=%d)\n",
    key.face, key.lod, key.x, key.y);
```

### ImGui Debug Overlay
```cpp
void showDebugOverlay() {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("Debug", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::Text("World Origin: (%.0f, %.0f, %.0f)", 
        world.m_worldOrigin.x, world.m_worldOrigin.y, world.m_worldOrigin.z);
    ImGui::Text("Camera Local: (%.1f, %.1f, %.1f)",
        worldSystem.toLocal(camPos).x, 
        worldSystem.toLocal(camPos).y, 
        worldSystem.toLocal(camPos).z);
    ImGui::Text("Cached Chunks: %zu", cache.size());
    ImGui::Text("Pending Loads: %zu", streaming.pendingCount());
    
    ImGui::End();
}
```

---

## 🚀 Optimizaciones Futuras

### Candidatos para mejora

1. **Integrated Orbital Mechanics**
   - Actual N-body simulation
   - Kepler elements para performance

2. **Advanced Terrain Features**
   - Erosion simulation
   - Biomes
   - Water simulation

3. **GPU Terrain Generation**
   - Compute shaders en lugar de CPU
   - Real-time displacement mapping

4. **Streaming Improvements**
   - Predictive loading (donde irá la cámara)
   - Priority queue por visibilidad

5. **Memory Management**
   - Memory pooling
   - Compactación automática

---

## 📞 Contacto / Dudas

Si tienes dudas sobre:
- **Arquitectura**: Consulta [ANALISIS_SISTEMAS.md](ANALISIS_SISTEMAS.md)
- **Flujos**: Consulta [DIAGRAMAS_FLUJO.md](DIAGRAMAS_FLUJO.md)
- **Implementación**: Consulta [PATRONES_USO.md](PATRONES_USO.md)
- **Rápido**: Esta guía

---

**Última actualización**: 6 de Mayo de 2026
**Versión del código**: haruka-cpp v0.5.0
