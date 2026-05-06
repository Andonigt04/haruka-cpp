# Patrones de Implementación y Casos de Uso

## 1. Patrón: Crear un Nuevo Objeto de Escena

### Escenario
Quieres agregar un nuevo planeta a tu escena en el editor.

### Paso a Paso

#### 1.1 Definir ObjectType (si es nuevo tipo)
```cpp
// Si es un tipo de objeto completamente nuevo
// src/tools/object_types.h

enum class ObjectType {
    // ... tipos existentes ...
    MOON = 9,  // Nuevo: Luna (similar a planeta pero más pequeño)
};

// Agregar helper de conversión
inline ObjectType stringToObjectType(const std::string& s) {
    // ... existentes ...
    if (s == "moon") return ObjectType::MOON;
    return ObjectType::UNKNOWN;
}

inline std::string objectTypeToString(ObjectType t) {
    switch (t) {
        // ... existentes ...
        case ObjectType::MOON: return "moon";
        default:               return "unknown";
    }
}
```

#### 1.2 Crear SceneObject en JSON
```json
// scenes/my_scene.json
{
  "scene": {
    "name": "My Solar System",
    "objects": [
      {
        "name": "Earth",
        "type": "planet",
        "position": [0.0, 0.0, 0.0],
        "rotation": [0.0, 0.0, 0.0],
        "scale": [1.0, 1.0, 1.0],
        "hasChunks": true,
        "isPersistent": true,
        "originShiftingTarget": true,
        
        "terrainSettings": {
          "seed": 42,
          "minHeight": -200.0,
          "maxHeight": 1200.0,
          "layers": [
            {
              "freq": 0.1,
              "octaves": 4,
              "persistence": 0.6,
              "lacunarity": 2.0,
              "strength": 1.0
            },
            {
              "freq": 1.0,
              "octaves": 3,
              "persistence": 0.5,
              "lacunarity": 2.0,
              "strength": 0.5
            }
          ]
        },
        
        "lodSettings": {
          "distances": [100.0, 500.0, 2000.0, 10000.0],
          "quadtreeMaxDepth": 12
        },
        
        "streamingSettings": {
          "mode": "procedural",
          "priority": 1,
          "cacheSizeMB": 128,
          "preloadRadius": 5000.0
        },
        
        "components": {
          "terrain": {
            "type": "TerrainComponent",
            "properties": {}
          },
          "atmosphere": {
            "type": "AtmosphereComponent",
            "density": 1.225,
            "height": 100.0
          }
        },
        
        "properties": {
          "radius": 6371.0,
          "mass": 5.972e24,
          "gravity": 9.81,
          "rotationSpeed": 0.00007292115,
          "color": [0.2, 0.4, 0.8]
        }
      }
    ]
  }
}
```

#### 1.3 Cargar en el Editor
```cpp
// haruka/src/main_editor.cpp
void loadScene(const std::string& filepath) {
    SceneLoader loader;
    
    // 1. Cargar JSON
    auto sceneName = loader.loadFromFile(filepath);
    
    // 2. Los objetos se registran automáticamente en SceneManager
    auto& sceneManager = getSceneManager();
    auto earth = sceneManager.getObjectByName("Earth");
    
    if (!earth) {
        ERROR("Earth not found in scene!");
        return;
    }
    
    // 3. Validar tipo
    ObjectType type = stringToObjectType(earth->type);
    if (!isPlanetary(type)) {
        WARN("Earth is not planetary type!");
    }
    
    // 4. Sincronizar con WorldSystem
    WorldSystem& world = getWorldSystem();
    world.getPlanetarySystem().addPlanet({
        .name = earth->name,
        .position = WorldPos(earth->position.x, earth->position.y, earth->position.z),
        .radius = earth->getProperty<double>("radius", 6371.0),
        .terrainSettings = earth->terrainSettings
    });
}
```

