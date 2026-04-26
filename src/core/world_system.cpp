#include "world_system.h"

#include <cmath>
#include <set>

#include "camera.h"

namespace Haruka {

WorldSystem::WorldSystem() : worldOrigin(0.0, 0.0, 0.0) {
    // LOD distances - ajustados para editor (unidades pequeñas)
    // La cámara está típicamente a 100-10000 unidades del planeta
    lodDistances[0] = 100.0f;         // < 100 unidades = LOD 0 (máximo detalle)
    lodDistances[1] = 500.0f;         // < 500 unidades = LOD 1
    lodDistances[2] = 2000.0f;        // < 2000 unidades = LOD 2
    lodDistances[3] = 10000.0f;       // < 10000 unidades = LOD 3
}

WorldSystem::~WorldSystem() {}

Haruka::LocalPos WorldSystem::toLocal(Haruka::WorldPos objectPos, Haruka::WorldPos referencePos) {
    glm::dvec3 diff = objectPos - referencePos;
    return Haruka::LocalPos(diff);
}

void WorldSystem::updateOrigin(Haruka::WorldPos newOrigin) {
    glm::dvec3 offset = newOrigin - worldOrigin;
    worldOrigin = newOrigin;

    for (auto& body : celestialBodies) {
        body.worldPos -= offset;
    }
}

void WorldSystem::updateLocalPositions(Haruka::WorldPos cameraWorldPos) {
    for (auto& body : celestialBodies) {
        body.localPos = toLocal(body.worldPos, cameraWorldPos);
    }
}

void WorldSystem::addBody(const CelestialBody& body) {
    celestialBodies.push_back(body);
}

void WorldSystem::removeBody(const std::string& name) {
    celestialBodies.erase(
        std::remove_if(celestialBodies.begin(), celestialBodies.end(),
            [&name](const CelestialBody& b) { return b.name == name; }),
        celestialBodies.end()
    );
}

CelestialBody* WorldSystem::findBody(const std::string& name) {
    for (auto& body : celestialBodies) {
        if (body.name == name) return &body;
    }
    return nullptr;
}

const std::vector<CelestialBody>& WorldSystem::getBodies() const {
    return celestialBodies;
}

size_t WorldSystem::getBodyCount() const {
    return celestialBodies.size();
}

Haruka::WorldPos WorldSystem::getOrigin() const {
    return worldOrigin;
}

void WorldSystem::initComputeShaders() {
    // Placeholder: CPU culling, no compute shader needed
}

void WorldSystem::setLODDistances(float lod0, float lod1, float lod2, float lod3) {
    lodDistances[0] = lod0;
    lodDistances[1] = lod1;
    lodDistances[2] = lod2;
    lodDistances[3] = lod3;
}

void WorldSystem::frustumCull(Haruka::WorldPos cameraPos, const glm::mat4& viewProj, float frustumDistance) {
    (void)viewProj;
    for (auto& body : celestialBodies) {
        float distance = static_cast<float>(glm::length(body.worldPos - cameraPos));
        
        // Aumentar radio de "nearby chunks" a 5000 unidades
        // Chunks dentro del frustum siempre son visibles
        bool nearbyChunk = (distance < 5000.0f);
        bool withinFrustum = (distance < frustumDistance + body.radius);
        
        if (nearbyChunk || withinFrustum) {
            body.visible = 1;
            
            // Asignar LOD basado en distancia
            // IMPORTANTE: Siempre mostrar algo, incluso si está muy lejos
            if (distance < lodDistances[0]) {
                body.lodLevel = 0;
            } else if (distance < lodDistances[1]) {
                body.lodLevel = 1;
            } else if (distance < lodDistances[2]) {
                body.lodLevel = 2;
            } else {
                // BUGFIX: En lugar de marcar como no visible, usar LOD 3 para distancias lejanas
                body.lodLevel = 3;  // Máximo LOD lejano
            }
        } else {
            body.visible = 0;
        }
    }
}

void WorldSystem::setChunkStreamingBudgets(size_t maxLoads, size_t maxEvicts, size_t maxResident, size_t maxMemoryMB) {
    maxLoadsPerFrame = std::max<size_t>(1, maxLoads);
    maxEvictsPerFrame = std::max<size_t>(1, maxEvicts);
    maxResidentChunks = std::max<size_t>(64, maxResident);
    maxMemoryBytes = std::max<size_t>(64 * 1024 * 1024, maxMemoryMB * 1024 * 1024);
}

void WorldSystem::setChunkGrid(int face, int lod, int tilesX, int tilesY, int maxLod) {
    chunkGridFace = std::clamp(face, 0, 5);
    chunkGridLod = std::max(0, lod);
    chunkGridTilesX = std::max(0, tilesX);
    chunkGridTilesY = std::max(0, tilesY);
    chunkGridMaxLod = std::max(0, maxLod);
}

void WorldSystem::updateVisibleChunks(float viewDistanceKm, int lod, Camera* camera) {
    chunkFrameCounter++;
    visibleChunks.clear();
    renderBaseMeshOnly = true; // Por defecto, solo mesh base

    const double camLen = glm::length(camera->position);
    if (camLen <= 1e-6) return;

    glm::dvec3 camDir;
    if (camera != nullptr && camLen > 1e-6) {
        camDir = camera->position / camLen;
    } else {
        return;
    }

    const int lodClamped = std::clamp(lod, 0, 16);
    const int binsX = (chunkGridTilesX > 0) ? chunkGridTilesX : std::clamp(1 << std::min(8, 3 + lodClamped), 8, 256);
    const int binsY = (chunkGridTilesY > 0) ? chunkGridTilesY : std::clamp(1 << std::min(8, 3 + lodClamped), 8, 256);
    const bool fixedGrid = (chunkGridTilesX > 0 && chunkGridTilesY > 0);
    const int keyLodBase = (chunkGridTilesX > 0 && chunkGridTilesY > 0) ? chunkGridLod : lodClamped;

    const double viewRatio = std::clamp(static_cast<double>(viewDistanceKm) / std::max(1.0, camLen), 0.05, 2.5);
    const double horizonDot = -0.8;

    std::set<PlanetChunkKey> uniqueVisible;
    const int firstFace = 0;
    const int lastFace = 5;
    for (int f = firstFace; f <= lastFace; ++f) {
        for (int y = 0; y < binsY; ++y) {
            const double v = ((static_cast<double>(y) + 0.5) / static_cast<double>(binsY)) * 2.0 - 1.0;
            for (int x = 0; x < binsX; ++x) {
                const double u = ((static_cast<double>(x) + 0.5) / static_cast<double>(binsX)) * 2.0 - 1.0;

                glm::dvec3 cube;
                switch (f) {
                    case 0: cube = glm::dvec3( 1.0,  v, -u); break;
                    case 1: cube = glm::dvec3(-1.0,  v,  u); break;
                    case 2: cube = glm::dvec3( u,  1.0, -v); break;
                    case 3: cube = glm::dvec3( u, -1.0,  v); break;
                    case 4: cube = glm::dvec3( u,  v,  1.0); break;
                    default:cube = glm::dvec3(-u,  v, -1.0); break;
                }

                glm::dvec3 dir = glm::normalize(cube);
                const double dotv = glm::dot(dir, camDir);
                if (dotv < horizonDot) continue;

                glm::dvec3 chunkWorldPos = dir * camLen;
                double realDistance = glm::length(camera->position - chunkWorldPos);

                // Lógica de cúpulas LOD
                int chunkLOD = -1;
                if (realDistance < domeRadius0) {
                    chunkLOD = 0; // Máxima calidad
                } else if (realDistance < domeRadius1) {
                    chunkLOD = 1; // Media
                } else if (realDistance < domeRadius2) {
                    chunkLOD = 2; // Mínima
                } else {
                    // Demasiado lejos, no renderizar chunk
                    continue;
                }

                int lodOffset = 0;
                const int keyLod = chunkLOD; // Usar LOD según cúpula
                const int keyX = x >> lodOffset;
                const int keyY = y >> lodOffset;

                PlanetChunkKey key{f, keyLod, keyX, keyY};
                if (!uniqueVisible.insert(key).second) continue;
                auto& st = chunkStates[key];
                st.visible = true;
                st.lastTouchedFrame = chunkFrameCounter;
                st.distanceKm = static_cast<float>(realDistance / 1000.0);
                visibleChunks.push_back(key);
                renderBaseMeshOnly = false; // Hay al menos un chunk visible
            }
        }
    }

    for (auto& [_, st] : chunkStates) {
        if (st.lastTouchedFrame != chunkFrameCounter) st.visible = false;
    }
}

void WorldSystem::scheduleChunkStreaming() {
    // =============================================================================
    // CHUNK STREAMING SCHEDULER: Decidir qué chunks cargar/descargar cada frame
    // =============================================================================
    // POLÍTICA DE CARGA:
    // - Cargar: Chunks visibles que NO están residentes (en memoria)
    // - Límite: maxLoadsPerFrame (evitar stalls de GPU)
    //
    // POLÍTICA DE EVICCIÓN (LRU + Distance-based + Timeout):
    // - Candidatos: Chunks residentes que NO son visibles (lejanos/fuera de vista)
    // - Orden: 1) Chunks con timeout expirado (> 300 frames no usados)
    //          2) lastTouchedFrame ascendente (menos reciente primero)
    //          3) distanceKm descendente (más lejano primero)
    // - Límites: maxResidentChunks Y/O maxMemoryBytes (lo que se alcance primero)
    // - Max evicts: Aumenta si hay chunks cercanos pendientes de carga
    // =============================================================================

    pendingChunkLoads.clear();
    pendingChunkEvictions.clear();

    // FASE 1: Programar cargas de chunks visibles
    for (const auto& key : visibleChunks) {
        auto it = chunkStates.find(key);
        if (it != chunkStates.end() && it->second.visible && !it->second.resident) {
            if (pendingChunkLoads.size() < maxLoadsPerFrame) {
                pendingChunkLoads.push_back(key);
            }
        }
    }

    // FASE 2: Identificar candidatos a evicción
    size_t residentCount = 0;
    std::vector<std::pair<PlanetChunkKey, PlanetChunkState>> evictionCandidates;
    evictionCandidates.reserve(chunkStates.size());
    
    // Separar candidatos en prioritarios (viejos/lejanos) y normales
    std::vector<std::pair<PlanetChunkKey, PlanetChunkState>> priorityEvictions;  // Chunks con timeout
    
    const uint64_t CHUNK_TIMEOUT_FRAMES = 300;  // Timeout de 300 frames (~5 segundos a 60FPS)
    
    for (const auto& [key, st] : chunkStates) {
        if (!st.resident) continue;
        residentCount++;
        
        // Solo considerar evicción de chunks que no son visibles
        if (!st.visible) {
            // Separar chunks con timeout en categoría prioritaria
            if ((chunkFrameCounter - st.lastTouchedFrame) > CHUNK_TIMEOUT_FRAMES) {
                priorityEvictions.push_back({key, st});
            } else {
                evictionCandidates.push_back({key, st});
            }
        }
    }

    // FASE 3: Determinar si hay presión de memoria o límite de chunks
    bool overCount = residentCount > maxResidentChunks;
    bool overMemory = residentMemoryBytes > maxMemoryBytes;
    bool hasPendingLoads = !pendingChunkLoads.empty();
    
    if (overCount || overMemory || (hasPendingLoads && residentCount > maxResidentChunks * 0.8f)) {
        // Si hay chunks cercanos pendientes, ser más agresivo con evicción
        size_t maxEvicts = hasPendingLoads ? (maxEvictsPerFrame * 2) : maxEvictsPerFrame;
        
        // Primero descargar chunks con timeout expirado
        std::sort(priorityEvictions.begin(), priorityEvictions.end(),
                  [](const auto& a, const auto& b) {
                      // Ordenar por distancia (más lejano primero)
                      return a.second.distanceKm > b.second.distanceKm;
                  });
        
        for (const auto& [key, st] : priorityEvictions) {
            if (pendingChunkEvictions.size() >= maxEvicts) break;
            pendingChunkEvictions.push_back(key);
        }
        
        // Luego descargar chunks normales (LRU + distancia)
        std::sort(evictionCandidates.begin(), evictionCandidates.end(),
                  [](const auto& a, const auto& b) {
                      // Prioridad 1: lastTouchedFrame (menos reciente = eliminar primero)
                      if (a.second.lastTouchedFrame != b.second.lastTouchedFrame) {
                          return a.second.lastTouchedFrame < b.second.lastTouchedFrame;
                      }
                      // Prioridad 2: distancia (más lejano = eliminar primero)
                      return a.second.distanceKm > b.second.distanceKm;
                  });

        // Calcular cuánto hay que liberar
        size_t bytesToFree = overMemory ? (residentMemoryBytes - maxMemoryBytes) : 0;
        size_t countToFree = overCount ? (residentCount - maxResidentChunks) : 0;
        
        // Programar eviciones hasta alcanzar límites
        for (size_t i = 0; i < evictionCandidates.size() && pendingChunkEvictions.size() < maxEvicts; ++i) {
            pendingChunkEvictions.push_back(evictionCandidates[i].first);
            bytesToFree = (bytesToFree >= evictionCandidates[i].second.bytesUsed) ? 
                         (bytesToFree - evictionCandidates[i].second.bytesUsed) : 0;
            if (countToFree > 0) countToFree--;
            // Parar cuando se alcancen ambos límites
            if (bytesToFree == 0 && countToFree == 0) break;
        }
    }
}

void WorldSystem::markChunkResident(const PlanetChunkKey& key, bool resident) {
    auto& st = chunkStates[key];
    if (resident && !st.resident) {
        residentMemoryBytes += st.bytesUsed;
    } else if (!resident && st.resident) {
        residentMemoryBytes = (st.bytesUsed <= residentMemoryBytes) ? (residentMemoryBytes - st.bytesUsed) : 0;
    }
    st.resident = resident;
    if (resident) st.lastTouchedFrame = chunkFrameCounter;
}

size_t WorldSystem::getResidentChunkCount() const {
    size_t count = 0;
    for (const auto& [_, st] : chunkStates) {
        if (st.resident) count++;
    }
    return count;
}

void WorldSystem::setChunkSize(const PlanetChunkKey& key, uint32_t bytes) {
    auto it = chunkStates.find(key);
    if (it == chunkStates.end()) return;
    if (it->second.resident) {
        if (bytes >= it->second.bytesUsed) {
            residentMemoryBytes += static_cast<size_t>(bytes - it->second.bytesUsed);
        } else {
            residentMemoryBytes -= static_cast<size_t>(it->second.bytesUsed - bytes);
        }
    }
    it->second.bytesUsed = bytes;
}

void WorldSystem::getNeighborLods(const PlanetChunkKey& key, int& outN, int& outS, int& outE, int& outW) const {
    // =============================================================================
    // Busca los LODs de los chunks vecinos (N, S, E, W) en el espacio cube-sphere.
    // Si un vecino no existe, retorna el LOD actual como fallback (seguro).
    //
    // TOPOLOGY: En un cube-sphere, cada cara tiene su propio grid de chunks [0,tilesX) x [0,tilesY).
    // - Y es vertical (vinculado a polos): puede cruzar límites de cara en Y
    // - X es cilíndrico: puede wrappear dentro de la misma cara
    // =============================================================================

    // --- NORTE: decrementar Y ---
    // Busca el vecino al norte (y-1)
    {
        PlanetChunkKey northKey = key;
        northKey.y -= 1;
        auto itN = chunkStates.find(northKey);
        outN = (itN != chunkStates.end()) ? itN->first.lod : key.lod;
    }

    // --- SUR: incrementar Y ---
    // Busca el vecino al sur (y+1)
    {
        PlanetChunkKey southKey = key;
        southKey.y += 1;
        auto itS = chunkStates.find(southKey);
        outS = (itS != chunkStates.end()) ? itS->first.lod : key.lod;
    }

    // --- ESTE: incrementar X (con wrap-around cilíndrico) ---
    // En cube-sphere, X es cilíndrico: [0, tilesPerFace) con envoltura
    // NOTA: En esta implementación básica, los chunks no llevan info de tilesPerFace,
    // así que no podemos hacer wrap-around. Se requeriría pasar tilesPerFace como parámetro.
    {
        PlanetChunkKey eastKey = key;
        eastKey.x += 1;
        // TODO: Implementar wrap-around si tilesPerFace fuera accesible
        // if (eastKey.x >= tilesPerFace) eastKey.x = 0;
        auto itE = chunkStates.find(eastKey);
        outE = (itE != chunkStates.end()) ? itE->first.lod : key.lod;
    }

    // --- OESTE: decrementar X (con wrap-around cilíndrico) ---
    {
        PlanetChunkKey westKey = key;
        westKey.x -= 1;
        // TODO: Implementar wrap-around si tilesPerFace fuera accesible
        // if (westKey.x < 0) westKey.x = tilesPerFace - 1;
        auto itW = chunkStates.find(westKey);
        outW = (itW != chunkStates.end()) ? itW->first.lod : key.lod;
    }
}
} // namespace Haruka