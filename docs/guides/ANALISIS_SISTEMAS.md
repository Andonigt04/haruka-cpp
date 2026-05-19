# Análisis de Sistemas: Object Types, Scene System y World System

## 📋 Índice
1. [Tipos de Objetos (ObjectType)](#tipos-de-objetos)
2. [Sistema de Escenas (SceneManager)](#sistema-de-escenas)
3. [Sistema de Mundo (WorldSystem)](#sistema-de-mundo)
4. [Sistema Planetario (PlanetarySystem)](#sistema-planetario)
5. [Tipos Planetarios](#tipos-planetarios)
6. [Flujo de Integración](#flujo-de-integración)

---

## 1. Tipos de Objetos (ObjectType) {#tipos-de-objetos}

### 📌 Ubicación
[src/tools/object_types.h](src/tools/object_types.h)

### 🎯 Propósito
Define la clasificación de todos los objetos en la escena. Es un sistema de **etiquetado jerárquico** que permite a diferentes subsistemas (renderer, physics, streaming) identificar qué pipeline usar para cada entidad.

### 📊 Estructura de Tipos

```
ObjectType (enum)
├── RENDERIZABLES ESTÁNDAR
│   ├── MESH (1)              → Primitivas (cubo, esfera, plano)
│   └── MODEL (2)             → Modelos externos (.obj, .fbx, .gltf)
│
├── SISTEMA PLANETARIO
│   ├── PLANET (6)            → Cuerpo con QuadTree, LOD y Terreno
│   ├── STAR (7)              → Astro masivo (Sol)
│   └── SATELLITE (8)         → Lunas o estaciones espaciales
│
├── LUCES
│   ├── LIGHT (3)             → Luz puntual
│   ├── DIRECTIONAL_LIGHT (4) → Luz global (Sol distante)
│   └── SPOTLIGHT (5)         → Foco / Linterna
│
└── UTILIDADES Y JUEGO
    ├── CHARACTER (10)        → Jugador o NPCs
    ├── CAMERA (11)           → Cámaras
    ├── EMPTY (12)            → Nodo de transformación vacío
    ├── PARTICLE_SYSTEM (13)  → Sistemas de partículas
    ├── AUDIO_SOURCE (14)     → Fuentes de audio
    └── COLLIDER (15)         → Volúmenes de colisión puros
```

### 🔧 Helpers Funcionales

#### 1. Conversión String ↔ Enum
```cpp
// Carga desde JSON
ObjectType stringToObjectType(const std::string& s)

// Guardado a JSON
std::string objectTypeToString(ObjectType t)
```

#### 2. Filtros de Procesamiento
```cpp
// ¿Necesita pipeline de terreno procedural?
bool isPlanetary(ObjectType type)
  → PLANET | STAR | SATELLITE

// ¿Necesita pipeline de mallas estándar?
bool isRenderable(ObjectType type)
  → MESH | MODEL | PLANET
```

### 💡 Casos de Uso
- **Editor**: Mostrar/ocultar iconos y propiedades según tipo
- **Renderer**: Seleccionar shader y técnica de renderizado
- **Physics**: Determinar si aplicar física planetaria o estándar
- **Streaming**: Saber si un objeto necesita LOD y procedural generation

---

## 2. Sistema de Escenas (SceneManager) {#sistema-de-escenas}

### 📌 Ubicación
[src/core/scene/scene_manager.h](src/core/scene/scene_manager.h)

### 🎯 Propósito
Gestiona una **colección unificada de entidades** que representa el universo completo. Soporta desde objetos microscópicos hasta planetas procedurales usando **doble precisión** para evitar jittering.

### 📊 Estructura

#### SceneObject (struct)
```
SceneObject
├── IDENTIFICACIÓN
│   ├── name              : std::string
│   ├── type              : std::string (ej: "CelestialBody", "Spacecraft")
│   └── templateName      : std::string (arquetipo opcional)
│
├── TRANSFORMACIÓN (double precision)
│   ├── position          : glm::dvec3    (¡CRITICAL para escalas astronómicas!)
│   ├── rotation          : glm::dvec3
│   └── scale             : glm::dvec3
│
├── FLAGS DE SISTEMA
│   ├── hasChunks         : bool    (¿Usa QuadTree/Octree?)
│   ├── isPersistent      : bool    (¿Siempre en RAM como el Sol?)
│   └── originShiftingTarget : bool (¿Candidato para Floating Origin?)
│
├── DATOS DE CONFIGURACIÓN (JSON)
│   ├── lodSettings       : nlohmann::json
│   │   └── Distancias de LOD, configuración de QuadTree
│   ├── streamingSettings : nlohmann::json
│   │   └── Modo (Disk/Procedural), prioridad
│   ├── terrainSettings   : nlohmann::json
│   │   └── Capas de ruido, semilla, biomas
│   ├── components        : nlohmann::json
│   │   └── Luces, scripts, colisionadores
│   └── properties        : nlohmann::json
│       └── Metadatos extra (velocidad, facción, etc.)
│
└── JERARQUÍA (Opcional)
    ├── parentIndex       : int
    └── childrenIndices   : std::vector<int>
```

#### SceneManager (class)
```
SceneManager
├── MÉTODOS
│   ├── addLoadedObject(obj)     : Registra un objeto cargado
│   ├── getObjectByName(name)    : Búsqueda O(1) por nombre
│   ├── getAllObjects()          : Lista completa para renderizado
│   └── clear()                  : Limpia la escena
│
└── DATOS INTERNOS
    ├── m_objects[]              : Arreglo principal
    ├── m_registry[name]         : HashMap para búsquedas rápidas
    ├── m_idRegistry[hash]       : HashMap por ID único
    └── m_mutex                  : Thread-safety para streaming paralelo
```

### 🔑 Características Clave

#### ✅ Doble Precisión
```cpp
glm::dvec3 position  // double (64 bits) = precisión de milímetros a millones de km
```
**¿Por qué?** La GPU usa float (32 bits) que pierde precisión después de ~10 km. Con doble precisión en CPU:
- A 1 Millón de km: resolución de ~0.1 mm
- A 1 Billion de km: resolución de ~100 m

#### ✅ JSON Flexible
Cada subsistema extrae lo que necesita sin sobrecargar la estructura:
- **LOD System**: Lee `lodSettings`
- **Terrain Streaming**: Lee `streamingSettings` + `terrainSettings`
- **Renderer**: Lee `components` y `properties`

#### ✅ Thread-Safety
```cpp
std::mutex m_mutex;  // TerrainStreamingSystem puede leer mientras carga
```

#### ✅ Búsquedas Rápidas
```cpp
// O(1) por nombre
m_registry[obj->name]

// O(1) por ID numérico
m_idRegistry[hash(obj->name)]
```

### 💡 Casos de Uso
- Cargar escenas desde disco (terrenos, prefabs)
- Sincronizar el editor con el viewport
- Pasar objetos a diferentes subsistemas
- Búsquedas rápidas por nombre/ID

---

## 3. Sistema de Mundo (WorldSystem) {#sistema-de-mundo}

### 📌 Ubicación
[src/core/world_system.h](src/core/world_system.h) / [src/core/world_system.cpp](src/core/world_system.cpp)

### 🎯 Propósito
**Orquestador central** que gestiona:
1. **Floating Origin** (precisión astronómica)
2. **Cuerpos celestes macroscópicos**
3. **Coordinación de subsistemas** (PlanetarySystem)

### 📊 Estructura

```
WorldSystem
├── GESTIÓN DE ORIGEN
│   ├── m_worldOrigin       : glm::dvec3 (centro del universo matemático)
│   └── [MÉTODO] shiftOrigin(newOrigin)
│       └── Recentra todo cuando la cámara se aleja >5000 km
│
├── ENTIDADES MACROSCÓPICAS
│   └── m_bodies[]          : std::vector<CelestialBody>
│
├── SUBSISTEMAS COORDINADOS
│   └── m_planetarySystem   : std::unique_ptr<PlanetarySystem>
│
└── MÉTODOS PÚBLICOS
    ├── init()              : Inicializa PlanetarySystem
    ├── update(dt, camPos)  : Loop principal
    ├── toLocal(worldPos)   : Convierte double → float
    └── shiftOrigin()       : Recentra el universo
```

### 🔑 CelestialBody (struct)

```cpp
struct CelestialBody {
    std::string name;
    ObjectType type;              // PLANET, STAR, SATELLITE, etc.
    WorldPos worldPos;            // Posición en km (double)
    glm::vec3 velocity;           // km/s
    float radius;                 // km
    glm::vec3 color;              // RGB [0,1]
    LocalPos localPos;            // Calculado cada frame = worldPos - origin
};
```

### 🎬 Flujo de Update

```
Frame N
│
├─ 1. PRECISIÓN (Floating Origin)
│  │
│  └─ if (distance(camPos) > 5000km)
│     ├─ shiftOrigin(camPos)          ← Recentra el universo
│     ├─ for each CelestialBody       ← Resta el offset a todos
│     │   body.worldPos -= offset
│     └─ toLocal() ahora da coords float cercanas a origen
│
└─ 2. ACTUALIZAR SUBSISTEMAS
   │
   └─ m_planetarySystem->update(dt, camPos)
      ├─ Simula órbitas
      ├─ Genera/carga chunks de terreno
      ├─ Calcula LOD
      └─ Renderiza

Frame N+1
```

### 🔐 Conversión de Coordenadas

```cpp
// En CPU: trabajamos en double (5 dígitos de precisión astronómica)
glm::dvec3 worldPos(1234567.89012, 9876543.21098, ...);

// Para GPU: convertimos a float, restando el origen
glm::vec3 localPos = toLocal(worldPos);
  = glm::vec3(worldPos - m_worldOrigin)
  = Valores pequeños (~0 a ±1000) que la GPU puede manejar con precisión

// Vertex Shader final
gl_Position = projection * view * model * vec4(vertexPos + localPos, 1.0);
```

### 💡 Casos de Uso
- Mantener precisión en escalas astronómicas (km a mm)
- Evitar que los modelos tiemblen (jittering) lejos del origen
- Coordinar múltiples planetas, órbitas y terreno procedural
- Reasignar dinámicamente el "centro del universo"

---

## 4. Sistema Planetario (PlanetarySystem) {#sistema-planetario}

### 📌 Ubicación
[src/game/planetary_system.h](src/game/planetary_system.h)

### 🎯 Propósito
Motor de **simulación y renderizado de planetas procedurales** con:
- Órbitas
- Terreno LOD/Streaming
- Física planetaria

### 📊 Estructura

```
PlanetarySystem
├── ESTRUCTURA DE DATOS
│   ├── Planet (struct)
│   │   ├── name              : std::string
│   │   ├── position          : Haruka::WorldPos (double)
│   │   ├── radius            : double (km)
│   │   └── terrainSettings   : nlohmann::json
│   │       ├── Semilla (seed)
│   │       ├── Capas de ruido (Perlin, Worley)
│   │       ├── Octavas, persistencia, lacunarity
│   │       └── Rangos de altura
│   │
│   └── m_planets[]           : std::vector<Planet>
│
├── SUBSISTEMAS INTERNOS
│   ├── m_cache               : ChunkCache (caché de mallas generadas)
│   ├── m_generator           : TerrainGenerator (Perlin, procedural)
│   ├── m_renderer            : TerrainRenderer (dibuja chunks)
│   ├── m_streaming           : TerrainStreamingSystem (carga/descarga)
│   └── m_lod                 : LODSystem (Level of Detail)
│
├── SIMULACIÓN
│   ├── m_simulationTime      : double (tiempo total transcurrido)
│   ├── updateOrbits(dt)      : Integra órbitas (Kepler)
│   └── G = 6.67430e-11       : Constante gravitacional
│
└── MÉTODOS PÚBLICOS
    ├── init()
    ├── update(dt, cameraPos)
    └── addPlanet(Planet)
```

### 🔑 Flujo de Update

```
PlanetarySystem::update(dt, camPos)
│
├─ 1. ÓRBITAS
│  └─ updateOrbits(dt)
│     ├─ for each Planet
│     │   ├─ Calcula: a = GM/r² (aceleración gravitacional)
│     │   ├─ Integra:  v += a*dt
│     │   └─ Actualiza: pos += v*dt
│     └─ Propaga hacia WorldSystem::getBodies()
│
├─ 2. TERRENO PROCEDURAL (LOD/Streaming)
│  │
│  ├─ Obtiene chunks visibles desde cámara
│  ├─ m_lod->calculateLOD(camPos)
│  │   └─ Distancia → nivel de detalle (0 = máx, 3 = mín)
│  ├─ m_streaming->updateQueue()
│  │   ├─ Carga chunks necesarios (disk o genera on-the-fly)
│  │   └─ Descarga chunks lejanos
│  └─ m_generator->generate() o m_cache->retrieve()
│
└─ 3. RENDERIZADO
   └─ m_renderer->render()
      ├─ Vincula buffers VAO/VBO
      ├─ Aplica shader
      └─ Draw calls por chunk
```

---

## 5. Tipos Planetarios {#tipos-planetarios}

### 📌 Ubicación
[src/tools/planetary_types.h](src/tools/planetary_types.h)

### 📊 Estructura

#### PlanetFace (enum)
Represent the 6 faces of a cube-sphere projection:
```
PlanetFace
├── FRONT  (0)
├── BACK   (1)
├── TOP    (2)
├── BOTTOM (3)
├── RIGHT  (4)
└── LEFT   (5)
```

#### PlanetChunkKey (struct)
Identificador único de un chunk en el QuadTree:
```cpp
struct PlanetChunkKey {
    PlanetFace face;      // ¿En qué cara de la esfera?
    uint8_t lod;          // ¿Qué nivel de detalle? (0 = máx, N = mín)
    uint32_t x;           // Coordenada X en el QuadTree
    uint32_t y;           // Coordenada Y en el QuadTree
    
    // Operador == para usar como clave en std::unordered_map
};
```

**Ejemplo:**
```
Planet "Earth"
├─ FRONT face
│  ├─ LOD 0 (máx detalle)
│  │  ├─ Chunk (x=0, y=0)   → 100 m²/vértice
│  │  ├─ Chunk (x=1, y=0)   → 100 m²/vértice
│  │  └─ Chunk (x=0, y=1)   → 100 m²/vértice
│  ├─ LOD 1
│  │  ├─ Chunk (x=0, y=0)   → 400 m²/vértice (4x menos vértices)
│  │  └─ ...
│  └─ LOD 2, LOD 3, ...
└─ BACK, TOP, BOTTOM, RIGHT, LEFT faces con su propia jerarquía
```

#### ChunkData (struct)
Datos físicos de la malla generada:
```cpp
struct ChunkData {
    std::vector<glm::vec3> vertices;      // Posiciones
    std::vector<glm::vec3> normals;       // Normales para iluminación
    std::vector<glm::vec3> colors;        // Altura mapeada a color
    std::vector<unsigned int> indices;    // Triángulos
    
    PlanetChunkKey key;                   // ¿Cuál soy?
    std::string planetName;               // ¿A qué planeta pertenezco?
    
    size_t getSizeBytes() const;          // Memoria ocupada
};
```

#### LayerSettings (struct)
Configuración de una capa de ruido Perlin:
```cpp
struct LayerSettings {
    float freq = 1.0f;             // Frecuencia base
    float strength = 1.0f;         // Amplitud
    int octaves = 5;               // Capas de Perlin superpuestas
    float persistence = 0.5f;      // Cómo disminuye amplitud
    float lacunarity = 2.0f;       // Cómo aumenta frecuencia
};
```

**Uso:**
```json
{
  "terrainSettings": {
    "layers": [
      { "freq": 0.1, "octaves": 4, "persistence": 0.6 },  // Macroterrain
      { "freq": 1.0, "octaves": 3, "persistence": 0.5 },  // Detalle medio
      { "freq": 4.0, "octaves": 2, "persistence": 0.4 }   // Ruido fino
    ],
    "seed": 42,
    "minHeight": -200.0,
    "maxHeight": 1200.0
  }
}
```

---

## 6. Flujo de Integración {#flujo-de-integración}

### 🔄 Arquitectura General

```
┌─────────────────────────────────────────────────────────┐
│                      EDITOR / RUNTIME                   │
└─────────────┬───────────────────────────────┬───────────┘
              │                               │
        [Scene Load]                    [Update Loop]
              │                               │
              ▼                               ▼
      ┌──────────────────┐          ┌────────────────────┐
      │ SceneLoader      │          │   MainLoop         │
      │  (JSON Parser)   │          │ (Game/Editor)      │
      └────────┬─────────┘          └────────┬───────────┘
               │                             │
               │ Deserializa                 │ dt, camera pos
               │ SceneObject                 │
               │                             │
               ▼                             ▼
      ┌──────────────────┐          ┌────────────────────┐
      │  SceneManager    │◄─────────│  WorldSystem       │
      │                  │          │                    │
      │ • m_objects[]    │  datos   │ • Floating Origin  │
      │ • m_registry     │ asigna   │ • CelestialBody[]  │
      │ • Thread-safe    │          │ • toLocal()        │
      └──────┬───────────┘          └────────┬───────────┘
             │                               │
             │ getObjectByName()            │ Coordina
             │ getAllObjects()              │
             │                              ▼
             │                      ┌────────────────────┐
             │                      │ PlanetarySystem    │
             │                      │                    │
             │                      │ • updateOrbits()   │
             │                      │ • LOD/Streaming    │
             │                      │ • m_cache          │
             │                      │ • m_generator      │
             │                      │ • m_renderer       │
             │                      │ • m_streaming      │
             │                      │ • m_lod            │
             │                      └────────────────────┘
             │                              │
             │                              │ chunk requests
             └──────────────────────────────┘
```

### 🎬 Ciclo Típico de Frame

```
Frame N
│
├─ 1. INPUT & CAMERA UPDATE
│  └─ camera.pos = input + physics
│
├─ 2. WORLD SYSTEM UPDATE
│  │
│  └─ WorldSystem::update(dt, camPos)
│     │
│     ├─ if (distance(camPos) > 5000km)
│     │  └─ shiftOrigin(camPos)  ← Recentra si necesario
│     │
│     └─ PlanetarySystem::update(dt, camPos)
│        │
│        ├─ updateOrbits(dt)
│        │  └─ Cada planeta se mueve según física
│        │
│        ├─ m_lod->calculateLOD(camPos)
│        │  └─ Determina qué chunks necesita cada LOD
│        │
│        ├─ m_streaming->updateQueue()
│        │  ├─ Carga chunks nuevos (desde cache o genera)
│        │  └─ Descarga chunks lejanos
│        │
│        └─ m_generator->generateChunk(chunkKey)
│           ├─ Perlin noise a partir de LayerSettings
│           ├─ Suavizado y normales
│           └─ Retorna ChunkData con vértices
│
├─ 3. RENDER PHASE
│  │
│  └─ Renderer::render()
│     │
│     ├─ Para cada SceneObject
│     │  │
│     │  ├─ if (ObjectType::MESH || MODEL)
│     │  │  └─ Renderiza malla estándar (VAO)
│     │  │
│     │  ├─ if (ObjectType::PLANET || STAR || SATELLITE)
│     │  │  └─ PlanetarySystem::renderTerrain()
│     │  │     ├─ Para cada chunk en caché
│     │  │     │  ├─ Aplica Floating Origin correction
│     │  │     │  │  localPos = worldPos - origin
│     │  │     │  ├─ Vertex = localPos + chunkData.vertices
│     │  │     │  └─ Draw call
│     │  │     └─ Dibuja solo chunks visibles
│     │  │
│     │  └─ if (ObjectType::LIGHT)
│     │     └─ Shadowmap, light buffer
│     │
│     └─ Post-process (Bloom, Tone mapping, etc.)
│
└─ 4. SINCRONIZACIÓN
   │
   └─ Si es editor:
      ├─ Actualiza Inspector con propiedades seleccionadas
      ├─ Redibuja Scene Hierarchy
      └─ Debug overlay (memoria, chunks cargados, etc.)
```

### 🔗 Relaciones entre Sistemas

| Sistema | Propietario | Responsabilidades |
|---------|-------------|-------------------|
| **ObjectType** | - | Define categorías de objetos |
| **SceneManager** | Usuario / Editor | Almacena y busca objetos |
| **SceneObject** | SceneManager | Entidades con propiedades |
| **WorldSystem** | Runtime | Orquesta subsistemas |
| **CelestialBody** | WorldSystem | Cuerpos macroscópicos |
| **PlanetarySystem** | WorldSystem | Simula y renderiza planetas |
| **Planet** | PlanetarySystem | Configuración de planeta |
| **PlanetChunkKey** | - | Identifica chunks únicamente |
| **ChunkData** | TerrainGenerator | Malla generada |
| **LayerSettings** | Configuración | Parámetros de ruido |

---

## 🎯 Resumen de Diseño

### ✅ Fortalezas
1. **Separación de responsabilidades clara**
   - ObjectType: Clasificación
   - SceneManager: Almacenamiento
   - WorldSystem: Orquestación
   - PlanetarySystem: Simulación específica

2. **Precisión astronómica**
   - Doble precisión en CPU
   - Conversión inteligente a float para GPU
   - Floating Origin dinámico

3. **Escalabilidad**
   - LOD automático por distancia
   - Streaming asincrónico
   - Caché eficiente de chunks

4. **Flexibilidad de datos**
   - JSON para configuración
   - Componentes modulares
   - Sin fuerte acoplamiento

### ⚠️ Consideraciones
1. **Sincronización:** El mutex en SceneManager puede ser un cuello de botella con muchos threads
2. **Generación:** La generación procedural en tiempo real puede causar picos
3. **Memoria:** Cada chunk cargado ocupa RAM. Necesita limpieza cuidadosa
4. **Órbitas:** Integración simple (Euler). Podrías mejorar a RK4 para precisión

---

## 📚 Referencias
- Editor: [haruka/src/panels/viewport.cpp](haruka/src/panels/viewport.cpp)
- Runtime: [haruka-cpp/src/main.cpp](haruka-cpp/src/main.cpp)
- Inicialización: [haruka/template/scripts/init.cpp](haruka/template/scripts/init.cpp)