#### 1.4 Renderizar
```cpp
// haruka/src/renderer/main_renderer.cpp
void Renderer::render() {
    auto& sceneManager = getSceneManager();
    
    for (const auto& obj : sceneManager.getAllObjects()) {
        ObjectType type = stringToObjectType(obj->type);
        
        if (isPlanetary(type)) {
            // ✅ Usar TerrainRenderer
            m_planetarySystem.renderTerrain(
                m_shaderManager.get("terrain"),
                m_camera
            );
        }
        else if (isRenderable(type)) {
            // ✅ Usar MeshRenderer
            renderMesh(*obj);
        }
        else if (type == ObjectType::LIGHT) {
            // ✅ Usar LightAccumulator
            accumulateLight(*obj);
        }
    }
}
```

---

## 2. Patrón: Implementar Floating Origin

### Escenario
La cámara se aleja mucho del origen (>5000 km) y los vértices empiezan a temblar. Necesitas recentrar el universo.

### Problema Subyacente
```cpp
// SIN Floating Origin: GPU usa float
glm::vec3 vertexPos = vec3(1234567.89012f, 9876543.21098f, ...);
// ¿Problema? float solo tiene 7 dígitos de precisión
// En 1M km, resolución ≈ 100m (¡muy grueso!)

// CON Floating Origin
glm::dvec3 worldPos = dvec3(1234567.89012, 9876543.21098, ...);  // CPU: double
glm::vec3 localPos = toLocal(worldPos);  // Resta origin
// Ahora localPos ≈ [100, 200, 300] - valores pequeños
// GPU: float tiene 7 dígitos de precisión en valores pequeños ✅
```

### Implementación

#### 2.1 WorldSystem coordina el shifting
```cpp
// src/core/world_system.cpp

void WorldSystem::update(double dt, const glm::dvec3& cameraPos) {
    // CRITICAL: Verificar si estamos muy lejos del origen
    const double SHIFT_THRESHOLD = 5000.0;  // 5000 km
    
    if (glm::length(cameraPos) > SHIFT_THRESHOLD) {
        LOG("Shifting origin from " << m_worldOrigin << " to " << cameraPos);
        shiftOrigin(cameraPos);
    }
    
    // Pasar a subsistemas
    m_planetarySystem->update(dt, cameraPos);
}

void WorldSystem::shiftOrigin(const glm::dvec3& newOrigin) {
    // 1. Calcular offset
    glm::dvec3 offset = newOrigin - m_worldOrigin;
    
    // 2. Actualizar origen global
    m_worldOrigin = newOrigin;
    
    // 3. CRITICO: Actualizar todas las entidades
    for (auto& body : m_bodies) {
        body.worldPos -= offset;
        // Ahora: body.worldPos ≈ [0..100]  ✅
        // Mientras: m_worldOrigin = newOrigin  ✅
        // Invariante: body.worldPos siempre pequeño
    }
    
    // 4. Notificar a subsistemas
    m_planetarySystem->onOriginShifted(offset);
}

glm::vec3 WorldSystem::toLocal(const glm::dvec3& worldPos) const {
    // Conversión final para la GPU
    // High precision (double) en CPU → Low precision (float) en GPU
    return glm::vec3(worldPos - m_worldOrigin);
}
```

#### 2.2 En el Vertex Shader
```glsl
// shaders/terrain.vert

#version 460

// Entrada: vértice relativo al chunk (pequeño: -500 a +500)
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;

// Uniforms: información de posicionamiento
uniform mat4 uModel;           // Transf del chunk
uniform mat4 uView;            // Cámara
uniform mat4 uProjection;      // Perspectiva

// FLOATING ORIGIN: Corrección de precisión
uniform vec3 uOriginOffset;    // (calcularado como toLocal() en CPU)

void main() {
    // 1. Posición del vértice en el chunk
    vec3 vertexPos = aPosition;
    
    // 2. Transformación del modelo (chunk local → planeta local)
    vertexPos = (uModel * vec4(vertexPos, 1.0)).xyz;
    
    // 3. FLOATING ORIGIN: Aplicar corrección
    vertexPos += uOriginOffset;  // toLocal() ya hecho en CPU
    
    // 4. Proyección estándar
    gl_Position = uProjection * uView * vec4(vertexPos, 1.0);
    
    // Normales
    vNormal = normalize((uModel * vec4(aNormal, 0.0)).xyz);
}
```

