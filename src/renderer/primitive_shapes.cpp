#include "primitive_shapes.h"

#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>

void PrimitiveShapes::createSphere(float radius, int sectors, int stacks, std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& normals, std::vector<unsigned int>& indices) {
    vertices.clear();
    normals.clear();
    indices.clear();

    float x, y, z, xy;
    float sectorStep = 2 * glm::pi<float>() / sectors;
    float stackStep = glm::pi<float>() / stacks;
    float sectorAngle, stackAngle;

    for (int i = 0; i <= stacks; ++i) {
        stackAngle = glm::pi<float>() / 2 - i * stackStep;
        xy = radius * cos(stackAngle);
        z = radius * sin(stackAngle);

        for (int j = 0; j <= sectors; ++j) {
            sectorAngle = j * sectorStep;
            x = xy * cos(sectorAngle);
            y = xy * sin(sectorAngle);

            vertices.push_back(glm::vec3(x, y, z));
            glm::vec3 normal = glm::normalize(glm::vec3(x, y, z));
            normals.push_back(normal);
        }
    }

    unsigned int k1, k2;
    for (int i = 0; i < stacks; ++i) {
        k1 = i * (sectors + 1);
        k2 = k1 + sectors + 1;

        for (int j = 0; j < sectors; ++j, ++k1, ++k2) {
            if (i != 0) {
                indices.push_back(k1);
                indices.push_back(k2);
                indices.push_back(k1 + 1);
            }

            if (i != (stacks - 1)) {
                indices.push_back(k1 + 1);
                indices.push_back(k2);
                indices.push_back(k2 + 1);
            }
        }
    }
}

void PrimitiveShapes::createSphereLOD(float radius, int sectors, int stacks, std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& normals, std::vector<unsigned int>& indices)
{
    createSphere(radius, sectors, stacks, vertices, normals, indices);
}

void PrimitiveShapes::createCube(float size, std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& normals, std::vector<unsigned int>& indices)
{
    vertices.clear();
    normals.clear();
    indices.clear();

    float s = size / 2.0f;

    // Crear 24 vértices (4 por cara, para normales per-face)
    // Back face (z = -s)
    vertices.push_back({-s, -s, -s}); vertices.push_back({s, -s, -s}); 
    vertices.push_back({s, s, -s}); vertices.push_back({-s, s, -s});
    
    // Front face (z = s)
    vertices.push_back({-s, -s, s}); vertices.push_back({s, -s, s}); 
    vertices.push_back({s, s, s}); vertices.push_back({-s, s, s});
    
    // Left face (x = -s)
    vertices.push_back({-s, -s, -s}); vertices.push_back({-s, -s, s}); 
    vertices.push_back({-s, s, s}); vertices.push_back({-s, s, -s});
    
    // Right face (x = s)
    vertices.push_back({s, -s, -s}); vertices.push_back({s, -s, s}); 
    vertices.push_back({s, s, s}); vertices.push_back({s, s, -s});
    
    // Bottom face (y = -s)
    vertices.push_back({-s, -s, -s}); vertices.push_back({s, -s, -s}); 
    vertices.push_back({s, -s, s}); vertices.push_back({-s, -s, s});
    
    // Top face (y = s)
    vertices.push_back({-s, s, -s}); vertices.push_back({s, s, -s}); 
    vertices.push_back({s, s, s}); vertices.push_back({-s, s, s});

    // Normals (una por cara, aplicada a los 4 vértices de esa cara)
    glm::vec3 faceNormals[6] = {
        {0, 0, -1}, {0, 0, 1},   // Back, Front
        {-1, 0, 0}, {1, 0, 0},   // Left, Right
        {0, -1, 0}, {0, 1, 0}    // Bottom, Top
    };

    // Asignar normales (4 vértices por cara)
    for (int face = 0; face < 6; ++face) {
        for (int i = 0; i < 4; ++i) {
            normals.push_back(faceNormals[face]);
        }
    }

    // Índices (2 triángulos por cara)
    for (int face = 0; face < 6; ++face) {
        unsigned int baseIdx = face * 4;
        // Triángulo 1
        indices.push_back(baseIdx);
        indices.push_back(baseIdx + 2);
        indices.push_back(baseIdx + 1);
        // Triángulo 2
        indices.push_back(baseIdx);
        indices.push_back(baseIdx + 3);
        indices.push_back(baseIdx + 2);
    }
}

