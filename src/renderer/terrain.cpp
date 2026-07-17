#include "terrain.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include "tools/error_reporter.h"
#include "rhi/rhi_device.h"

namespace { // Libera los buffers de un patch por el RHI.
    void freePatchBuffers(Haruka::TerrainPatch& p) {
        using namespace Haruka;
        if (RHI::valid(p.hVbo)) { if (RHI::Device* dev = RHI::device()) { dev->destroy(p.hVbo); dev->destroy(p.hEbo); } p.hVbo = p.hEbo = {}; }
    }
}
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
        freePatchBuffers(patch);
    }
}

void Terrain::loadHeightmap(const std::string& filepath) {
    int width, height, channels;
    unsigned char* data = stbi_load(filepath.c_str(), &width, &height, &channels, 1);
    
    if (!data) {
        HARUKA_RENDERER_ERROR(ErrorCode::TEXTURE_LOAD_FAILED, "Failed to load heightmap: " + filepath);
        return;
    }

    if (width != size || height != size) {
        HARUKA_RENDERER_ERROR(ErrorCode::ASSET_FORMAT_INVALID,
            "Heightmap size mismatch. Expected " + std::to_string(size) + "x" + std::to_string(size)
            + ", got " + std::to_string(width) + "x" + std::to_string(height));
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

// Hash-based value noise — deterministic, no global state, no periodic patterns.
static float valueNoise(int x, int z, int seed) {
    int n = x + z * 57 + seed * 131;
    n = (n << 13) ^ n;
    return 1.0f - static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0f;
}

static float smoothNoise(float x, float z, int seed) {
    int ix = static_cast<int>(std::floor(x));
    int iz = static_cast<int>(std::floor(z));
    float fx = x - ix;
    float fz = z - iz;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fz = fz * fz * (3.0f - 2.0f * fz);

    float v00 = valueNoise(ix,     iz,     seed);
    float v10 = valueNoise(ix + 1, iz,     seed);
    float v01 = valueNoise(ix,     iz + 1, seed);
    float v11 = valueNoise(ix + 1, iz + 1, seed);

    return v00 * (1 - fx) * (1 - fz) + v10 * fx * (1 - fz)
         + v01 * (1 - fx) *      fz  + v11 * fx *      fz;
}

void Terrain::generatePerlin(int seed) {
    for (int z = 0; z < size; z++) {
        for (int x = 0; x < size; x++) {
            float height = 0.0f;
            float amplitude = 1.0f;
            float frequency = 0.005f;

            for (int octave = 0; octave < 6; octave++) {
                height += smoothNoise(x * frequency, z * frequency, seed + octave * 1000) * amplitude;
                amplitude *= 0.5f;
                frequency *= 2.0f;
            }

            heightData[z * size + x] = (height + 1.0f) * 0.5f;
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

    // Skirt geometry: downward-hanging quads along each edge to cover LOD-mismatch cracks.
    // Each skirt vertex is a copy of the corresponding edge surface vertex, Y lowered by skirtDepth.
    const float skirtDepth = scale * terrainScale.y * 0.5f;
    int mainVertCount = verticesPerSide * verticesPerSide;

    // Helper: append a skirt vertex (copy of surface vertex at flat index fi, Y -= skirtDepth)
    auto addSkirtVert = [&](int fi) {
        int base = fi * 8;
        vertices.push_back(vertices[base + 0]);              // X
        vertices.push_back(vertices[base + 1] - skirtDepth); // Y (lowered)
        vertices.push_back(vertices[base + 2]);              // Z
        vertices.push_back(vertices[base + 3]);              // nx
        vertices.push_back(vertices[base + 4]);              // ny
        vertices.push_back(vertices[base + 5]);              // nz
        vertices.push_back(vertices[base + 6]);              // u
        vertices.push_back(vertices[base + 7]);              // v
    };

    // Helper: emit two triangles for a skirt quad (surface edge s0,s1 → skirt sk0,sk1)
    auto addSkirtQuad = [&](int s0, int s1, int sk0, int sk1) {
        indices.push_back(s0);  indices.push_back(s1);  indices.push_back(sk0);
        indices.push_back(s1);  indices.push_back(sk1); indices.push_back(sk0);
    };

    int N = verticesPerSide;

    // Bottom edge (z == 0): surface row 0, skirt appended starting at mainVertCount
    int skirtBase = mainVertCount;
    for (int x = 0; x < N; ++x) addSkirtVert(0 * N + x);
    for (int x = 0; x < N - 1; ++x)
        addSkirtQuad(0 * N + x, 0 * N + x + 1, skirtBase + x, skirtBase + x + 1);

    // Top edge (z == N-1)
    skirtBase = mainVertCount + N;
    for (int x = 0; x < N; ++x) addSkirtVert((N - 1) * N + x);
    for (int x = 0; x < N - 1; ++x)
        addSkirtQuad((N-1)*N + x + 1, (N-1)*N + x, skirtBase + x + 1, skirtBase + x);

    // Left edge (x == 0)
    skirtBase = mainVertCount + 2 * N;
    for (int z = 0; z < N; ++z) addSkirtVert(z * N + 0);
    for (int z = 0; z < N - 1; ++z)
        addSkirtQuad((z+1)*N, z*N, skirtBase + z + 1, skirtBase + z);

    // Right edge (x == N-1)
    skirtBase = mainVertCount + 3 * N;
    for (int z = 0; z < N; ++z) addSkirtVert(z * N + (N - 1));
    for (int z = 0; z < N - 1; ++z)
        addSkirtQuad(z*N + (N-1), (z+1)*N + (N-1), skirtBase + z, skirtBase + z + 1);

    // Create OpenGL buffers
    TerrainPatch patch;
    patch.offset = glm::vec2(startX, startZ);
    patch.lod = lod;
    patch.indexCount = indices.size();
    
    glGenVertexArrays(1, &patch.VAO);
    glBindVertexArray(patch.VAO);

    RHI::Device* dev = RHI::device();
    patch.hVbo = dev->createBuffer(RHI::BufferUsage::Vertex, vertices.size() * sizeof(float), vertices.data());
    patch.hEbo = dev->createBuffer(RHI::BufferUsage::Index,  indices.size() * sizeof(unsigned int), indices.data());
    patch.VBO = dev->nativeBuffer(patch.hVbo); patch.EBO = dev->nativeBuffer(patch.hEbo);
    glBindBuffer(GL_ARRAY_BUFFER, patch.VBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, patch.EBO);
    
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

    // LOD update pass: collect patches that need rebuilding, then recreate them.
    // Must NOT modify `patches` while iterating (push_back would invalidate refs).
    struct PatchUpdate { int idx; int newLod; };
    std::vector<PatchUpdate> updates;
    for (int i = 0; i < (int)patches.size(); ++i) {
        float cx = patches[i].offset.x + patchSize * 0.5f;
        float cz = patches[i].offset.y + patchSize * 0.5f;
        int target = calculateLOD(glm::vec2(cx, cz), camera->position);
        if (target != patches[i].lod) updates.push_back({i, target});
    }
    for (auto& u : updates) {
        TerrainPatch& p = patches[u.idx];
        if (p.VAO) glDeleteVertexArrays(1, &p.VAO);
        freePatchBuffers(p);
        glm::vec2 off = p.offset;
        createPatch(off.x, off.y, u.newLod); // appends to patches
        patches[u.idx] = patches.back();     // copy new data into slot
        patches.pop_back();                  // remove the duplicate at the end
    }

    // Compute frustum planes once for all patches (Gribb-Hartmann, glm column-major).
    glm::mat4 vp = camera->getProjectionMatrix() * camera->getViewMatrix();
    glm::vec4 planes[6];
    for (int i = 0; i < 4; ++i) {
        planes[0][i] = vp[i][3] + vp[i][0]; // left
        planes[1][i] = vp[i][3] - vp[i][0]; // right
        planes[2][i] = vp[i][3] + vp[i][1]; // bottom
        planes[3][i] = vp[i][3] - vp[i][1]; // top
        planes[4][i] = vp[i][3] + vp[i][2]; // near
        planes[5][i] = vp[i][3] - vp[i][2]; // far
    }

    glm::mat4 model = glm::translate(glm::mat4(1.0f), position);
    shader.setMat4("model", model);

    for (auto& patch : patches) {

        // AABB del patch en mundo
        float minX = patch.offset.x * terrainScale.x + position.x;
        float minZ = patch.offset.y * terrainScale.z + position.z;
        float maxX = (patch.offset.x + patchSize) * terrainScale.x + position.x;
        float maxZ = (patch.offset.y + patchSize) * terrainScale.z + position.z;
        float minY = position.y;
        float maxY = position.y + scale * terrainScale.y;

        glm::vec3 aabbCorners[8] = {
            {minX, minY, minZ}, {maxX, minY, minZ}, {minX, minY, maxZ}, {maxX, minY, maxZ},
            {minX, maxY, minZ}, {maxX, maxY, minZ}, {minX, maxY, maxZ}, {maxX, maxY, maxZ}
        };
        bool visible = true;
        for (int p = 0; p < 6 && visible; ++p) {
            bool allOut = true;
            for (int c = 0; c < 8; ++c) {
                float d = planes[p].x * aabbCorners[c].x
                        + planes[p].y * aabbCorners[c].y
                        + planes[p].z * aabbCorners[c].z
                        + planes[p].w;
                if (d >= 0.0f) { allOut = false; break; }
            }
            if (allOut) visible = false;
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

} // namespace Haruka