#### 2.3 Sincronización en tiempo real
```cpp
// haruka/src/panels/viewport.cpp

void ViewportPanel::render() {
    // Obtener posición de cámara actual
    glm::dvec3 camWorldPos = m_camera->getWorldPosition();  // double
    
    // Actualizar WorldSystem (Floating Origin si es necesario)
    m_worldSystem->update(deltaTime, camWorldPos);
    
    // Obtener offset para shader
    glm::vec3 originOffset = m_worldSystem->toLocal(camWorldPos);
    
    // Pasar a shader
    m_shaderManager.get("terrain")->setUniform("uOriginOffset", originOffset);
    
    // Renderizar
    m_planetarySystem->renderTerrain(m_shaderManager.get("terrain"), m_camera);
}
```

---

## 3. Patrón: Generar y Cachear Chunks

### Escenario
La cámara se acerca a un planeta. Los chunks aparecen lentamente (generación on-demand).

### Flujo

#### 3.1 Request chunk
```cpp
// Alguien pide un chunk (ej: LODSystem o TerrainRenderer)
PlanetChunkKey key{
    .face = PlanetFace::FRONT,
    .lod = 0,
    .x = 5,
    .y = 3
};

// Preguntar: ¿Existe?
ChunkData* chunk = m_cache->retrieve(key);

if (!chunk) {
    // No existe → generar
    chunk = m_generator->generateChunk(key, terrainSettings);
    
    // Cachear para próxima vez
    m_cache->store(key, *chunk);
}

// Usar chunk
m_renderer->addChunk(*chunk);
```

#### 3.2 Generación procedural
```cpp
// src/core/terrain/terrain_generator.cpp

ChunkData TerrainGenerator::generateChunk(
    const PlanetChunkKey& key,
    const nlohmann::json& terrainSettings
) {
    ChunkData result;
    result.key = key;
    
    // 1. Obtener parámetros
    int seed = terrainSettings["seed"].get<int>();
    float minH = terrainSettings["minHeight"].get<float>();
    float maxH = terrainSettings["maxHeight"].get<float>();
    
    auto layers = terrainSettings["layers"];  // array de LayerSettings
    
    // 2. Generar malla base (QuadTree quad)
    int resolution = 64;  // 64×64 vértices = ~4000 triángulos
    
    for (int y = 0; y < resolution; ++y) {
        for (int x = 0; x < resolution; ++x) {
            // Normalizar a [0, 1]
            float u = static_cast<float>(x) / (resolution - 1);
            float v = static_cast<float>(y) / (resolution - 1);
            
            // Posición en la cara del cubo
            glm::vec3 pos = mapQuadToSphere(u, v, key.face);
            
            // 3. Aplicar ruido Perlin (multi-capa)
            float height = 0.0f;
            float amplitude = 1.0f;
            float frequency = 1.0f;
            float maxAmplitude = 0.0f;
            
            for (const auto& layer : layers) {
                float freq = layer["freq"].get<float>();
                int octaves = layer["octaves"].get<int>();
                float persistence = layer["persistence"].get<float>();
                float strength = layer["strength"].get<float>();
                
                // Perlin noise multioactava
                for (int o = 0; o < octaves; ++o) {
                    float noise = perlinNoise(pos * freq * frequency, seed);
                    height += noise * amplitude * strength;
                    maxAmplitude += amplitude;
                    
                    amplitude *= persistence;
                    frequency *= 2.0f;  // lacunarity = 2.0
                }
            }
            
            // Normalizar amplitud
            height /= maxAmplitude;
            
            // 4. Mapear altura a [minH, maxH]
            height = minH + height * (maxH - minH);
            
            // 5. Escalar radialmente desde el planeta
            glm::vec3 scaledPos = pos * (1.0f + height / planetRadius);
            
            result.vertices.push_back(scaledPos);
            
            // Color basado en altura (terreno verde, montaña gris, nieve blanca)
            glm::vec3 color = heightToColor(height);
            result.colors.push_back(color);
        }
    }
    
    // 6. Calcular normales
    result.normals.resize(result.vertices.size());
    for (size_t i = 0; i < result.vertices.size(); ++i) {
        // Normales simples: normalize(vertex - planetCenter)
        result.normals[i] = glm::normalize(result.vertices[i]);
    }
    
    // 7. Generar índices (triángulos)
    for (int y = 0; y < resolution - 1; ++y) {
        for (int x = 0; x < resolution - 1; ++x) {
            int a = y * resolution + x;
            int b = y * resolution + (x + 1);
            int c = (y + 1) * resolution + x;
            int d = (y + 1) * resolution + (x + 1);
            
            // Triángulo 1
            result.indices.push_back(a);
            result.indices.push_back(c);
            result.indices.push_back(b);
            
            // Triángulo 2
            result.indices.push_back(b);
            result.indices.push_back(c);
            result.indices.push_back(d);
        }
    }
    
    return result;
}
```

