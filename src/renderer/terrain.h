#ifndef TERRAIN_H
#define TERRAIN_H

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>
#include "rhi/rhi_types.h"

namespace Haruka { namespace RHI { class Context; class Device; } }
namespace Haruka { namespace Core { class Camera; } } using Haruka::Core::Camera;

namespace Haruka {

struct TerrainPatch {
    glm::vec2 offset;
    int lod;
    Haruka::RHI::BufferHandle hVbo, hEbo;
    unsigned int indexCount;
};

class Terrain {
public:
    Terrain(int size = 1024, float scale = 100.0f);
    ~Terrain();

    void loadHeightmap(const std::string& filepath);
    void generatePerlin(int seed = 0);

    void setPosition(const glm::vec3& pos) { position = pos; }
    void setScale(const glm::vec3& scale) { terrainScale = scale; }

    void render(Haruka::RHI::Context& ctx);

    float getHeight(float x, float z) const;
    glm::vec3 getNormal(float x, float z) const;

    void generateMesh();
    void createPatch(int x, int z, int lod);
    int calculateLOD(const glm::vec2& patchCenter, const glm::dvec3& cameraPos);

    float getHeightNormalized(int x, int z) const;

    void setCamera(const Camera* cam) { m_camera = cam; }

private:
    int size;
    float scale;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 terrainScale = glm::vec3(1.0f);

    std::vector<float> heightData;
    std::vector<TerrainPatch> patches;

    float lodDistance[4] = {50.0f, 100.0f, 200.0f, 400.0f};
    int patchSize = 64;

    const Camera* m_camera = nullptr;
    Haruka::RHI::BufferHandle m_patchUBO;
};

} // namespace Haruka

#endif
