#include "lod_system.h"
#include "chunk_cache.h" // Para usar keyToHash

namespace Haruka {

LODSystem::LODSystem(double splitFactor, int maxLOD)
    : m_splitFactor(splitFactor), m_maxLOD(maxLOD) {}

void LODSystem::forgetChunk(const PlanetChunkKey& key) {
    m_lastFrameChunks.erase(ChunkCache::keyToHash(key));
}

bool LODSystem::isVisible(const PlanetChunkKey& key) const {
    return m_currentFrameChunks.count(ChunkCache::keyToHash(key)) > 0;
}

LODUpdate LODSystem::updatePlanetLOD(const std::shared_ptr<SceneObject>& planet, const glm::dvec3& cameraPos,
                                    const ResidencyFn& isResident) {
    LODUpdate update;
    update.planetName = planet->name;
    m_currentFrameChunks.clear();

    double radius = planet->scale.x; // Asumimos escala uniforme para el radio

    const glm::dvec3 planetPos = planet->position;

    // RESIDENCIA DEL PLANETA ENTERO: procesamos las 6 caras del cubo-esfera SIN
    // descartar ninguna. El quadtree mantiene las caras lejanas gruesas (no se
    // subdividen porque están lejos), así que TODO el planeta queda representado
    // como hojas finas-cerca + gruesas-lejos, una por zona (sin solape). Qué se
    // DIBUJA lo decide por chunk el horizon+frustum cull preciso del render
    // (TerrainRenderer/WaterRenderer), y el caché LRU lo mantiene residente
    // mientras haya presupuesto de memoria. Así girar o mirar a la cara opuesta
    // NUNCA recarga terreno (antes el face-cull la descargaba y regeneraba).
    for (int face = 0; face < 6; ++face) {
        const PlanetFace planetFace = static_cast<PlanetFace>(face);
        PlanetChunkKey rootKey{ planetFace, 0, 0, 0 };
        glm::dvec3 center = getCubeToSpherePos(planetFace, 0.5, 0.5, radius) + planetPos;

        LODNode root(rootKey, center, radius * 2.0);
        recursiveProcess(&root, cameraPos, update, radius, planetPos, isResident);
    }

    // 2:1 balance: ningún chunk vecino puede diferir en más de 1 nivel de LOD.
    // Sin esto, dos chunks adyacentes con diferencia ≥2 niveles dejan un escalón
    // recto que el morph (que solo salva 1 nivel) no puede cerrar.
    balanceLeaves();

    // Diff contra el frame anterior para load/keep/unload.
    for (auto& [hash, key] : m_currentFrameChunks) {
        if (m_lastFrameChunks.count(hash)) update.chunksToKeep.push_back(key);
        else                               update.chunksToLoad.push_back(key);
    }
    for (auto& [hash, key] : m_lastFrameChunks) {
        if (!m_currentFrameChunks.count(hash))
            update.chunksToUnload.push_back(key);
    }

    m_lastFrameChunks = m_currentFrameChunks;
    return update;
}

// Encuentra la hoja (en m_currentFrameChunks) que cubre la celda (face,lod,x,y),
// subiendo por los ancestros hasta la raíz. Devuelve false si ninguna existe.
bool LODSystem::findCoveringLeaf(PlanetFace face, int lod, uint32_t x, uint32_t y,
                                 PlanetChunkKey& out) const {
    for (int l = lod; l >= 0; --l) {
        uint32_t ax = x >> (lod - l);
        uint32_t ay = y >> (lod - l);
        PlanetChunkKey k{ face, (uint8_t)l, ax, ay };
        auto it = m_currentFrameChunks.find(ChunkCache::keyToHash(k));
        if (it != m_currentFrameChunks.end()) { out = it->second; return true; }
    }
    return false;
}

// Sustituye una hoja por sus 4 hijos en el conjunto de hojas.
void LODSystem::subdivideLeafKey(const PlanetChunkKey& k) {
    m_currentFrameChunks.erase(ChunkCache::keyToHash(k));
    int nl = k.lod + 1;
    for (int i = 0; i < 4; ++i) {
        PlanetChunkKey c{ k.face, (uint8_t)nl, k.x * 2 + (i % 2), k.y * 2 + (i / 2) };
        m_currentFrameChunks[ChunkCache::keyToHash(c)] = c;
    }
}

void LODSystem::balanceLeaves() {
    const int dx[4] = { -1, 1, 0, 0 };
    const int dy[4] = { 0, 0, -1, 1 };

    bool changed = true;
    int  guard   = 0;
    while (changed && guard++ < 128) {
        changed = false;
        // Snapshot de las hojas actuales (vamos a modificar el mapa al subdividir).
        std::vector<PlanetChunkKey> leaves;
        leaves.reserve(m_currentFrameChunks.size());
        for (auto& [h, k] : m_currentFrameChunks) leaves.push_back(k);

        for (const auto& L : leaves) {
            // Puede haber dejado de ser hoja en esta misma pasada.
            if (!m_currentFrameChunks.count(ChunkCache::keyToHash(L))) continue;

            const int n = 1 << L.lod;
            for (int d = 0; d < 4; ++d) {
                long nx = (long)L.x + dx[d];
                long ny = (long)L.y + dy[d];
                if (nx < 0 || ny < 0 || nx >= n || ny >= n) continue; // cross-face: omitido

                PlanetChunkKey cover;
                if (!findCoveringLeaf(L.face, L.lod, (uint32_t)nx, (uint32_t)ny, cover)) continue;

                if ((int)L.lod - (int)cover.lod > 1) {
                    subdivideLeafKey(cover); // acerca el vecino un nivel; el while repite hasta ≤1
                    changed = true;
                }
            }
        }
    }
}

// Altitude-based LOD ceiling: far from the planet (orbit / space) we DON'T subdivide to fine
// detail — the whole planet loads as a few coarse chunks (mountain-level, cheap). Near the
// surface the full m_maxLOD is allowed. `alt` is the camera altitude in planet RADII.
static int altitudeMaxLOD(double camDist, double radius, int hardMax) {
    double alt = (radius > 1e-9) ? std::max(0.0, (camDist - radius) / radius) : 0.0;
    if (alt < 0.02) return hardMax;                    // ~near surface: full detail
    if (alt < 0.08) return std::max(hardMax - 2, 10);  // low flight
    if (alt < 0.25) return 8;                           // high flight
    if (alt < 1.0)  return 7;                           // low orbit (antes 5 → solo ~39 chunks: muy bloqueado)
    if (alt < 4.0)  return 5;                           // orbit: planeta entero, más detalle
    return 4;                                           // deep space: coarsest
}

void LODSystem::recursiveProcess(LODNode* node, const glm::dvec3& cameraPos, LODUpdate& update, double radius, const glm::dvec3& planetPos, const ResidencyFn& isResident) {
    // Use the distance from camera to the SURFACE of the planet in the chunk's
    // direction rather than to the chunk center.  This allows side faces to
    // subdivide near the horizon even though their centers are far away.
    const glm::dvec3 chunkDir    = glm::normalize(node->center - planetPos);
    const glm::dvec3 surfacePoint = planetPos + chunkDir * radius;
    double dist = glm::distance(cameraPos, surfacePoint);
    uint64_t hash = ChunkCache::keyToHash(node->key);

    // Cap subdivision by camera altitude so a distant view shows the WHOLE planet cheaply.
    const double camDist = glm::distance(cameraPos, planetPos);
    const int camMaxLOD = altitudeMaxLOD(camDist, radius, m_maxLOD);

    // SUELO de LOD: el planeta ENTERO se subdivide hasta m_minLOD (todas las ~1536 piezas
    // base cargadas), PERO solo cuando estás CERCA de él (alt < ~1.5 radios). Sin este
    // gate, los planetas LEJANOS (Luna/Júpiter, simples puntos en el cielo) también se
    // forzarían a 1536 chunks cada uno → 3×1536 satura generación/memoria y no carga nada.
    const double camAlt = (radius > 1e-9) ? (camDist / radius - 1.0) : 1e9; // en radios
    const bool belowMin = (node->key.lod < m_minLOD) && (camAlt < 1.5);
    const bool distSplit = (dist < node->size * m_splitFactor && node->key.lod < camMaxLOD);
    bool wantSplit = belowMin || distSplit;

    // CARGA PROGRESIVA (grueso→fino, sin huecos negros): solo bajamos un nivel si este
    // nodo YA está residente (dibujándose) o ya tenía hijos residentes de un frame
    // previo. Así nunca pedimos chunks finos sin tener antes el grueso encima: siempre
    // hay algo dibujado mientras el detalle llega, y refina nivel a nivel. Sin predicado
    // (isResident=nullptr) → comportamiento clásico (subdivide directo al objetivo).
    if (wantSplit && isResident) {
        const uint8_t  cl = (uint8_t)(node->key.lod + 1);
        const uint32_t bx = node->key.x * 2, by = node->key.y * 2;
        const bool childIn = isResident({ node->key.face, cl, bx,     by     })
                          || isResident({ node->key.face, cl, bx + 1, by     })
                          || isResident({ node->key.face, cl, bx,     by + 1 })
                          || isResident({ node->key.face, cl, bx + 1, by + 1 });
        if (!isResident(node->key) && !childIn) {
            wantSplit = false;             // aún no está el grueso → espera
            update.residencyLimited = true; // el llamador debe recalcular hasta refinar
        }
    }

    if (wantSplit) {
        subdivide(node, radius, planetPos);
        for (auto& child : node->children) {
            recursiveProcess(child.get(), cameraPos, update, radius, planetPos, isResident);
        }
    } else {
        // Nodo hoja: solo registrar. El load/keep/unload se calcula tras el
        // balanceo 2:1 en updatePlanetLOD (las hojas pueden subdividirse luego).
        m_currentFrameChunks[hash] = node->key;
    }
}

void LODSystem::subdivide(LODNode* node, double radius, const glm::dvec3& planetPos) {
    node->isSubdivided = true;

    int nextLOD = node->key.lod + 1;
    double childSize = node->size * 0.5;

    uint32_t baseX = node->key.x * 2;
    uint32_t baseY = node->key.y * 2;
    double chunksPerAxis = std::pow(2.0, nextLOD);

    for (int i = 0; i < 4; ++i) {
        uint32_t cx = baseX + (i % 2);
        uint32_t cy = baseY + (i / 2);

        double u = (double(cx) + 0.5) / chunksPerAxis;
        double v = (double(cy) + 0.5) / chunksPerAxis;

        glm::dvec3 childCenter = getCubeToSpherePos(node->key.face, u, v, radius) + planetPos;
        PlanetChunkKey childKey = { node->key.face, (uint8_t)nextLOD, cx, cy };
        node->children[i] = std::make_unique<LODNode>(childKey, childCenter, childSize);
    }
}

glm::dvec3 LODSystem::getCubeToSpherePos(PlanetFace face, double u, double v, double radius) {
    // 1. Convertir UV [0, 1] a Espacio Local del Cubo [-1, 1]
    double localX = (u * 2.0) - 1.0;
    double localY = (v * 2.0) - 1.0;

    // 2. Proyectar en la cara del cubo (Misma convención que en TerrainGenerator)
    glm::dvec3 p;
    switch (face) {
        case PlanetFace::FRONT: p = {  1.0,    localY, -localX }; break;
        case PlanetFace::BACK: p = { -1.0,    localY,  localX }; break;
        case PlanetFace::TOP: p = {  localX,  1.0,   -localY }; break;
        case PlanetFace::BOTTOM: p = {  localX, -1.0,    localY }; break;
        case PlanetFace::RIGHT: p = {  localX,  localY,  1.0   }; break;
        case PlanetFace::LEFT: p = { -localX,  localY, -1.0   }; break;
    }

    // 3. Esferificación (Spherify) - Mapeo matemático para evitar distorsión
    double x2 = p.x * p.x;
    double y2 = p.y * p.y;
    double z2 = p.z * p.z;

    glm::dvec3 spherePos;
    spherePos.x = p.x * std::sqrt(1.0 - (y2 * 0.5) - (z2 * 0.5) + (y2 * z2 / 3.0));
    spherePos.y = p.y * std::sqrt(1.0 - (z2 * 0.5) - (x2 * 0.5) + (z2 * x2 / 3.0));
    spherePos.z = p.z * std::sqrt(1.0 - (x2 * 0.5) - (y2 * 0.5) + (x2 * y2 / 3.0));

    // 4. Escalar al radio del planeta
    return spherePos * radius;
}

} // namespace Haruka