void PrimitiveShapes::createCapsule(float radius, float height, int sectors, int stacks, std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& normals, std::vector<unsigned int>& indices)
{
    vertices.clear();
    normals.clear();
    indices.clear();

    float cylinderHeight = std::max(0.0f, height - 2.0f * radius);
    float halfCylinder = cylinderHeight * 0.5f;
    int hemisphereStacks = std::max(2, stacks / 2);
    int cylinderStacks = std::max(1, stacks - hemisphereStacks * 2);

    auto addRing = [&](float y, float ringRadius) {
        for (int j = 0; j <= sectors; ++j) {
            float sectorAngle = 2.0f * glm::pi<float>() * (float)j / (float)sectors;
            float x = ringRadius * std::cos(sectorAngle);
            float z = ringRadius * std::sin(sectorAngle);
            vertices.push_back({x, y, z});
            normals.push_back(glm::normalize(glm::vec3(x, y >= 0.0f ? (y - halfCylinder) : (y + halfCylinder), z)));
        }
    };

    // Top pole
    vertices.push_back({0.0f, halfCylinder + radius, 0.0f});
    normals.push_back({0.0f, 1.0f, 0.0f});

    // Top hemisphere
    for (int i = 1; i < hemisphereStacks; ++i) {
        float t = (float)i / (float)hemisphereStacks;
        float theta = t * (glm::half_pi<float>());
        float ringRadius = radius * std::sin(theta);
        float y = halfCylinder + radius * std::cos(theta);
        addRing(y, ringRadius);
    }

    // Cylinder rings
    for (int i = 0; i <= cylinderStacks; ++i) {
        float t = (float)i / (float)cylinderStacks;
        float y = halfCylinder - t * cylinderHeight;
        addRing(y, radius);
    }

    // Bottom hemisphere
    for (int i = 1; i < hemisphereStacks; ++i) {
        float t = (float)i / (float)hemisphereStacks;
        float theta = t * (glm::half_pi<float>());
        float ringRadius = radius * std::cos(theta);
        float y = -halfCylinder - radius * std::sin(theta);
        addRing(y, ringRadius);
    }

    // Bottom pole
    vertices.push_back({0.0f, -halfCylinder - radius, 0.0f});
    normals.push_back({0.0f, -1.0f, 0.0f});

    // Build indices between consecutive rings
    auto ringStart = [&](int ringIndex, int ringSize) {
        return ringIndex * ringSize;
    };

    int ringSize = sectors + 1;
    int totalRings = (int)vertices.size() / ringSize; // approximate, enough for generated layout
    int currentRing = 0;

    // Top fan
    int firstRingStart = 1;
    if (totalRings > 1) {
        for (int j = 0; j < sectors; ++j) {
            indices.push_back(0);
            indices.push_back(firstRingStart + j);
            indices.push_back(firstRingStart + j + 1);
        }
    }

    // Quads between rings
    for (int ring = 0; ring + 1 < totalRings - 1; ++ring) {
        int aStart = ringStart(ring, ringSize) + 1;
        int bStart = ringStart(ring + 1, ringSize) + 1;

        // Skip if ring layout is not perfectly aligned; this still produces a valid mesh
        for (int j = 0; j < sectors; ++j) {
            unsigned int a = aStart + j;
            unsigned int b = aStart + j + 1;
            unsigned int c = bStart + j;
            unsigned int d = bStart + j + 1;
            if (a < vertices.size() && b < vertices.size() && c < vertices.size() && d < vertices.size()) {
                indices.push_back(a);
                indices.push_back(c);
                indices.push_back(b);

                indices.push_back(b);
                indices.push_back(c);
                indices.push_back(d);
            }
        }
    }

    // Bottom fan (best-effort)
    if (vertices.size() >= 2) {
        unsigned int bottomIndex = (unsigned int)vertices.size() - 1;
        unsigned int prevRingStart = bottomIndex > (unsigned int)(ringSize + 1) ? bottomIndex - (ringSize) : 0;
        for (int j = 0; j < sectors; ++j) {
            indices.push_back(bottomIndex);
            indices.push_back(prevRingStart + j + 1);
            indices.push_back(prevRingStart + j);
        }
    }
}

void PrimitiveShapes::createPlane(float width, float height, int subdivisions, std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& normals, std::vector<unsigned int>& indices)
{
    vertices.clear();
    normals.clear();
    indices.clear();

    float w = width / 2.0f;
    float h = height / 2.0f;
    float stepX = width / subdivisions;
    float stepY = height / subdivisions;

    // Generate vertices
    for (int y = 0; y <= subdivisions; ++y) {
        for (int x = 0; x <= subdivisions; ++x) {
            float px = -w + x * stepX;
            float py = 0.0f;
            float pz = -h + y * stepY;
            vertices.push_back({px, py, pz});
            normals.push_back({0, 1, 0});  // Up normal
        }
    }

    // Generate indices
    for (int y = 0; y < subdivisions; ++y) {
        for (int x = 0; x < subdivisions; ++x) {
            unsigned int a = y * (subdivisions + 1) + x;
            unsigned int b = a + 1;
            unsigned int c = a + (subdivisions + 1);
            unsigned int d = c + 1;

            indices.push_back(a);
            indices.push_back(c);
            indices.push_back(b);

            indices.push_back(b);
            indices.push_back(c);
            indices.push_back(d);
        }
    }
}