#### 3.3 Caché y evicción
```cpp
// src/core/terrain/chunk_cache.cpp

class ChunkCache {
private:
    std::unordered_map<uint64_t, ChunkData> m_cache;  // clave→datos
    std::list<uint64_t> m_lru;                        // Lista de último acceso
    size_t m_maxMemoryBytes = 256 * 1024 * 1024;     // 256 MB
    size_t m_currentMemoryBytes = 0;
    
    uint64_t hashKey(const PlanetChunkKey& key) {
        // Combinar face, lod, x, y en un hash único
        return (key.face << 40) | (key.lod << 32) | (key.x << 16) | key.y;
    }
    
public:
    ChunkData* retrieve(const PlanetChunkKey& key) {
        uint64_t h = hashKey(key);
        auto it = m_cache.find(h);
        
        if (it != m_cache.end()) {
            // ✅ Hit! Mover a frente de LRU
            m_lru.remove(h);
            m_lru.push_front(h);
            return &it->second;
        }
        
        // ❌ Miss
        return nullptr;
    }
    
    void store(const PlanetChunkKey& key, const ChunkData& data) {
        uint64_t h = hashKey(key);
        size_t dataSize = data.getSizeBytes();
        
        // Evicción si es necesario
        while (m_currentMemoryBytes + dataSize > m_maxMemoryBytes && !m_lru.empty()) {
            uint64_t lru_key = m_lru.back();  // Chunk menos usado recientemente
            m_lru.pop_back();
            
            auto it = m_cache.find(lru_key);
            if (it != m_cache.end()) {
                m_currentMemoryBytes -= it->second.getSizeBytes();
                m_cache.erase(it);
            }
        }
        
        // Guardar nuevo chunk
        m_cache[h] = data;
        m_currentMemoryBytes += dataSize;
        m_lru.push_front(h);
        
        LOG("Cache: " << m_cache.size() << " chunks, " 
            << m_currentMemoryBytes / (1024.0 * 1024.0) << " MB");
    }
    
    void clear() {
        m_cache.clear();
        m_lru.clear();
        m_currentMemoryBytes = 0;
    }
};
```

---

## 4. Patrón: LOD (Level of Detail)

### Escenario
La cámara está a 1000 m del planeta. Necesitas renderizar LOD 1, no LOD 0 (demasiados triángulos).

### Implementación

#### 4.1 Calcular LOD basado en distancia
```cpp
// src/core/lod_system.cpp

class LODSystem {
    struct LODLevel {
        float distance;
        int vertexDensity;  // vértices por unidad cuadrada
    };
    
    // Distancias configurables (desde WorldSystem)
    LODLevel lodLevels[4] = {
        {100.0f,   100},  // LOD 0: <100m, 100 verts/m²
        {500.0f,    25},  // LOD 1: <500m,  25 verts/m²
        {2000.0f,    6},  // LOD 2: <2000m,  6 verts/m²
        {10000.0f,  1}    // LOD 3: <10000m, 1 verts/m²
    };
    
public:
    int calculateLOD(float distance) {
        for (int i = 0; i < 4; ++i) {
            if (distance < lodLevels[i].distance) {
                return i;
            }
        }
        return 3;  // LOD máximo
    }
    
    int getVertexDensity(int lod) {
        return lodLevels[lod].vertexDensity;
    }
};
```

