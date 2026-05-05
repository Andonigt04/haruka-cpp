#include "lod_system.h"
#include "chunk_cache.h" // Para usar keyToHash

namespace Haruka {

LODSystem::LODSystem(double splitFactor, int maxLOD) 
    : m_splitFactor(splitFactor), m_maxLOD(maxLOD) {}

LODUpdate LODSystem::updatePlanetLOD(const std::shared_ptr<SceneObject>& planet, const glm::dvec3& cameraPos) {
    LODUpdate update;
    update.planetName = planet->name;
    m_currentFrameChunks.clear();

    double radius = planet->scale.x; // Asumimos escala uniforme para el radio

    // Procesar las 6 caras del cubo-esfera
    for (int face = 0; face < 6; ++face) {
        // Crear un nodo raíz para cada cara (LOD 0)
        PlanetChunkKey rootKey{ (uint8_t)face, 0, 0, 0 };
        glm::dvec3 center = getCubeToSpherePos(face, 0.5, 0.5, radius) + planet->position;
        
        LODNode root(rootKey, center, radius * 2.0);
        recursiveProcess(&root, cameraPos, update, radius);
    }

    // Calcular chunksToUnload (Estaban antes, pero no ahora)
    for (uint64_t oldHash : m_lastFrameChunks) {
        if (m_currentFrameChunks.find(oldHash) == m_currentFrameChunks.end()) {
            // Reconstruir key (o podrías guardar keys en lugar de hashes)
            // Para simplificar, aquí el StreamingSystem debería recibir los hashes a descargar
        }
    }

    m_lastFrameChunks = m_currentFrameChunks;
    return update;
}

void LODSystem::recursiveProcess(LODNode* node, const glm::dvec3& cameraPos, LODUpdate& update, double radius) {
    double dist = glm::distance(cameraPos, node->center);
    uint64_t hash = ChunkCache::keyToHash(node->key);

    // CRITERIO DE SUBDIVISIÓN
    if (dist < node->size * m_splitFactor && node->key.lod < m_maxLOD) {
        subdivide(node, radius);
        for (auto& child : node->children) {
            recursiveProcess(child.get(), cameraPos, update, radius);
        }
    } else {
        // Es un nodo hoja: registrar para renderizar
        m_currentFrameChunks.insert(hash);
        
        if (m_lastFrameChunks.count(hash)) {
            update.chunksToKeep.push_back(node->key);
        } else {
            update.chunksToLoad.push_back(node->key);
        }
    }
}

void LODSystem::subdivide(LODNode* node, double radius) {
    node->isSubdivided = true;
    
    int nextLOD = node->key.lod + 1;
    double childSize = node->size * 0.5; // El tamaño visual se reduce a la mitad
    
    // Al aumentar el LOD, la grilla se duplica. 
    // Un chunk en (x=1, y=0) genera hijos en x={2,3} e y={0,1}
    uint32_t baseX = node->key.x * 2;
    uint32_t baseY = node->key.y * 2;

    // Total de chunks a lo largo de un eje para este nuevo nivel de LOD (2^nextLOD)
    double chunksPerAxis = std::pow(2.0, nextLOD);

    for (int i = 0; i < 4; ++i) {
        // Calcular offset para cada uno de los 4 cuadrantes (00, 10, 01, 11)
        uint32_t cx = baseX + (i % 2);
        uint32_t cy = baseY + (i / 2);

        // Calcular el centro UV de este hijo en la cara del cubo
        // Sumamos 0.5 para apuntar al centro exacto del chunk, no a su esquina
        double u = (double(cx) + 0.5) / chunksPerAxis;
        double v = (double(cy) + 0.5) / chunksPerAxis;

        // Calcular la posición 3D real en la superficie del planeta
        glm::dvec3 childCenter = getCubeToSpherePos(node->key.face, u, v, radius);
        
        // ¡Importante! Si la posición del planeta no es (0,0,0), debemos sumarla
        // Asumiendo que radius ya considera la escala, aquí solo es posición relativa.
        // En `updatePlanetLOD` deberías sumar `planet->position` al final si manejas coordenadas globales.

        // Crear la llave del hijo
        PlanetChunkKey childKey = { node->key.face, (uint8_t)nextLOD, cx, cy };

        // Instanciar el nodo hijo
        node->children[i] = std::make_unique<LODNode>(childKey, childCenter, childSize);
    }
}

glm::dvec3 LODSystem::getCubeToSpherePos(int face, double u, double v, double radius) {
    // 1. Convertir UV [0, 1] a Espacio Local del Cubo [-1, 1]
    double localX = (u * 2.0) - 1.0;
    double localY = (v * 2.0) - 1.0;

    // 2. Proyectar en la cara del cubo (Misma convención que en TerrainGenerator)
    glm::dvec3 p;
    switch (face) {
        case 0: p = {  1.0,    localY, -localX }; break; // Front
        case 1: p = { -1.0,    localY,  localX }; break; // Back
        case 2: p = {  localX,  1.0,   -localY }; break; // Top
        case 3: p = {  localX, -1.0,    localY }; break; // Bottom
        case 4: p = {  localX,  localY,  1.0   }; break; // Right
        case 5: p = { -localX,  localY, -1.0   }; break; // Left
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