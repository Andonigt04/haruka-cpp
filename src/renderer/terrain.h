/**
 * @file terrain.h
 * @brief Heightmap-based terrain mesh with patch LOD support.
 */
#ifndef TERRAIN_H
#define TERRAIN_H

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>
#include "shader.h"
#include "texture.h"

class Camera;

namespace Haruka {

/** @brief Patch of terrain geometry with its own LOD buffers. */
struct TerrainPatch {
    glm::vec2 offset;
    int lod;
    unsigned int VAO, VBO, EBO;
    unsigned int indexCount;
};

/**
 * @brief Heightmap-based terrain mesh with patch LOD support.
 */
class Terrain {
public:
    /** @brief Constructs terrain with a fixed logical size and scale. */
    Terrain(int size = 1024, float scale = 100.0f);
    /** @brief Releases terrain resources. */
    ~Terrain();
    
    /** @brief Loads height data from an image file. */
    void loadHeightmap(const std::string& filepath);
    /** @brief Generates terrain height data procedurally using Perlin noise. */
    void generatePerlin(int seed = 0);
    
    /** @brief Sets world position of the terrain origin. */
    void setPosition(const glm::vec3& pos) { position = pos; }
    /** @brief Sets scale applied to terrain geometry. */
    void setScale(const glm::vec3& scale) { terrainScale = scale; }
    
    /** @brief Renders terrain patches using the provided shader and camera position. */
    void render(Shader& shader, const Camera* camera);
    
    /** @brief Samples world-space height at X/Z. */
    float getHeight(float x, float z) const;
    /** @brief Computes a terrain normal at X/Z. */
    glm::vec3 getNormal(float x, float z) const;

    /** @brief Generates mesh buffers from current height data. */
    void generateMesh();
    /** @brief Creates one terrain patch at grid coordinates. */
    void createPatch(int x, int z, int lod);
    /** @brief Calculates the best LOD for a patch given camera position. */
    int calculateLOD(const glm::vec2& patchCenter, const glm::dvec3& cameraPos);
    
    /** @brief Returns normalized height sample. */
    float getHeightNormalized(int x, int z) const;
    bool isPatchVisible(const glm::vec2& patchCenter, const Camera* camera);
    glm::mat4 calculateProjectionMatrix(float aspectRatio);
    bool isInsideFrustum(const glm::vec3& point, const Camera* camera);

private:
    int size;
    float scale;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 terrainScale = glm::vec3(1.0f);
    
    std::vector<float> heightData;
    std::vector<TerrainPatch> patches;
    
    // LOD settings
    float lodDistance[4] = {50.0f, 100.0f, 200.0f, 400.0f};
    int patchSize = 64;
};

}

#endif