#### 4.2 Usar LOD en TerrainRenderer
```cpp
// src/renderer/terrain_renderer.cpp

void TerrainRenderer::render(const Camera* camera) {
    glm::dvec3 camPos = camera->getWorldPosition();
    
    for (const auto& chunk : m_cache->getAllChunks()) {
        // Distancia desde cámara a centro del chunk
        float distance = glm::length(
            glm::vec3(chunk.key.center - camPos)
        );
        
        // ¿Qué LOD usar?
        int lod = m_lodSystem->calculateLOD(distance);
        
        // Si el chunk es LOD 0 pero deberías usar LOD 1...
        if (chunk.key.lod > lod) {
            // Saltar: Hay una versión más detallada disponible
            continue;
        }
        
        if (chunk.key.lod < lod) {
            // Saltar: Este chunk es más detallado de lo que necesitas
            continue;
        }
        
        // ✅ Este es el LOD correcto → renderizar
        glBindVertexArray(chunk.vao);
        glDrawElements(GL_TRIANGLES, chunk.indexCount, GL_UNSIGNED_INT, 0);
    }
}
```

---

## 5. Patrón: Sincronización Editor ↔ Runtime

### Escenario
Cambias un setting de terreno en el inspector del editor. Quieres que se aplique en tiempo real en el viewport.

#### 5.1 Editor modifica propiedad
```cpp
// haruka/src/panels/inspector.cpp

void InspectorPanel::onTerrainSettingChanged(
    const std::string& objectName,
    const std::string& settingKey,
    float newValue
) {
    auto& sceneManager = getSceneManager();
    auto obj = sceneManager.getObjectByName(objectName);
    
    if (!obj) return;
    
    // 1. Actualizar en SceneObject
    obj->terrainSettings[settingKey] = newValue;
    
    // 2. Notificar a WorldSystem
    auto& world = getWorldSystem();
    
    if (isPlanetary(stringToObjectType(obj->type))) {
        auto& planetary = world.getPlanetarySystem();
        
        // 3. Invalidar caché
        //    (Los chunks con ajustes viejos no son válidos)
        planetary.invalidateCache(objectName);
        
        // 4. Próximo frame, se regenerarán con ajustes nuevos
    }
    
    // 5. Guardar cambio a disco (opcional)
    sceneManager.saveObject(obj->name);
}
```

#### 5.2 Invalidación de caché
```cpp
// src/game/planetary_system.cpp

void PlanetarySystem::invalidateCache(const std::string& planetName) {
    // Encontrar todos los chunks de este planeta
    std::vector<uint64_t> keysToRemove;
    
    for (const auto& [key, data] : m_cache) {
        if (data.planetName == planetName) {
            keysToRemove.push_back(key);
        }
    }
    
    // Eliminar
    for (auto key : keysToRemove) {
        m_cache.erase(key);
    }
    
    // Próximo update(): se regenerarán
}
```

#### 5.3 Regeneración automática
```cpp
// src/game/planetary_system.cpp

void PlanetarySystem::update(double dt, const glm::dvec3& cameraPos) {
    // ...
    
    // TerrainStreamingSystem solicitará chunks nuevos
    std::vector<PlanetChunkKey> visibleChunks = calculateVisibleChunks(cameraPos);
    
    for (const auto& key : visibleChunks) {
        if (!m_cache->retrieve(key)) {
            // Generar con ajustes NUEVOS
            auto planet = findPlanetByChunkKey(key);
            auto data = m_generator->generateChunk(key, planet->terrainSettings);
            m_cache->store(key, data);
        }
    }
}
```

---

## 6. Checklist: Agregar un Nuevo Sistema

### Cuando necesites agregar un sistema nuevo (ej: WeatherSystem)

#### ✅ 1. Define ObjectType (si aplica)
```cpp
enum class ObjectType {
    // ... existentes ...
    WEATHER_VOLUME = 20,
};
```

#### ✅ 2. Define SceneObject JSON schema
```json
{
  "name": "Thunderstorm_1",
  "type": "weather_volume",
  "position": [100, 200, 300],
  "properties": {
    "windSpeed": 50.0,
    "precipitation": 0.8,
    "temperature": 5.0
  }
}
```

