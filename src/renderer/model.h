/**
 * @file model.h
 * @brief Assimp-backed model loader — parses 3D assets into engine `Mesh` instances.
 */
#ifndef MODEL_H
#define MODEL_H

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <vector>
#include <string>

#include "mesh.h"
#include "shader.h"

/** @brief Loads texture resource from model directory context. */
unsigned int TextureFromFile(const char *path, const std::string &directory, const aiScene *scene);

/**
 * @brief Assimp-backed model loader and draw wrapper.
 *
 * Owns parsed meshes and deduplicated texture descriptors.
 */
class Model
{
public:
    /** @brief Loads model at construction time. */
    Model(const std::string &path) { loadModel(path); }
    
    /** @brief Draws all internal meshes. */
    void Draw(Shader &shader);

    /** @brief Aggregated vertex count across all sub-meshes. */
    int getVertexCount() const {
        int total = 0;
        for (const auto& mesh : meshes) total += mesh.getVertexCount();
        return total;
    }
    /** @brief Aggregated triangle count across all sub-meshes. */
    int getTriangleCount() const {
        int total = 0;
        for (const auto& mesh : meshes) total += mesh.getTriangleCount();
        return total;
    }

    /** @brief AABB del modelo (espacio del modelo, ya con las transforms de nodos
     *  aplicadas). Para colisión: caja ajustada al modelo, sin tunear a mano. */
    bool      hasBounds()  const { return m_hasBounds; }
    glm::vec3 boundsMin()  const { return m_min; }
    glm::vec3 boundsMax()  const { return m_max; }
private:
    std::vector<Mesh> meshes;
    std::string directory;
    std::vector<MeshTexture> textures_loaded;

    Assimp::Importer importer;
    const aiScene* scene = nullptr;

    glm::vec3 m_min{0.0f}, m_max{0.0f}; // AABB acumulada al cargar
    bool      m_hasBounds = false;

    /** @brief Parses model file and initializes node traversal. */
    void loadModel(std::string const &path);
    /** @brief Recursively processes one Assimp node hierarchy branch, acumulando la
     *  transform del nodo (escala/posición de cada parte del modelo). */
    void processNode(aiNode *node, const aiScene *scene, const glm::mat4& parentTransform);
    /** @brief Converts one Assimp mesh into engine `Mesh`, aplicando la transform del nodo. */
    Mesh processMesh(aiMesh *mesh, const aiScene *scene, const glm::mat4& transform);
    /** @brief Loads material textures by semantic type with deduplication. */
    std::vector<MeshTexture> loadMaterialTextures(aiMaterial *mat, aiTextureType type, std::string typeName);
};
#endif