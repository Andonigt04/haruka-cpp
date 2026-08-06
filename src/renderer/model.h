#ifndef MODEL_H
#define MODEL_H

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <vector>
#include <string>

#include "mesh.h"

namespace Haruka { namespace RHI { class Context; } }
namespace Haruka { namespace Renderer { class GPUInstancing; } }

namespace Haruka { namespace Renderer {

unsigned int TextureFromFile(const char *path, const std::string &directory, const aiScene *scene,
                             Haruka::RHI::TextureHandle* outHandle = nullptr);

class Model
{
public:
    Model(const std::string &path) { loadModel(path); }
    ~Model();

    void drawRHI(Haruka::RHI::Context& ctx);
    void drawInstancedRHI(Haruka::RHI::Context& ctx, Haruka::Renderer::GPUInstancing& inst,
                          uint32_t instanceBinding = 1);

    int getVertexCount() const {
        int total = 0;
        for (const auto& mesh : meshes) total += mesh.getVertexCount();
        return total;
    }
    int getTriangleCount() const {
        int total = 0;
        for (const auto& mesh : meshes) total += mesh.getTriangleCount();
        return total;
    }

    bool      hasBounds()  const { return m_hasBounds; }
    glm::vec3 boundsMin()  const { return m_min; }
    glm::vec3 boundsMax()  const { return m_max; }

    const std::vector<Mesh>& getMeshes() const { return meshes; }
private:
    std::vector<Mesh> meshes;
    std::string directory;
    std::vector<MeshTexture> textures_loaded;

    Assimp::Importer importer;
    const aiScene* scene = nullptr;

    glm::vec3 m_min{0.0f}, m_max{0.0f};
    bool      m_hasBounds = false;

    void loadModel(std::string const &path);
    void processNode(aiNode *node, const aiScene *scene, const glm::mat4& parentTransform);
    Mesh processMesh(aiMesh *mesh, const aiScene *scene, const glm::mat4& transform);
    std::vector<MeshTexture> loadMaterialTextures(aiMaterial *mat, aiTextureType type, std::string typeName);
};

}} // namespace Haruka::Renderer

using Haruka::Renderer::Model;
using Haruka::Renderer::TextureFromFile;
namespace Haruka { using Renderer::Model; }
#endif
