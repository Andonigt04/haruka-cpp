#include "terrain.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <cmath>
#include <algorithm>
#include "stb_image.h"
#include "../core/camera.h"

namespace Haruka {

Terrain::Terrain(int size, float scale)
    : size(size), scale(scale) {
    heightData.resize(size * size, 0.0f);
}

Terrain::~Terrain() {
    for (auto& patch : patches) {
        if (patch.VAO) glDeleteVertexArrays(1, &patch.VAO);
        if (patch.VBO) glDeleteBuffers(1, &patch.VBO);
        if (patch.EBO) glDeleteBuffers(1, &patch.EBO);
    }
}

void Terrain::loadHeightmap(const std::string& filepath) {
    int width, height, channels;
    unsigned char* data = stbi_load(filepath.c_str(), &width, &height, &channels, 1);
    
    if (!data) {
        std::cerr << "[Terrain] Failed to load heightmap: " << filepath << std::endl;
        return;
    }
    
    // Resize if needed
    if (width != size || height != size) {
        std::cerr << "[Terrain] Heightmap size mismatch. Expected " << size << "x" << size 
                  << ", got " << width << "x" << height << std::endl;
    }
    
    int actualSize = std::min(width, size);
    for (int z = 0; z < actualSize; z++) {
        for (int x = 0; x < actualSize; x++) {
            heightData[z * size + x] = data[z * width + x] / 255.0f;
        }
    }
    
    stbi_image_free(data);
    
    std::cout << "[Terrain] Loaded heightmap: " << filepath << std::endl;
    generateMesh();
}

void Terrain::generatePerlin(int seed) {
    // Simple Perlin-like noise
    srand(seed);
    
    for (int z = 0; z < size; z++) {
        for (int x = 0; x < size; x++) {
            float height = 0.0f;
            float amplitude = 1.0f;
            float frequency = 0.005f;
            
            // Multiple octaves
            for (int octave = 0; octave < 6; octave++) {
                float sampleX = x * frequency;
                float sampleZ = z * frequency;
                
                float noise = sin(sampleX) * cos(sampleZ) + 
                             sin(sampleX * 2.0f) * cos(sampleZ * 2.0f) * 0.5f;
                
                height += noise * amplitude;
                
                amplitude *= 0.5f;
                frequency *= 2.0f;
            }
            
            heightData[z * size + x] = (height + 1.0f) * 0.5f; // Normalize to 0-1
        }
    }
    
    std::cout << "[Terrain] Generated procedural terrain" << std::endl;
    generateMesh();
}

void Terrain::generateMesh() {
    patches.clear();
    std::cout << "[Terrain] GENERATE MESH: size=" << size << ", patchSize=" << patchSize << ", terrainScale=(" << terrainScale.x << "," << terrainScale.y << "," << terrainScale.z << ") position=(" << position.x << "," << position.y << "," << position.z << ")" << std::endl << std::flush;

    int numPatchesX = (size + patchSize - 1) / patchSize;
    int numPatchesZ = (size + patchSize - 1) / patchSize;

    for (int pz = 0; pz < numPatchesZ; pz++) {
        for (int px = 0; px < numPatchesX; px++) {
            int startX = px * patchSize;
            int startZ = pz * patchSize;
            int patchWidth = std::min(patchSize, size - startX);
            int patchHeight = std::min(patchSize, size - startZ);
            if (patchWidth > 1 && patchHeight > 1) {
                std::cout << "[Terrain] Patch px=" << px << ", pz=" << pz << ", startX=" << startX << ", startZ=" << startZ << ", patchWidth=" << patchWidth << ", patchHeight=" << patchHeight << std::endl << std::flush;
                createPatch(startX, startZ, 0);
            }
        }
    }

    std::cout << "[Terrain] Generated " << patches.size() << " patches" << std::endl << std::flush;
}

void Terrain::createPatch(int startX, int startZ, int lod) {
    int step = 1 << lod; // 1, 2, 4, 8...
    int verticesPerSide = (patchSize / step) + 1;
    
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    
    // Generate vertices
    for (int z = 0; z < verticesPerSide; z++) {
        for (int x = 0; x < verticesPerSide; x++) {
            int actualX = startX + x * step;
            int actualZ = startZ + z * step;
            
            if (actualX >= size) actualX = size - 1;
            if (actualZ >= size) actualZ = size - 1;
            
            float height = getHeightNormalized(actualX, actualZ) * scale;
            
            // Position
            vertices.push_back(actualX * terrainScale.x);
            vertices.push_back(height * terrainScale.y);
            vertices.push_back(actualZ * terrainScale.z);
            
            // Normal (calculated from neighbors)
            glm::vec3 normal = getNormal(actualX, actualZ);
            vertices.push_back(normal.x);
            vertices.push_back(normal.y);
            vertices.push_back(normal.z);
            
            // TexCoord globales para continuidad entre patches
            vertices.push_back((float)actualX / (size - 1));
            vertices.push_back((float)actualZ / (size - 1));
        }
    }
    
    // Generate indices
    for (int z = 0; z < verticesPerSide - 1; z++) {
        for (int x = 0; x < verticesPerSide - 1; x++) {
            int topLeft = z * verticesPerSide + x;
            int topRight = topLeft + 1;
            int bottomLeft = (z + 1) * verticesPerSide + x;
            int bottomRight = bottomLeft + 1;
            
            // Triangle 1
            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);
            
            // Triangle 2
            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }
    
    // Create OpenGL buffers
    TerrainPatch patch;
    patch.offset = glm::vec2(startX, startZ);
    patch.lod = lod;
    patch.indexCount = indices.size();
    
    glGenVertexArrays(1, &patch.VAO);
    glGenBuffers(1, &patch.VBO);
    glGenBuffers(1, &patch.EBO);
    
    glBindVertexArray(patch.VAO);
    
    glBindBuffer(GL_ARRAY_BUFFER, patch.VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, patch.EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
    
    // Position
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Normal
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // TexCoord
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    
    glBindVertexArray(0);
    
    patches.push_back(patch);
}

int Terrain::calculateLOD(const glm::vec2& patchCenter, const glm::dvec3& cameraPos) {
    float distance = glm::length(cameraPos - glm::dvec3(patchCenter.x, 0.0, patchCenter.y));
    
    for (int i = 0; i < 4; i++) {
        if (distance < lodDistance[i]) {
            return i;
        }
    }
    
    return 3; // Max LOD
}

void Terrain::render(Shader& shader, const Camera* camera) {
    shader.use();
    
    for (auto& patch : patches) {
        // Centro real del patch en mundo
        float patchCenterX = patch.offset.x + patchSize * 0.5f;
        float patchCenterZ = patch.offset.y + patchSize * 0.5f;
        glm::vec2 patchCenter = glm::vec2(patchCenterX, patchCenterZ);
        int targetLOD = calculateLOD(patchCenter, camera->position);

        // Recreate patch if LOD changed
        if (targetLOD != patch.lod) {
            if (patch.VAO) glDeleteVertexArrays(1, &patch.VAO);
            if (patch.VBO) glDeleteBuffers(1, &patch.VBO);
            if (patch.EBO) glDeleteBuffers(1, &patch.EBO);

            createPatch(patch.offset.x, patch.offset.y, targetLOD);
            patch = patches.back();
        }

        glm::mat4 model = glm::translate(glm::mat4(1.0f), position);
        shader.setMat4("model", model);

        // AABB del patch en mundo
        float minX = patch.offset.x * terrainScale.x + position.x;
        float minZ = patch.offset.y * terrainScale.z + position.z;
        float maxX = (patch.offset.x + patchSize) * terrainScale.x + position.x;
        float maxZ = (patch.offset.y + patchSize) * terrainScale.z + position.z;
        float minY = position.y; // Asumimos terreno plano en Y, o puedes calcular min/max real de alturas
        float maxY = position.y + scale * terrainScale.y;

        // 8 vértices del AABB
        glm::vec3 aabbCorners[8] = {
            {minX, minY, minZ}, {maxX, minY, minZ}, {minX, minY, maxZ}, {maxX, minY, maxZ},
            {minX, maxY, minZ}, {maxX, maxY, minZ}, {minX, maxY, maxZ}, {maxX, maxY, maxZ}
        };

        bool visible = false;
        for (int i = 0; i < 8; ++i) {
            if (isInsideFrustum(aabbCorners[i], camera)) {
                visible = true;
                break;
            }
        }
        if (visible) {
            glBindVertexArray(patch.VAO);
            glDrawElements(GL_TRIANGLES, patch.indexCount, GL_UNSIGNED_INT, 0);
            glBindVertexArray(0);
        }
    }
}

float Terrain::getHeightNormalized(int x, int z) const {
    // Clamp en vez de devolver 0 para bordes
    int clampedX = std::max(0, std::min(x, size - 1));
    int clampedZ = std::max(0, std::min(z, size - 1));
    return heightData[clampedZ * size + clampedX];
}

float Terrain::getHeight(float x, float z) const {
    x /= terrainScale.x;
    z /= terrainScale.z;
    
    if (x < 0 || x >= size - 1 || z < 0 || z >= size - 1) return 0.0f;
    
    int ix = (int)x;
    int iz = (int)z;
    float fx = x - ix;
    float fz = z - iz;
    
    // Bilinear interpolation
    float h00 = getHeightNormalized(ix, iz);
    float h10 = getHeightNormalized(ix + 1, iz);
    float h01 = getHeightNormalized(ix, iz + 1);
    float h11 = getHeightNormalized(ix + 1, iz + 1);
    
    float h0 = h00 * (1.0f - fx) + h10 * fx;
    float h1 = h01 * (1.0f - fx) + h11 * fx;
    
    return (h0 * (1.0f - fz) + h1 * fz) * scale * terrainScale.y;
}

glm::vec3 Terrain::getNormal(float x, float z) const {
    float heightL = getHeight(x - 1, z);
    float heightR = getHeight(x + 1, z);
    float heightD = getHeight(x, z - 1);
    float heightU = getHeight(x, z + 1);
    
    glm::vec3 normal = glm::normalize(glm::vec3(heightL - heightR, 2.0f, heightD - heightU));
    return normal;
}

bool Terrain::isPatchVisible(const glm::vec2& patchCenter, const Camera* camera) {
    // Convención OpenGL: +Y arriba, +Z adelante
    // Usar matriz de vista y proyección estándar
    glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();

    // Extraer planos del frustum (formato: ax + by + cz + d = 0)
    // Referencia: Gribb & Hartmann, "Fast Extraction of Viewing Frustum Planes from the World-View-Projection Matrix"
    glm::vec4 planes[6];
    // Left
    planes[0] = glm::vec4(
        viewProj[0][3] + viewProj[0][0],
        viewProj[1][3] + viewProj[1][0],
        viewProj[2][3] + viewProj[2][0],
        viewProj[3][3] + viewProj[3][0]
    );
    // Right
    planes[1] = glm::vec4(
        viewProj[0][3] - viewProj[0][0],
        viewProj[1][3] - viewProj[1][0],
        viewProj[2][3] - viewProj[2][0],
        viewProj[3][3] - viewProj[3][0]
    );
    // Bottom
    planes[2] = glm::vec4(
        viewProj[0][3] + viewProj[0][1],
        viewProj[1][3] + viewProj[1][1],
        viewProj[2][3] + viewProj[2][1],
        viewProj[3][3] + viewProj[3][1]
    );
    // Top
    planes[3] = glm::vec4(
        viewProj[0][3] - viewProj[0][1],
        viewProj[1][3] - viewProj[1][1],
        viewProj[2][3] - viewProj[2][1],
        viewProj[3][3] - viewProj[3][1]
    );
    // Near
    planes[4] = glm::vec4(
        viewProj[0][3] + viewProj[0][2],
        viewProj[1][3] + viewProj[1][2],
        viewProj[2][3] + viewProj[2][2],
        viewProj[3][3] + viewProj[3][2]
    );
    // Far
    planes[5] = glm::vec4(
        viewProj[0][3] - viewProj[0][2],
        viewProj[1][3] - viewProj[1][2],
        viewProj[2][3] - viewProj[2][2],
        viewProj[3][3] - viewProj[3][2]
    );

    // Normalizar planos
    for (int i = 0; i < 6; ++i) {
        float len = glm::length(glm::vec3(planes[i]));
        if (len > 0.0f) planes[i] /= len;
    }


    glm::vec3 center(patchCenter.x, 0.0f, patchCenter.y);
    for (int i = 0; i < 6; ++i) {
        if (planes[i].x * center.x + planes[i].y * center.y + planes[i].z * center.z + planes[i].w < 0.0f) {
            return false;
        }
    }
    return true;
}

// Comprueba si un punto está dentro del frustum de la cámara
bool Terrain::isInsideFrustum(const glm::vec3& point, const Camera* camera) {
    glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
    glm::vec4 clipSpace = viewProj * glm::vec4(point, 1.0f);
    // Perspectiva: divide por w
    if (clipSpace.w == 0.0f) return false;
    glm::vec3 ndc = glm::vec3(clipSpace) / clipSpace.w;
    // NDC debe estar en [-1, 1]
    return ndc.x >= -1.0f && ndc.x <= 1.0f && ndc.y >= -1.0f && ndc.y <= 1.0f && ndc.z >= -1.0f && ndc.z <= 1.0f;
}

glm::mat4 Terrain::calculateProjectionMatrix(float aspectRatio) {
    float fov = glm::radians(60.0f);
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    return glm::perspective(fov, aspectRatio, nearPlane, farPlane);
}

} // namespace Haruka