void PrimitiveShapes::createCubeSphere(float radius, int subdivisions, std::vector<glm::vec3>& vertices, std::vector<glm::vec3>& normals, std::vector<unsigned int>& indices)
{
    vertices.clear();
    normals.clear();
    indices.clear();

    // Crear las 6 caras de un cubo
    std::vector<glm::vec3> faceVertices[6];
    std::vector<unsigned int> faceIndices[6];
    
    // Definir las 6 caras del cubo (cada cara es un grid)
    glm::vec3 faceNormals[6] = {
        glm::vec3(1, 0, 0),   // Derecha
        glm::vec3(-1, 0, 0),  // Izquierda
        glm::vec3(0, 1, 0),   // Arriba
        glm::vec3(0, -1, 0),  // Abajo
        glm::vec3(0, 0, 1),   // Frente
        glm::vec3(0, 0, -1)   // Atrás
    };
    
    int gridSize = 2 << subdivisions; // 2^(subdivisions+1)
    float step = 2.0f / gridSize;
    
    // Para cada cara del cubo
    for (int face = 0; face < 6; face++) {
        std::vector<glm::vec3> faceVerts;
        
        // Generar grid de vértices para esta cara
        for (int i = 0; i <= gridSize; i++) {
            for (int j = 0; j <= gridSize; j++) {
                float u = -1.0f + i * step;
                float v = -1.0f + j * step;
                
                glm::vec3 p;
                if (face == 0) p = glm::vec3(1, v, -u);      // Derecha
                else if (face == 1) p = glm::vec3(-1, v, u); // Izquierda
                else if (face == 2) p = glm::vec3(u, 1, v);  // Arriba
                else if (face == 3) p = glm::vec3(u, -1, -v);// Abajo
                else if (face == 4) p = glm::vec3(u, v, 1);  // Frente
                else p = glm::vec3(-u, v, -1);               // Atrás
                
                // Normalizar para convertir a esfera
                glm::vec3 normalized = glm::normalize(p) * radius;
                faceVerts.push_back(normalized);
                vertices.push_back(normalized);
                normals.push_back(glm::normalize(normalized));
            }
        }
        
        // Generar índices para esta cara (quads -> triangles)
        int stride = gridSize + 1;
        for (int i = 0; i < gridSize; i++) {
            for (int j = 0; j < gridSize; j++) {
                int a = i * stride + j;
                int b = a + 1;
                int c = a + stride;
                int d = c + 1;
                
                unsigned int baseIndex = vertices.size() - faceVerts.size() + a;
                unsigned int baseB = baseIndex + 1;
                unsigned int baseC = baseIndex + stride;
                unsigned int baseD = baseC + 1;
                
                // Primer triángulo
                indices.push_back(baseIndex);
                indices.push_back(baseC);
                indices.push_back(baseB);
                
                // Segundo triángulo
                indices.push_back(baseB);
                indices.push_back(baseC);
                indices.push_back(baseD);
            }
        }
    }
}