#### ✅ 3. Create system class
```cpp
class WeatherSystem {
public:
    void init(SceneManager* sceneManager);
    void update(double dt, const glm::dvec3& cameraPos);
    void applyToPlayer(Character& player);
};
```

#### ✅ 4. Agregar a WorldSystem
```cpp
class WorldSystem {
private:
    std::unique_ptr<WeatherSystem> m_weatherSystem;
public:
    void init() {
        m_planetarySystem = std::make_unique<PlanetarySystem>();
        m_weatherSystem = std::make_unique<WeatherSystem>();  // ← NUEVO
    }
    
    void update(double dt, const glm::dvec3& cameraPos) {
        m_planetarySystem->update(dt, cameraPos);
        m_weatherSystem->update(dt, cameraPos);  // ← NUEVO
    }
};
```

#### ✅ 5. Integrate with renderer
```cpp
// src/renderer/main_renderer.cpp
void Renderer::render() {
    // ... existentes ...
    
    for (const auto& obj : sceneManager.getAllObjects()) {
        ObjectType type = stringToObjectType(obj->type);
        
        if (type == ObjectType::WEATHER_VOLUME) {
            m_weatherSystem->render(*obj);  // ← NUEVO
        }
    }
}
```

#### ✅ 6. Test
- Cargar escena con objetos WEATHER_VOLUME
- Verificar que se inicializan correctamente
- Verificar que update se llama cada frame
- Verificar que se renderiza correctamente

---

## 7. Troubleshooting Común

### Problema: Los chunks tardan demasiado en generar

**Síntomas:**
- Frame rate cae a <30 FPS cuando la cámara se mueve rápido
- Vértices visibles desaparecen y reaparecen

**Soluciones:**
1. Reducir resolución de chunks
   ```cpp
   int resolution = 32;  // En lugar de 64
   ```

2. Usar threading asincrónico
   ```cpp
   m_generatorThread = std::thread([this]() {
       while (running) {
           auto request = m_generationQueue.dequeue();
           if (request) {
               auto data = generateChunk(...);
               m_generatedChunks.enqueue(data);
           }
       }
   });
   ```

3. Cachear agresivamente
   ```cpp
   m_maxMemoryBytes = 512 * 1024 * 1024;  // 512 MB
   ```

---

### Problema: Jittering/Shaking de la cámara

**Síntomas:**
- Objetos tiemblan o vibran cuando la cámara está quieta
- Peor a mayor distancia del origen

**Causa:**
- No estás usando Floating Origin correctamente

**Solución:**
```cpp
// ✅ CORRECTO
glm::vec3 localPos = m_worldSystem->toLocal(worldPos);
gl_Position = projection * view * vec4(vertexPos + localPos, 1.0);

// ❌ INCORRECTO
gl_Position = projection * view * vec4(vertexPos + glm::vec3(worldPos), 1.0);
// ↑ Pierdes precisión double al convertir
```

---

### Problema: Memory leak con chunks

**Síntomas:**
- Memoria sigue creciendo aunque cambies de escena
- ChunkCache nunca libera datos

**Causa:**
- m_cache no es destruido correctamente

**Solución:**
```cpp
// ✅ En destructor de PlanetarySystem
PlanetarySystem::~PlanetarySystem() {
    m_cache->clear();      // Liberar todos los chunks
    m_streaming->stop();   // Detener thread de streaming
    m_lod.reset();         // Liberar LODSystem
    // ... etc ...
}
```

---

### Problema: Los planetas no aparecen en el editor

**Síntomas:**
- Escena carga correctamente (no hay errores)
- Pero SceneHierarchy no muestra objetos PLANET

**Causa:**
- stringToObjectType() retorna UNKNOWN

**Solución:**
```cpp
// Verificar que el JSON tiene "type": "planet"
// Y que stringToObjectType("planet") == ObjectType::PLANET

inline ObjectType stringToObjectType(const std::string& s) {
    if (s == "planet") return ObjectType::PLANET;  // ✅ TIENE ESTO?
    // ...
}
```