void PrimitiveShapes::createCubeVertex(float size, std::vector<Vertex>& vertices, std::vector<unsigned int>& indices) {
    vertices.clear();
    indices.clear();
    
    float s = size / 2.0f;
    
    // Crear 24 vértices (4 por cara para texturas UV)
    // Cara frontal (+Z)
    vertices.push_back({glm::vec3(-s, -s,  s), glm::vec3(0, 0, 1), glm::vec2(0, 0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)});
    vertices.push_back({glm::vec3( s, -s,  s), glm::vec3(0, 0, 1), glm::vec2(1, 0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)});
    vertices.push_back({glm::vec3( s,  s,  s), glm::vec3(0, 0, 1), glm::vec2(1, 1), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)});
    vertices.push_back({glm::vec3(-s,  s,  s), glm::vec3(0, 0, 1), glm::vec2(0, 1), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)});
    
    // Cara trasera (-Z)
    vertices.push_back({glm::vec3( s, -s, -s), glm::vec3(0, 0, -1), glm::vec2(0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0)});
    vertices.push_back({glm::vec3(-s, -s, -s), glm::vec3(0, 0, -1), glm::vec2(1, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0)});
    vertices.push_back({glm::vec3(-s,  s, -s), glm::vec3(0, 0, -1), glm::vec2(1, 1), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0)});
    vertices.push_back({glm::vec3( s,  s, -s), glm::vec3(0, 0, -1), glm::vec2(0, 1), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0)});
    
    // Cara izquierda (-X)
    vertices.push_back({glm::vec3(-s, -s, -s), glm::vec3(-1, 0, 0), glm::vec2(0, 0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0)});
    vertices.push_back({glm::vec3(-s, -s,  s), glm::vec3(-1, 0, 0), glm::vec2(1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0)});
    vertices.push_back({glm::vec3(-s,  s,  s), glm::vec3(-1, 0, 0), glm::vec2(1, 1), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0)});
    vertices.push_back({glm::vec3(-s,  s, -s), glm::vec3(-1, 0, 0), glm::vec2(0, 1), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0)});
    
    // Cara derecha (+X)
    vertices.push_back({glm::vec3( s, -s,  s), glm::vec3(1, 0, 0), glm::vec2(0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0)});
    vertices.push_back({glm::vec3( s, -s, -s), glm::vec3(1, 0, 0), glm::vec2(1, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0)});
    vertices.push_back({glm::vec3( s,  s, -s), glm::vec3(1, 0, 0), glm::vec2(1, 1), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0)});
    vertices.push_back({glm::vec3( s,  s,  s), glm::vec3(1, 0, 0), glm::vec2(0, 1), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0)});
    
    // Cara superior (+Y)
    vertices.push_back({glm::vec3(-s,  s,  s), glm::vec3(0, 1, 0), glm::vec2(0, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1)});
    vertices.push_back({glm::vec3( s,  s,  s), glm::vec3(0, 1, 0), glm::vec2(1, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1)});
    vertices.push_back({glm::vec3( s,  s, -s), glm::vec3(0, 1, 0), glm::vec2(1, 1), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1)});
    vertices.push_back({glm::vec3(-s,  s, -s), glm::vec3(0, 1, 0), glm::vec2(0, 1), glm::vec3(1, 0, 0), glm::vec3(0, 0, -1)});
    
    // Cara inferior (-Y)
    vertices.push_back({glm::vec3(-s, -s, -s), glm::vec3(0, -1, 0), glm::vec2(0, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1)});
    vertices.push_back({glm::vec3( s, -s, -s), glm::vec3(0, -1, 0), glm::vec2(1, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1)});
    vertices.push_back({glm::vec3( s, -s,  s), glm::vec3(0, -1, 0), glm::vec2(1, 1), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1)});
    vertices.push_back({glm::vec3(-s, -s,  s), glm::vec3(0, -1, 0), glm::vec2(0, 1), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1)});
    
    // Índices (2 triángulos por cara, 6 caras = 36 índices)
    for (unsigned int i = 0; i < 6; ++i) {
        unsigned int base = i * 4;
        indices.push_back(base);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        
        indices.push_back(base);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
}

void PrimitiveShapes::createSphereVertex(float radius, int sectors, int stacks, std::vector<Vertex>& vertices, std::vector<unsigned int>& indices) {
    vertices.clear();
    indices.clear();
    
    float x, y, z, xy;
    float sectorStep = 2 * glm::pi<float>() / sectors;
    float stackStep = glm::pi<float>() / stacks;
    
    for (int i = 0; i <= stacks; ++i) {
        float stackAngle = glm::pi<float>() / 2 - i * stackStep;
        xy = radius * cos(stackAngle);
        z = radius * sin(stackAngle);
        
        for (int j = 0; j <= sectors; ++j) {
            float sectorAngle = j * sectorStep;
            x = xy * cos(sectorAngle);
            y = xy * sin(sectorAngle);
            
            glm::vec3 pos(x, y, z);
            glm::vec3 normal = glm::normalize(pos);
            glm::vec2 texCoord(float(j) / sectors, float(i) / stacks);
            
            Vertex v;
            v.Position = pos;
            v.Normal = normal;
            v.TexCoords = texCoord;
            v.Tangent = glm::vec3(1, 0, 0);
            v.Bitangent = glm::vec3(0, 1, 0);
            vertices.push_back(v);
        }
    }
    
    for (int i = 0; i < stacks; ++i) {
        unsigned int k1 = i * (sectors + 1);
        unsigned int k2 = k1 + sectors + 1;
        
        for (int j = 0; j < sectors; ++j, ++k1, ++k2) {
            if (i != 0) {
                indices.push_back(k1);
                indices.push_back(k2);
                indices.push_back(k1 + 1);
            }
            if (i != (stacks - 1)) {
                indices.push_back(k1 + 1);
                indices.push_back(k2);
                indices.push_back(k2 + 1);
            }
        }
    }
}